# Prometheus Metrics

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

All metrics use the prefix `uds_rdma_proxy_`. Metrics are exposed via an HTTP endpoint using the `metrics-exporter-prometheus` crate.

## 11.1 Connection Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `connections_active` | gauge | `role` | Currently active UDS connections |
| `connections_total` | counter | `role` | Total connections accepted/established |
| `connection_duration_seconds` | histogram | | Connection lifetime |
| `streams_active` | gauge | `peer` | Active multiplexed streams per peer (v3+) |
| `streams_total` | counter | `peer`, `initiator` | Total streams created (`local`, `remote`) |
| `stream_syn_duration_seconds` | histogram | | Time from SYN sent to SYN received (handshake latency) |
| `stream_syn_timeouts_total` | counter | | SYN handshake timeouts |
| `stream_syn_rejects_total` | counter | | SYN rejected by peer (RST response) |
| `stream_duration_seconds` | histogram | | Stream lifetime (SYN to final FIN) |
| `stream_window_stalls_total` | counter | | Times a stream was blocked on window exhaustion |
| `stream_window_stall_duration_seconds` | histogram | | Duration of stream window stalls |
| `stream_window_updates_total` | counter | | Stream window update messages sent |
| `stream_scheduler_rounds_total` | counter | | DRR scheduling rounds |
| `peer_connections_active` | gauge | | Currently active peer QP sets |
| `peer_connections_total` | counter | | Total peer QP sets established |
| `peer_idle_teardowns_total` | counter | | Peer QP sets torn down due to idle timeout |

## 11.2 Throughput Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `bytes_total` | counter | `direction` | Total bytes transferred (`uds_to_rdma`, `rdma_to_uds`) |
| `frames_total` | counter | `direction` | Total frames transferred |
| `payload_size_bytes` | histogram | `direction` | Distribution of frame payload sizes |

## 11.3 Latency Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `frame_latency_seconds` | histogram | `direction` | Time from UDS read CQE to RDMA send CQE (one-way proxy latency) |
| `uds_read_latency_seconds` | histogram | | io_uring read completion latency |
| `uds_write_latency_seconds` | histogram | | io_uring write completion latency |
| `rdma_send_latency_seconds` | histogram | | RDMA send CQE latency (post_send to poll_cq) |
| `rdma_recv_latency_seconds` | histogram | | Time between post_recv and recv CQE |

## 11.4 Flow Control Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `credits_available` | gauge | `qp_index`, `side` | Current send/recv credits per QP |
| `credit_stalls_total` | counter | `qp_index` | Times sender blocked on zero credits |
| `credit_stall_duration_seconds` | histogram | | Duration of credit stalls |
| `credit_grants_total` | counter | `qp_index` | Credit grant messages sent |
| `credit_grants_piggybacked_total` | counter | `qp_index` | Credit grants piggybacked on data |

## 11.5 Buffer Pool Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `buffers_total` | gauge | `pool` | Pool capacity (`tx`, `rx`) |
| `buffers_in_use` | gauge | `pool` | Currently allocated buffers |
| `buffer_alloc_failures_total` | counter | `pool` | Pool exhaustion events |
| `buffer_alloc_latency_seconds` | histogram | `pool` | Time to allocate a buffer (should be ~0 with lock-free pool) |

## 11.6 RDMA QP Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `cq_polls_total` | counter | `qp_index` | Number of ibv_poll_cq calls |
| `cq_empty_polls_total` | counter | `qp_index` | Polls that returned no completions |
| `cq_batch_size` | histogram | `qp_index` | Completions per poll call |
| `wc_errors_total` | counter | `qp_index`, `status` | Work completion errors by status code |
| `qp_state` | gauge | `qp_index` | Current QP state (encoded as integer) |
| `inline_sends_total` | counter | `qp_index` | Sends that used inline data path |
| `qp_health_state` | gauge | `qp_index` | Current QP lifecycle state (0=Qualifying, 1=Active, 2=Draining, 3=Removed) |
| `qp_removals_total` | counter | `qp_index`, `reason` | QP removal events (`error_wc`, `latency_outlier`, `credit_stall`, `qp_error_state`, `probe_failure`) |
| `qp_active_count` | gauge | | Number of QPs currently in Active state |
| `qp_send_outstanding` | gauge | `qp_index` | In-flight sends not yet completed (CQE not received) |
| `qp_selection_weight` | gauge | `qp_index` | Normalized weight in adaptive selector (0.0-1.0) |
| `qp_latency_ewma_seconds` | gauge | `qp_index` | EWMA-smoothed send completion latency |
| `qp_disable_notifications_sent_total` | counter | `qp_index` | QP_DISABLE control frames sent |
| `qp_disable_notifications_received_total` | counter | `qp_index` | QP_DISABLE control frames received from peer |
| `reorder_drain_timeout_extensions_total` | counter | | Times the gap timeout was extended for a draining QP |

## 11.7 Multi-QP Reorder Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `reorder_buffer_depth` | gauge | | Current frames in reorder buffer |
| `reorder_buffer_max_depth` | gauge | | High-water mark |
| `reorder_wait_seconds` | histogram | | Time frames spend buffered before delivery |
| `reorder_out_of_order_total` | counter | | Frames received out of order |
| `reorder_gap_timeouts_total` | counter | | Gap timeout events (indicates issues) |

## 11.8 io_uring Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `uring_sqe_submitted_total` | counter | `op` | SQEs submitted by operation type |
| `uring_cqe_completed_total` | counter | `op` | CQEs completed by operation type |
| `uring_sq_full_total` | counter | | Times the SQ was full (had to wait) |
| `uring_cq_overflow_total` | counter | | CQ overflow events (CQEs dropped) |

## 11.9 Adaptive Batching Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `batch_flush_reason_total` | counter | `reason` (`size`, `timeout`, `fin`) | Why each batch was flushed |
| `batch_size_messages` | histogram | | Messages per batch |
| `batch_size_bytes` | histogram | | Bytes per batch |
| `batch_latency_added_seconds` | histogram | | Time between first message arrival and batch flush |

## 11.10 Ancillary Data Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `ancillary_data_stripped_total` | counter | `type` (`scm_rights`, `scm_credentials`) | Ancillary data stripped in graceful mode |
| `ancillary_data_rejected_total` | counter | `type` | Connections rejected in strict mode due to ancillary data |

## 11.11 Health Probe Metrics

See [Section 8a](08a-qp-health-probes.md) for probe protocol details.

Each latency measurement follows a **three-tier** approach: a **histogram** (distribution, for percentile queries), an **EWMA gauge** (smoothed, for trend monitoring and the adaptive QP selector), and a **latest-sample gauge** (raw, for exact min/max queries via `min_over_time()` / `max_over_time()`). See [Section 8a.9](08a-qp-health-probes.md#8a9-prometheus-metrics) for the full rationale and example PromQL queries.

Probe latency histograms use RDMA-appropriate bucket boundaries: `[1us, 2us, 5us, 10us, 20us, 50us, 100us, 200us, 500us, 1ms, 5ms, 10ms, 50ms]`.

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `qp_probe_rtt_seconds` | histogram | `qp_index` | Per-QP probe RTT distribution |
| `qp_probe_rtt_ewma_seconds` | gauge | `qp_index` | EWMA-smoothed RTT (α=0.2). Feeds the adaptive QP selector. |
| `qp_probe_rtt_last_seconds` | gauge | `qp_index` | Last raw RTT sample. Enables `min_over_time()` / `max_over_time()` for exact min/max queries. |
| `qp_probe_rtt_jitter_seconds` | gauge | `qp_index` | EWMA of `|rtt_sample - rtt_ewma|` (same α). Path stability indicator. Analogous to RTTVAR (RFC 6298). |
| `qp_baseline_rtt_seconds` | gauge | `qp_index` | RTT baseline established during qualifying |
| `qp_probe_oneway_seconds` | histogram | `qp_index`, `direction` | One-way latency distribution (`a_to_b`, `b_to_a`). Only populated when PTP available. |
| `qp_probe_oneway_ewma_seconds` | gauge | `qp_index`, `direction` | EWMA-smoothed one-way latency per QP per direction |
| `qp_probe_oneway_last_seconds` | gauge | `qp_index`, `direction` | Last raw one-way latency sample per direction. PTP-only. |
| `qp_probe_asymmetry_ratio` | gauge | `qp_index` | Current `max(oneway_ab, oneway_ba) / min(...)`. 0 when PTP unavailable or insufficient samples. |
| `qp_probe_responder_processing_seconds` | gauge | `qp_index` | Last responder processing delay (`t_pong_real - t_recv_real`). PTP-only. Separates path latency from peer CPU load. |
| `qp_probe_sent_total` | counter | `qp_index` | Total PINGs sent per QP |
| `qp_probe_received_total` | counter | `qp_index` | Total PONGs received per QP |
| `qp_probe_timeouts_total` | counter | `qp_index` | Probes that timed out (no PONG within `probe_timeout`) |
| `qp_probe_consecutive_misses` | gauge | `qp_index` | Current consecutive miss count per QP (resets on success) |
| `qp_probe_success_rate` | gauge | `qp_index` | PONG/PING ratio over sliding window (default 20 probes). Range [0.0, 1.0]. |
| `qp_qualifying_duration_seconds` | histogram | `qp_index` | Time spent in Qualifying state |
| `qp_qualifying_failures_total` | counter | | QPs that failed qualifying |
| `qp_asymmetry_detected_total` | counter | `qp_index` | Times asymmetry threshold was exceeded |
| `qp_probe_ptp_available` | gauge | | 1 if PTP-synced clock is detected, 0 otherwise |

## 11.12 Process Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `process_cpu_seconds_total` | counter | | CPU time consumed |
| `process_resident_memory_bytes` | gauge | | RSS |
| `process_open_fds` | gauge | | Open file descriptors |
| `build_info` | gauge | `version`, `commit` | Build metadata |


[Back to Design Overview](../DESIGN.md)
