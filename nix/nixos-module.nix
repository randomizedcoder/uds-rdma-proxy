# Reusable NixOS module: `nixosModules.urp` (design 32).
#
# Builds urp.ko against the *host's* kernel (so it tracks whatever kernel the
# machine runs — e.g. hp1/hp3's custom net-next 7.2-rc1), loads the RDMA stack,
# and materialises a declarative list of urp endpoints as systemd oneshots.
#
# Consumed by a flake that pins this repo:
#   imports = [ uds-rdma-proxy.nixosModules.urp ];
#   services.urp.enable = true;
#   services.urp.endpoints.pair_acceptor = {
#     role = "acceptor";
#     connectPath = "/run/urp-echo.sock";
#     bind = "10.10.2.1:4791";
#   };
#
# This module is system-agnostic and therefore lives OUTSIDE the flake's
# `flake-utils.eachDefaultSystem` wrapper; it receives `self` so it can resolve
# the per-system `self.lib.<sys>.buildUrpKo` and `self.packages.<sys>.urp-cli`.
{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.urp;
  system = pkgs.stdenv.hostPlatform.system;

  # urp.ko built against the host kernel, and the netlink CLI, from this flake.
  urpKo = self.lib.${system}.buildUrpKo config.boot.kernelPackages;
  urpCli = self.packages.${system}.urp-cli;

  endpointOpts = { name, config, ... }: {
    options = {
      name = lib.mkOption {
        type = lib.types.str;
        default = name;
        description = "Endpoint name (<= 15 bytes), as passed to `urp add`.";
      };
      role = lib.mkOption {
        type = lib.types.enum [ "acceptor" "initiator" ];
        description = ''
          "acceptor" (RDMA listener; needs connectPath + bind) or
          "initiator" (RDMA connector; needs listenPath + peer).
        '';
      };
      connectPath = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        description = "UDS the module connects OUT to per stream (acceptor side).";
      };
      listenPath = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        description = "UDS the module listens ON for app connections (initiator side).";
      };
      bind = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        example = "10.10.2.1:4791";
        description = "IP:port to bind (acceptor). The IP selects the RDMA device via CM.";
      };
      peer = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        example = "10.10.2.1:4791";
        description = "Peer IP:port to connect to (initiator).";
      };
      numQps = lib.mkOption {
        type = lib.types.nullOr (lib.types.ints.between 1 32);
        default = null;
        description = "Number of QPs (1..32). Null = CLI default.";
      };
      bufferCount = lib.mkOption {
        type = lib.types.nullOr (lib.types.ints.unsigned);
        default = null;
        description = "Buffer-pool count (>= 16). Null = CLI default.";
      };
      bufferSize = lib.mkOption {
        type = lib.types.nullOr (lib.types.ints.between 20 65536);
        default = null;
        description = "Per-buffer size in bytes (20..65536). Null = CLI default.";
      };
      passwordFile = lib.mkOption {
        type = lib.types.nullOr lib.types.path;
        default = null;
        description = ''
          Path to a file whose contents are the PSK. Read at ExecStart and passed
          as `--password` (the CLI has no file variant). NOTE: this exposes the
          PSK on the `urp` process argv (visible in /proc) — acceptable for a
          trusted lab, revisit if the CLI grows a file-based flag.
        '';
      };
      rdmaDevice = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        description = "Optional RDMA device hint (recorded; CM still selects by IP).";
      };
    };
  };

  # Build the `urp add <name> …` argv (everything except the secret).
  addArgs = ep: lib.concatStringsSep " " (
    [ "add" ep.name ]
    ++ lib.optional (ep.connectPath != null) "--connect-path ${ep.connectPath}"
    ++ lib.optional (ep.listenPath != null) "--listen-path ${ep.listenPath}"
    ++ lib.optional (ep.bind != null) "--bind ${ep.bind}"
    ++ lib.optional (ep.peer != null) "--peer ${ep.peer}"
    ++ lib.optional (ep.numQps != null) "--num-qps ${toString ep.numQps}"
    ++ lib.optional (ep.bufferCount != null) "--buffer-count ${toString ep.bufferCount}"
    ++ lib.optional (ep.bufferSize != null) "--buffer-size ${toString ep.bufferSize}"
    ++ lib.optional (ep.rdmaDevice != null) "--rdma-device ${ep.rdmaDevice}"
  );

  mkUnit = ep: lib.nameValuePair "urp-endpoint-${ep.name}" {
    description = "urp endpoint ${ep.name} (${ep.role})";
    after = [ "network-online.target" "systemd-modules-load.service" ];
    wants = [ "network-online.target" ];
    wantedBy = [ "multi-user.target" ];
    path = [ urpCli ];
    serviceConfig = {
      Type = "oneshot";
      RemainAfterExit = true;
    };
    # `urp add` establishes the RDMA session; the UDS backend behind an
    # acceptor's connectPath is supplied later (e.g. by the hw-matrix runner),
    # so the endpoint comes up even with no backend listening yet.
    script = ''
      pw=""
      ${lib.optionalString (ep.passwordFile != null)
        ''pw="--password $(cat ${ep.passwordFile})"''}
      # shellcheck disable=SC2086
      exec urp ${addArgs ep} $pw
    '';
    preStop = ''
      urp drain ${ep.name} || true
      urp remove ${ep.name} || true
    '';
  };
in
{
  options.services.urp = {
    enable = lib.mkEnableOption "urp (UDS-over-RoCEv2) kernel module + declarative endpoints";

    rdmaKernelModules = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ "ib_core" "rdma_cm" "mlx5_ib" ];
      description = "RDMA stack modules loaded before urp (mlx5 for ConnectX hardware).";
    };

    endpoints = lib.mkOption {
      type = lib.types.attrsOf (lib.types.submodule endpointOpts);
      default = { };
      description = "Declarative urp endpoints; one systemd oneshot each.";
    };

    extraPackages = lib.mkOption {
      type = lib.types.listOf lib.types.package;
      default = [ pkgs.rdma-core ];
      description = "Extra userspace packages (default: rdma-core for ibv_devices/show_gids).";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = lib.mapAttrsToList (n: ep: {
      assertion =
        if ep.role == "acceptor"
        then ep.connectPath != null && ep.bind != null
        else ep.listenPath != null && ep.peer != null;
      message = "services.urp.endpoints.${n}: acceptor needs connectPath+bind; initiator needs listenPath+peer.";
    }) cfg.endpoints;

    boot.extraModulePackages = [ urpKo ];
    boot.kernelModules = cfg.rdmaKernelModules ++ [ "urp" ];

    environment.systemPackages = [ urpCli ] ++ cfg.extraPackages;

    systemd.services = lib.mapAttrs' (_: ep: mkUnit ep) cfg.endpoints;
  };
}
