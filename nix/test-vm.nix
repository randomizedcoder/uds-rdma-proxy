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
{ pkgs, urpKo, testKmodK0, urpTestClient }:

let
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
        memorySize = 2048;
        cores = 2;
        graphics = false;
      };

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
}
