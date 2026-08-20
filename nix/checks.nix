{ pkgs, rustToolchain }:

let
  src = builtins.path {
    path = ../.;
    name = "uds-rdma-proxy-src";
    filter = path: type:
      let
        baseName = builtins.baseNameOf path;
      in
      # Exclude kbuild artifacts that a local `make -C kernel` leaves in
      # the working tree (urp.mod.c would otherwise match the *.c rule
      # below and pollute static-analysis runs).
      !(pkgs.lib.hasSuffix ".mod.c" baseName) &&
      (
      # Include Cargo workspace and every member crate. cargo refuses to
      # operate on a workspace whose members are missing -- even if we
      # only invoke it on one crate -- so all member directories must be
      # present in the source closure.
      (type == "directory" && (
        baseName == "crates" ||
        baseName == "uds-rdma-protocol" ||
        baseName == "uds-rdma-protocol-ffi" ||
        baseName == "urp-bench" ||
        baseName == "urp-cli" ||
        baseName == "urp-netlink" ||
        baseName == "urp-control" ||
        baseName == "src" ||
        baseName == "commands" ||
        baseName == "tests"
      )) ||
      # Plus the proto tree -- urp-control's build.rs codegens from it, and
      # cargo parses every workspace member even for `-p <other>`.
      (type == "directory" && (
        baseName == "proto" ||
        baseName == "urp_control" ||
        baseName == "v1"
      )) ||
      (pkgs.lib.hasSuffix ".proto" baseName) ||
      (baseName == "Cargo.toml") ||
      (baseName == "Cargo.lock") ||
      (pkgs.lib.hasSuffix ".rs" baseName) ||
      # Include kernel sources and subdirectories
      (type == "directory" && (
        baseName == "kernel" ||
        baseName == "include" ||
        baseName == "uapi" ||
        baseName == "linux"
      )) ||
      (baseName == "Kbuild") ||
      (baseName == "Makefile" && pkgs.lib.hasInfix "/kernel" (toString path)) ||
      (pkgs.lib.hasSuffix ".c" baseName) ||
      (pkgs.lib.hasSuffix ".h" baseName)
      );
  };

  # Build urp.ko against a given kernel package. The optional
  # @rustFfi argument selects the Rust-backed reorder buffer
  # (Phase 3a Step 5b): if non-null, its result/lib/liburp_protocol_ffi.a
  # is copied into kernel/ and Kbuild sets CONFIG_URP_REORDER_RUST=y
  # via KCFLAGS.
  buildUrpKoWith = { kernelPackages, rustFfi ? null }:
    pkgs.stdenv.mkDerivation {
      name =
        if rustFfi == null
        then "urp-ko-${kernelPackages.kernel.version}"
        else "urp-ko-rust-${kernelPackages.kernel.version}";
      inherit src;
      nativeBuildInputs = [ pkgs.gnumake ] ++
        kernelPackages.kernel.moduleBuildDependencies;
      buildPhase =
        let
          kdir = "${kernelPackages.kernel.dev}/lib/modules/${kernelPackages.kernel.modDirVersion}/build";
          extraCfg = if rustFfi == null then "" else ''
            # Pre-extract the Rust staticlib's objects so Kbuild can list
            # them as urp-objs. ld -r (used to build module objects) does
            # not pull symbols out of static archives the way a normal
            # link would, so the cleanest option is to materialize the
            # constituent .o files and link them directly.
            mkdir -p kernel/rust_ffi
            (cd kernel/rust_ffi && \
                ar x ${rustFfi}/lib/liburp_protocol_ffi.a)

            # Strip compiler_builtins x86 FMA helpers that objtool can't
            # decode (compiler_builtins::math::libm_math::arch::x86::fma::*).
            # The reorder buffer doesn't use floating point at all, so
            # these are dead weight pulled in by rustc's standard
            # library glue. Walk every extracted .o and drop sections
            # whose name mentions compiler_builtins ... arch::x86::fma.
            for o in kernel/rust_ffi/*.o; do
              # List sections containing the problem prefix.
              for sec in $(${pkgs.binutils}/bin/objdump -h "$o" \
                  | awk '/compiler_builtins.*arch.*x86.*fma/ {print $2}'); do
                ${pkgs.binutils}/bin/objcopy --remove-section="$sec" "$o" || true
              done
            done

            export KCFLAGS="-DCONFIG_URP_REORDER_RUST=1"
            export CONFIG_URP_REORDER_RUST=y
          '';
          # Cross-compilation (Phase 5 cross-arch): derive ARCH +
          # CROSS_COMPILE from this derivation's own target platform. For a
          # native build pkgs.stdenv.hostPlatform is x86_64 -> ARCH=x86,
          # targetPrefix="" (both no-ops matching the kernel's own default).
          # When checks.nix is imported with a pkgsCross.<arch> pkgs the same
          # expressions yield e.g. ARCH=arm64 CROSS_COMPILE=aarch64-...-.
          crossVars = "ARCH=${pkgs.stdenv.hostPlatform.linuxArch} CROSS_COMPILE=${pkgs.stdenv.cc.targetPrefix}";
          rustVar = if rustFfi == null then "" else "CONFIG_URP_REORDER_RUST=y";
        in ''
          ${extraCfg}
          make -C ${kdir} M=$PWD/kernel ${crossVars} ${rustVar} modules
        '';
      installPhase = ''
        mkdir -p $out/lib/modules/${kernelPackages.kernel.modDirVersion}
        cp kernel/urp.ko $out/lib/modules/${kernelPackages.kernel.modDirVersion}/
      '';
    };

  buildUrpKo = kernelPackages: buildUrpKoWith {
    inherit kernelPackages;
    rustFfi = null;
  };
in
{
  # src is consumed by nix/analysis/ (static-analysis targets) so the
  # analyzers see exactly the sources the module build sees.
  inherit src buildUrpKo buildUrpKoWith;

  # cargo test -- runs all protocol crate tests. Uses rustPlatform.buildRustPackage
  # so cargo dependencies are vendored from Cargo.lock rather than fetched from
  # crates.io (the Nix sandbox has no network access). The derivation runs the
  # tests during its check phase; the install phase is a sentinel so Nix can
  # cache the success.
  protocol-tests = pkgs.rustPlatform.buildRustPackage {
    pname = "uds-rdma-protocol-tests";
    version = "0.1.0";
    inherit src;

    cargoLock.lockFile = ../Cargo.lock;
    nativeBuildInputs = [ rustToolchain ];

    cargoBuildFlags = [ "-p" "uds-rdma-protocol" ];
    cargoTestFlags  = [ "-p" "uds-rdma-protocol" ];

    # Also exercise the `--no-default-features` (no_std + alloc) path so the
    # crate's no_std posture stays honest. We piggy-back on the pre-checkPhase
    # because the default check runs with default features.
    preCheck = ''
      cargo test -p uds-rdma-protocol --no-default-features --offline
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p $out
      touch $out/passed
      runHook postInstall
    '';
  };

  # urp-bench Rust twin tests (design 30 B5, Rust half). Same sentinel
  # pattern as protocol-tests; the crate is dependency-free until the
  # io_uring backend (B4) lands, so this runs fully sandboxed.
  urp-bench-rs-tests = pkgs.rustPlatform.buildRustPackage {
    pname = "urp-bench-rs-tests";
    version = "0.1.0";
    inherit src;

    cargoLock.lockFile = ../Cargo.lock;
    nativeBuildInputs = [ rustToolchain ];

    cargoBuildFlags = [ "-p" "urp-bench" ];
    cargoTestFlags  = [ "-p" "urp-bench" ];

    installPhase = ''
      runHook preInstall
      mkdir -p $out
      touch $out/passed
      runHook postInstall
    '';
  };

  # urp-netlink lib tests (design 33 Phase 3 PR1). Same sentinel pattern;
  # covers the uapi<->kernel-header mirror test (moved here from urp-cli)
  # plus the attr/format codecs and the is_endpoint_ready predicate. The
  # crate is dependency-free (libc/thiserror/serde), so this runs fully
  # sandboxed. Also builds urp-cli's tests, which now depend on the lib.
  urp-netlink-tests = pkgs.rustPlatform.buildRustPackage {
    pname = "urp-netlink-tests";
    version = "0.1.0";
    inherit src;

    cargoLock.lockFile = ../Cargo.lock;
    nativeBuildInputs = [ rustToolchain ];

    cargoBuildFlags = [ "-p" "urp-netlink" "-p" "urp-cli" ];
    cargoTestFlags  = [ "-p" "urp-netlink" "-p" "urp-cli" ];

    installPhase = ''
      runHook preInstall
      mkdir -p $out
      touch $out/passed
      runHook postInstall
    '';
  };

  # urp-control control-plane tests (design 33 Phase 3): the pure logic.rs
  # truth tables + the two-process loopback integration (serve+connect over
  # 127.0.0.1). build.rs runs tonic-build, so protoc is a nativeBuildInput and
  # PROTOC is set for the offline sandbox. Runs fully sandboxed (loopback TCP,
  # fake state source, injected sd_notify + RDMA-failure signal -- no hardware).
  urp-control-tests = pkgs.rustPlatform.buildRustPackage {
    pname = "urp-control-tests";
    version = "0.1.0";
    inherit src;

    cargoLock.lockFile = ../Cargo.lock;
    nativeBuildInputs = [ rustToolchain pkgs.protobuf ];
    PROTOC = "${pkgs.protobuf}/bin/protoc";

    cargoBuildFlags = [ "-p" "urp-control" ];
    cargoTestFlags  = [ "-p" "urp-control" ];

    installPhase = ''
      runHook preInstall
      mkdir -p $out
      touch $out/passed
      runHook postInstall
    '';
  };

  # urp-bench pure-core table tests (design 30 B5, C half). The core is
  # liburing-free by design, so this builds and runs fully inside the
  # sandbox. Own fileset (not the shared `src` above): tools/ is outside
  # that filter's directory allowlist, and the kernel/analysis builds
  # shouldn't grow a tools/ dependency just for this check.
  urp-bench-units = pkgs.stdenv.mkDerivation {
    name = "urp-bench-units";
    src = pkgs.lib.fileset.toSource {
      root = ../.;
      fileset = pkgs.lib.fileset.unions [
        ../tools/urp-bench-core.c
        ../tools/urp-bench-core.h
        ../tools/urp-bench-test.c
      ];
    };
    buildPhase = ''
      $CC -Wall -Wextra -Werror -O2 -I tools \
        -o urp-bench-test tools/urp-bench-test.c tools/urp-bench-core.c
    '';
    doCheck = true;
    checkPhase = ''
      ./urp-bench-test
    '';
    installPhase = ''
      mkdir -p $out
      touch $out/passed
    '';
  };

  # urp-fast command validators (design 31 section 31.10). The pure
  # trust-boundary checks in kernel/urp_cmd_validate.c compile both into
  # urp.ko and, here, into a plain userspace binary -- the same source, so a
  # boundary bug is caught by this fast sandboxed run, not only by the slow
  # KUnit-in-VM pass. -I kernel lets the "" includes resolve exactly as they
  # do under kbuild.
  urp-fast-validate-units = pkgs.stdenv.mkDerivation {
    name = "urp-fast-validate-units";
    src = pkgs.lib.fileset.toSource {
      root = ../.;
      fileset = pkgs.lib.fileset.unions [
        ../kernel/urp_cmd_validate.c
        ../kernel/urp_cmd.h
        ../kernel/urp_cmd_own.h
        ../kernel/urp_cmd_compat.h
        ../kernel/include/uapi/linux/urp_cmd.h
        ../tools/urp-fast-validate-test.c
      ];
    };
    buildPhase = ''
      $CC -Wall -Wextra -Werror -O2 -I kernel \
        -o urp-fast-validate-test \
        tools/urp-fast-validate-test.c kernel/urp_cmd_validate.c
    '';
    doCheck = true;
    checkPhase = ''
      ./urp-fast-validate-test
    '';
    installPhase = ''
      mkdir -p $out
      touch $out/passed
    '';
  };

  # NOTE: miri requires network access to build its sysroot (fetches cfg-if for std).
  # This is incompatible with Nix's sandbox. Run miri via devshell instead:
  #   nix develop --command cargo miri test -p uds-rdma-protocol

  # kernel module build (against flake-pinned latest mainline kernel, for CI).
  # Using linuxPackages_latest (rather than linuxPackages) because this code
  # is intended for upstream review by the Linux kernel network dev team --
  # they expect new code to compile against current mainline.
  kernel-module-build = buildUrpKo pkgs.linuxPackages_latest;

  # Kernel-version matrix (Phase 5 DoD 7): build urp.ko against each
  # supported LTS in addition to `kernel-module-build` (= latest mainline).
  # These are ordinary derivations -- the kernel dev headers come from the
  # binary cache -- so they double as sandbox-safe `nix flake check` gates.
  # The `urp_sockaddr_t` compat shim in kernel/urp.h is what keeps the
  # pre-7.0 LTS builds green (kernel_connect/kernel_bind took
  # `struct sockaddr *` before the 7.0 unsized-sockaddr rework).
  urp-ko-6_1  = buildUrpKo pkgs.linuxPackages_6_1;
  urp-ko-6_6  = buildUrpKo pkgs.linuxPackages_6_6;
  urp-ko-6_12 = buildUrpKo pkgs.linuxPackages_6_12;
}
