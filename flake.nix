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
  };

  outputs = { self, nixpkgs, rust-overlay, flake-utils, microvm }:
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
      in
      {
        devShells.default = devshell;

        checks = {
          inherit (nixChecks) protocol-tests kernel-module-build;
        };

        packages = {
          urp-ko = urpKo;
          urp-ko-rust = urpKoRust;
          urp-cli = urpCli;
          urp-protocol-ffi = urpProtocolFfi;
          test-kmod-k0 = testKmodK0;
          soak-1h = soak1h;
          urp-vm = urpVm;
          urp-vm-debug = urpVmDebug;
        } // microvms.packages;

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
