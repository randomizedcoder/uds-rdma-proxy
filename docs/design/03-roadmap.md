# Phased Implementation Roadmap

Each phase builds on the previous, validating one layer at a time.

```
 v0 (TCP)          v1 (rsockets)       v2 (native ibverbs)     v3 (optimized)        v4 (one-sided)
 +----------+      +----------+        +----------+            +----------+           +----------+
 | UDS I/O  |      | UDS I/O  |        | UDS I/O  |            | UDS I/O  |           | UDS I/O  |
 | Framing  |      | Framing  |        | Framing  |            | Framing  |           | Framing  |
 | Metrics  |      | Metrics  |        | Metrics  |            | Metrics  |           | Metrics  |
 | Pump     |      | Pump     |        | Pump     |            | Pump     |           | Pump     |
 |          |      |          |        |          |            |          |           |          |
 | TCP sock |  ->  | rsocket  |   ->   | ibverbs  |   ->       | multi-QP |   ->      | RDMA     |
 | transport|      | transport|        | SEND/RECV|            | ECMP     |           | WRITE    |
 |          |      |          |        | credit FC|            | reorder  |           | ring buf |
 |          |      |          |        | buf pool |            | batching |           | Falcon   |
 +----------+      +----------+        +----------+            +----------+           +----------+
     |                  |                    |                       |
     |                  |                    |                       |
     validates:         validates:           validates:              validates:
     - architecture     - RDMA path          - flow control          - ECMP scaling
     - framing          - basic perf         - buffer mgmt           - reorder perf
     - metrics          - end-to-end         - CQ polling            - batching gains
     - pump logic                            - direct verbs
```

## Phase v0: UDS-TCP-UDS Baseline

**Goal**: Get the architecture right using familiar TCP transport.

- Bidirectional pump with io_uring on the UDS side
- Framing protocol implementation (encode/decode)
- Connection management (accept, teardown, half-close)
- Full Prometheus metrics pipeline
- Load generator tool
- **No RDMA dependency** -- runs anywhere, validates everything except the network transport

## Phase v1: UDS-rsockets-UDS

**Goal**: Fastest path to proving RDMA works end-to-end.

- Replace TCP with rsockets (`librspreload` or direct rsocket API)
- Minimal code changes from v0 (rsockets is a socket API replacement)
- First RDMA performance numbers
- **Limitation**: rsockets adds its own buffering layer; not zero-copy to the NIC

## Phase v2: Native Ibverbs Two-Sided SEND/RECV

**Goal**: The core engineering challenge -- direct RDMA verbs in Rust.

- QP setup via rdma_cm (RC -- Reliable Connected)
- Shared Receive Queue (SRQ) from the start for scalability
- Pre-registered buffer pool with huge page backing
- Credit-based flow control
- CQ polling (busy-poll, event-driven, adaptive)
- Single QP per connection
- This is where the real performance gains appear

## Phase v3: Multi-QP, ECMP, Multiplexing, Optimization

**Goal**: Scale beyond a single RDMA path and optimize throughput.

- Configurable QP count (1/8/16/32) for ECMP path diversity
- Sequence-numbered frames with B-tree reorder buffer
- Stream multiplexing (many UDS connections over fewer QPs)
- Send batching with configurable flush timeout
- Adaptive CQ polling (busy-poll under load, event-driven when idle)
- Huge pages, NUMA-aware buffer placement, CPU pinning

## Phase v4: One-Sided RDMA Ring Buffers (Future)

**Goal**: Eliminate receiver-side CPU involvement for posting receive WQEs.

- Replace two-sided SEND/RECV with one-sided RDMA WRITE into pre-shared ring buffers
- Producer-consumer ring with memory-mapped indices and atomic synchronization
- Falcon hardware reordering offload when available
- Significantly more complex but higher throughput ceiling


[Back to Design Overview](../DESIGN.md)
