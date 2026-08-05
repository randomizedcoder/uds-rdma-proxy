# nix/microvms/default.nix
#
# Entry point for URP microvm pair test infrastructure.
# Mirrors xdp2/nix/microvms/default.nix in shape; URP only ships
# x86_64 for now (Phase 5 cross-arch is deferred).
#
# Two flavours of VMs:
#   - default        production kernel, fast smoke test (~38 s)
#   - withSanitizers KASAN + KMEMLEAK kernel for leak / use-after-free
#                    coverage. ~30 min rebuild first time, cached after.
#
{ pkgs, lib, microvm, nixpkgs, buildUrpKo, urpCli }:

let
  constants  = import ./constants.nix;
  scriptsDir = ./scripts;

  microvmLib = import ./lib.nix {
    inherit pkgs lib constants scriptsDir;
    pairLabel = "";
    sanitizer = false;
  };
  microvmLibDebug = import ./lib.nix {
    inherit pkgs lib constants scriptsDir;
    pairLabel = "-debug";
    sanitizer = true;
  };

  vm1 = import ./mkVm.nix {
    inherit pkgs lib microvm nixpkgs buildUrpKo urpCli;
    vmId = "vm1";
  };

  vm2 = import ./mkVm.nix {
    inherit pkgs lib microvm nixpkgs buildUrpKo urpCli;
    vmId = "vm2";
  };

  vm1Debug = import ./mkVm.nix {
    inherit pkgs lib microvm nixpkgs buildUrpKo urpCli;
    vmId = "vm1";
    withSanitizers = true;
  };

  vm2Debug = import ./mkVm.nix {
    inherit pkgs lib microvm nixpkgs buildUrpKo urpCli;
    vmId = "vm2";
    withSanitizers = true;
  };

  # ------------------------------------------------------------------
  # Phase 5 cross-arch (Track B): aarch64 / riscv64 pair, emulated (TCG).
  #
  # Each cross arch gets its own pkgsCross so the kernel, rootfs, urp.ko
  # and urp CLI are all target-arch. The orchestrator (lib.nix) still runs
  # NATIVE on the host (it just drives QEMU + consoles), so it takes the
  # native pkgs and only the timeout multiplier + a "-<arch>" label.
  # ------------------------------------------------------------------
  mkCrossArch = arch: crossName:
    let
      cross       = pkgs.pkgsCross.${crossName};
      crossUrpCli = import ../urp-cli.nix { pkgs = cross; };
      crossBuild  = (import ../checks.nix { pkgs = cross; rustToolchain = null; }).buildUrpKo;
      crossVm = vmId: import ./mkVm.nix {
        inherit lib microvm nixpkgs vmId arch;
        pkgs       = cross;
        buildUrpKo = crossBuild;
        urpCli     = crossUrpCli;
      };
      crossLib = import ./lib.nix {
        inherit pkgs lib constants scriptsDir;
        pairLabel         = "-${arch}";
        timeoutMultiplier = constants.arches.${arch}.timeoutMultiplier;
      };
    in {
      "microvm-vm1-${arch}"           = crossVm "vm1";
      "microvm-vm2-${arch}"           = crossVm "vm2";
      "urp-microvm-pair-test-${arch}" = crossLib.fullPairTest;
    };

  crossPackages =
    (mkCrossArch "aarch64" "aarch64-multiplatform") //
    (mkCrossArch "riscv64" "riscv64");

in {
  inherit constants;
  inherit vm1 vm2 vm1Debug vm2Debug;
  inherit (microvmLib)
    fullPairTest status forceKill
    vm1Serial vm1Virtio vm2Serial vm2Virtio;

  # Flat package set the flake can splice into packages.*
  packages = {
    microvm-vm1                = vm1;
    microvm-vm2                = vm2;
    microvm-vm1-debug          = vm1Debug;
    microvm-vm2-debug          = vm2Debug;

    urp-microvm-pair-test      = microvmLib.fullPairTest;
    urp-microvm-pair-test-debug = microvmLibDebug.fullPairTest;

    urp-microvm-status         = microvmLib.status;
    urp-microvm-force-kill     = microvmLib.forceKill;
    urp-microvm-vm1-serial     = microvmLib.vm1Serial;
    urp-microvm-vm1-virtio     = microvmLib.vm1Virtio;
    urp-microvm-vm2-serial     = microvmLib.vm2Serial;
    urp-microvm-vm2-virtio     = microvmLib.vm2Virtio;
  } // crossPackages;
}
