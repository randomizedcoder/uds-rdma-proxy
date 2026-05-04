{ pkgs, rustToolchain }:

let
  src = builtins.path {
    path = ../.;
    name = "uds-rdma-proxy-src";
    filter = path: type:
      let
        baseName = builtins.baseNameOf path;
      in
      # Include Cargo workspace and every member crate. cargo refuses to
      # operate on a workspace whose members are missing -- even if we
      # only invoke it on one crate -- so all member directories must be
      # present in the source closure.
      (type == "directory" && (
        baseName == "crates" ||
        baseName == "uds-rdma-protocol" ||
        baseName == "uds-rdma-protocol-ffi" ||
        baseName == "urp-cli" ||
        baseName == "src" ||
        baseName == "commands"
      )) ||
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
      (pkgs.lib.hasSuffix ".h" baseName);
  };

  # Build urp.ko against a given kernel package
  buildUrpKo = kernelPackages: pkgs.stdenv.mkDerivation {
    name = "urp-ko-${kernelPackages.kernel.version}";
    inherit src;
    nativeBuildInputs = [ pkgs.gnumake ] ++
      kernelPackages.kernel.moduleBuildDependencies;
    buildPhase =
      let
        kdir = "${kernelPackages.kernel.dev}/lib/modules/${kernelPackages.kernel.modDirVersion}/build";
      in ''
        make -C ${kdir} M=$PWD/kernel modules
      '';
    installPhase = ''
      mkdir -p $out/lib/modules/${kernelPackages.kernel.modDirVersion}
      cp kernel/urp.ko $out/lib/modules/${kernelPackages.kernel.modDirVersion}/
    '';
  };
in
{
  inherit buildUrpKo;

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

  # NOTE: miri requires network access to build its sysroot (fetches cfg-if for std).
  # This is incompatible with Nix's sandbox. Run miri via devshell instead:
  #   nix develop --command cargo miri test -p uds-rdma-protocol

  # kernel module build (against flake-pinned latest mainline kernel, for CI).
  # Using linuxPackages_latest (rather than linuxPackages) because this code
  # is intended for upstream review by the Linux kernel network dev team --
  # they expect new code to compile against current mainline.
  kernel-module-build = buildUrpKo pkgs.linuxPackages_latest;
}
