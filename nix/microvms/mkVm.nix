# nix/microvms/mkVm.nix
#
# Parameterised microvm-based VM for the URP kernel-module pair tests.
# Two VMs (vm1 = acceptor / listen, vm2 = initiator / connect) share a
# private QEMU socket netdev so RoCEv2 traffic stays on the host.
#
# Adopts the xdp2 patterns (kernel match assertion, dual TCP consoles,
# pgrep-friendly -name process=hostname, qemu.extraArgs hooks) and
# layers the urp-specific bits (rxe modules, urp.ko in rootfs, urp CLI)
# on top.
#
# Inputs:
#   pkgs, lib, microvm, nixpkgs - standard flake context
#   vmId          - "vm1" or "vm2"
#   buildUrpKo    - the buildUrpKo function from nix/checks.nix
#   urpCli        - the urp CLI package
#
{ pkgs, lib, microvm, nixpkgs, vmId, buildUrpKo, urpCli,
  # Phase 5: enable KASAN_GENERIC + DEBUG_KMEMLEAK in the guest
  # kernel. Triggers a full kernel rebuild (~30 min first time,
  # cached after). The urpKo build and the guest boot use the
  # same package, so vermagic matches.
  withSanitizers ? false,
  # Phase 5 cross-arch (Track B): "x86_64" | "aarch64" | "riscv64".
  # For non-x86 arches the caller passes a pkgsCross.<arch> `pkgs`
  # (so kernel + rootfs + urpCli are the target arch) and this selects
  # the matching qemu machine / cpu (null=KVM vs max=TCG) / console.
  arch ? "x86_64" }:

let
  constants = import ./constants.nix;
  archCfg = constants.arches.${arch};
  vmCfg = constants.vms.${vmId};
  hostname = vmCfg.hostname;
  pairOctet = toString vmCfg.ipLastOctet;
  pairMac = constants.pairMacPrefix
    + (if vmCfg.ipLastOctet < 16 then "0" else "")
    + lib.toHexString vmCfg.ipLastOctet;

  baseKernelPackages = pkgs.${constants.kernelPackage};

  sanitizerKernel = baseKernelPackages.kernel.override {
    # Tolerate "unused option" config errors (DRM_NOVA, RUST, NOVA_CORE, ...)
    # that the base nixpkgs 7.1.6 config trips when structuredExtraConfig forces
    # a config regen. These are unrelated to the KASAN/KMEMLEAK options we add.
    ignoreConfigErrors = true;
    kernelPatches = baseKernelPackages.kernel.kernelPatches ++ [{
      name = "urp-kasan-kmemleak";
      patch = null;
      structuredExtraConfig = with lib.kernel; {
        KASAN              = lib.mkForce yes;
        KASAN_GENERIC      = lib.mkForce yes;
        KASAN_INLINE       = lib.mkForce yes;
        DEBUG_KMEMLEAK     = lib.mkForce yes;
        DEBUG_KMEMLEAK_AUTO_SCAN  = lib.mkForce yes;
        DEBUG_KMEMLEAK_DEFAULT_OFF = lib.mkForce no;
        FRAME_WARN         = lib.mkForce (freeform "8192");
        # KCOV: per-task coverage feedback for the coverage-guided netlink
        # fuzzer (design 27 F2). Exposes /sys/kernel/debug/kcov; the genl
        # doit/dumpit handlers run synchronously in the caller's syscall
        # context, so per-task KCOV captures their edges. KCOV_ENABLE_COMPARISONS
        # adds KCOV_TRACE_CMP (comparison operands) for smarter mutation.
        # DEBUG_FS is required for the kcov debugfs node. Instruments all
        # kernel code -> full rebuild, cached thereafter.
        KCOV                     = lib.mkForce yes;
        KCOV_ENABLE_COMPARISONS  = lib.mkForce yes;
        DEBUG_FS                 = lib.mkForce yes;
      };
    }];
  };

  kernelPackages =
    if withSanitizers
    then pkgs.linuxPackagesFor sanitizerKernel
    else baseKernelPackages;

  urpKo = buildUrpKo kernelPackages;
  modVer = kernelPackages.kernel.modDirVersion;

  # Kernel-module / kernel match assertion. The .ko is hash-derived
  # from the kernel + urp source; if either side drifts, the
  # derivation fails to build with this message rather than dmesg-
  # complaining about vermagic at runtime.
  _koSanity =
    assert urpKo != null || throw "buildUrpKo returned null for ${constants.kernelPackage}";
    true;

  urpKoStorePath = "${urpKo}/lib/modules/${modVer}/urp.ko";

in
(nixpkgs.lib.nixosSystem {
  inherit pkgs;

  modules = [
    microvm.nixosModules.microvm

    ({ config, pkgs, lib, ... }: {
      system.stateVersion = "26.05";
      networking.hostName = hostname;
      networking.firewall.enable = false;

      # ----------------------------------------------------------------
      # MicroVM hypervisor configuration
      # ----------------------------------------------------------------
      microvm = {
        hypervisor = "qemu";
        # Not exactly 2048: microvm.nix warns QEMU hangs at 2 GiB
        # (https://github.com/microvm-nix/microvm.nix/issues/171).
        # KASAN ~2-3x RSS so debug VMs need more headroom.
        mem = if withSanitizers then 3584 else archCfg.mem;
        vcpu = 2;

        # x86_64: null cpu => -enable-kvm -cpu host (microvm.nix gates KVM
        # on cpu == null). aarch64/riscv64 on an x86 host have no KVM, so
        # archCfg.cpu is "max", which forces QEMU TCG (full emulation).
        cpu = archCfg.cpu;

        volumes = [];

        # eth0: QEMU user networking (NAT to host). Used only for
        # /nix/store mount; we drive everything via the console.
        interfaces = [{
          type = "user";
          id = "eth0";
          mac = "52:54:00:00:42:0${toString vmCfg.ipLastOctet}";
        }];

        # Mount the host /nix/store via 9p so the VM sees the same
        # urp.ko store path the orchestrator references — this is what
        # prevents the "vermagic '6.18.22' should be '7.0.3'" drift
        # we saw with the hand-rolled qemu-vm.nix harness.
        shares = [{
          source = "/nix/store";
          mountPoint = "/nix/store";
          tag = "nix-store";
          proto = "9p";
        }];

        qemu = {
          serialConsole = false;
          machine = archCfg.qemuMachine;
          # Native KVM build for x86 (cpu==null). For the emulated arches the
          # qemu binary must come from buildPackages (native, all-targets) so
          # it can run qemu-system-<arch> against the cross guest -- AND with
          # seccompSupport disabled, because microvm.nix passes `-sandbox on`
          # which does not work for cross-arch qemu (per xdp2's TCG harness).
          package =
            if archCfg.cpu == null
            then pkgs.qemu_kvm
            else pkgs.buildPackages.qemu.override { seccompSupport = false; };
        } // lib.optionalAttrs (archCfg.machineOpts != null) {
          # riscv64-linux has no machineOpts default in microvm.nix -> set it.
          machineOpts = archCfg.machineOpts;
        } // {

          extraArgs = [
            # pgrep-friendly process name (-name <hostname>,process=<hostname>
            # sets argv[0] so `pgrep -f process=urp-test-1` finds it).
            "-name" "${hostname},process=${hostname}"

            # Dual TCP consoles for boot debug + scripted interaction.
            "-serial" "tcp:127.0.0.1:${toString vmCfg.consoleSerialPort},server,nowait"
            "-device" "virtio-serial-pci"
            "-chardev" "socket,id=virtcon,port=${toString vmCfg.consoleVirtioPort},host=127.0.0.1,server=on,wait=off"
            "-device" "virtconsole,chardev=virtcon"

            # Inter-VM private RoCEv2 link. One VM listens, the other
            # connects; QEMU bridges them at L2.
            "-netdev" "socket,id=urppair,${vmCfg.role}=127.0.0.1:${toString constants.pairSocketPort}"
            "-device" "virtio-net-pci,netdev=urppair,mac=${pairMac}"

            # Kernel command line. microvm.nix's qemu-vm-like
            # machine type doesn't auto-append init=, so we do.
            "-append" (builtins.concatStringsSep " " ([
              "console=${archCfg.serialConsole},115200"
              "console=hvc0"
              "reboot=t"
              "panic=-1"
              "loglevel=4"
              "init=${config.system.build.toplevel}/init"
            ] ++ config.boot.kernelParams))
          ];
        };
      };

      # ----------------------------------------------------------------
      # Kernel + RDMA modules
      # ----------------------------------------------------------------
      boot.kernelPackages = kernelPackages;

      boot.kernelParams = [
        "console=${archCfg.serialConsole},115200"
        "console=hvc0"
        "systemd.default_standard_error=journal+console"
        "systemd.show_status=true"
        # Disable predictable interface names so the orchestrator and
        # the networking.interfaces.eth1 config below agree on naming.
        # microvm.nix attaches the user-mode NIC first (eth0) and the
        # pair socket NIC second (eth1).
        "net.ifnames=0"
      ] ++ lib.optionals withSanitizers [
        # KMEMLEAK starts disabled by default; the kernel param turns
        # it on at boot. Periodic scan still managed by
        # DEBUG_KMEMLEAK_AUTO_SCAN.
        "kmemleak=on"
        # Slow KASAN test output to journal-only to keep the console
        # readable while still capturing in dmesg.
        "kasan_multi_shot"
      ];

      boot.initrd.availableKernelModules = [
        "9p" "9pnet" "9pnet_virtio"
        "virtio_pci" "virtio_console" "virtio_net" "virtio_blk"
      ];
      boot.initrd.systemd.emergencyAccess = true;
      systemd.enableEmergencyMode = false;

      # rxe (soft-RoCE) for the inter-VM link.
      boot.kernelModules = [ "ib_core" "rdma_cm" "rdma_rxe" ];

      # ----------------------------------------------------------------
      # eth1 static IP on the pair link
      # ----------------------------------------------------------------
      # eth0 = user-mode NAT (eth0 in guest); eth1 = inter-VM socket.
      networking.interfaces.eth1 = {
        useDHCP = false;
        ipv4.addresses = [{
          address = "10.99.99.${pairOctet}";
          prefixLength = constants.pairNetmaskPrefix;
        }];
      };

      # ----------------------------------------------------------------
      # Auto-login on console so the expect prompt-match works without
      # passwords. The pair test VMs are local-dev only — this matches
      # xdp2's pattern.
      # ----------------------------------------------------------------
      services.getty.autologinUser = "root";
      users.users.root.hashedPassword = "";  # passwordless

      # ----------------------------------------------------------------
      # urp tooling baked into rootfs
      # ----------------------------------------------------------------
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
        urpCli
        # Live-kernel netlink fuzzer (design 27 F2). Tiny standalone binary;
        # invoked by the pair harness's fuzz phase against the loaded module.
        (import ../fuzz { inherit pkgs lib; }).netlinkFuzz
        # KCOV coverage-guided netlink fuzzer (design 27 F2, S3). Needs the
        # CONFIG_KCOV sanitizer kernel; run in the sanitizer pair test.
        (import ../fuzz { inherit pkgs lib; }).covFuzz
        # Hostile-peer RDMA wire fuzzer (design 27 F2, S1/S2). Run from the
        # peer VM against the acceptor to fuzz the RX frame path under KASAN.
        (import ../fuzz { inherit pkgs lib; }).wireFuzz
      ];

      # Expose the .ko via an env var on root's shell so test scripts
      # can `insmod $URP_KO` without hardcoding store paths.
      environment.variables.URP_KO = urpKoStorePath;

      # ----------------------------------------------------------------
      # Trim NixOS bloat that's irrelevant to kernel-module tests.
      # ----------------------------------------------------------------
      documentation.enable = false;
      documentation.man.enable = false;
      documentation.doc.enable = false;
      documentation.info.enable = false;
      documentation.nixos.enable = false;
      services.udisks2.enable = false;
      programs.command-not-found.enable = false;
      fonts.fontconfig.enable = false;
      xdg.mime.enable = false;
      nix.enable = false;
      hardware.enableRedistributableFirmware = false;
      boot.supportedFilesystems = lib.mkForce [ "vfat" "ext4" ];
    })
  ];
}).config.microvm.declaredRunner
