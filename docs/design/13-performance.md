# Performance Optimization

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

## 13.1 The Copy Problem

The fundamental performance ceiling of this proxy is the **4 copies** per end-to-end message (2 at each UDS hop). There is no way to eliminate these within the UDS + RDMA proxy model without kernel modifications.

However, we can minimize everything else:

```
 Optimization target       Technique                        Expected impact
 ─────────────────────     ─────────────────────────────    ────────────────
 Syscall overhead          io_uring batching + SQPOLL       Eliminate per-I/O syscalls
 UDS copies                splice()/vmsplice() (exp.)       Reduce 4 copies to 2-3
 Buffer allocation         Pre-registered pool              Eliminate per-I/O alloc
 TLB misses                Huge pages (2MB)                 10-30% throughput improvement
 Memory registration       Single MR for entire pool        Eliminate per-I/O ibv_reg_mr
 CQ processing             Batched polling + adaptive       Reduce CPU per completion
 RDMA message rate         Adaptive (Nagle-like) batching   Fewer sends under high load
 Credit overhead           Piggybacking                     Eliminate most credit-only msgs
 Cross-NUMA traffic        NUMA-aware placement             Avoid remote memory access
 Context switches          CPU pinning + dedicated threads  Deterministic scheduling
 Small message overhead    Inline sends (< 64B)             Skip DMA for tiny payloads
 CQ overhead               Signaled every Nth send          Fewer CQEs to process
 Recv queue scaling        SRQ from v2 onward               Prevent per-QP RQ starvation
```

## 13.2 io_uring SQ Polling

With `IORING_SETUP_SQPOLL`, a kernel thread continuously polls the submission queue. The proxy can submit io_uring operations by simply writing to the SQ ring buffer -- no `io_uring_enter()` syscall needed.

```
 Without SQPOLL:
   User writes SQE to SQ  ->  io_uring_enter() syscall  ->  kernel processes SQE

 With SQPOLL:
   User writes SQE to SQ  ->  kernel poller thread picks up SQE  ->  no syscall!
```

Requires `CAP_SYS_NICE` or root. The kernel polling thread should be pinned to its own CPU core.

## 13.3 RDMA Send Batching

> For the conceptual framework on message sizing strategies (immediate vs. adaptive dispatch), prior art from RPC-over-RDMA and iWARP, and page alignment analysis, see [Section 3a.5](03a-stream-message-adaptation.md#3a5-message-sizing-strategies).

When the UDS produces many small writes in rapid succession, batching them into a single RDMA SEND reduces per-message overhead:

```
 Without batching:
   UDS read (100B) -> RDMA SEND (120B with header)
   UDS read (50B)  -> RDMA SEND (70B with header)
   UDS read (200B) -> RDMA SEND (220B with header)
   3 RDMA sends, 3 CQ entries

 With batching:
   UDS read (100B) -> buffer
   UDS read (50B)  -> buffer
   UDS read (200B) -> buffer
   Flush timeout (50us) fires
   -> RDMA SEND (370B with single header)
   1 RDMA send, 1 CQ entry
```

**Flush triggers**: Send the accumulated batch when either:
- Accumulated payload size reaches a threshold (e.g., buffer slot size minus header)
- A configurable timeout expires (e.g., 50us) since the first buffered read
- The application closes the connection (FIN)

**Trade-off**: Batching adds latency (up to the flush timeout) for the benefit of reduced message rate and CQ processing. It should be optional and configurable, with a sensible default timeout of 50us.

#### Adaptive (Nagle-like) Batching

A fixed flush timeout is suboptimal: under low load, even 50us of added latency is undesirable; under high load, longer batching windows improve CPU efficiency. Instead, implement **adaptive batching** inspired by Nagle's algorithm:

```
 Throughput (msgs/sec)     Flush Timeout (max_flush_timeout_us = 100)
 ─────────────────────     ──────────────────────────────────────────
 < 1,000                   0 (immediate send, no batching)
 1,000 - 10,000            max_flush_timeout_us * 25%  =  25us
 10,000 - 100,000          max_flush_timeout_us * 50%  =  50us
 > 100,000                 max_flush_timeout_us * 100% = 100us
```

The algorithm measures the moving average of message arrival rate (using an exponentially weighted moving average over the last 1ms window). As throughput increases, the flush timeout grows toward the configured ceiling (`max_flush_timeout_us`, default 100us). As throughput drops, the timeout shrinks toward zero, converging to immediate dispatch behavior.

**Why microseconds**: RDMA hardware RTT is ~1-2us. Even the default 100us ceiling adds 50-100x the wire latency. Milliseconds would be far too coarse — a 1ms flush timeout would dominate the end-to-end latency budget. The typical useful range is 50-200us; users with latency-critical workloads who still want some coalescing can set it as low as 10-25us.

The ceiling is configurable via `max_flush_timeout_us` in the `[batching]` section of the TOML config ([Section 10.2](10-configuration.md#102-configuration-file-toml)).

Additionally, apply a **size-triggered flush**: if the accumulated payload reaches 75% of the buffer slot size (`size_threshold_pct`), flush immediately regardless of the timer. This ensures large bursts are sent without waiting for the timer.

```rust
struct AdaptiveBatcher {
    accumulated: usize,
    first_arrival: Instant,
    msg_rate_ewma: f64,          // exponentially weighted moving average
    ewma_alpha: f64,             // smoothing factor (e.g., 0.1)
    max_flush_timeout: Duration, // from config: max_flush_timeout_us
    size_threshold_pct: u8,      // from config: size_threshold_pct
}

impl AdaptiveBatcher {
    fn flush_timeout(&self) -> Duration {
        let fraction = match self.msg_rate_ewma as u64 {
            0..=999       => 0.0,    // immediate
            1_000..=9_999 => 0.25,
            10_000..=99_999 => 0.50,
            _             => 1.0,    // full ceiling
        };
        self.max_flush_timeout.mul_f64(fraction)
    }

    fn should_flush(&self, buffer_capacity: usize) -> bool {
        let threshold = buffer_capacity * self.size_threshold_pct as usize / 100;
        self.accumulated >= threshold
            || self.first_arrival.elapsed() >= self.flush_timeout()
    }
}
```

**Metrics for adaptive batching**:
| Metric | Type | Description |
|--------|------|-------------|
| `batch_flush_reason` | counter (labels: `size`, `timeout`, `fin`) | Why each batch was flushed |
| `batch_size_messages` | histogram | Messages per batch |
| `batch_size_bytes` | histogram | Bytes per batch |
| `batch_latency_added_seconds` | histogram | Time between first message arrival and batch flush |

## 13.4 Signaled Completions

Not every RDMA send needs to generate a CQ entry. Using unsignaled sends (no `IBV_SEND_SIGNALED` flag) for most sends and signaling only every Nth send reduces CQ processing overhead:

```
 Send 1:  unsignaled  (no CQE)
 Send 2:  unsignaled  (no CQE)
 ...
 Send 16: SIGNALED    (generates CQE)
   -> All 16 sends complete atomically (in-order guarantee of RC)
   -> Free buffers for sends 1-16

 Result: 1 CQE per 16 sends instead of 16 CQEs
```

**Buffer management**: With unsignaled sends, you can't free the buffer immediately because there's no CQE to confirm completion. Track the batch: when the Nth (signaled) send completes, free all N buffers.

**Signal frequency**: Every 16 sends is a common choice. Must ensure `max_send_wr` is large enough to hold the unsignaled batch.

## 13.5 Inline Sends

For very small messages (under ~64 bytes), the NIC can read the payload directly from the Work Queue Element (WQE) instead of issuing a DMA read from the registered buffer:

```
 Normal send:
   WQE posted -> NIC reads WQE -> NIC issues DMA read from MR buffer -> NIC sends

 Inline send:
   WQE posted (payload embedded in WQE) -> NIC reads WQE -> NIC sends immediately
   (one fewer DMA operation)
```

The `max_inline_data` QP attribute controls the threshold. Set to 64 bytes. Messages larger than this use normal DMA.

## 13.6 NUMA-Aware Buffer Placement

On multi-socket servers, memory access latency depends on which NUMA node the memory is allocated from. Buffers should be allocated on the same NUMA node as the RDMA NIC:

```rust
// Determine RDMA device's NUMA node
let numa_node = fs::read_to_string(
    format!("/sys/class/infiniband/{}/device/numa_node", device_name)
)?.trim().parse::<i32>()?;

// Allocate buffers on that node
use libc::{mmap, MAP_HUGETLB, MPOL_BIND};
// Set memory policy to bind to the NIC's NUMA node
```

Also pin the CQ polling thread and io_uring threads to cores on the same NUMA node.

## 13.7 CPU Pinning Strategy

```
 Core 0: OS / misc
 Core 1: io_uring SQPOLL kernel thread
 Core 2: CQ polling thread (dedicated, busy-poll capable)
 Core 3: Async runtime (pump tasks, connection management)
 Core 4+: Application
```

The proxy should recommend at least 2-3 dedicated cores. The CQ polling thread is the most latency-sensitive and benefits most from dedicated pinning.

## 13.8 Zero-Copy UDS via splice()

While [Section 2.4](02-architecture.md#24-copy-analysis) identifies 4 copies as the minimum, `splice()` can potentially eliminate one copy on each UDS hop by keeping data in kernel page cache pages rather than copying to userspace:

```
 Standard path (2 copies per UDS hop):
   UDS socket -> kernel buf -> [copy to userspace] -> registered MR buf -> NIC DMA

 splice() path (1 copy per UDS hop):
   UDS socket -> kernel pipe buf -> [splice to registered MR] -> NIC DMA
```

The approach:
1. Create a kernel pipe (`pipe2()`)
2. `splice()` from the UDS socket fd into the pipe (moves data within kernel, no userspace copy)
3. `vmsplice()` from the pipe into the pre-registered MR buffer (maps the kernel pages into the registered buffer region)

```rust
// Conceptual splice path
let (pipe_rd, pipe_wr) = pipe2(O_NONBLOCK)?;

// Step 1: UDS -> pipe (kernel-to-kernel, no userspace copy)
let n = splice(uds_fd, None, pipe_wr, None, buf_size, SPLICE_F_NONBLOCK)?;

// Step 2: pipe -> registered buffer (kernel pages mapped to MR)
let iov = IoSlice::new(&mr_buffer[header_len..header_len + n]);
vmsplice(pipe_rd, &[iov], SPLICE_F_GIFT)?;
```

**Caveats**:
- `splice()` works with UDS `SOCK_STREAM` sockets on Linux, but `vmsplice()` with `SPLICE_F_GIFT` requires careful page lifetime management -- the kernel may reuse the pages after `vmsplice` returns, so the buffer must not be modified until the RDMA send CQE confirms completion.
- The registered MR buffer and the spliced pages must align on page boundaries. Buffer pool slots should be page-aligned (4KB-aligned for 4KB pages, or 2MB-aligned with huge pages).
- `splice()` cannot be used with io_uring's registered buffer path (`IORING_OP_READ_FIXED`). This is an alternative I/O path, not a complement. The proxy should support both modes and benchmark to determine which is faster for a given workload.
- The `SPLICE_F_GIFT` flag tells the kernel it can take ownership of the pages, but this interacts poorly with the buffer pool model where we want predictable buffer reuse. This path requires careful design and benchmarking.

**Recommendation**: Implement the standard dual-registered buffer path (io_uring + ibverbs) first. Add `splice()` as an experimental alternative in v3, behind a feature flag, with benchmark comparison. The theoretical benefit is reducing 4 copies to 2-3, but the practical gain depends on message sizes and pipe overhead.

## 13.9 Copy Count Summary

| Path | Copies | Notes |
|------|--------|-------|
| Standard (io_uring + ibverbs) | 4 end-to-end | 2 per UDS hop, baseline |
| With splice() on both hops | 2-3 end-to-end | Depends on vmsplice page reuse |
| Shared memory fast path (future) | 0-1 end-to-end | Requires application modification |


[Back to Design Overview](../DESIGN.md)
