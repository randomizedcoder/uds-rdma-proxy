# QP Health Probes & Latency Measurement

## 8a.1 Motivation and Prior Art

Each QP maps to a different ECMP path via its unique source port hash. Before sending data traffic, the proxy must verify that the path actually works and establish a latency baseline. During operation, continuous probing detects path degradation before it causes data loss or reorder buffer timeouts.

This is an application-layer measurement system embedded in the proxy protocol, following an established pattern:

- **SRT (Secure Reliable Transport)** embeds latency measurement in its protocol. SRT control packets include timestamps for RTT estimation and bandwidth probing, enabling the receiver to report path conditions back to the sender. Our probe protocol serves the same purpose but measures per-QP path quality rather than a single stream.
- **BFD (Bidirectional Forwarding Detection, RFC 5880)** provides active path liveness detection with configurable probe intervals and a consecutive-miss failure model. Our probe state machine mirrors BFD's detection logic.
- **LACP (802.3ad)** uses periodic LACPDUs for link health in bonded interfaces. Our probes serve a similar role but with richer data (latency measurement, not just liveness).

The common pattern: **the data transport includes its own health measurement plane** rather than relying on external monitoring. This ensures latency data is always available to the QP selector and that path failures are detected at the application layer, not just the link layer.

Key capabilities:

- QP qualification before carrying traffic ([Section 8.9](08-multi-qp-ecmp.md#89-qp-health-state-machine) Qualifying state)
- RTT measurement (always available, uses `CLOCK_MONOTONIC`)
- One-way latency measurement (requires PTP-synced `CLOCK_REALTIME` on both sides)
- Path asymmetry detection (compare one-way A->B vs B->A)
- Feeds the adaptive weighted QP selector with latency data ([Section 8.5](08-multi-qp-ecmp.md#85-qp-selection-strategies))

## 8a.2 PING/PONG Payload Wire Format

Health probes use `frame_type = PROBE` (0x02) from the frame header ([Section 4.4](04-framing-protocol.md#44-frame-types)). The `flags` byte distinguishes PING (bit 0 = 0) from PONG (bit 0 = 1). Probes are control frames (`stream_id = 0`) and do not consume global sequence numbers.

Both payloads fit within the 64-byte `max_inline_data` threshold ([Section 5.2](05-rdma-transport.md#52-queue-pair-configuration)), so probes use the fast inline WQE path -- the NIC reads the payload directly from the WQE without issuing a separate DMA read.

### PING Payload (32 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                       probe_seq                               |  4 bytes
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |          qp_index             |  clock_flags  |   reserved    |  4 bytes
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_send_mono (ns)                          |  8 bytes
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_send_real (ns)                          |  8 bytes
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     padding (reserved)                        |  8 bytes
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### PONG Payload (48 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                       probe_seq                               |  4 bytes (echoed)
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |          qp_index             |  clock_flags  |   reserved    |  4 bytes (echoed)
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_send_mono (ns)                          |  8 bytes (echoed)
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_send_real (ns)                          |  8 bytes (echoed)
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_recv_real (ns)                          |  8 bytes (responder)
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_pong_mono (ns)                          |  8 bytes (responder)
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                     t_pong_real (ns)                          |  8 bytes (responder)
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Field Definitions

| Field | Type | Description |
|-------|------|-------------|
| `probe_seq` | `u32` | Per-QP probe sequence number. Starts at 0 for each QP, increments per PING. Wraps at `u32::MAX`. |
| `qp_index` | `u16` | Which QP this probe measures. Must match the QP the frame is sent on. |
| `clock_flags` | `u8` | Bit 0: `HAS_REALTIME` -- sender populated `t_send_real` with PTP-synced clock. Bit 1: `HAS_RECV_REALTIME` (PONG only) -- responder populated `t_recv_real` and `t_pong_real` with PTP-synced clock. Bits 2-7: reserved. |
| `reserved` | `u8` | Must be zero. |
| `t_send_mono` | `u64` | Sender's `CLOCK_MONOTONIC` timestamp in nanoseconds at the moment of posting the PING. |
| `t_send_real` | `u64` | Sender's `CLOCK_REALTIME` timestamp in nanoseconds. Zero if PTP not available (bit 0 of `clock_flags` unset). |
| `padding` | `u64` | Reserved. Keeps PING at 32 bytes (8-byte aligned). |
| `t_recv_real` | `u64` | Responder's `CLOCK_REALTIME` at PING reception. Zero if PTP not available on responder. |
| `t_pong_mono` | `u64` | Responder's `CLOCK_MONOTONIC` at PONG send. |
| `t_pong_real` | `u64` | Responder's `CLOCK_REALTIME` at PONG send. Zero if PTP not available on responder. |

### Rust Representation

```rust
#[repr(C, packed)]
struct PingPayload {
    probe_seq: u32,
    qp_index: u16,
    clock_flags: u8,
    reserved: u8,
    t_send_mono: u64,
    t_send_real: u64,
    padding: u64,
}

const PING_PAYLOAD_SIZE: usize = 32;

#[repr(C, packed)]
struct PongPayload {
    probe_seq: u32,       // echoed from PING
    qp_index: u16,        // echoed from PING
    clock_flags: u8,      // echoed + bit 1 set by responder
    reserved: u8,         // echoed
    t_send_mono: u64,     // echoed from PING
    t_send_real: u64,     // echoed from PING
    t_recv_real: u64,     // responder's CLOCK_REALTIME at PING arrival
    t_pong_mono: u64,     // responder's CLOCK_MONOTONIC at PONG send
    t_pong_real: u64,     // responder's CLOCK_REALTIME at PONG send
}

const PONG_PAYLOAD_SIZE: usize = 48;
```

Both structs belong in the shared `uds-rdma-protocol` crate (`no_std + alloc`), making them available to both userspace and kernel module.

### Design Decisions

| Decision | Rationale |
|----------|-----------|
| `stream_id = 0` for probes | Probes are control frames, not application data. They bypass the reorder buffer. |
| Separate `probe_seq` per QP | The global `sequence_number` in the frame header is per-direction across all QPs. Using it for probes would create gaps in the reorder buffer. Probe loss is tracked independently. |
| Probes on the measured QP | The PING must traverse the specific QP being measured. The PONG returns on the same QP. Otherwise we measure a different ECMP path. |
| Both mono and real timestamps | Enables both RTT and one-way measurement from a single probe type. The receiver ignores `t_send_real` if PTP is unavailable. |
| PING = 32B, PONG = 48B | Both fit within the 64B `max_inline_data` threshold, using the fast inline WQE path without a separate DMA read. |

## 8a.3 Probe State Machine

Each QP tracks its own probe state, independent of the QP lifecycle state machine ([Section 8.9](08-multi-qp-ecmp.md#89-qp-health-state-machine)). Probes run during both the Qualifying and Active lifecycle states.

```
                +---------------------+
                | AwaitingFirstProbe  |   (QP just entered RTS, no probes sent yet)
                +---------+-----------+
                          |
                     send first PING
                          |
                          v
                +---------------------+
         +----->| PingSent            |   (waiting for PONG response)
         |      +---------+-----------+
         |                |
         |         +------+------+
         |         |             |
         |    PONG received   timeout (probe_timeout)
         |         |             |
         |         v             v
         |  +-------------+  +-------------+
         |  | ProbeComplete|  | ProbeFailed  |
         |  +------+------+  +------+------+
         |         |                |
         |    update EWMA      misses++
         |    evaluate           |
         |    criteria      +----+----+
         |         |        |         |
         |         v    < max      >= max_consecutive_misses
         |   wait interval  |         |
         |         |        v         v
         +---------+   wait interval  QP FAILURE
         |              |         (Qualifying: -> Removed)
         +--------------+         (Active: -> Draining)
```

### Per-QP Probe State

```rust
struct QpProbeState {
    probe_seq: u32,                    // next probe sequence number for this QP
    phase: ProbePhase,                 // AwaitingFirstProbe | PingSent | ProbeComplete | ProbeFailed
    consecutive_misses: u32,           // resets to 0 on any successful probe
    total_probes_sent: u64,
    total_probes_received: u64,
    last_ping_sent_at: Instant,        // for timeout detection
    qualifying_successes: u32,         // consecutive successes since entering Qualifying

    // Smoothed latency (EWMA, α=0.2)
    rtt_ewma_ns: f64,                  // EWMA-smoothed RTT
    oneway_ab_ewma_ns: Option<f64>,    // A->B one-way (None if PTP unavailable)
    oneway_ba_ewma_ns: Option<f64>,    // B->A one-way (None if PTP unavailable)
    baseline_rtt_ns: Option<u64>,      // established during qualifying, used as reference

    // Latest sample (raw, unsmoothed — for min/max queries via PromQL)
    rtt_last_ns: u64,                           // last raw RTT sample
    oneway_ab_last_ns: Option<u64>,             // last raw A->B one-way sample
    oneway_ba_last_ns: Option<u64>,             // last raw B->A one-way sample

    // Derived operational metrics
    rtt_jitter_ewma_ns: f64,                    // EWMA of |rtt - rtt_ewma| (path stability)
    asymmetry_ratio: f64,                       // max(oneway_ab, oneway_ba) / min(...)
    responder_processing_last_ns: Option<u64>,  // t_pong_real - t_recv_real (peer delay)
    probe_success_window: VecDeque<bool>,       // sliding window for success rate
}
```

## 8a.4 One-Way vs RTT Measurement

### RTT (Always Available)

On receiving a PONG, the original sender computes:

```
rtt = now_mono - t_send_mono
```

where `now_mono` is the sender's `CLOCK_MONOTONIC` at PONG reception and `t_send_mono` is echoed from the original PING. This uses only the sender's own monotonic clock -- no synchronization needed, always reliable.

### One-Way (Requires PTP on Both Sides)

If both sides have PTP-synced `CLOCK_REALTIME` (both `HAS_REALTIME` and `HAS_RECV_REALTIME` bits set in `clock_flags`):

```
oneway_A_to_B = t_recv_real - t_send_real          (PING direction)
oneway_B_to_A = now_real - t_pong_real              (PONG direction)
responder_processing = t_pong_real - t_recv_real    (time spent constructing PONG)
```

The responder's processing delay is typically sub-microsecond but can spike if the CQ polling thread is busy. Having both timestamps (`t_recv_real` and `t_pong_real`) allows the sender to separate path latency from processing delay.

### EWMA Smoothing

Both RTT and one-way measurements feed an exponentially weighted moving average:

```
ewma = alpha * new_sample + (1 - alpha) * ewma
```

Default `alpha = 0.2` (same constant used for TCP SRTT in RFC 6298). Lower values produce smoother estimates; higher values track changes faster.

### Latest Sample and Derived Metrics

On each PONG reception, after updating the EWMA, record the raw sample and compute derived metrics:

```rust
// Record raw sample (for min/max PromQL queries)
self.rtt_last_ns = rtt;

// Jitter: EWMA of |sample - smoothed| (analogous to RTTVAR in RFC 6298)
self.rtt_jitter_ewma_ns = alpha * (rtt as f64 - self.rtt_ewma_ns).abs()
                        + (1.0 - alpha) * self.rtt_jitter_ewma_ns;

// If PTP available on both sides:
if has_ptp {
    self.oneway_ab_last_ns = Some(oneway_ab);
    self.oneway_ba_last_ns = Some(oneway_ba);
    self.responder_processing_last_ns = Some(t_pong_real - t_recv_real);

    // Update asymmetry ratio (only after sufficient samples)
    if oneway_ab_samples >= min_samples && oneway_ba_samples >= min_samples {
        let (hi, lo) = (oneway_ab_ewma.max(oneway_ba_ewma),
                        oneway_ab_ewma.min(oneway_ba_ewma));
        self.asymmetry_ratio = if lo > 0.0 { hi / lo } else { 0.0 };
    }
}

// Probe success tracking (sliding window)
self.probe_success_window.push_back(true);
if self.probe_success_window.len() > window_size {
    self.probe_success_window.pop_front();
}
```

On probe timeout, push `false` into the success window instead.

The **jitter metric** indicates path stability. A QP with 5us mean RTT and 0.2us jitter is healthy; the same QP with 4us jitter is about to be flagged. Alerting rule: `jitter > 3 * baseline_rtt` suggests the path is becoming unreliable.

The **asymmetry ratio** gives operators a live view of developing asymmetry before it crosses the threshold (default 3.0). An alert on `ratio > 2.0` provides early warning.

The **responder processing delay** separates path latency from peer CPU load. If RTT increases but responder processing also spikes, the QP path is fine — the peer is just busy. This prevents false degradation signals.

### Latency Source Priority

The adaptive QP selector ([Section 8.5](08-multi-qp-ecmp.md#85-qp-selection-strategies)) needs a per-QP latency value. Two sources are available:

1. **CQ completion latency** -- measured on every data send (high frequency, accurate under load)
2. **Probe RTT EWMA** -- measured at `probe_interval` (lower frequency, works when QP is idle)

Use CQ completion latency when the QP carries data traffic. Fall back to probe RTT EWMA when the QP is idle or has low traffic. This ensures latency data is always available even for underutilized QPs.

## 8a.5 PTP Availability Detection

The proxy checks for PTP-synchronized clocks at startup and periodically (default: every 60 seconds).

### Userspace (v3)

1. Check for PTP hardware clock devices at `/sys/class/ptp/ptp*/`.
2. Optionally read `offset_from_master` via the `PTP_SYS_OFFSET_PRECISE` ioctl. If the offset is below `ptp_offset_threshold_us` (default: 1us), PTP is considered synced.
3. Alternative: check if `ptp4l` or `phc2sys` processes are running (simpler but environment-specific).

### Kernel Module (k1+)

Use `ktime_get_real_ns()` for `CLOCK_REALTIME` and `ktime_get_ns()` for `CLOCK_MONOTONIC`. PTP detection: check the RDMA device's associated `net_device` for a `ptp_clock` via `ethtool_get_ts_info()` or by inspecting `net_device->ptp_clock`.

### Fallback

If PTP is not detected:
- Set `clock_flags` bit 0 to 0, fill `t_send_real` with 0.
- One-way measurements are unavailable. RTT measurement remains fully functional.
- Log once at INFO: "PTP clock not detected, one-way latency measurement disabled, RTT measurement active."
- If PTP is detected but later degrades (offset exceeds threshold), the periodic check clears the flag and invalidates existing one-way EWMA values (set to `None`).

## 8a.6 Asymmetry Detection

Path asymmetry -- where the A->B path has significantly different latency than B->A -- can indicate a routing issue, congestion on one direction, or a misconfigured switch. Detection requires PTP on both sides.

### Algorithm

Given one-way EWMA values `oneway_ab` and `oneway_ba`:

```
ratio = max(oneway_ab, oneway_ba) / min(oneway_ab, oneway_ba)
```

If `ratio > asymmetry_threshold` (default: 3.0), flag the QP path as asymmetric.

### Action on Detection

- Increment `qp_asymmetry_detected_total` counter.
- Log a WARNING with the QP index, the two one-way values, and the ratio.
- The adaptive QP selector naturally reduces the QP's weight via the higher-latency direction's EWMA. No additional selector logic is needed.
- Do NOT automatically remove the QP. Asymmetry may be intentional (e.g., different physical path lengths in the topology). Operators decide based on the metric whether to investigate.

### Guard Against False Positives

Only flag asymmetry after at least `asymmetry_min_samples` (default: 5) one-way measurements have been collected on both sides. This prevents transient spikes during startup from triggering the flag.

## 8a.7 Qualifying Criteria

When a QP enters the Qualifying state (just transitioned to RTS), probes determine whether it is safe to carry data traffic.

### Requirements for Transition to Active

1. At least `qualifying_probe_count` (default: 3) **consecutive** successful probe round-trips.
2. All measured RTTs below `qualifying_rtt_threshold_us` (default: 1000us = 1ms). This is a sanity check, not a tight bound -- hardware RDMA RTT is typically 1-5us; this threshold catches completely broken or wildly misconfigured paths.
3. No more than `qualifying_max_misses` (default: 0) missed probes during the entire qualifying phase. Any miss during qualifying fails the QP -- the path is unreliable before it has even started carrying data.

### Qualifying Probe Interval

During qualifying, probes are sent at `qualifying_probe_interval` (default: 50ms) -- faster than the normal operational `probe_interval` (default: 250ms). This qualifies the QP quickly: 3 probes at 50ms = qualified in 150ms.

### On Qualifying Success

- Record `baseline_rtt_ns` as the average of the qualifying probe RTTs. This baseline serves as a reference for future degradation detection.
- Transition QP lifecycle state from Qualifying to Active.
- Log at INFO: "QP {qp_index} qualified: baseline RTT {baseline_rtt_ns}ns after {qualifying_probe_count} probes."

### On Qualifying Failure

- Transition QP lifecycle state directly from Qualifying to Removed (skip Draining -- no data was in flight).
- Log at WARN: "QP {qp_index} failed qualifying after {n} probes: {reason}."
- The system may attempt to create a replacement QP in a future version.

## 8a.8 Probe Frequency Recommendations

| Scenario | `probe_interval` | `qualifying_probe_interval` | Credit cost (8 QPs) | Failure detection time |
|----------|------------------|-----------------------------|---------------------|------------------------|
| Low-latency production | 100ms | 25ms | 80/sec | 600ms |
| Standard production (default) | 250ms | 50ms | 32/sec | 1.5s |
| High-QP-count (32 QPs) | 500ms | 100ms | 64/sec | 3.0s |
| Development/testing | 1s | 250ms | 8/sec | 6.0s |
| Aggressive monitoring | 50ms | 10ms | 160/sec | 300ms |

**Credit impact**: At default settings (250ms interval, 8 QPs), probes consume 32 credits/sec each way. With `initial_credits = 128` per QP ([Section 10](10-configuration.md)), this is 0.25 credits/sec per QP -- negligible compared to the data path throughput of ~100K+ credits/sec.

**Probe timeout**: `probe_timeout` (default: `probe_interval * 2` = 500ms). How long to wait for a PONG before declaring a miss. Must be greater than `probe_interval`.

**Max consecutive misses**: `max_consecutive_misses` (default: 3). After this many consecutive missed probes, the QP is declared failed. Total failure detection time = `max_consecutive_misses * probe_timeout`.

## 8a.9 Prometheus Metrics

All metrics use the `uds_rdma_proxy_` prefix.

### Three-Tier Metrics Philosophy

Each latency measurement is exposed in three tiers to serve different operational needs:

| Tier | Prometheus Type | Purpose | Example query |
|------|----------------|---------|---------------|
| **Histogram** | histogram | Distribution, percentile queries via `histogram_quantile()` | SLO alerting: "p99 RTT < 50us" |
| **EWMA gauge** | gauge (`*_ewma_*`) | Smoothed current value, stable dashboard trend line | Trend monitoring, adaptive QP selector input |
| **Latest-sample gauge** | gauge (`*_last_*`) | Raw last measurement, enables `min_over_time()` / `max_over_time()` | Min/max alerting: "worst-case RTT across all QPs" |

Histograms lose exact values in bucket quantization. EWMA smooths out spikes. Only the latest-sample gauge supports exact min/max queries — e.g., `max_over_time(qp_probe_rtt_last_seconds[5m])` returns the true worst-case RTT in the last 5 minutes, not an approximation.

### Histogram Bucket Configuration

Probe latency histograms should use RDMA-appropriate bucket boundaries:

```
buckets: [0.000001, 0.000002, 0.000005, 0.00001, 0.00002, 0.00005,
          0.0001,   0.0002,   0.0005,   0.001,   0.005,   0.01, 0.05]
          (1us      2us       5us       10us     20us     50us
           100us    200us     500us     1ms      5ms      10ms  50ms)
```

Hardware RDMA RTT: 1-5us. Software rxe: 50-200us. Anything above 1ms is degraded. The sub-microsecond resolution at the low end captures hardware-level path differences between QPs.

### Metrics Table

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| | | | **RTT (Round-Trip Time)** |
| `qp_probe_rtt_seconds` | histogram | `qp_index` | Per-QP probe RTT distribution |
| `qp_probe_rtt_ewma_seconds` | gauge | `qp_index` | EWMA-smoothed RTT (α=0.2). Feeds the adaptive QP selector. |
| `qp_probe_rtt_last_seconds` | gauge | `qp_index` | Last raw RTT sample. Enables `min_over_time()` / `max_over_time()` for exact min/max queries. |
| `qp_probe_rtt_jitter_seconds` | gauge | `qp_index` | EWMA of `|rtt_sample - rtt_ewma|` (same α). Low jitter = stable path. Analogous to RTTVAR in RFC 6298. |
| `qp_baseline_rtt_seconds` | gauge | `qp_index` | RTT baseline established during qualifying. Reference for degradation detection. |
| | | | **One-Way Latency (PTP required)** |
| `qp_probe_oneway_seconds` | histogram | `qp_index`, `direction` | One-way latency distribution (`a_to_b`, `b_to_a`). Only populated when PTP available on both sides. |
| `qp_probe_oneway_ewma_seconds` | gauge | `qp_index`, `direction` | EWMA-smoothed one-way latency per QP per direction |
| `qp_probe_oneway_last_seconds` | gauge | `qp_index`, `direction` | Last raw one-way latency sample per direction. PTP-only. |
| `qp_probe_asymmetry_ratio` | gauge | `qp_index` | Current `max(oneway_ab, oneway_ba) / min(...)`. Set to 0 when PTP unavailable or insufficient samples. Enables pre-threshold alerting. |
| `qp_probe_responder_processing_seconds` | gauge | `qp_index` | Last responder processing delay (`t_pong_real - t_recv_real`). PTP-only. Separates path latency from peer CPU load. |
| | | | **Probe Lifecycle** |
| `qp_probe_sent_total` | counter | `qp_index` | Total PINGs sent per QP |
| `qp_probe_received_total` | counter | `qp_index` | Total PONGs received per QP |
| `qp_probe_timeouts_total` | counter | `qp_index` | Probes that timed out (no PONG within `probe_timeout`) |
| `qp_probe_consecutive_misses` | gauge | `qp_index` | Current consecutive miss count per QP (resets on success) |
| `qp_probe_success_rate` | gauge | `qp_index` | Ratio of PONGs received to PINGs sent over a sliding window (default last 20 probes). Range [0.0, 1.0]. |
| | | | **Qualifying** |
| `qp_qualifying_duration_seconds` | histogram | `qp_index` | Time spent in Qualifying state before transitioning |
| `qp_qualifying_failures_total` | counter | | QPs that failed qualifying |
| | | | **PTP & Asymmetry** |
| `qp_asymmetry_detected_total` | counter | `qp_index` | Times asymmetry threshold was exceeded |
| `qp_probe_ptp_available` | gauge | | 1 if PTP-synced clock is detected, 0 otherwise |

### Example PromQL Queries

| Use Case | PromQL |
|----------|--------|
| Worst-case RTT across all QPs (5m window) | `max_over_time(uds_rdma_proxy_qp_probe_rtt_last_seconds[5m])` |
| Best QP RTT for baseline comparison | `min_over_time(uds_rdma_proxy_qp_probe_rtt_last_seconds[5m])` |
| RTT spread (indicator of ECMP path diversity) | `max_over_time(...[5m]) - min_over_time(...[5m])` |
| p99 RTT over last 5 minutes | `histogram_quantile(0.99, rate(uds_rdma_proxy_qp_probe_rtt_seconds_bucket[5m]))` |
| Asymmetry developing (early warning) | `uds_rdma_proxy_qp_probe_asymmetry_ratio > 2.0` |
| Unstable path (jitter alert) | `uds_rdma_proxy_qp_probe_rtt_jitter_seconds > 3 * uds_rdma_proxy_qp_baseline_rtt_seconds` |
| Peer overloaded (processing delay spike) | `uds_rdma_proxy_qp_probe_responder_processing_seconds > 0.001` |
| QP probe health degradation | `uds_rdma_proxy_qp_probe_success_rate < 0.9` |
| QPs with RTT > 2× baseline | `uds_rdma_proxy_qp_probe_rtt_ewma_seconds > 2 * uds_rdma_proxy_qp_baseline_rtt_seconds` |

## 8a.10 Configuration

```toml
[health_probes]
enabled = true                       # Enable QP health probing
probe_interval = "250ms"             # Interval between probes on each QP
probe_timeout = "500ms"              # Time to wait for PONG before declaring miss
max_consecutive_misses = 3           # Misses before QP is declared failed

# Qualifying phase
qualifying_probe_count = 3           # Successful probes required to qualify
qualifying_probe_interval = "50ms"   # Faster probes during qualifying
qualifying_rtt_threshold_us = 1000   # Max acceptable RTT during qualifying (sanity check)
qualifying_max_misses = 0            # Max missed probes during qualifying (0 = strict)

# Latency tracking
ewma_alpha = 0.2                     # EWMA smoothing factor (0.0-1.0, also used for jitter EWMA)
probe_success_window_size = 20       # Sliding window for qp_probe_success_rate (probes)

# Asymmetry detection (requires PTP on both sides)
asymmetry_threshold = 3.0            # One-way ratio to flag asymmetry
asymmetry_min_samples = 5            # Min one-way samples before flagging

# PTP clock detection
ptp_check_interval = "60s"           # How often to re-check PTP availability
ptp_offset_threshold_us = 1          # Max PTP offset_from_master to consider synced
```

**Key parameter relationships**:
```
probe_timeout > probe_interval              (otherwise every probe would timeout)
qualifying_rtt_threshold_us >> expected_hardware_rtt  (sanity check, not tight bound)
ewma_alpha in (0, 1)                        (lower = smoother, higher = more responsive)
```

## 8a.11 Kernel Module Considerations

For the kernel module (k1+), the health probe system is adapted to kernel APIs:

- **Timestamps**: `ktime_get_ns()` for `CLOCK_MONOTONIC`, `ktime_get_real_ns()` for `CLOCK_REALTIME`. Both return nanoseconds, matching the payload format directly.
- **Probe timer**: `hrtimer_start()` with `HRTIMER_MODE_REL` for high-resolution probe scheduling. The default kernel timer resolution (`CONFIG_HZ` dependent, typically 1ms or 4ms) is too coarse for the 50ms qualifying interval -- `hrtimer` provides nanosecond resolution.
- **PTP detection**: Check the RDMA device's associated `net_device` for a `ptp_clock` via `ethtool_get_ts_info()` or by inspecting `net_device->ptp_clock`.
- **Shared structs**: `PingPayload` and `PongPayload` are defined in the shared `uds-rdma-protocol` crate (`no_std + alloc`), making them available to both userspace and kernel module code paths. The `QpProbeState` logic can also be shared.
- **Timestamp capture**: Probe processing runs in the CQ completion callback context (or the CQ polling kthread). The PONG timestamp capture should happen as early as possible in the completion handler to minimize measurement noise from scheduling delays.


[Back to Design Overview](../DESIGN.md)
