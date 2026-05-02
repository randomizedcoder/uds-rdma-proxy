# NixOS test VM for kernel module development.
#
# Provides a QEMU VM with:
#   - SSH root access via generated key pair
#   - rdma_rxe soft-RDMA driver
#   - test-kmod-k0 pre-installed (with matching urp.ko)
#   - All test dependencies (socat, iproute2, rdma-core, etc.)
#
# The VM kernel matches the flake-pinned nixpkgs, so urp.ko built by
# buildUrpKo loads without vermagic mismatch.
#
# Set enableSanitizers = true to build a debug kernel with KASAN + KMEMLEAK.
# This triggers a full kernel rebuild (~30 min first time, cached after).
{ pkgs, buildUrpKo, urpTestClient, enableSanitizers ? false }:

let
  sanitizerPatches = [{
    name = "kasan-kmemleak-kunit";
    patch = null;
    extraStructuredConfig = with pkgs.lib.kernel; {
      KASAN = yes;
      KASAN_GENERIC = yes;
      DEBUG_KMEMLEAK = yes;
      DEBUG_KMEMLEAK_AUTO_SCAN = yes;
      KUNIT = yes;
    };
  }];

  baseKernel = pkgs.linuxPackages.kernel;

  vmKernelPackages =
    if enableSanitizers then
      pkgs.linuxPackagesFor (baseKernel.override {
        kernelPatches = baseKernel.kernelPatches ++ sanitizerPatches;
      })
    else
      pkgs.linuxPackages;

  urpKo = buildUrpKo vmKernelPackages;

  testKmodK0 = import ./test-kmod-k0.nix {
    inherit pkgs urpKo urpTestClient;
  };

  # SSH key pair for programmatic VM access.
  # Stored in nix store (world-readable) — acceptable for a local dev/test VM.
  sshKeys = pkgs.runCommand "urp-vm-ssh-keys" {
    nativeBuildInputs = [ pkgs.openssh ];
  } ''
    mkdir -p $out
    ssh-keygen -t ed25519 -f $out/id_ed25519 -N "" -C "urp-test-vm"
  '';

  nixos = import "${pkgs.path}/nixos" {
    system = pkgs.stdenv.hostPlatform.system;
    configuration = { config, pkgs, lib, modulesPath, ... }: {
      imports = [
        "${modulesPath}/virtualisation/qemu-vm.nix"
      ];

      virtualisation = {
        forwardPorts = [{
          from = "host";
          host.port = 2222;
          guest.port = 22;
        }];
        # KASAN adds ~2-3x memory overhead
        memorySize = if enableSanitizers then 4096 else 2048;
        cores = 2;
        graphics = false;
      };

      boot.kernelPackages = vmKernelPackages;

      services.openssh = {
        enable = true;
        settings = {
          PermitRootLogin = "prohibit-password";
          PasswordAuthentication = false;
        };
      };

      users.users.root.openssh.authorizedKeys.keyFiles = [
        "${sshKeys}/id_ed25519.pub"
      ];

      environment.systemPackages = with pkgs; [
        iproute2
        rdma-core
        kmod
        socat
        coreutils
        gnugrep
        gnused
        procps
        util-linux
      ] ++ [
        testKmodK0
        urpTestClient
      ];

      boot.kernelModules = [ "ib_core" "rdma_cm" "rdma_rxe" ];

      # KMEMLEAK: enable scan trigger via debugfs
      boot.kernelParams = pkgs.lib.optionals enableSanitizers [
        "kmemleak=on"
      ];

      networking = {
        hostName = "urp-test";
        firewall.enable = false;
      };

      documentation.enable = false;
      system.stateVersion = "25.05";
    };
  };
in {
  vm = nixos.config.system.build.vm;
  inherit sshKeys;
  inherit enableSanitizers;
}
