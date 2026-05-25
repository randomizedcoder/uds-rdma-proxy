# nix/microvms/constants.nix
#
# Configuration constants for URP MicroVM pair test infrastructure.
#
# Phase 5: x86_64 only. Pattern adopted from xdp2 + pcp.
#
# Two VMs ("vm1" / acceptor and "vm2" / initiator) share a private
# QEMU socket network for RoCEv2 traffic. Each VM gets its own port
# block so they can coexist on the same host:
#
#   vm1 (listen):  console block 23500-23509, pair listens on 23600
#   vm2 (connect): console block 23510-23519, pair connects to 23600
#
# Within each console block:
#   +0 = serial console (ttyS0, available early in boot)
#   +1 = virtio console (hvc0, faster, available after virtio drivers)
#   +2..9 = reserved
#
rec {
  # ==========================================================================
  # Port allocation
  # ==========================================================================

  portBase = 23500;

  vms = {
    vm1 = {
      role = "listen";              # QEMU socket netdev "listen"
      pairRole = "acceptor";        # URP endpoint role
      ipLastOctet = 1;              # 10.99.99.1
      hostname = "urp-test-1";
      consoleSerialPort = portBase + 0;       # 23500
      consoleVirtioPort = portBase + 1;       # 23501
    };
    vm2 = {
      role = "connect";             # QEMU socket netdev "connect"
      pairRole = "initiator";
      ipLastOctet = 2;              # 10.99.99.2
      hostname = "urp-test-2";
      consoleSerialPort = portBase + 10;      # 23510
      consoleVirtioPort = portBase + 11;      # 23511
    };
  };

  # Inter-VM RoCEv2 link
  pairSocketPort = 23600;            # QEMU -netdev socket endpoint
  pairSubnet = "10.99.99.0/24";
  pairNetmaskPrefix = 24;
  pairMacPrefix = "52:54:00:00:99:"; # last octet = ipLastOctet

  # ==========================================================================
  # Hostname / process name helpers (used for pgrep matching)
  # ==========================================================================

  # The QEMU `-name <hostname>,process=<hostname>` argument sets argv[0],
  # so `pgrep -f process=<hostname>` finds the right VM even if the
  # orchestrator wrapper exits.
  getProcessName = vmId: vms.${vmId}.hostname;

  # ==========================================================================
  # Lifecycle timing (seconds)
  # ==========================================================================

  pollInterval = 1;

  # x86_64 KVM is fast; numbers track xdp2's `timeouts` (not `timeoutsQemu`).
  timeouts = {
    build         = 600;   # 10 min — kernel rebuild from scratch
    processStart  = 5;     # QEMU should be in ps within 5s
    serialReady   = 30;    # ttyS0 TCP listener
    virtioReady   = 45;    # hvc0 TCP listener
    serviceReady  = 60;    # systemd multi-user.target
    pairLink      = 30;    # eth1 up + ping between VMs
    rxeReady      = 15;    # rdma link add succeeds
    urpReady      = 15;    # urp add ... returns ok
    cmEstablished = 20;    # RDMA CM ESTABLISHED on both sides
    echo          = 10;    # socat round-trip
    drainRemove   = 15;    # urp drain + remove
    shutdown      = 30;    # poweroff -> process exit
    command       = 10;    # generic single-command nc timeout
  };

  # ==========================================================================
  # Kernel selection
  # ==========================================================================
  #
  # Both VMs and the urp.ko host build use linuxPackages_latest; the
  # assertion in mkVm.nix verifies they agree (vermagic match).
  #
  kernelPackage = "linuxPackages_latest";
}
