# Reusable NixOS module: `nixosModules.urp-exporter` (design 39 §39.5, PR2).
#
# Runs the `urp-exporter` Prometheus agent as a hardened, CPU-bounded,
# unprivileged systemd service. It scrapes the urp kernel module's generic
# netlink counters and serves /metrics on localhost. The overriding design
# constraint is COST: the mesh is per-node-CPU / copy-path bound (design 38
# §38.5), so any CPU the agent steals shows up as lost goodput. The unit is
# therefore CPUQuota- and MemoryMax-capped by default (§39.3 rule 6) -- the
# quota is the copy-path-theft backstop, the memory cap the leak backstop.
#
# Consumed alongside `nixosModules.urp` (which loads urp.ko + brings up
# endpoints):
#   imports = [
#     uds-rdma-proxy.nixosModules.urp
#     uds-rdma-proxy.nixosModules.urp-exporter
#   ];
#   services.urp.enable = true;
#   services.urp-exporter.enable = true;                # scrape 127.0.0.1:9975
#
# It binds localhost only (design 39 §39.3 rule 7); front it with a reverse
# proxy for remote scrape / TLS. It needs NO secret (the netlink surface is
# read-only, root-less), so there is no LoadCredential -- unlike the control
# plane's PSK. Prometheus alert rules ship separately as
# `nix/urp-exporter-alerts.yml`; a Grafana dashboard as
# `nix/urp-exporter-dashboard.json`.
#
# Like `nixosModules.urp`, this is system-agnostic and lives OUTSIDE the flake's
# `eachDefaultSystem` wrapper; it receives `self` to resolve the per-system
# `self.packages.<sys>.urp-exporter`.
{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.urp-exporter;
  system = pkgs.stdenv.hostPlatform.system;
  urpExporter = self.packages.${system}.urp-exporter;

  # Assemble the argv from the typed options -- mirrors the flag surface of
  # crates/urp-exporter/src/config.rs exactly (--per-qp is on by default, so
  # emit --no-per-qp when the option is cleared).
  execArgs = lib.concatStringsSep " " (
    [ "--listen ${cfg.listenAddress}:${toString cfg.port}" ]
    ++ [ "--cache-ttl-ms ${toString cfg.cacheTtlMs}" ]
    ++ [ "--scrape-timeout-ms ${toString cfg.scrapeTimeoutMs}" ]
    ++ [ "--max-series ${toString cfg.maxSeries}" ]
    ++ lib.optional (!cfg.perQp) "--no-per-qp"
    ++ lib.optional cfg.perStream "--per-stream"
  );
in
{
  options.services.urp-exporter = {
    enable = lib.mkEnableOption ''
      the urp Prometheus exporter (design 39): a hardened, CPU-bounded,
      localhost-only systemd service that scrapes the urp kernel module's
      generic-netlink counters and serves /metrics
    '';

    listenAddress = lib.mkOption {
      type = lib.types.str;
      default = "127.0.0.1";
      description = ''
        Address the /metrics HTTP listener binds. Localhost by default -- front
        with a reverse proxy for remote scrape / TLS (design 39 §39.3 rule 7).
        Setting a non-loopback address exposes an unauthenticated read-only
        surface directly; do so only on a trusted private fabric.
      '';
    };

    port = lib.mkOption {
      type = lib.types.port;
      default = 9975;
      description = "TCP port for the /metrics HTTP listener.";
    };

    cacheTtlMs = lib.mkOption {
      type = lib.types.ints.unsigned;
      default = 250;
      description = ''
        Serve the cached render if the last scrape is younger than this (ms) --
        bounds netlink load under over-eager scrapers (design 39 §39.3 rule 5).
        0 disables the cache (scrape every request).
      '';
    };

    scrapeTimeoutMs = lib.mkOption {
      type = lib.types.ints.positive;
      default = 2000;
      description = "Per-scrape netlink deadline, also the HTTP header read timeout (ms).";
    };

    maxSeries = lib.mkOption {
      type = lib.types.ints.positive;
      default = 100000;
      description = ''
        Hard cap on emitted sample lines; past it the renderer stops and bumps
        `urp_exporter_series_capped_total` rather than serve an unbounded
        payload (design 39 §39.2 cardinality guard).
      '';
    };

    perQp = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Emit per-QP series (`urp_qp_*`). On by default -- QP granularity is the
        view that visualises the mesh fairness latch (design 38 §38.5).
      '';
    };

    perStream = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Emit per-stream series (`urp_stream_*`). OFF by default -- streams are
        ephemeral and churn label cardinality (design 39 §39.2).
      '';
    };

    cpuQuota = lib.mkOption {
      type = lib.types.str;
      default = "10%";
      example = "5%";
      description = ''
        systemd `CPUQuota` for the unit -- the copy-path-theft backstop
        (design 39 §39.3 rule 6). The mesh is per-node-CPU bound, so hard-cap
        the exporter's share so it can never starve the data path.
      '';
    };

    memoryMax = lib.mkOption {
      type = lib.types.str;
      default = "64M";
      example = "128M";
      description = "systemd `MemoryMax` for the unit -- the leak backstop.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Open `port` in the firewall. Off by default -- the exporter is
        localhost-only by default and meant to be reverse-proxied.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    networking.firewall.allowedTCPPorts = lib.mkIf cfg.openFirewall [ cfg.port ];

    systemd.services.urp-exporter = {
      description = "urp Prometheus exporter (design 39)";
      # The exporter opens the genl "urp" family, which only exists once urp.ko
      # is loaded (by services.urp). Order behind module load; stay up (Restart)
      # and render `urp_up 0` while the module/socket is absent rather than fail.
      after = [ "systemd-modules-load.service" "network.target" ];
      wantedBy = [ "multi-user.target" ];
      serviceConfig = {
        Type = "simple";
        ExecStart = "${urpExporter}/bin/urp-exporter ${execArgs}";
        Restart = "on-failure";
        RestartSec = 2;

        # --- Hardening (design 39 §39.5): unprivileged, sandboxed, capped. ---
        DynamicUser = true;
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        ProtectKernelTunables = true;
        ProtectKernelModules = true;
        ProtectControlGroups = true;
        ProtectClock = true;
        ProtectHostname = true;
        PrivateTmp = true;
        PrivateDevices = true;
        RestrictNamespaces = true;
        RestrictRealtime = true;
        RestrictSUIDSGID = true;
        LockPersonality = true;
        MemoryDenyWriteExecute = true;
        # AF_NETLINK: read the urp genl counters. AF_INET/AF_INET6: /metrics.
        RestrictAddressFamilies = [ "AF_NETLINK" "AF_INET" "AF_INET6" ];
        SystemCallFilter = [ "@system-service" ];
        SystemCallErrorNumber = "EPERM";
        SystemCallArchitectures = "native";
        CapabilityBoundingSet = "";
        UMask = "0077";

        # --- Resource caps: the whole point (§39.3 rule 6). ---
        CPUQuota = cfg.cpuQuota;
        MemoryMax = cfg.memoryMax;
        TasksMax = 16;
      };
    };
  };
}
