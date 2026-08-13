# Connection Management & Stream Multiplexing

> **Note (2026-08-11):** Written during the original userspace-proxy design
> era (2026-05). The protocol and architecture content below still describes
> the implemented wire behavior, but implementation specifics are
> userspace-flavored (Rust proxy process, userspace ibverbs) — the shipped
> implementation is the `urp` **kernel module** (see [DESIGN.md](../DESIGN.md)),
> and v0–v4 phase references follow the abandoned userspace roadmap (see the
> phase-numbering note in [KERNEL-MODULE-PLAN.md](../KERNEL-MODULE-PLAN.md)).

## 9.1 Scale Analysis

At cluster scale, QP consumption dominates RDMA resource planning. Consider a full-mesh deployment where every node pair has a tunnel:

```
 Node pairs = n × (n - 1) / 2

 n = 100    →       4,950 pairs
 n = 1,000  →     499,500 pairs
 n = 3,500  →   6,123,250 pairs
```

**Per-node QP counts** (each node connects to `n - 1` peers):

| Model | QPs per peer | n = 100 | n = 1,000 | n = 3,500 |
|-------|-------------|---------|-----------|-----------|
| v2: 1 QP set per UDS connection, 10 conns, 8 QPs | 80 | 7,920 | 79,920 | **279,920** |
| v2: 1 QP set per UDS connection, 50 conns, 8 QPs | 400 | 39,600 | 399,600 | **1,399,600** |
| v3: 1 shared QP set per peer, 8 QPs | 8 | 792 | 7,992 | **27,992** |
| v3: 1 shared QP set per peer, 16 QPs | 16 | 1,584 | 15,984 | **55,984** |

**NIC QP limits** (approximate):

| NIC | Max QPs | Notes |
|-----|---------|-------|
| ConnectX-6 Dx | ~128K | Practical limit depends on QP context cache |
| ConnectX-7 | ~256K | Higher QP context cache (on-chip SRAM) |
| Software RDMA (rxe) | ~64K | Limited by kernel memory |

At 3,500 nodes the v2 model with just 10 concurrent connections per peer **exceeds ConnectX-7 limits**. With 50 connections (realistic for Redpanda with many partitions) it requires **5× the NIC capacity**. Multiple applications per node multiply this further.

Stream multiplexing over shared QP sets reduces per-node QP count by **10-50×**, making large-cluster deployment feasible. The `stream_id` field in the frame header (4 bytes, `u32`) exists precisely for this purpose.

## 9.2 v2: One QP Set Per UDS Connection

The simplest model: each UDS `accept()` triggers creation of a new RDMA connection (or set of QPs in multi-QP mode).

```
 UDS Connection 1  <-->  QP Set 1 (N QPs)  <-->  UDS Connection 1
 UDS Connection 2  <-->  QP Set 2 (N QPs)  <-->  UDS Connection 2
 UDS Connection 3  <-->  QP Set 3 (N QPs)  <-->  UDS Connection 3
```

**Advantages**: Simple. Isolated failures — one connection's QP error doesn't affect others. Global sequence numbers with a single reorder buffer. Straightforward credit accounting.

**Disadvantages**: NIC resource consumption scales with connection count × QP count. QP context cache thrashing at high connection counts. `rdma_cm` connection setup per UDS connection adds latency for short-lived connections.

**When v2 is sufficient**: Small clusters (< 100 nodes), few concurrent connections per peer, dedicated RDMA hardware with large QP capacity.

## 9.3 v3: Per-Peer Shared QP Sets

Multiple UDS connections between the same node pair share a single QP set, distinguished by `stream_id`:

```
 Node A                                                    Node B
 +-------------------+                                    +-------------------+
 | UDS conn 1 (sid=1)|--\                            /----| UDS conn 1 (sid=1)|
 | UDS conn 2 (sid=3)|---+--[ QP Set (8 QPs) ]---+------| UDS conn 2 (sid=3)|
 | UDS conn 3 (sid=5)|--/     shared across       \-----| UDS conn 3 (sid=5)|
 +-------------------+        all streams                +-------------------+
```

The proxy maintains a **peer table** mapping remote addresses to shared QP sets, and a **stream table** mapping `stream_id` values to per-stream state:

```rust
/// Per-peer RDMA connection, shared across all streams to that peer.
struct PeerConnection {
    qp_set: Vec<QueuePair>,          // N QPs for ECMP
    srq: SharedReceiveQueue,         // Shared across all QPs to this peer
    cq: CompletionQueue,             // Shared CQ (or per-QP CQs)
    qp_selector: AdaptiveQpSelector, // Weighted QP selection
    streams: HashMap<u32, StreamState>,
    next_stream_id: u32,             // Monotonically increasing, odd or even
    qp_credits: Vec<CreditState>,    // Per-QP credit counters
}

/// Per-stream state within a shared QP set.
struct StreamState {
    stream_id: u32,
    uds_fd: RawFd,
    state: StreamLifecycleState,
    tx_seq: u64,                     // Per-stream send sequence counter
    reorder_buffer: ReorderBuffer,   // Per-stream reorder buffer
    recv_window: StreamWindow,       // Per-stream receive window
    pump_handles: (JoinHandle<()>, JoinHandle<()>),
    stats: StreamStats,
}
```

**Key change from v2**: The QP set is established once per peer and reused for all UDS connections to that peer. Stream creation is a lightweight operation (allocate `stream_id`, send SYN frame) rather than a heavy RDMA connection setup.

## 9.4 Stream Lifecycle Protocol

Stream lifecycle uses the existing `DATA_FLAG_SYN`, `DATA_FLAG_FIN`, and `DATA_FLAG_RST` flags. No new frame types or flags are required.

### Stream States

```
 Initiator (accepts UDS connection):

   IDLE ──(accept UDS, alloc stream_id)──→ SYN_SENT ──(recv peer SYN)──→ ESTABLISHED
                                               │                              │
                                          (recv RST)                    (send/recv FIN)
                                               │                              │
                                               v                              v
                                            CLOSED                     HALF_CLOSED
                                                                           │
                                                                     (both FINs)
                                                                           │
                                                                           v
                                                                        CLOSED

 Responder (receives SYN, creates UDS connection):

   IDLE ──(recv SYN with new stream_id)──→ CONNECTING ──(UDS connect ok)──→ ESTABLISHED
                                               │                              │
                                          (UDS connect fails)           (send/recv FIN)
                                               │                              │
                                               v                              v
                                        send RST, CLOSED               HALF_CLOSED
                                                                           │
                                                                     (both FINs)
                                                                           │
                                                                           v
                                                                        CLOSED
```

### Handshake

```
 Initiator                                              Responder

 accept(uds_fd)
 alloc stream_id = S
 insert StreamState(sid=S, SYN_SENT)

 DATA(sid=S, flags=SYN, seq=0) ─────────────────────→  lookup sid=S → not found → new stream
                                                        connect(target_uds_path)
                                                        insert StreamState(sid=S, CONNECTING)
                                                         │
                                                        on success:
 stream S → ESTABLISHED  ←───────────────────────────  DATA(sid=S, flags=SYN, seq=0)
                                                        stream S → ESTABLISHED
                                                         │
                                                        on failure:
 stream S → CLOSED       ←───────────────────────────  DATA(sid=S, flags=RST)
 close(uds_fd)
```

**SYN as acknowledgment**: The responder's SYN frame for stream_id=S serves as both "stream accepted" and "ready to receive." The initiator distinguishes a new-stream SYN from an acknowledgment SYN by checking its stream table — if `stream_id=S` is in `SYN_SENT` state, the incoming SYN is an acknowledgment. This is analogous to TCP's simultaneous open.

**First data can ride the SYN**: The initiator's SYN frame can carry a payload (first UDS read). The responder buffers it until the UDS connection to the target is established, then writes it. This eliminates one RTT of latency for the first message.

**SYN timeout**: If the responder doesn't reply (SYN or RST) within `stream_syn_timeout` (default: 5s), the initiator RSTs the stream and closes the UDS connection.

### Teardown

Half-close follows the same pattern as v2 ([Section 7.2](07-bidirectional-pump.md#72-half-close-handling)), scoped to the stream:

```
 Side A: UDS EOF → send DATA(sid=S, flags=FIN) → stop uds_to_rdma pump for stream S
 Side B: recv FIN for sid=S → shutdown(uds_fd, SHUT_WR) for stream S
 When both sides have sent FIN: stream S fully closed, resources freed, stream_id eligible for reuse
```

RST at any point tears down the stream immediately. The shared QP set is unaffected — other streams continue operating.

## 9.5 Stream ID Allocation

The `stream_id` field is `u32`, providing ~4.3 billion possible values. `stream_id = 0` is reserved for connection-level control frames (credit grants, QP management, health probes).

**Even/odd allocation** (following HTTP/2 convention):

| RDMA Role | Stream ID range | Examples |
|-----------|----------------|----------|
| RDMA initiator | Odd | 1, 3, 5, 7, ... |
| RDMA acceptor | Even | 2, 4, 6, 8, ... |
| Control | 0 | Probes, credits, QP management |

Stream ID parity is determined by RDMA directionality (which side called `rdma_connect` vs `rdma_listen`), not by UDS mode. In bidirectional mode, both sides can initiate application streams — the RDMA initiator uses odd IDs and the RDMA acceptor uses even IDs regardless of which direction the UDS connection flows.

Each side increments its own counter by 2. No coordination needed — collisions are impossible.

**Exhaustion**: At 1,000 new streams per second (high for UDS tunneling), the odd-only space (~2.15 billion IDs) lasts **~25 days**. If exhaustion approaches, the proxy can:

1. Recycle closed stream IDs (maintain a free list of IDs below the high-water mark)
2. Reset the connection with a full QP set teardown and re-establishment (rare, expected to be a non-event in practice)

**ID reuse safety**: A stream ID must not be reused while the peer might still have in-flight frames referencing it. After a stream reaches `CLOSED` state on both sides, a **quiet period** of `2 × reorder.buffer_timeout` (default: 200ms) must elapse before the ID is recycled. This ensures all frames from the old stream have been delivered or timed out.

## 9.6 Per-Stream Sequence Numbers and Reorder Buffers

### Global vs. Per-Stream Sequencing

In v2 (single stream per QP set), `sequence_number` is a global counter and there is one reorder buffer. In v3 with multiplexing, this creates **cross-stream head-of-line (HOL) blocking**: a gap in stream A's sequence prevents delivery of stream B's higher-numbered frames.

v3 transitions to **per-stream sequence numbers**:

```
 v2 (single stream):                    v3 (multiplexed):
                                        
 Global seq: 1, 2, 3, 4, 5, 6...       Stream 1 seq: 1, 2, 3, 4...
 One reorder buffer                     Stream 3 seq: 1, 2, 3, 4...
                                        Stream 5 seq: 1, 2, 3, 4...
                                        Per-stream reorder buffers
```

The `sequence_number` field in the frame header is unchanged — it is now scoped to the `(stream_id, direction)` tuple rather than being globally unique. Each `StreamState` maintains its own monotonic counter:

```rust
impl StreamState {
    fn next_sequence(&mut self) -> u64 {
        let seq = self.tx_seq;
        self.tx_seq += 1;
        seq
    }
}
```

### Per-Stream Reorder Buffers

Each stream has its own `ReorderBuffer` (same B-tree implementation as v2, [Section 8.4](08-multi-qp-ecmp.md#84-b-tree-reorder-buffer)). On the receive path:

```
 CQ poll thread
      │
      ├── decode frame → extract (stream_id, sequence_number)
      │
      ├── lookup stream_id in stream table
      │
      ├── insert into stream's reorder buffer
      │
      └── deliver in-order frames → stream's UDS write task
```

**Independence**: A gap in stream A's sequence (e.g., seq 5 delayed on a slow QP) does not block stream B's delivery. Stream B's reorder buffer operates on its own sequence space.

**Memory**: Each reorder buffer consumes memory proportional to its maximum depth. With 1,000 concurrent streams and `max_buffered = 64` per stream:
- Worst case: 64,000 buffered frame references × ~80 bytes each = **~5 MB**
- Typical case (most streams have 0-2 buffered frames): **< 1 MB**

### Backward Compatibility

v2 mode (single stream) still works — there is one stream with `stream_id = 1` and one reorder buffer. The per-stream model degenerates to the global model when there is only one stream.

## 9.7 Two-Layer Flow Control

Multiplexing introduces a dual flow control requirement, directly paralleling HTTP/2's design:

```
 Layer 1: Per-QP credits (RDMA-level, prevents RNR errors)
 Layer 2: Per-stream receive windows (application-level, prevents stream starvation)
```

### Layer 1: Per-QP Credits (Unchanged)

Credit-based flow control ([Section 5.3](05-rdma-transport.md#53-credit-based-flow-control)) remains per-QP. Credits track receive buffer availability on the NIC/SRQ and prevent fatal RNR errors. Credits are shared across all streams using a QP — they are a NIC-level resource concern, not an application-level one.

### Layer 2: Per-Stream Receive Windows

Without stream-level flow control, one stream with a slow consumer (slow UDS write target) can consume all SRQ receive buffers, starving other streams. The failure mode:

```
 Stream A: slow consumer → recv buffers not recycled → SRQ depleted → ALL streams stall
```

Per-stream receive windows bound how much data can be in-flight to a single stream:

```rust
struct StreamWindow {
    size: u32,          // Total window size (bytes), default 65536
    remaining: u32,     // Bytes the sender can still send
}
```

**Sender side**: Before sending a DATA frame for stream S, check `stream_windows[S].remaining >= payload_length`. If insufficient, the stream is **window-stalled** — skip it and send from other streams that have window space. This is NOT a credit stall (QP credits may be available; the stream is just at its window limit).

**Receiver side**: After writing stream S's data to the UDS socket and freeing the receive buffer, increment the window and send a window update:

```
 CONTROL frame:
   stream_id = S (target stream, NOT 0)
   flags = CTRL_FLAG_STREAM_WINDOW
   payload = { window_increment: u32 }
```

Window updates can be piggybacked on DATA frames flowing in the reverse direction (the `credits_granted` field handles QP credits; stream window updates use CONTROL frames). The receiver batches window updates using the same threshold logic as credit grants — send when `accumulated_grants >= window_size / 4`.

**Initial window**: Exchanged during the SYN handshake. The SYN frame's payload can carry the initial stream window size (4 bytes). Default: 65,536 bytes (matching HTTP/2's default).

### Interaction Between Layers

```
 Can I send a frame for stream S on QP Q?

   1. Check QP Q credits > 0              (Layer 1: NIC-level)
   2. Check stream S window > payload_len  (Layer 2: app-level)
   3. Both pass → send
   4. QP credits exhausted → credit stall (blocks ALL streams on that QP)
   5. Stream window exhausted → window stall (blocks only stream S, try other streams)
```

A QP credit stall is more severe (blocks everything on that QP), but short-lived if the receiver is processing. A stream window stall is targeted and expected — it is the mechanism that prevents a single slow stream from monopolizing resources.

## 9.8 Stream Scheduling and Fairness

With multiple streams sharing QPs, the send path must decide **which stream to service next** when multiple streams have data ready. Without scheduling, a high-throughput stream could monopolize QP bandwidth and starve other streams.

### Weighted Round-Robin Scheduler

```rust
struct StreamScheduler {
    ready_streams: VecDeque<u32>,    // Stream IDs with pending data
    deficit: HashMap<u32, u32>,      // Deficit counter per stream (DRR)
    quantum: u32,                    // Bytes per scheduling round (default: buffer_size)
}
```

The scheduler uses **Deficit Round-Robin (DRR)**, the same algorithm used in Linux's `sch_drr` qdisc and in many NIC hardware schedulers:

1. Each stream starts with `deficit = 0`
2. When a stream is scheduled: `deficit += quantum`
3. Send frames for this stream while `deficit >= frame_size` and stream window allows
4. `deficit -= bytes_sent`
5. If stream still has data and deficit remaining: it stays at the front
6. Otherwise: move to back of queue, next stream

**Why DRR**: O(1) per-packet scheduling. Fair bandwidth sharing regardless of frame sizes. Well-understood in networking (used in NIC hardware, Linux tc, DPDK).

**Priority streams**: A stream can be marked as high-priority (e.g., Redpanda Raft heartbeats vs. bulk replication). High-priority streams get a larger quantum or are serviced from a separate priority queue. This is configurable per UDS socket path.

### Interaction with QP Selection

Stream scheduling and QP selection are orthogonal:

```
 1. StreamScheduler picks stream S     (which stream to send)
 2. AdaptiveQpSelector picks QP Q      (which path to use)
 3. Send frame: stream_id=S, on QP Q
```

The scheduler doesn't know or care about QPs. The QP selector doesn't know or care about streams.

## 9.9 Head-of-Line Blocking Analysis

HOL blocking is the primary concern for multiplexed protocols. Our design avoids it at multiple levels:

### Comparison with HTTP/2 and QUIC

| Protocol | Transport | Stream ordering | HOL blocking? |
|----------|-----------|----------------|---------------|
| **HTTP/2** | Single TCP connection | Per-stream, but TCP enforces global ordering | **Yes** — TCP loss blocks all streams |
| **QUIC** | UDP + per-stream reassembly | Per-stream, independent | **No** — loss on stream A doesn't block B |
| **SCTP** | Multi-stream within association | Per-stream, independent | **No** (with unordered delivery option) |
| **v2 (ours)** | Multi-QP RC, global sequence | Global across all streams | **Yes** — same as HTTP/2's problem |
| **v3 (ours)** | Multi-QP RC, per-stream sequence | Per-stream, independent | **No** — same as QUIC's design |

Our v3 multiplexing is architecturally closest to **QUIC**: independent streams with per-stream sequence numbering over multiple paths (QPs instead of UDP packets). The per-stream reorder buffer ensures that a slow path affecting stream A does not delay stream B.

### Remaining HOL Blocking Vectors

Per-stream sequencing eliminates cross-stream HOL blocking, but two residual vectors remain:

1. **Intra-stream, cross-QP**: Within a single stream, frames spread across QPs can arrive out of order. The per-stream reorder buffer handles this (same as v2's global reorder buffer, but scoped to one stream). This is inherent to multi-QP and acceptable — the reorder buffer gap timeout (100ms) bounds the worst case.

2. **SRQ exhaustion**: If the total number of in-flight receives across ALL streams exceeds SRQ capacity, new receives cannot be posted. This is mitigated by per-stream receive windows (Section 9.7) which cap each stream's buffer consumption.

## 9.10 Connection Table Architecture

The proxy maintains a two-level lookup structure:

```
 Peer Table                          Stream Table (per peer)
 ┌──────────────┬───────────────┐    ┌────────────┬───────────────┐
 │ Peer Address │ PeerConnection│    │ stream_id  │ StreamState   │
 ├──────────────┼───────────────┤    ├────────────┼───────────────┤
 │ 10.0.1.2     │ {qp_set, ...}│───→│ 1          │ {uds_fd, ...} │
 │ 10.0.1.3     │ {qp_set, ...}│    │ 3          │ {uds_fd, ...} │
 │ 10.0.1.4     │ {qp_set, ...}│    │ 5          │ {uds_fd, ...} │
 └──────────────┴───────────────┘    └────────────┴───────────────┘
```

### Lazy vs. Eager QP Establishment

| Strategy | Behavior | Trade-off |
|----------|----------|-----------|
| **Lazy** (default) | QP set created on first UDS connection to a peer | No wasted resources; first-connection latency includes RDMA setup (~1-10ms) |
| **Eager** | QP sets pre-established to configured peers at startup | Zero latency on first connection; wastes QPs for unused peers |
| **Warm pool** | Pre-establish QPs to the N most recent peers, evict LRU | Balances latency and resource usage; good for stable clusters |

**Recommendation**: Lazy establishment by default, with an optional `peer_warmup_list` in config for latency-sensitive deployments.

### QP Set Lifecycle

A shared QP set is torn down when:

1. All streams to that peer are closed AND no new streams for `idle_peer_timeout` (default: 60s)
2. The peer becomes unreachable (all QPs enter ERROR state)
3. Explicit operator action (admin API / signal)

The idle timeout prevents QP thrashing for applications that frequently open/close connections to the same peer (e.g., short-lived gRPC calls).

## 9.11 Multi-Application Topologies

### One Proxy Per Application (Recommended)

```
 Node A
 ┌──────────────────────────────────────────────────────────────┐
 │  ┌──────────────┐  UDS  ┌─────────────────────┐             │
 │  │ Redpanda     │◄─────►│ proxy (redpanda)     │──┐         │
 │  └──────────────┘       │ /tmp/redpanda.sock   │  │ QP Set  │
 │                         └─────────────────────┘  │ to B    │
 │                                                   │         │
 │  ┌──────────────┐  UDS  ┌─────────────────────┐  │         │
 │  │ PostgreSQL   │◄─────►│ proxy (postgres)     │──┘ separate│
 │  └──────────────┘       │ /tmp/postgres.sock   │    QP Sets │
 │                         └─────────────────────┘             │
 └──────────────────────────────────────────────────────────────┘
```

Each application runs its own proxy instance with its own QP sets. Advantages:
- **Isolation**: A Redpanda bug crashing its proxy doesn't affect PostgreSQL
- **Independent configuration**: Different buffer sizes, QP counts, batching strategies per application
- **Simple operations**: Restart one proxy without affecting others

Cost: 2× QP consumption per peer (one set per application). At 3,500 nodes with 2 applications: 2 × 8 × 3,499 = **55,984 QPs per node** — still within NIC limits.

### Shared Proxy (Future, Deferred)

A single proxy daemon serving multiple UDS paths over shared QPs:

```toml
[[tunnels]]
uds_path = "/tmp/redpanda.sock"
peer_address = "10.0.1.2:4791"

[[tunnels]]
uds_path = "/tmp/postgres.sock"
peer_address = "10.0.1.2:4791"   # same peer, shared QP set
```

Both tunnels to the same peer would share one QP set, with stream IDs distinguishing the application. This halves QP consumption but couples application lifecycles. **Deferred to v4** — the per-application model is simpler and sufficient for v3.

## 9.12 NIC Resource Budget

### Per-Node Resource Consumption (v3, 3,500 Nodes, 8 QPs/Peer)

| Resource | Per peer | Total (3,499 peers) | NIC limit (CX-7) | Utilization |
|----------|---------|---------------------|-------------------|-------------|
| QPs | 8 | 27,992 | ~256K | 11% |
| CQs | 1-2 | 3,499-6,998 | ~64K | 5-11% |
| SRQ entries | 1,024 shared | 1,024 (one SRQ) | — | — |
| Memory Regions | 1 (per pool) | ~1 | ~256K | <1% |
| QP context cache | 8 × ~256B | ~7 MB | ~32 MB (on-chip) | 22% |

**QP context cache pressure**: Each QP context consumes ~256 bytes of on-chip SRAM. At 27,992 QPs, the total context is ~7 MB, within the CX-7's ~32 MB budget. Context cache misses cause the NIC to fetch QP state from host memory via PCIe, adding ~100-500ns per access. Keeping QP count low via multiplexing directly improves NIC cache hit rates.

**SRQ scaling**: With multiplexing, a single SRQ serves all streams to a peer. SRQ size should be tuned based on the expected peak concurrent streams, not the QP count:
- Conservative: `srq_max_wr = max(1024, expected_streams × 4)`
- Aggressive: `srq_max_wr = 4096` (shared across all streams, refilled on completion)

### Multi-Application Budget

| Scenario | Per-node QPs | CX-7 headroom |
|----------|-------------|----------------|
| 1 app, 8 QPs/peer, 3,499 peers | 27,992 | 89% free |
| 2 apps, 8 QPs/peer, 3,499 peers | 55,984 | 78% free |
| 4 apps, 8 QPs/peer, 3,499 peers | 111,968 | 56% free |
| 4 apps, 16 QPs/peer, 3,499 peers | 223,936 | 13% free |

At 4 applications with 16 QPs each, the CX-7 is at 87% utilization. This is the practical ceiling for a full-mesh 3,500-node deployment. Beyond this, either reduce QPs per peer (8 is sufficient for most workloads) or use hierarchical topologies (not all nodes need direct tunnels to all others).

## 9.13 Protocol Comparisons

| Feature | HTTP/2 | QUIC | SCTP | uds-rdma-proxy v3 |
|---------|--------|------|------|-------------------|
| **Transport** | TCP | UDP | IP (protocol 132) | RDMA RC QPs |
| **Streams per connection** | Unlimited (practical ~100) | Unlimited | Up to 65K | Up to ~2.15B (u32/2) |
| **Stream ID size** | 31 bits | 62 bits | 16 bits | 32 bits |
| **HOL blocking** | Yes (TCP-level) | No | No (with unordered) | No (per-stream reorder) |
| **Flow control layers** | Connection + stream | Connection + stream | Association + stream | QP credits + stream windows |
| **Stream initiation** | Client odd / server even | Client-initiated / server push | Either side | RDMA initiator odd / acceptor even |
| **Ordered delivery** | Per-stream | Per-stream | Per-stream | Per-stream |
| **Multi-path** | No | Connection migration | Multi-homing | Multi-QP ECMP |
| **Reliable delivery** | TCP guarantees | QUIC guarantees | SCTP guarantees | RC QP guarantees |

**Key differences from HTTP/2**: We avoid HTTP/2's fundamental HOL blocking problem because our "transport" (multiple RC QPs) provides multiple independent reliable channels. HTTP/2 suffers because all streams share a single TCP byte stream. We have per-stream sequence numbers and per-stream reorder buffers — structurally identical to QUIC's design.

**Key differences from QUIC**: QUIC handles loss detection and retransmission in software. We delegate reliable delivery to the RDMA RC transport (NIC hardware handles retransmission via Go-Back-N). Our reorder buffers handle only inter-QP reordering, not loss recovery.

## 9.14 Pump Architecture Changes

The per-connection pump model ([Section 7.1](07-bidirectional-pump.md#71-per-connection-task-model)) extends naturally to multiplexed streams:

### Send Path (Per-Stream)

Each stream still has its own `uds_to_rdma` task. The difference is that the task sends through the shared QP set rather than a dedicated one:

```
 Per-stream uds_to_rdma task:
   loop {
     buf = pool.alloc_tx()
     n = uring_read(uds_fd, buf)        // stream's UDS fd
     encode_header(buf, stream_id, n)
     seq = stream_state.next_sequence()  // per-stream sequence number
     // Stream scheduler decides if this stream can send now
     scheduler.wait_turn(stream_id)
     qp = selector.select_qp(seq)        // shared QP selector
     wait_credit(qp)                      // shared QP credits
     wait_stream_window(stream_id)        // per-stream window check
     post_send(qp, buf)
   }
```

### Receive Path (Shared Demuxer)

The CQ polling thread is shared across all streams. After receiving a frame, it demuxes to the correct stream:

```
 CQ poll thread (one per peer connection):
   loop {
     wc = poll_cq()
     frame = decode(wc.buf)
     match frame.stream_id {
       0 => handle_control(frame),                    // connection-level
       sid => {
         stream = streams.get(sid)
         stream.reorder_buffer.insert(frame)          // per-stream reorder
         while let Some(f) = stream.reorder_buffer.pop_ready() {
           stream.uds_write_channel.send(f)           // wake stream's write task
         }
       }
     }
   }
```

### Task Count

| Component | v2 (per-connection QPs) | v3 (multiplexed) |
|-----------|------------------------|-------------------|
| CQ poll threads | 1 per connection | 1 per peer |
| uds_to_rdma tasks | 1 per connection | 1 per stream |
| rdma_to_uds tasks | 1 per connection | 1 per stream |

The total task count is similar (still 2 tasks per UDS connection), but CQ poll threads are reduced to 1 per peer regardless of stream count.

## 9.15 Configuration

New and updated configuration keys for stream multiplexing:

```toml
[streams]
max_concurrent = 4096             # Max simultaneous streams per peer (0 = unlimited)
initial_window_size = 65536       # Per-stream receive window (bytes)
window_update_threshold = 0.25    # Grant window when this fraction consumed
syn_timeout = "5s"                # Timeout for SYN handshake completion
idle_stream_timeout = "30s"       # Close streams idle for this long (0 = disabled)
scheduling = "drr"                # "drr" (deficit round-robin) or "round-robin"
quantum = 4096                    # DRR quantum (bytes per scheduling round)

[proxy]
idle_peer_timeout = "60s"         # Tear down peer QP set after this idle period
peer_warmup_list = []             # Peer addresses to pre-establish QPs at startup
```

### Parameter Relationships

```
 initial_window_size >= buffer_size (must be at least one frame)
 max_concurrent × buffer_size <= available SRQ capacity (prevent SRQ exhaustion)
 quantum >= max_payload_size (at least one frame per scheduling round)
 syn_timeout < credit_stall_timeout (SYN should fail before credit stall triggers RST)
```

## 9.16 Metrics

New stream multiplexing metrics (prefix `uds_rdma_proxy_`):

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `streams_active` | gauge | `peer` | Currently active streams per peer |
| `streams_total` | counter | `peer`, `initiator` | Total streams created (`local`, `remote`) |
| `stream_syn_duration_seconds` | histogram | | Time from SYN sent to SYN received (handshake latency) |
| `stream_syn_timeouts_total` | counter | | SYN handshake timeouts |
| `stream_syn_rejects_total` | counter | | SYN rejected by peer (RST response) |
| `stream_duration_seconds` | histogram | | Stream lifetime (SYN to final FIN) |
| `stream_window_stalls_total` | counter | `stream_id` | Times a stream was blocked on window exhaustion |
| `stream_window_stall_duration_seconds` | histogram | | Duration of stream window stalls |
| `stream_window_updates_total` | counter | | Stream window update messages sent |
| `stream_scheduler_rounds_total` | counter | | DRR scheduling rounds |
| `peer_connections_active` | gauge | | Currently active peer QP sets |
| `peer_connections_total` | counter | | Total peer QP sets established |
| `peer_idle_teardowns_total` | counter | | Peer QP sets torn down due to idle timeout |

The existing `connections_active` / `connections_total` metrics ([Section 11.1](11-metrics.md)) continue to track UDS connections, which now map 1:1 to streams.


[Back to Design Overview](../DESIGN.md)
