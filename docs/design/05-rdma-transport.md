# RDMA Transport Layer

> **Note (2026-08-11):** Written during the original userspace-proxy design
> era (2026-05). The protocol and architecture content below still describes
> the implemented wire behavior, but implementation specifics are
> userspace-flavored (Rust proxy process, userspace ibverbs) — the shipped
> implementation is the `urp` **kernel module** (see [DESIGN.md](../DESIGN.md)),
> and v0–v4 phase references follow the abandoned userspace roadmap (see the
> phase-numbering note in [KERNEL-MODULE-PLAN.md](../KERNEL-MODULE-PLAN.md)).

## 5.1 Connection Establishment

Connection setup uses the RDMA Connection Manager (`rdma_cm`), which provides a socket-like API for establishing RC (Reliable Connected) Queue Pairs.

```
 RDMA Acceptor                                    RDMA Initiator
 +------------------+                             +------------------+
 | rdma_create_id() |                             | rdma_create_id() |
 | rdma_bind_addr() |                             |                  |
 | rdma_listen()    |                             |                  |
 +--------+---------+                             +--------+---------+
          |                                                |
          |          <--- RDMA_CM_EVENT_CONNECT_REQUEST --- |
          |              (private_data: see below)          |
          v                                                |
 +------------------+                             +--------+---------+
 | rdma_create_qp() |                             | rdma_resolve_addr|
 | (create N QPs)   |                             | rdma_resolve_route|
 +--------+---------+                             | rdma_create_qp() |
          |                                       | (create N QPs)   |
          |                                       | rdma_connect()   |
          |          --- RDMA_CM_EVENT_ESTABLISHED -->      |
          v                                                v
 +------------------+                             +------------------+
 | Pre-post N recv  |                             | Pre-post N recv  |
 | WQEs per QP      |                             | WQEs per QP      |
 | Begin pumping    |                             | Begin pumping    |
 +------------------+                             +------------------+
```

**Terminology**: The side that calls `rdma_listen()` / `rdma_bind_addr()` is the **RDMA acceptor**. The side that calls `rdma_connect()` is the **RDMA initiator**. These terms describe RDMA connection directionality only and are independent of the proxy's UDS mode (listen, connect, or bidirectional). See [Section 2.1](02-architecture.md#21-end-to-end-system-overview) for how UDS and RDMA modes are configured orthogonally.

**Initial parameter exchange**: The `private_data` field of the connection request/response (up to 196 bytes for RC) carries:

| Field | Size | Description |
|-------|------|-------------|
| `num_qps` | 4B | Number of QPs to create (for multi-QP ECMP) |
| `initial_credits` | 4B | Initial credit count per QP |
| `buffer_size` | 4B | Buffer slot size |
| `max_payload` | 4B | Maximum payload size |
| `auth_method` | 1B | Authentication method: `0` = none, `1` = password (PSK), `2` = certificate (future) |
| `auth_hash` | 32B | SHA-256(password) when `auth_method=1`, zeroed when `auth_method=0` |
| **Total** | **49B** | Well within the 196-byte RC limit (147B remaining for future use) |

When `auth_method=1` (password), both sides include `SHA-256(password)` in their `private_data`. The RDMA acceptor verifies the initiator's hash before calling `rdma_accept()` — on mismatch, it calls `rdma_reject()` and logs the peer address. The RDMA initiator similarly verifies the acceptor's hash from the accept response. See [Section 17.5](17-security.md#175-tier-05-shared-password-psk) for details.

## 5.2 Queue Pair Configuration

Each QP is configured as **RC (Reliable Connected)** for guaranteed, in-order delivery within a single QP.

Key QP attributes:

| Attribute | Recommended Value | Rationale |
|-----------|------------------|-----------|
| `max_send_wr` | 256 | Depth of send queue; matches credit window |
| `max_recv_wr` | 256 | Depth of receive queue; matches pre-posted buffers |
| `max_send_sge` | 1 | Single scatter-gather entry (contiguous buffer) |
| `max_recv_sge` | 1 | Single scatter-gather entry |
| `max_inline_data` | 64 | Inline small messages in the WQE itself |
| `sq_sig_all` | false | Only signal every Nth send for CQ efficiency |

#### Shared Receive Queue (SRQ)

A Shared Receive Queue (SRQ) should be used **from v2 onward**, not deferred to v3. High-performance NICs have limited on-card cache for QP contexts, and per-QP receive queues consume this cache proportionally. With an SRQ, all QPs share a single pool of receive buffers:

```
 Without SRQ (per-QP RQ):                With SRQ:
 QP0: [recv buf] [recv buf] [recv buf]   QP0 --\
 QP1: [recv buf] [recv buf] [recv buf]   QP1 ---+--> SRQ: [buf] [buf] [buf] [buf] [buf]
 QP2: [recv buf] [recv buf] [recv buf]   QP2 --/
 = 9 buffers, 3 per QP                   = 5 buffers shared across all QPs
```

Benefits:
- **Memory efficiency**: With 100 connections at 256 recv buffers each, that's 25,600 buffers without SRQ vs. a single shared pool of ~1,024 buffers with SRQ.
- **Prevents Receive Queue Starvation**: Without SRQ, if one QP is hammered with traffic while others are idle, that QP can exhaust its private receive buffers while the idle QPs' buffers sit unused. SRQ dynamically allocates from the shared pool based on demand.
- **NIC cache pressure**: Fewer total QP contexts to cache on the NIC, improving NIC-level performance.

SRQ configuration:
| Attribute | Recommended Value | Rationale |
|-----------|------------------|-----------|
| `max_wr` | 1024 | Shared across all QPs; larger than any single QP would need |
| `max_sge` | 1 | Single scatter-gather entry per receive |

The credit-based flow control must account for SRQ: credits are still tracked per-QP (because the sender needs to know *which QP* has capacity), but the underlying receive buffers come from the shared pool.

## 5.3 Credit-Based Flow Control

Each side pre-posts `N` receive Work Queue Elements (WQEs). A sender can only issue a SEND when it knows the peer has a receive WQE available. Without flow control, a SEND to a peer with no posted receives causes a fatal QP error (RNR -- Receiver Not Ready).

#### Flow Control State Machine

```
 SENDER STATE                                      RECEIVER STATE
 +------------------+                              +------------------+
 | send_credits = N |  <-- initial exchange -->    | recv_posted = N  |
 | (per QP)         |                              | credits_to_grant |
 +--------+---------+                              | = 0 (per QP)    |
          |                                        +--------+---------+
          v                                                 |
 +------------------+                                       v
 | Want to send?    |                              +------------------+
 |                  |                              | Recv CQE fires   |
 | if credits > 0:  |                              |                  |
 |   send frame     |                              | Process payload  |
 |   credits--      |                              | Re-post recv WQE |
 |                  |                              | credits_to_grant++|
 | if credits == 0: |                              |                  |
 |   WAIT (stall)   |                              | if credits_to_grant|
 |   metric++       |                              |    >= threshold: |
 +--------+---------+                              |   piggyback on   |
          |                                        |   next data frame|
          |                                        |   OR send CREDIT |
          |    <-- credits_granted in frame hdr --  |   frame          |
          |                                        +------------------+
          v
 +------------------+
 | credits +=       |
 |  granted amount  |
 | Resume sending   |
 +------------------+
```

**Key design decisions**:

- **Credit threshold**: Grant credits when `credits_to_grant >= N/4`. This batches credit grants to reduce message overhead while keeping the pipeline flowing. The threshold is configurable.
- **Deadlock avoidance**: Always reserve at least 1 receive buffer for credit-only messages. If the receiver's application processing stalls, the sender needs to be able to receive a credit grant to resume sending.
- **Per-QP credits**: In multi-QP mode, each QP tracks its own credits independently. A stalled QP does not block sends on other QPs.
- **Piggybacking**: The `credits_granted` field in the frame header allows credits to be granted alongside data, avoiding separate credit-only messages in the common case.

## 5.4 Registered Buffer Pool

Memory registration (`ibv_reg_mr()`) is expensive -- it pins physical pages and programs the NIC's translation table. Registering per-message would destroy performance. Instead, we pre-allocate and register a buffer pool at startup.

```
 +------------------------------------------------------------------+
 | Buffer Pool (single MR registration)                             |
 |                                                                  |
 | +--------+--------+--------+--------+--------+--------+---      |
 | | Slot 0 | Slot 1 | Slot 2 | Slot 3 | Slot 4 | Slot 5 | ...    |
 | | 4KB    | 4KB    | 4KB    | 4KB    | 4KB    | 4KB    |         |
 | +--------+--------+--------+--------+--------+--------+---      |
 |                                                                  |
 | Backed by: mmap(MAP_HUGETLB) for 2MB huge pages                 |
 | Registered: ibv_reg_mr(pd, base, total_size, access_flags)      |
 | Free list: crossbeam::ArrayQueue<u32> (lock-free, slot indices)  |
 +------------------------------------------------------------------+

 Allocation: slot_idx = free_list.pop()  -> O(1)
 Dealloc:    free_list.push(slot_idx)    -> O(1)
```

**Pool configuration**:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `buffer_count` | 256 per direction | Number of buffer slots (TX and RX pools are separate) |
| `buffer_size` | 4,096 bytes | Size of each slot (frame header + max payload) |
| `huge_pages` | true | Use 2MB huge pages to reduce TLB misses |

**Why separate TX and RX pools**: TX buffers are managed by the sender (allocated on UDS read, freed on send CQE). RX buffers are managed by the receiver (allocated on recv post, freed after UDS write completes). Separate pools avoid contention between the two directions.

**Dual registration**: The same physical buffers are registered with both `ibv_reg_mr()` (for RDMA) and `IORING_REGISTER_BUFFERS` (for io_uring). This eliminates the copy between io_uring and RDMA buffers -- the UDS read lands directly in a buffer that the NIC can DMA from.

> **Kernel module variant**: The kernel module alternative ([Section 21](21-kernel-module.md)) replaces this userspace buffer pool with the kernel's `page_pool` API ([Section 21.9](21-kernel-module.md#219-comparison-with-nic-driver-architecture)). `page_pool` provides the same pre-allocated, reusable DMA-mapped buffer pool but with kernel-native per-CPU caching (lock-free allocation), DMA map persistence across recycles (no per-use `ib_dma_map_page` overhead), and refcount-based lifecycle. No dual registration is needed — there is no io_uring layer in kernel space.

## 5.5 Completion Queue Polling

The CQ is where the proxy learns that RDMA operations have completed. The polling strategy directly impacts latency and CPU usage.

#### Busy-Poll Mode

```rust
// Dedicated thread, pinned to a CPU core
loop {
    let n = ibv_poll_cq(cq, batch_size, &mut wc_array);
    if n > 0 {
        for wc in &wc_array[..n] {
            process_completion(wc);
        }
    }
    // No sleep, no yield -- tight spin
}
```

- **Latency**: Lowest possible (sub-microsecond CQE processing)
- **CPU**: Burns an entire core continuously
- **When to use**: Maximum throughput scenarios, dedicated hardware

#### Event-Driven Mode

```rust
loop {
    ibv_req_notify_cq(cq, solicited_only=0);
    ibv_get_cq_event(cq_channel, &cq, &ctx);  // blocks until CQ event
    ibv_ack_cq_events(cq, 1);

    loop {
        let n = ibv_poll_cq(cq, batch_size, &mut wc_array);
        if n == 0 { break; }
        for wc in &wc_array[..n] {
            process_completion(wc);
        }
    }
}
```

- **Latency**: Higher (interrupt delivery + context switch, typically 5-20us added)
- **CPU**: Minimal when idle
- **When to use**: Low-throughput connections, mixed workloads

#### Adaptive Mode (Recommended)

```
 +-------------+     load > high_threshold     +-------------+
 |  Event-     | -------------------------------->|  Busy-     |
 |  Driven     |                                  |  Poll      |
 |  Mode       | <--------------------------------|  Mode      |
 +-------------+     idle > idle_timeout       +-------------+
                      (e.g., 100us no CQEs)
```

Start in event-driven mode. When the CQ completion rate exceeds a threshold (e.g., >1000 completions/second), switch to busy-polling. When busy-poll finds no completions for a configurable idle timeout (e.g., 100us), switch back to event-driven.

**Implementation**: The CQ polling runs on a dedicated thread (not on the async runtime). It communicates with the pump tasks via lock-free channels (crossbeam or tokio mpsc) or by directly waking async task wakers.


[Back to Design Overview](../DESIGN.md)
