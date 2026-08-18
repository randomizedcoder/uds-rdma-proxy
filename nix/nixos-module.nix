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
# Optional design-33 Phase-3 control plane (deterministic cold-boot bring-up):
#   services.urp.control.enable = true;
#   services.urp.control.passwordFile = "/run/credentials/urp-ctl.psk";
# For each endpoint this adds, on the same host, `urp-control-serve-<name>`
# (acceptor side) and/or `urp-control-connect-<name>` (initiator side). The
# initiator unit is Type=notify and fires sd_notify(READY=1) once the peer
# reports ready. gRPC-OK is only a HINT — the kernel's Phase-1 retry stays the
# safety net; the control plane never touches /run/urp.sock.
#
# APP ORDERING (not auto-generated — the app unit is out of this module's scope):
# order your application/bench unit After + Wants the initiator control unit so
# it starts behind the readiness gate, e.g.
#   systemd.services.my-app = {
#     after = [ "urp-control-connect-pair_initiator.service" ];
#     wants = [ "urp-control-connect-pair_initiator.service" ];
#   };
# The app's own UDS connect then fires the Phase-2 lazy RDMA dial.
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
  urpControl = self.packages.${system}.urp-control;

  # "10.10.2.1:4791" -> "10.10.2.1". The control plane rides the same L2/L3 as
  # the RDMA data fabric (design 33 §33.6.3), so the acceptor's own bind IP /
  # the initiator's peer IP is exactly the private control interface to use.
  hostOf = hostPort: lib.head (lib.splitString ":" hostPort);

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

  ctl = cfg.control;

  # Acceptor control unit: serve Rendezvous/Heartbeat once the local endpoint
  # is up. Plain long-running service (it never signals sd_notify READY — the
  # *initiator* is the one that gates the app). The PSK is delivered via a
  # systemd credential and passed as a FILE PATH, never on argv.
  mkServeUnit = ep:
    let
      ip = if ctl.listenAddress != null then ctl.listenAddress else hostOf ep.bind;
      listen = "${ip}:${toString ctl.port}";
    in
    lib.nameValuePair "urp-control-serve-${ep.name}" {
      description = "urp-control acceptor (serve) for ${ep.name}";
      # After the endpoint so it can read its own state to answer `ready`.
      after = [ "urp-endpoint-${ep.name}.service" ];
      requires = [ "urp-endpoint-${ep.name}.service" ];
      wantedBy = [ "multi-user.target" ];
      path = [ urpControl ];
      serviceConfig = {
        Type = "simple";
        Restart = "on-failure";
        RestartSec = 2;
        LoadCredential = [ "psk:${toString ctl.passwordFile}" ];
      };
      script = ''
        exec urp-control serve \
          --endpoint ${ep.name} \
          --listen ${listen} \
          --session-cap ${toString ctl.sessionCap} \
          --local-id ${ep.name} \
          --password-file "$CREDENTIALS_DIRECTORY/psk"
      '';
    };

  # Initiator control unit: dial the peer acceptor and gate the app on the peer
  # reporting ready. Type=notify + sd_notify(READY=1) on gate-open; the app unit
  # orders After/Wants this unit (see the module header). TimeoutStartSec is
  # infinite so a slow-to-appear peer makes the daemon keep backing off/redialing
  # (its own retry loop) rather than systemd killing it for missing READY.
  mkConnectUnit = ep:
    let
      target = "http://${hostOf ep.peer}:${toString ctl.port}";
    in
    lib.nameValuePair "urp-control-connect-${ep.name}" {
      description = "urp-control initiator (connect) for ${ep.name}";
      after = [ "urp-endpoint-${ep.name}.service" ];
      requires = [ "urp-endpoint-${ep.name}.service" ];
      wantedBy = [ "multi-user.target" ];
      path = [ urpControl ];
      serviceConfig = {
        Type = "notify";
        NotifyAccess = "main";
        TimeoutStartSec = "infinity";
        Restart = "on-failure";
        RestartSec = 2;
        LoadCredential = [ "psk:${toString ctl.passwordFile}" ];
      };
      script = ''
        exec urp-control connect \
          --endpoint ${ep.name} \
          --target ${target} \
          --local-id ${ep.name} \
          --heartbeat-ms ${toString ctl.heartbeatMs} \
          --jitter-frac ${toString ctl.jitterFrac} \
          --poll-ms ${toString ctl.pollIntervalMs} \
          --password-file "$CREDENTIALS_DIRECTORY/psk"
      '';
    };

  acceptorEndpoints = lib.filter (ep: ep.role == "acceptor") (lib.attrValues cfg.endpoints);
  initiatorEndpoints = lib.filter (ep: ep.role == "initiator") (lib.attrValues cfg.endpoints);
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

    control = {
      enable = lib.mkEnableOption ''
        the urp-control gRPC control plane (design 33 Phase 3). For each declared
        endpoint it runs, on the same host, an acceptor `serve` unit and/or an
        initiator `connect` unit. The initiator gates the application on the peer
        reporting ready (systemd Type=notify), giving deterministic cold-boot
        bring-up on top of the kernel's Phase-1 retry safety net
      '';

      port = lib.mkOption {
        type = lib.types.port;
        default = 50051;
        description = "TCP port for the control plane (acceptor listens, initiator dials).";
      };

      passwordFile = lib.mkOption {
        type = lib.types.nullOr lib.types.path;
        default = null;
        description = ''
          Path to the control-plane PSK file. Delivered to the units via a systemd
          credential (`LoadCredential`) and passed to the daemon as a FILE PATH
          (`--password-file`), never on argv/env. Hashed (SHA-256) in-process and
          constant-time compared. Distinct from an endpoint's own `passwordFile`.
        '';
      };

      listenAddress = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = null;
        example = "10.10.2.1";
        description = ''
          Address the acceptor `serve` binds. When null (default) it is derived
          per-endpoint as the IP of that endpoint's `bind` — the private control
          fabric — so the control plane is NEVER exposed on 0.0.0.0 in a
          deployment. Set explicitly only to override that derivation.
        '';
      };

      sessionCap = lib.mkOption {
        type = lib.types.ints.unsigned;
        default = 256;
        description = "Max concurrent Heartbeat streams the acceptor admits (0 = unbounded).";
      };

      pollIntervalMs = lib.mkOption {
        type = lib.types.ints.positive;
        default = 1000;
        description = "Initiator kernel-state poll interval feeding the RDMA-failure probe (ms).";
      };

      heartbeatMs = lib.mkOption {
        type = lib.types.ints.positive;
        default = 60000;
        description = "Initiator base heartbeat cadence (ms), jittered by jitterFrac.";
      };

      jitterFrac = lib.mkOption {
        type = lib.types.float;
        default = 0.10;
        description = "Heartbeat jitter fraction (0.10 = +/-10%) to desync many hosts.";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = (lib.mapAttrsToList (n: ep: {
      assertion =
        if ep.role == "acceptor"
        then ep.connectPath != null && ep.bind != null
        else ep.listenPath != null && ep.peer != null;
      message = "services.urp.endpoints.${n}: acceptor needs connectPath+bind; initiator needs listenPath+peer.";
    }) cfg.endpoints)
    ++ [
      {
        assertion = !cfg.control.enable || cfg.control.passwordFile != null;
        message = "services.urp.control.enable requires services.urp.control.passwordFile (the gRPC PSK).";
      }
    ];

    boot.extraModulePackages = [ urpKo ];
    boot.kernelModules = cfg.rdmaKernelModules ++ [ "urp" ];

    environment.systemPackages = [ urpCli ] ++ cfg.extraPackages;

    systemd.services =
      (lib.mapAttrs' (_: ep: mkUnit ep) cfg.endpoints)
      // lib.optionalAttrs cfg.control.enable (lib.listToAttrs (
        (map mkServeUnit acceptorEndpoints)
        ++ (map mkConnectUnit initiatorEndpoints)
      ));
  };
}
