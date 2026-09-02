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
        # Real-hardware client matrix (design 32): ssh-driven C/Rust interop
        # sweep over a standing RoCEv2 session. `nix run .#urp-hw-matrix`.
        urpHwMatrix = import ./nix/urp-hw-matrix.nix { inherit pkgs; };
        # One-way bulk-throughput runner (design 34): sink-measured goodput +
        # iperf2/ib_write_bw baselines. `nix run .#urp-bw-matrix`.
        urpBwMatrix = import ./nix/urp-bw-matrix.nix { inherit pkgs; };
        # F2 aggregate / N-flow runner (design 34 Option F2, design 37 §37.6):
        # N independent streams, one per endpoint pair; sums sink goodputs.
        # `nix run .#urp-f2-matrix`.
        urpF2Matrix = import ./nix/urp-f2-matrix.nix { inherit pkgs; };
        # Zero-copy twin of urp-f2-matrix (design 37 §37.6): N independent fast
        # endpoint pairs, `urp-bench --mode uring-cmd`. `nix run .#urp-fast-f2-matrix`.
        urpFastF2Matrix = import ./nix/urp-fast-f2-matrix.nix { inherit pkgs; };
        # Multi-QP reorder validation (status.md gap #1): sweep num_qps, assert
        # reorder-insertions>0 + byte-exact delivery. `nix run .#urp-reorder-matrix`.
        urpReorderMatrix = import ./nix/urp-reorder-matrix.nix { inherit pkgs; };
        # 3-node full-mesh concurrency (design 32 mesh extension): the new "2-way"
        # regime — a node serving/driving two RDMA sessions on its two ports to two
        # peers at once. per-edge / hub-rx / hub-tx / ring / all2all scenarios.
        # `nix run .#urp-mesh-matrix -- hp1 hp2 hp3`.
        urpMeshMatrix = import ./nix/urp-mesh-matrix.nix { inherit pkgs; };
        # Long-duration (default 8h) soak of the mesh benchmark: loops a scenario,
        # samples per-host kernel slab / MemAvailable / endpoint count / dmesg
        # faults each iteration, and prints a leak + perf-drift verdict.
        # `nix run .#urp-mesh-soak -- hp1 hp2 hp3`.
        urpMeshSoak = import ./nix/urp-mesh-soak.nix { inherit pkgs; };
        # Zero-copy fast-path twins of the two matrices above (design 31 PR5b):
        # dedicated `--kind fast` endpoint pair, `urp-bench --mode uring-cmd`.
        #   .#urp-fast-bw-matrix (goodput), .#urp-fast-hw-matrix (RTT).
        urpFastBwMatrix = import ./nix/urp-fast-bw-matrix.nix { inherit pkgs; };
        urpFastHwMatrix = import ./nix/urp-fast-hw-matrix.nix { inherit pkgs; };
        urpFastPoc = import ./nix/urp-fast-poc.nix { inherit pkgs; };
        ciLocal = import ./nix/ci-local.nix {
          inherit pkgs fuzz;
          checks = nixChecks;
          urpCli = urpCli;
          urpExporter = urpExporter;
        };
        fuzzRust = import ./nix/fuzz-rust.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        urpCli = import ./nix/urp-cli.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        urpControl = import ./nix/urp-control.nix {
          inherit pkgs;
          inherit (packages) rustToolchain;
        };

        urpExporter = import ./nix/urp-exporter.nix {
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
            urp-netlink-tests urp-control-tests urp-exporter-tests
            urp-fast-validate-units urp-reorder-units urp-conn-slot-units urp-window-units
            kernel-module-build
            urp-ko-6_1 urp-ko-6_6 urp-ko-6_12;
        };

        packages = {
          urp-ko = urpKo;
          urp-ko-rust = urpKoRust;
          # Kernel-version matrix (Phase 5 DoD 7): `nix build .#urp-ko-6_1` etc.
          inherit (nixChecks) urp-ko-6_1 urp-ko-6_6 urp-ko-6_12;
          urp-cli = urpCli;
          urp-control = urpControl;
          # Prometheus metrics exporter (design 39): `nix run .#urp-exporter`
          urp-exporter = urpExporter;
          urp-protocol-ffi = urpProtocolFfi;
          # io_uring UDS benchmark (design 30): `nix run .#urp-bench-local`
          urp-bench-c = urpBenchC;
          urp-bench-rs = urpBenchRs;
          urp-bench-local = urpBenchLocal;
          urp-bench-matrix = urpBenchMatrix;
          # Real-hardware interop matrix over RoCEv2 (design 32; needs the boxes):
          #   nix run .#urp-hw-matrix -- <acceptor> <initiator> <acceptor-ip>
          urp-hw-matrix = urpHwMatrix;
          # Bulk-throughput sweep (design 34):
          #   nix run .#urp-bw-matrix -- <acceptor> <initiator> <acceptor-ip>
          urp-bw-matrix = urpBwMatrix;
          # F2 aggregate / N-flow throughput (design 34 F2, 37 §37.6; needs boxes):
          #   nix run .#urp-f2-matrix -- <acceptor> <initiator> <acceptor-ip>
          urp-f2-matrix = urpF2Matrix;
          # Zero-copy twin of the F2 runner (design 37 §37.6; needs boxes):
          #   nix run .#urp-fast-f2-matrix -- <acceptor> <initiator> <acceptor-ip>
          urp-fast-f2-matrix = urpFastF2Matrix;
          # Multi-QP reorder validation over RoCEv2 (status gap #1; needs boxes):
          #   nix run .#urp-reorder-matrix -- <acceptor> <initiator> <acceptor-ip>
          urp-reorder-matrix = urpReorderMatrix;
          # 3-node full-mesh concurrency / 2-way regime (design 32 mesh; needs boxes):
          #   nix run .#urp-mesh-matrix -- [h1 h2 h3]   (default hp1 hp2 hp3)
          urp-mesh-matrix = urpMeshMatrix;
          # Long-duration soak of the mesh benchmark (leak + perf-drift; needs boxes):
          #   SOAK_SECONDS=28800 nix run .#urp-mesh-soak -- [h1 h2 h3]
          urp-mesh-soak = urpMeshSoak;
          # Zero-copy fast-path twins (design 31 PR5b; needs the boxes):
          #   nix run .#urp-fast-bw-matrix -- <acceptor> <initiator> <acceptor-ip>
          #   nix run .#urp-fast-hw-matrix -- <acceptor> <initiator> <acceptor-ip>
          urp-fast-bw-matrix = urpFastBwMatrix;
          urp-fast-hw-matrix = urpFastHwMatrix;
          urp-fast-poc = urpFastPoc;
          # Local reproduction of the every-push CI: `nix run .#ci-local`
          # (for hosts without a GitHub runner). Builds the 9 nix-checks
          # targets + runs the fuzz-smoke harnesses; skips the KVM tiers.
          ci-local = ciLocal;
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
    ) // {
      # System-agnostic outputs live OUTSIDE eachDefaultSystem. The reusable
      # NixOS module (design 32) resolves the per-system buildUrpKo / urp-cli
      # through `self`, so a host that pins this flake gets urp.ko built against
      # its own kernel plus declarative endpoints.
      nixosModules.urp = import ./nix/nixos-module.nix { inherit self; };
      nixosModules.urp-exporter = import ./nix/nixos-exporter-module.nix { inherit self; };
      nixosModules.default = self.nixosModules.urp;
    };
}
