# Configuration

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

## 10.1 CLI Interface

```
# Bidirectional mode (cluster peer):
uds-rdma-proxy \
  --uds-listen-path /var/run/urp/to-B.sock \
  --uds-connect-path /var/run/redpanda/rpc.sock \
  --peer-address 10.0.1.2:4791 \
  --rdma-device mlx5_0 \
  --num-qps 8 \
  --config /etc/uds-rdma-proxy/peer-B.toml

# Listen-only mode (client-side proxy):
uds-rdma-proxy \
  --uds-listen-path /var/run/urp/app.sock \
  --peer-address 10.0.1.2:4791

# Connect-only mode (server-side proxy):
uds-rdma-proxy \
  --uds-connect-path /var/run/app/server.sock \
  --rdma-bind-address 0.0.0.0:4791
```

## 10.2 Configuration File (TOML)

```toml
# --- UDS Endpoint ---
# The proxy's behavior is determined by which paths are configured.
# Set listen_path for outbound (app connects to proxy), connect_path for inbound
# (proxy connects to app), or both for bidirectional mode.
[uds]
listen_path = "/var/run/urp/to-B.sock"      # Proxy creates this socket, accepts local connections
connect_path = "/var/run/redpanda/rpc.sock"  # Proxy connects to this existing socket
owner = "appuser"                            # UDS socket file owner (listen_path only)
group = "appgroup"                           # UDS socket file group (listen_path only)
permissions = "0660"                         # UDS socket file permissions (listen_path only)
ancillary_data = "strict"                    # "strict" (reject SCM_RIGHTS) or "graceful" (strip + warn)

# --- RDMA Transport ---
# RDMA directionality is configured explicitly:
#   peer_address → RDMA initiator (calls rdma_connect)
#   bind_address → RDMA acceptor (calls rdma_listen)
[rdma]
device = "mlx5_0"               # RDMA device name (e.g., rxe0 for software)
peer_address = "10.0.1.2:4791"  # Remote proxy address (RDMA initiator mode)
# bind_address = "0.0.0.0:4791" # Local bind address (RDMA acceptor mode, mutually exclusive with peer_address)
num_qps = 8                     # Number of QPs per connection (ECMP)
qp_selection = "round-robin"    # "round-robin", "adaptive", "hash-affinity", "batch-affinity"
                                # "adaptive" uses EWMA-weighted random selection (Section 8.5)
                                # and requires health probes to be enabled

# --- Security ---
[security]
password = ""                              # Shared password (max 16 chars). If set, both sides must
                                           # configure the same password. SHA-256(password) is exchanged
                                           # in RDMA CM private_data during connection setup. Protects
                                           # against accidental misconfiguration (like VRRP Type 1 auth).
                                           # NOT cryptographically secure against active attackers.
# Future Tier 1 (certificate-based mutual authentication):
# mode = "auth"                            # "none" | "password" | "auth" | "mtls"
# cert_file = "/etc/certs/proxy.crt"       # PEM certificate
# key_file = "/etc/certs/proxy.key"        # PEM private key
# ca_file = "/etc/certs/ca.crt"            # CA certificate for peer verification
# auth_timeout = "5s"                      # Timeout for post-QP auth handshake

[proxy]
idle_peer_timeout = "60s"       # Tear down peer QP set after this idle period
peer_warmup_list = []           # Peer addresses to pre-establish QPs at startup

[buffers]
count = 256                     # Buffer slots per direction (TX and RX)
size = 4096                     # Bytes per buffer slot
huge_pages = true               # Use 2MB huge pages

[flow_control]
initial_credits = 128           # Pre-posted receive buffers per QP
credit_threshold = 32           # Grant credits when this many accumulated
credit_stall_timeout = "5s"     # Timeout for credit stall before RST

[reorder]
buffer_timeout = "100ms"        # Gap timeout before declaring failure
max_buffered = 1024             # Max frames in reorder buffer before backpressure

[cq]
poll_mode = "adaptive"          # "busy", "event", "adaptive"
busy_poll_threshold = 1000      # Completions/sec to switch to busy-poll
idle_timeout_us = 100           # Microseconds of no CQEs to switch to event-driven

[batching]
dispatch_mode = "adaptive"      # "immediate": each UDS read → one RDMA frame (preserves
                                #   write boundaries, minimum latency, higher message rate)
                                # "adaptive": Nagle-like coalescing (default, better throughput)
max_flush_timeout_us = 100      # Ceiling for adaptive flush timer (microseconds).
                                # The adaptive algorithm scales from 0us (low load, converges
                                # to immediate behavior) up to this value (high load).
                                # Microseconds because RDMA RTT is ~1-2us on hardware.
                                # Typical range: 50-200us. Higher values improve throughput
                                # at cost of tail latency. Ignored in "immediate" mode.
size_threshold_pct = 75         # Flush when buffer is this % full (adaptive mode only)

[qp_health]
check_interval = "250ms"          # Health check frequency (soft degradation detection)
latency_outlier_factor = 0        # 0=disabled. Flag QP if p99 > factor * median(other QPs)
credit_stall_rate_threshold = 10  # Stalls/sec to flag QP (0=disabled)
credit_stall_window = "5s"        # Sustained for this long before flagging
enable_reprobing = false          # Whether removed QPs are re-probed and re-added
reprobe_interval = "30s"          # How often to re-probe removed QPs (if enabled)
drain_gap_timeout = "500ms"       # Extended gap timeout for draining QPs (> reorder.buffer_timeout)

[health_probes]
enabled = true                       # Enable QP health probing
probe_interval = "250ms"             # Interval between probes on each QP
probe_timeout = "500ms"              # Time to wait for PONG before declaring miss
max_consecutive_misses = 3           # Misses before QP is declared failed
qualifying_probe_count = 3           # Successful probes required to qualify
qualifying_probe_interval = "50ms"   # Faster probes during qualifying
qualifying_rtt_threshold_us = 1000   # Max acceptable RTT during qualifying
qualifying_max_misses = 0            # Max missed probes during qualifying (0 = strict)
ewma_alpha = 0.2                     # EWMA smoothing factor (0.0-1.0, also used for jitter EWMA)
probe_success_window_size = 20       # Sliding window for qp_probe_success_rate (probes)
asymmetry_threshold = 3.0            # One-way ratio to flag asymmetry (requires PTP)
asymmetry_min_samples = 5            # Min one-way samples before flagging
ptp_check_interval = "60s"           # How often to re-check PTP availability
ptp_offset_threshold_us = 1          # Max PTP offset_from_master to consider synced

[streams]
max_concurrent = 4096             # Max simultaneous streams per peer (0 = unlimited)
initial_window_size = 65536       # Per-stream receive window (bytes)
window_update_threshold = 0.25    # Grant window when this fraction consumed
syn_timeout = "5s"                # Timeout for SYN handshake completion
idle_stream_timeout = "30s"       # Close streams idle for this long (0 = disabled)
scheduling = "drr"                # "drr" (deficit round-robin) or "round-robin"
quantum = 4096                    # DRR quantum (bytes per scheduling round)

[performance]
cpu_affinity = [2, 3]           # Pin proxy threads to these cores
sq_poll = true                  # io_uring SQPOLL mode (requires CAP_SYS_NICE)
uds_io_mode = "io_uring"       # "io_uring" (default) or "splice" (experimental)

[metrics]
bind_address = "0.0.0.0:9090"  # Prometheus HTTP endpoint
path = "/metrics"               # Metrics path

[logging]
level = "info"                  # trace, debug, info, warn, error
format = "json"                 # "json" or "text"
```

## 10.3 Key Parameter Relationships

```
 buffer_count >= initial_credits (need enough buffers to pre-post)
 buffer_size >= FRAME_HEADER_SIZE + max_payload_size
 num_qps should match or exceed ECMP path count for full utilization
 credit_threshold < initial_credits (threshold must be reachable)
 probe_timeout > probe_interval (otherwise every probe times out)
 drain_gap_timeout > buffer_timeout (draining QP needs more time than normal reorder)
 qualifying_rtt_threshold_us >> expected_hardware_rtt (sanity check, not tight bound)
 initial_window_size >= buffer_size (stream window must fit at least one frame)
 max_concurrent × buffer_size <= SRQ capacity (prevent SRQ exhaustion)
 quantum >= max_payload_size (at least one frame per DRR scheduling round)
 syn_timeout < credit_stall_timeout (SYN should fail before credit stall triggers RST)
```


[Back to Design Overview](../DESIGN.md)
