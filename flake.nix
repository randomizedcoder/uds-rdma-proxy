{
  description = "UDS-RDMA Proxy: Tunneling Unix Domain Sockets over RoCEv2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    flake-utils.url = "github:numtide/flake-utils";

    # Phase 5: declarative microvm pair for kernel-module testing.
    # Provides the QEMU runner; lifecycle / pair orchestration lives
    # in nix/microvms/ (patterned after xdp2 + pcp).
    microvm = {
      url = "github:astro/microvm.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # Redpanda broker + rpk client with the native Kafka UDS listener
    # (PR #30240), built via that fork's own Nix/Bazel flake. Consumed only by
    # packages.test-redpanda-uds. Pinned to a specific fork commit so the test
    # is reproducible. Deliberately NOT `inputs.nixpkgs.follows = nixpkgs` --
    # the Redpanda build pins its own nixpkgs and the binaries are
    # self-contained subprocesses, so we keep its locked nixpkgs.
    #
    # Note: this is a heavy input (fetches the redpanda repo); it is only built
    # by `.#test-redpanda-uds` and is kept out of `checks`/CI.
    redpanda.url = "github:randomizedcoder/redpanda/d4b44629a5a8d06c4559e941428fd0249a5be643";
  };

  outputs = { self, nixpkgs, rust-overlay, flake-utils, microvm, redpanda }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };
        lib = nixpkgs.lib;

        packages = import ./nix/packages.nix { inherit pkgs; };
        envVars = import ./nix/env-vars.nix { inherit pkgs; };
        shellFunctions = import ./nix/shell-functions/build.nix { };
        devshell = import ./nix/devshell.nix {
          inherit pkgs packages envVars shellFunctions;
        };
        nixChecks = import ./nix/checks.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        # Static analysis (sparse/smatch/checkpatch/W=1/coccicheck for the
        # kernel module, clippy/rustfmt for the Rust workspace). Report
        # derivations, manual-run only -- deliberately NOT in checks/CI.
        # See nix/analysis/default.nix. Usage: nix build .#analysis-all -L
        analysis = import ./nix/analysis {
          inherit pkgs lib;
          inherit (packages) rustToolchain;
          inherit (nixChecks) src;
        };

        # Userspace libFuzzer harnesses over the pure kernel C parsing
        # surfaces (design 27 F1). Manual-run: nix run .#fuzz-classify.
        fuzz = import ./nix/fuzz { inherit pkgs lib; };

        # CI: build against flake-pinned nixpkgs kernel
        urpKo = nixChecks.kernel-module-build;

        # Phase 3a Step 5b: opt-in Rust-backed reorder buffer.
        # Built with CONFIG_URP_REORDER_RUST=y so the kernel module
        # links against liburp_protocol_ffi.a instead of compiling
        # the native rbtree implementation. Same kernel target as
        # urp-ko (linuxPackages_latest); switch via `nix build
        # .#urp-ko-rust` and use that .ko in place of the default.
        urpKoRust = nixChecks.buildUrpKoWith {
          kernelPackages = pkgs.linuxPackages_latest;
          rustFfi = urpProtocolFfi;
        };

        urpTestClient = import ./nix/urp-test-client.nix { inherit pkgs; };

        # urp-bench (design 30): io_uring UDS benchmark, C + Rust twins
        # with identical CLIs, plus the direct-topology smoke runner.
        urpBenchC = import ./nix/urp-bench.nix { inherit pkgs; };
        urpBenchRs = import ./nix/urp-bench-rs.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };
        urpBenchLocal = import ./nix/urp-bench-local.nix { inherit pkgs; };
        urpBenchMatrix = import ./nix/urp-bench-matrix.nix { inherit pkgs; };
        fuzzRust = import ./nix/fuzz-rust.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        urpCli = import ./nix/urp-cli.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        # Phase 3a: FFI staticlib for the optional Rust-backed reorder
        # buffer. Only consumed by the kernel build when
        # CONFIG_URP_REORDER_RUST=y; the default urp-ko build (C rbtree
        # backend) does not use it.
        urpProtocolFfi = import ./nix/urp-protocol-ffi.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        testKmodK0 = import ./nix/test-kmod-k0.nix {
          inherit pkgs urpKo urpTestClient urpCli;
        };

        soak1h = import ./nix/soak-1h.nix {
          inherit pkgs urpKo urpTestClient urpCli;
        };

        testVm = import ./nix/test-vm.nix {
          inherit pkgs urpTestClient urpCli;
          inherit (nixChecks) buildUrpKo;
          extraSystemPackages = [ soak1h ];
        };

        testVmDebug = import ./nix/test-vm.nix {
          inherit pkgs urpTestClient urpCli;
          inherit (nixChecks) buildUrpKo;
          enableSanitizers = true;
        };

        urpVm = import ./nix/urp-vm.nix {
          inherit pkgs testVm;
        };

        urpVmDebug = import ./nix/urp-vm.nix {
          inherit pkgs;
          testVm = testVmDebug;
        };

        # Phase 5: microvm.nix-based VM pair for URP-to-URP kernel
        # module testing. Two declarative microvms linked over a
        # private QEMU socket netdev. Patterned after xdp2/pcp:
        # dual TCP consoles, expect-driven orchestration, trap
        # cleanup, pgrep -f process=<hostname> for VM identification.
        # See nix/microvms/.
        microvms = import ./nix/microvms {
          inherit pkgs lib microvm nixpkgs urpCli;
          inherit (nixChecks) buildUrpKo;
        };

        # Phase 5 cross-arch (Track B): build urp.ko for aarch64 / riscv64
        # via pkgsCross. Guaranteed-green, build-only gates; the full
        # emulated pair-boot lives in the per-arch microvm packages.
        # Re-import checks.nix with the cross pkgs so buildUrpKoWith runs
        # under the cross stdenv (nativeBuildInputs splice to native
        # buildPackages automatically) and threads ARCH/CROSS_COMPILE.
        crossUrpKo = crossName: kpkgs:
          let cross = pkgs.pkgsCross.${crossName};
              c = import ./nix/checks.nix { pkgs = cross; rustToolchain = null; };
          in c.buildUrpKo cross.${kpkgs};
        urpKoAarch64 = crossUrpKo "aarch64-multiplatform" "linuxPackages_latest";
        urpKoRiscv64 = crossUrpKo "riscv64" "linuxPackages_latest";

        # Redpanda UDS-over-RDMA test. Only wired on the Linux systems the
        # Redpanda flake actually builds for (x86_64-linux / aarch64-linux);
        # absent on darwin so eval doesn't fault on a missing package set.
        hasRedpanda = redpanda.packages ? ${system};
        redpandaTestArgs = {
          inherit pkgs urpCli;
          urpKo = urpKo;
          redpanda = redpanda.packages.${system}.redpanda or null;
          rpk = redpanda.packages.${system}.rpk or null;
        };
        redpandaUdsTest = lib.optionalAttrs hasRedpanda {
          # Metadata bootstrap over UDS -> RDMA (rpk cluster info).
          test-redpanda-uds = import ./nix/test-redpanda-uds.nix redpandaTestArgs;
          # Full Kafka data plane (produce + consume) over RDMA via an
          # advertised-address bridge.
          test-redpanda-produce-consume =
            import ./nix/test-redpanda-produce-consume.nix redpandaTestArgs;
        };
      in
      {
        devShells.default = devshell;

        checks = {
          inherit (nixChecks)
            protocol-tests urp-bench-units urp-bench-rs-tests
            kernel-module-build
            urp-ko-6_1 urp-ko-6_6 urp-ko-6_12;
        };

        packages = {
          urp-ko = urpKo;
          urp-ko-rust = urpKoRust;
          # Kernel-version matrix (Phase 5 DoD 7): `nix build .#urp-ko-6_1` etc.
          inherit (nixChecks) urp-ko-6_1 urp-ko-6_6 urp-ko-6_12;
          urp-cli = urpCli;
          urp-protocol-ffi = urpProtocolFfi;
          # io_uring UDS benchmark (design 30): `nix run .#urp-bench-local`
          urp-bench-c = urpBenchC;
          urp-bench-rs = urpBenchRs;
          urp-bench-local = urpBenchLocal;
          urp-bench-matrix = urpBenchMatrix;
          fuzz-rust = fuzzRust;
          test-kmod-k0 = testKmodK0;
          soak-1h = soak1h;
          urp-vm = urpVm;
          urp-vm-debug = urpVmDebug;
          urp-ko-aarch64 = urpKoAarch64;
          urp-ko-riscv64 = urpKoRiscv64;
          # Static-analysis reports (manual): nix build .#analysis-all -L
          inherit (analysis)
            analysis-sparse analysis-smatch analysis-checkpatch
            analysis-w1 analysis-w2 analysis-coccicheck
            analysis-clippy analysis-rustfmt
            analysis-clang-tidy analysis-cppcheck analysis-all;
          # Userspace C fuzzers (manual): nix run .#fuzz-classify
          # Live-kernel fuzzers (baked into the pair-test rootfs; exposed here
          # so they build standalone): fuzz-netlink[-cov|-race] (S3),
          # fuzz-wire (S1/S2).
          inherit (fuzz) fuzz-classify fuzz-rx-seq fuzz-reorder
            fuzz-bench-deframe
            fuzz-netlink fuzz-netlink-cov fuzz-netlink-race fuzz-wire;
        } // microvms.packages // redpandaUdsTest;

        # `nix run .#test-redpanda-uds` (Linux only). Needs root at runtime.
        apps = lib.optionalAttrs hasRedpanda {
          test-redpanda-uds = {
            type = "app";
            program = "${redpandaUdsTest.test-redpanda-uds}/bin/test-redpanda-uds";
          };
          test-redpanda-produce-consume = {
            type = "app";
            program = "${redpandaUdsTest.test-redpanda-produce-consume}/bin/test-redpanda-produce-consume";
          };
        };

        # buildUrpKo: function to build against any kernel.
        #
        # For local testing (build against running kernel):
        #   nix build --impure --expr \
        #     'let f = builtins.getFlake (toString ./.);
        #          p = import <nixpkgs> {};
        #      in f.lib.x86_64-linux.buildUrpKo p.linuxPackages'
        #
        # Then: sudo test-kmod-k0 result/lib/modules/$(uname -r)/urp.ko
        lib = {
          inherit (nixChecks) buildUrpKo;
        };
      }
    );
}
