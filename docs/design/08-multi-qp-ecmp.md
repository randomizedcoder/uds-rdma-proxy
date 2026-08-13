# Multi-QP Transport & ECMP Path Diversity

> **Note (2026-08-11):** Written during the original userspace-proxy design
> era (2026-05). The protocol and architecture content below still describes
> the implemented wire behavior, but implementation specifics are
> userspace-flavored (Rust proxy process, userspace ibverbs) — the shipped
> implementation is the `urp` **kernel module** (see [DESIGN.md](../DESIGN.md)),
> and v0–v4 phase references follow the abandoned userspace roadmap (see the
> phase-numbering note in [KERNEL-MODULE-PLAN.md](../KERNEL-MODULE-PLAN.md)).

## 8.1 Motivation

Modern data center networks use ECMP (Equal-Cost Multi-Path) routing to spread traffic across multiple physical paths. ECMP hashes on the 5-tuple (src IP, dst IP, src port, dst port, protocol) to determine which path a flow takes. A single RDMA QP maps to a single 5-tuple, so all its traffic follows a single ECMP path.

By creating multiple QPs between two nodes -- each with a different source port -- we get different ECMP hash values, spreading traffic across different physical paths. With 8 QPs on a 4-path ECMP topology, we can aggregate up to 4x the bandwidth of a single path.

```
                          Switch Tier
                     +---+   +---+   +---+
                     | 1 |   | 2 |   | 3 |
                     +-+-+   +-+-+   +-+-+
                      / \     / \     / \
                     /   \   /   \   /   \
 Machine A:         QP0,3  QP1,4  QP2,5         Machine B:
 [proxy]  --------  (different ECMP paths)  ---- [proxy]
  8 QPs             each QP hashes to a           8 QPs
  src ports         different spine switch
  10000-10007
```

## 8.2 The Reordering Problem

Different ECMP paths have different physical lengths, switch hop counts, and congestion levels. A frame sent on QP-3 at time T may arrive before a frame sent on QP-1 at time T-1, because QP-3's path is shorter or less congested.

Within a single RC QP, RDMA guarantees in-order delivery. But across QPs, there is no ordering guarantee. Since UDS is a byte stream, the proxy must deliver data to the application in exactly the order it was read from the sending application.

## 8.3 Sequence Numbers

The sender maintains a **global monotonic sequence counter** (per-direction, per-connection/stream). Before distributing a frame to a QP, the sender assigns the next sequence number:

```rust
// Sender side
let seq = self.next_sequence.fetch_add(1, Ordering::Relaxed);
frame.header.sequence_number = seq;
let qp_index = self.qp_selector.select(seq, &self.qp_stats);
self.qps[qp_index].post_send(frame)?;
```

## 8.4 Reorder Buffer

The receiver maintains a **B-tree** (specifically, `BTreeMap<u64, Frame>`) that sorts received frames by sequence number and delivers them in strict order:

```rust
struct ReorderBuffer {
    pending: BTreeMap<u64, Frame>,  // out-of-order frames waiting for delivery
    next_expected: u64,             // next sequence number to deliver
    max_buffered: usize,            // limit to prevent unbounded memory usage
}

impl ReorderBuffer {
    fn insert(&mut self, frame: Frame) -> Vec<Frame> {
        self.pending.insert(frame.header.sequence_number, frame);

        // Drain all consecutive frames starting from next_expected
        let mut ready = Vec::new();
        while let Some(frame) = self.pending.remove(&self.next_expected) {
            ready.push(frame);
            self.next_expected += 1;
        }
        ready
    }
}
```

**Delivery logic**:
- If the received frame's seq == `next_expected`, deliver it immediately (and drain any consecutive buffered frames).
- If seq > `next_expected`, buffer it in the B-tree and wait.
- If seq < `next_expected`, it's a duplicate (shouldn't happen with RC QPs, but handle defensively -- discard and log).

**Gap timeout**: RC QPs guarantee delivery, so gaps should be transient (just reordering). If `next_expected` is not received within a configurable timeout (default: 100ms), something is seriously wrong (QP failure, bug). The proxy should RST the connection and log the gap.

**Per-stream reorder buffers (v3)**: When stream multiplexing is active ([Section 9.6](09-connection-multiplexing.md#96-per-stream-sequence-numbers-and-reorder-buffers)), sequence numbers are scoped per-stream rather than global. Each stream maintains its own `ReorderBuffer` instance with an independent `next_expected` counter. This eliminates cross-stream head-of-line blocking — a gap in stream A does not delay delivery of stream B's frames. The reorder algorithm and B-tree implementation are identical; only the scope changes from one-per-connection to one-per-stream.

## 8.5 QP Selection Strategies

| Strategy | Description | Reordering Impact | Best For |
|----------|-------------|-------------------|----------|
| **Round-robin** | Cycle through QPs sequentially | Maximum reordering (every frame may take a different path) | Maximum bandwidth aggregation |
| **Adaptive** | Weight QP selection by CQ completion rate; prefer QPs with lower latency | Moderate reordering | Asymmetric paths, partial congestion |
| **Hash-affinity** | Hash `stream_id` to a fixed QP | Zero reordering within a stream (only one QP per stream) | Many concurrent streams, latency-sensitive |
| **Batch-affinity** | Send N consecutive frames on the same QP before rotating | Reduced reordering (consecutive frames stay ordered) | Balance of bandwidth and ordering |

**Recommendation**: Start with round-robin for maximum ECMP utilization. Add hash-affinity as an option for latency-sensitive workloads. Adaptive is the most complex but best for production.

### Adaptive Weighted QP Selector Algorithm

The adaptive strategy uses **weighted random selection** with EWMA-smoothed latency scores. This distributes traffic proportionally to path quality -- analogous to WCMP (Weighted-Cost Multi-Path) routing in data center networks.

```rust
struct AdaptiveQpSelector {
    /// Per-QP weight, recalculated periodically.
    /// Higher weight = more likely to be selected.
    weights: Vec<f64>,
    /// Cumulative weight distribution for O(log N) weighted random selection.
    cumulative: Vec<f64>,
    /// Per-QP EWMA of send completion latency (nanoseconds).
    latency_ewma: Vec<f64>,
    /// EWMA smoothing factor. Default 0.2 (matches TCP SRTT).
    alpha: f64,
    /// Total weight (sum of all active QP weights).
    total_weight: f64,
}
```

**Weight calculation** (runs on each health check interval, default 250ms):

```
For each QP i:
    1. Update latency_ewma[i] = alpha * latest_sample[i] + (1 - alpha) * latency_ewma[i]
    2. If QP i is in Qualifying, Draining, or Removed state: weight[i] = 0
    3. If QP i has zero credits: weight[i] = 0
    4. Otherwise: weight[i] = 1.0 / latency_ewma[i]
       (inverse latency: faster QPs get higher weight)

Normalize: total_weight = sum(weights)
Build cumulative distribution for binary search.
```

**Selection** (on each frame send):

```
1. Generate uniform random f64 in [0, total_weight)
   (fast PRNG like xoshiro256++, not cryptographic)
2. Binary search cumulative[] to find the QP index — O(log N)
3. If selected QP has zero credits, fall through to next non-zero QP (round-robin from that index)
```

**Why weighted random, not strict best-pick**: Strict best-picks starve slower QPs, concentrating all traffic on one path and defeating ECMP utilization. Weighted random distributes traffic proportionally to path quality -- a QP with 2x lower latency gets ~2x more traffic, but slower QPs still contribute.

**Why latency EWMA, not completion rate**: Completion rate is a throughput metric but fails for idle QPs (zero rate looks broken). Latency directly measures path quality and works even at low traffic rates. The latency input comes from CQ completion latency when the QP carries data, falling back to probe RTT EWMA when idle (see [Section 8a.4](08a-qp-health-probes.md#8a4-one-way-vs-rtt-measurement)).

**Fallback**: If all Active QPs have zero credits, the sender blocks on `wait_credit()` as in the single-QP case. If all QPs are in Draining/Removed state (none Active), the connection is torn down (RST).

## 8.6 Per-QP Flow Control

With multiple QPs, credit accounting is **per-QP**, not global:

```rust
struct QpFlowControl {
    send_credits: AtomicU32,      // how many sends we can issue on this QP
    credits_to_grant: AtomicU32,  // how many recv buffers we've re-posted
}

struct MultiQpTransport {
    qps: Vec<QpState>,
    flow_control: Vec<QpFlowControl>,  // one per QP
}
```

A stalled QP (credits exhausted on one path due to congestion) does not block sends on other QPs. The QP selector skips QPs with zero credits.

## 8.7 Falcon Protocol Reference

Google's **Falcon** NIC implements multi-path transport with hardware reordering. Falcon:
- Spreads packets across multiple network paths at the NIC level
- Reorders them in the receiving NIC's hardware before delivering to the application
- Eliminates the software reorder buffer overhead entirely

The uds-rdma-proxy reorder buffer is the software equivalent. When Falcon hardware is available, the proxy can be configured to use a single logical connection (Falcon handles the multi-pathing) and bypass the software reorder buffer entirely. This is a v4+ optimization.

## 8.8 Multi-QP Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `uds_rdma_proxy_qp_bytes_total` | counter | `qp_index`, `direction` | Bytes sent/received per QP |
| `uds_rdma_proxy_qp_frames_total` | counter | `qp_index`, `direction` | Frames sent/received per QP |
| `uds_rdma_proxy_qp_latency_seconds` | histogram | `qp_index` | Per-QP send completion latency |
| `uds_rdma_proxy_reorder_buffer_depth` | gauge | `stream_id` | Current number of frames in reorder buffer |
| `uds_rdma_proxy_reorder_buffer_max_depth` | gauge | | High-water mark of reorder buffer |
| `uds_rdma_proxy_reorder_wait_seconds` | histogram | | Time frames spend in the reorder buffer before delivery |
| `uds_rdma_proxy_reorder_out_of_order_total` | counter | | Frames that arrived out of order (needed buffering) |
| `uds_rdma_proxy_reorder_gap_timeouts_total` | counter | | Times the gap timeout fired (indicates serious issues) |

See also [Section 8a.9](08a-qp-health-probes.md#8a9-prometheus-metrics) for health probe-specific metrics (RTT histograms, one-way latency, qualifying duration, asymmetry detection).

## 8.9 QP Health State Machine

Each QP follows a four-state lifecycle. The state determines whether the QP is eligible for data traffic selection.

| State | Meaning | Carries data? |
|-------|---------|---------------|
| `Qualifying` | QP is in RTS state, health probes running, no data traffic yet | No |
| `Active` | Probes passed, QP carries data, ongoing probe monitoring | Yes |
| `Draining` | Flagged for removal; no new frames assigned, in-flight CQEs completing | No |
| `Removed` | Fully idle (`send_outstanding == 0`), removed from working set | No |

```
 Qualifying ──(probes pass)──> Active ──(failure detected)──> Draining ──(drained)──> Removed
      |                                                                                 ^
      +──(probes fail)──────────────────────────────────────────────────────────────────+
      
                          Removed ──(optional: reprobe)──> Qualifying
```

**Transitions**:

- **Qualifying -> Active**: Health probes pass qualifying criteria ([Section 8a.7](08a-qp-health-probes.md#8a7-qualifying-criteria)) -- at least `qualifying_probe_count` consecutive successes, all RTTs below threshold, no misses.
- **Qualifying -> Removed**: Probes fail during qualifying (timeout, RTT too high). No data was in flight, so Draining is skipped.
- **Active -> Draining**: Hard failure (error WC, QP ERROR state, `ibv_post_send` error) or soft degradation thresholds exceeded ([Section 8.10](#810-health-metrics-and-degradation-thresholds)), or probe misses exceed `max_consecutive_misses`.
- **Draining -> Removed**: `send_outstanding[qp_index]` reaches zero -- all in-flight sends have completed.
- **Removed -> Qualifying** (optional, disabled by default via `enable_reprobing = false`): Re-probe timer fires, QP is destroyed and recreated (new `rdma_cm` negotiation), new QP enters Qualifying.

**No automatic re-addition by default**. RDMA QP ERROR state is terminal -- the QP must be destroyed and recreated. Automatic recovery requires `rdma_cm` renegotiation for a single QP while others are active, which is complex. The Removed -> Qualifying path is deferred to post-v3.

> **NIC driver parallel**: This mirrors the Linux bonding driver's slave state machine (`BOND_STATE_ACTIVE` / `BOND_STATE_BACKUP` / link-down via `miimon`). The Qualifying state is analogous to a NIC link-up event where the driver waits for autonegotiation to complete before enabling the interface.

## 8.10 Health Metrics and Degradation Thresholds

Two categories of signals trigger QP state transitions:

### Hard Failures (Immediate -> Draining)

| Signal | Source | Why it is fatal |
|--------|--------|-----------------|
| WC error status (not `IBV_WC_SUCCESS`) | `ibv_poll_cq` returns error WC | RC QP transitions to ERROR state. ERROR is terminal. |
| QP state != RTS | `ibv_query_qp` or cached from `ibv_modify_qp` | Any state other than RTS means the QP cannot send/receive. |
| `ibv_post_send` returns error | Post send call fails | QP has entered ERROR state or hardware has failed. |

Hard failures are detected immediately on the CQ polling fast path -- the polling thread sees the error WC and initiates the transition without waiting for the periodic health check.

### Soft Degradation (Threshold-Based, Configurable)

| Signal | Default Threshold | Window | Why this threshold |
|--------|-------------------|--------|--------------------|
| Send latency outlier | p99 > `latency_outlier_factor` * median of other Active QPs | 1s sliding | Relative comparison avoids per-deployment tuning. The default factor of 2.0 is conservative enough to tolerate normal ECMP path variance. |
| Credit stall rate | > `credit_stall_rate_threshold` stalls/sec sustained | `credit_stall_window` (5s) | A single stall is normal under backpressure. Sustained stalls indicate the peer's receive side for this QP is broken or the path is severely congested. |

**Health check frequency**: `check_interval` (default: 250ms), analogous to the bonding driver's `miimon` (default: 100ms). The periodic check handles soft degradation; hard failures are detected on the fast path.

**v3 scope**: Ship with hard failure detection enabled. Soft degradation is disabled by default (`latency_outlier_factor = 0` to disable) -- these thresholds need tuning per deployment. Enable via `[qp_health]` config.

## 8.11 Dynamic Working Set Management

### Local Removal Protocol

1. Health checker or CQ polling thread detects a QP should be removed.
2. QP transitions to **Draining**. The selector immediately stops assigning new frames to it.
3. Track `send_outstanding[qp_index]` (incremented on `ibv_post_send`, decremented on send CQE).
4. When `send_outstanding[qp_index] == 0`, transition to **Removed**.

### Peer Notification via QP_DISABLE Control Frame

The peer must be informed so it stops posting receives on the disabled QP and updates its credit accounting. The notification is a Control frame (`frame_type = CONTROL`, `flags` bit 1 = `QP_DISABLE`):

```
stream_id:        0 (control frame)
frame_type:       0x01 (CONTROL)
flags:            CTRL_FLAG_QP_DISABLE (bit 1)
sequence_number:  next global sequence (participates in reorder)
credits_granted:  0
payload_length:   1
payload:          [qp_index: u8]
```

**In-band, sequenced**: The notification participates in the reorder buffer. This is important -- if QP 3 is being disabled, there may be data frames from QP 3 with lower sequence numbers still in flight. The disable notification must be processed in sequence order so the receiver does not disable the QP before those frames arrive.

**Sent on any remaining Active QP**: The notification does not depend on a specific control QP being healthy.

**Idempotent**: If both sides independently detect the same QP failure, both send QP_DISABLE. Receiving a disable for an already-Draining QP is a no-op.

**What if the notification QP also fails?** RC guarantees delivery within a healthy QP. If the QP carrying the notification enters ERROR state, both sides independently detect the failure via their own health checks and both disable it. This is safe because the disable operation is idempotent.

### Re-Enable (Deferred)

Flag bit 2 (`CTRL_FLAG_QP_ENABLE`) is reserved for future QP re-addition. This requires QP destroy+recreate and `rdma_cm` parameter exchange (QP number, PSN) -- too complex for v3.

## 8.12 Reorder Buffer Interaction with QP Removal

**Key insight**: Sequence numbers are global (not per-QP), so the reorder buffer does not know or care which QP a frame arrived on. QP removal is almost free for the reorder buffer.

### Gap Timeout False Positives

The one concern: if a draining QP has in-flight sends that include the `next_expected` sequence number, the reorder buffer is blocked waiting for that frame. If the degraded path is slow (which is why we are removing the QP), the normal 100ms gap timeout may fire prematurely.

**Solution**: When a QP enters Draining, check if its in-flight sequence numbers (tracked per-QP) include `next_expected` or any value in the reorder buffer's pending range. If so, extend the gap timeout for those specific sequence numbers from 100ms to `drain_gap_timeout` (default: 500ms).

```rust
impl ReorderBuffer {
    fn extend_timeout_for_draining_qp(&mut self, in_flight_seqs: &[u64]) {
        for &seq in in_flight_seqs {
            if seq >= self.next_expected {
                self.extended_timeouts.insert(seq, Instant::now() + self.drain_gap_timeout);
            }
        }
    }
}
```

### Tracking In-Flight Sequence Numbers

The sender already tracks `send_outstanding` per QP as a count. Upgrade this to a per-QP bounded ring buffer of `(qp_index, sequence_number)` pairs. When a send CQE arrives, remove the entry. When a QP enters Draining, snapshot its remaining entries and pass them to the receiver's reorder buffer for timeout extension.

### If the Drain Gap Timeout Fires

The QP is truly broken and cannot deliver its in-flight frames. Since RC guarantees delivery, this means the QP is in ERROR state and the frames will never arrive. The connection must be RST'd. This is correct behavior -- if frames are lost, the byte stream is corrupted and recovery is impossible.

## 8.13 NIC Driver Parallels for QP Health

The QP health management design follows established patterns from Linux NIC drivers:

| NIC Driver Pattern | Our QP Equivalent |
|---|---|
| **ethtool -S per-queue stats** (tx_packets, tx_errors per queue) | Per-QP Prometheus metrics (`qp_bytes_total`, `wc_errors_total`, `qp_latency_seconds`) |
| **bonding miimon** (poll link state at configurable interval, default 100ms) | `check_interval` health check (default 250ms) |
| **bonding balance-rr** (mode 0: round-robin across slaves) | Round-robin QP selector |
| **bonding balance-xor** (mode 2: XOR hash for slave selection) | Hash-affinity QP selector |
| **bonding balance-tlb** (mode 5: adaptive TX load balancing, weight by link speed) | Adaptive weighted QP selector (weight by latency EWMA) |
| **Individual queue disable** (ethtool or driver sysfs, drain then stop scheduling) | Dynamic QP removal (Draining -> Removed) |
| **NIC link-up autonegotiation** (wait for link training before enabling interface) | QP qualifying (probes must pass before carrying data) |
| **BFD** (RFC 5880: active path liveness, configurable timers, consecutive miss model) | Health probes ([Section 8a](08a-qp-health-probes.md)) |

**Key difference**: Standard NIC drivers do not notify the remote side when a local queue fails -- traffic is simply redistributed. In our case, both sides share QP state (RC is a connected transport), so the peer must be notified via `QP_DISABLE` to stop posting receives and update credit accounting. This is more comparable to a **TCP offload engine (TOE)** or **RDMA NIC firmware** than a standard NIC driver.


[Back to Design Overview](../DESIGN.md)
