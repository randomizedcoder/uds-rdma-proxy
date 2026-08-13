# Bidirectional Pump Architecture

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

## 7.1 Per-Connection Task Model

Each UDS connection spawns two concurrent tasks (or threads, depending on the async model):

```
 +--------------------------------------------------------------+
 |  Per-Connection State                                         |
 |                                                               |
 |  Shared: uds_fd, qp_set[], credit_counters[], buffer_pool    |
 |                                                               |
 |  +---------------------------+  +---------------------------+ |
 |  |   uds_to_rdma task        |  |   rdma_to_uds task        | |
 |  |                           |  |                           | |
 |  |   loop {                  |  |   loop {                  | |
 |  |     buf = pool.alloc_tx() |  |     cqe = poll_recv_cq() | |
 |  |     n = uring_read(buf)   |  |     frame = decode(buf)  | |
 |  |     if n == 0: send FIN   |  |     if FIN: half_close() | |
 |  |     encode_header(buf, n) |  |                           | |
 |  |     seq = next_seq()      |  |     // Reorder buffer     | |
 |  |     qp = select_qp(seq)  |  |     reorder.insert(frame) | |
 |  |     wait_credit(qp)      |  |     while reorder.ready() | |
 |  |     post_send(qp, buf)   |  |       f = reorder.pop()   | |
 |  |     credits[qp]--        |  |       uring_write(f.buf)  | |
 |  |   }                      |  |       pool.free_rx(f.buf) | |
 |  |                           |  |       grant_credits()     | |
 |  +---------------------------+  |     }                     | |
 |                                 |   }                       | |
 |                                 +---------------------------+ |
 +--------------------------------------------------------------+
```

The pump supports two **dispatch modes**, configurable via `dispatch_mode` in the TOML config:
- **Immediate** (`"immediate"`): Each UDS read is framed and sent as one RDMA message immediately. Preserves write boundaries. Minimum latency, higher message rate.
- **Adaptive** (`"adaptive"`, default): Nagle-like coalescing with an adaptive flush timer (configurable ceiling via `max_flush_timeout_us`, default 100us). Write boundaries are not preserved. Better throughput for small-write-heavy workloads.

Both modes use the same loop structure — the only difference is when the buffer is flushed to RDMA. See [Section 3a.5](03a-stream-message-adaptation.md#3a5-message-sizing-strategies) for the design rationale and trade-off analysis, and [Section 13.3](13-performance.md#133-rdma-send-batching) for the adaptive batching algorithm.

## 7.2 Half-Close Handling

UDS stream sockets support half-close: one side can close its write end while the other side continues sending. The proxy must propagate this correctly.

```
 App A closes write end
       |
       v
 uds_to_rdma reads EOF (n == 0)
       |
       v
 Send FIN frame (frame_type = DATA, flags = DATA_FLAG_FIN, payload_length = 0)
       |
       v
 uds_to_rdma task exits
       |
       |    (rdma_to_uds task continues receiving from peer)
       |
       v
 When peer also sends FIN:
   rdma_to_uds task receives FIN
   -> uds.shutdown(Write)
   -> rdma_to_uds task exits
   -> Connection fully closed, all resources freed
```

**Timeout**: If only one side sends FIN, the other direction has a configurable timeout (default: 30 seconds) before forced teardown. This prevents resource leaks from applications that never close their sockets.

## 7.3 Error Propagation

| Error Source | Action |
|-------------|--------|
| UDS read fails (not EOF) | Send RST frame to peer, close connection |
| UDS write fails | Send RST frame to peer, close connection |
| RDMA send fails (QP error) | **Multi-QP**: Move failed QP to Draining, notify peer via `QP_DISABLE` ([Section 8.11](08-multi-qp-ecmp.md#811-dynamic-working-set-management)), continue on remaining Active QPs. **Single-QP or last Active QP**: Close UDS socket, log error, tear down connection. |
| RDMA recv gets error WC | Same as RDMA send fails -- per-QP recovery if other QPs remain Active. |
| QP transitions to ERROR state | Same as RDMA send fails -- per-QP recovery if other QPs remain Active. |
| All QPs removed (none Active) | Send RST, close connection, full teardown |
| Credit stall timeout | Send RST, close connection (possible peer failure) |
| Probe failure (consecutive misses) | Move QP to Draining, notify peer. See [Section 8.9](08-multi-qp-ecmp.md#89-qp-health-state-machine). |

## 7.4 Backpressure

The system has natural backpressure through credit flow control:

```
 Fast UDS reader, slow network:
   UDS read fills buffers quickly
   -> post_send() consumes credits quickly
   -> credits exhausted (credits == 0)
   -> uds_to_rdma task blocks on wait_credit()
   -> proxy stops reading from UDS
   -> UDS kernel buffer fills up
   -> application's write() blocks or returns EAGAIN
   -> APPLICATION is backpressured

 Fast network, slow UDS writer:
   recv CQE fires, data available
   -> io_uring write to UDS queued
   -> UDS kernel buffer full, write blocks
   -> rdma_to_uds task stalls
   -> recv buffers not re-posted, credits not granted
   -> peer's send_credits don't replenish
   -> peer's uds_to_rdma task blocks
   -> REMOTE APPLICATION is backpressured
```

This end-to-end backpressure is critical for correctness: without it, the proxy would buffer unbounded data in memory.

**Multiplexed streams (v3)**: With stream multiplexing ([Section 9](09-connection-multiplexing.md)), backpressure operates at two levels. Per-QP credits prevent SRQ exhaustion across all streams. Per-stream receive windows ([Section 9.7](09-connection-multiplexing.md#97-two-layer-flow-control)) prevent a single slow stream from consuming all receive buffers and starving other streams. A stream window stall blocks only the affected stream — other streams on the same QP set continue sending.

## 7.5 Multiplexed Pump Architecture (v3)

With stream multiplexing, the pump model extends from per-connection to per-stream:

- **Send path**: Each stream retains its own `uds_to_rdma` task, but shares the QP set with all other streams to the same peer. A stream scheduler ([Section 9.8](09-connection-multiplexing.md#98-stream-scheduling-and-fairness)) determines which stream sends next when multiple have data ready.
- **Receive path**: A single CQ poll thread per peer decodes incoming frames and routes them by `stream_id` to per-stream reorder buffers and UDS write tasks.

The task count per UDS connection is unchanged (2 tasks). The reduction is in CQ poll threads (1 per peer instead of 1 per connection). See [Section 9.14](09-connection-multiplexing.md#914-pump-architecture-changes) for details.


[Back to Design Overview](../DESIGN.md)
