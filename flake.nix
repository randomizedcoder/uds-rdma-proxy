{
  description = "UDS-RDMA Proxy: Tunneling Unix Domain Sockets over RoCEv2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, rust-overlay, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };

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

        urpTestClient = import ./nix/urp-test-client.nix { inherit pkgs; };

        testKmodK0 = import ./nix/test-kmod-k0.nix {
          inherit pkgs urpKo urpTestClient;
        };

        testVm = import ./nix/test-vm.nix {
          inherit pkgs urpTestClient;
          inherit (nixChecks) buildUrpKo;
        };

        testVmDebug = import ./nix/test-vm.nix {
          inherit pkgs urpTestClient;
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
      in
      {
        devShells.default = devshell;

        checks = {
          inherit (nixChecks) protocol-tests kernel-module-build;
        };

        packages = {
          urp-ko = urpKo;
          test-kmod-k0 = testKmodK0;
          urp-vm = urpVm;
          urp-vm-debug = urpVmDebug;
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
