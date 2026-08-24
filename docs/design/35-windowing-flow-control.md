# 35. Windowing & Flow Control (toward line rate)

Status: **Design only.** No code changes land with this doc. This is the
implementation-ready spec for the byte-based windowing that [design 34](34-bulk-throughput.md)
§34.6 sketched, plus the pump and scale-out work it must be co-designed with. It
supersedes §34.6 for implementation detail; §34.6 remains the summary.

## 35.1 What windowing is for — and what it is *not* for

Design 34's Phase-0 measurement (§34.5.1) put one-way bulk goodput at **~1.7 % of
the 25 Gb/s link** and diagnosed the cause as **post/serialization-bound**: a
single TX kthread at ~1000 frames/s for 64 KB frames, with the 1 ms pool-empty
poll and per-frame signaled completions as the prime suspects. It also found two
flow-control pathologies:

1. **No backpressure.** Best-effort credits (the sender *sends anyway* at 0 —
   `kernel/urp_pump.c:279-280`) let the source flood ~2849 MB/s into the UDS to
   deliver ~65 MB/s; the excess — **including the header-only FIN** — is dropped,
   so bulk transfers *flakily never complete*.
2. **The wasted flood.** Nearly all of that 2849 MB/s of encode/post work is
   thrown away; the sender's *useful* rate is a tiny fraction of its *attempted*
   rate.

Windowing fixes **both** — it is the difference between "bulk transfer sometimes
hangs" and "always completes," and it stops the sender doing ~40× useless work.

**But windowing does not, by itself, raise the peak.** This must be stated
plainly because it is counter-intuitive:

> **Bandwidth-delay product at this link is tiny.** 25 GbE = 3.125 GB/s; the
> measured per-QP RTT is **~37 µs** (`rtt_ewma_ns = 36896`, `urp_rdma.c:610`). So
> **BDP ≈ 3.125 GB/s × 37 µs ≈ 116 KB** — under **two** max-size (64 KB) frames.
> The current send pool is already **512 buffers ≈ 8 MB in flight (~70× BDP)**.
> The pipe is not under-filled for lack of window; it is under-*driven* for lack
> of frame rate.

So the path to line rate is: **Option B (pump) supplies the raw frame rate;
Option F2 (scale-out) multiplies it across cores; windowing (this doc) makes both
correct and waste-free by capping in-flight at ≈ BDP instead of flooding.** The
three are co-designed here because the windowing waitqueue and the pump's
completion signal are the *same* mechanism (§35.5).

## 35.2 The current mechanism (what we are replacing)

Per-QP **and** per-stream `struct urp_credit` (`kernel/urp_credit.h:19-24`):
`send_credits`, `credits_to_grant`, `threshold = initial/4`, `initial_credits`,
all **`u16` frame counts**. Per stream it is seeded to `ep->num_bufs / 2`
(`urp_stream.c:163`), i.e. the send-pool size.

- **Sender** (`urp_pump.c:277-281`): `urp_credit_consume` decrements; at 0 it
  returns `-EAGAIN`, the pump bumps `credit_stalls` and **posts the frame
  anyway**. Credits are a *stat*, not a gate.
- **Receiver** (`urp_rdma.c:698-717`): every DATA frame bumps `credits_to_grant`
  (`urp_credit_record_recv`); at `≥ threshold` it emits a CONTROL/CREDIT frame
  (`urp_emit_credit_frame`, `urp_pump.c:489`) carrying a **`u16`** count in the
  header field `credits_granted [14..16)` (`urp_frame.h:33`).
- **Sender applies grant** (`urp_rdma.c:562-590`): `urp_credit_grant` saturating-adds
  to `send_credits`, routed per-stream (`stream_id != 0`) or per-QP (k0).

Two properties make this inadequate as a window:

- It **never blocks**, so it does not bound in-flight — the *real* backstop today
  is send-pool exhaustion (`urp_buf_alloc_send` returns NULL → the **1 ms poll**,
  `urp_pump.c:229-232`) plus `rnr_retry_count = 7`.
- It counts **frames, not bytes**, and grants are **incremental `u16`**, so a lost
  CREDIT frame permanently shrinks the window (the split-brain hazard design 32's
  credit-routing fix already had to work around, `urp_credit_plan.h`).

The send pool itself already behaves as an ~8 MB frame-window; we are not adding a
window so much as **making the window byte-accurate, blocking, self-healing, and
sized to BDP instead of to the buffer pool.**

## 35.3 The byte-based windowing protocol

Per-stream state replaces the four `u16` frame counters with byte accounting
(new fields on `struct urp_stream`; the `urp_credit` struct stays for the k0
legacy path):

```
u64 window_bytes;      /* max unacknowledged bytes in flight (the limit)     */
u64 tx_bytes_sent;     /* cumulative bytes handed to ib_post_send            */
u64 tx_bytes_acked;    /* cumulative bytes the peer confirms delivered       */
/* in_flight = tx_bytes_sent - tx_bytes_acked                                */
wait_queue_head_t credit_wq;   /* sender blocks here; RX grant + CQE wake it */
```

### Sender gate (replaces the best-effort consume at `urp_pump.c:277-281`)

```c
avail = window_bytes - (tx_bytes_sent - tx_bytes_acked);
if (len > avail) {
    /* Block until a grant refills the window, or the stream is ending. */
    wait_event_interruptible(stream->credit_wq,
        (window_bytes - (READ_ONCE(stream->tx_bytes_sent)
                         - READ_ONCE(stream->tx_bytes_acked))) >= len
        || kthread_should_stop() || READ_ONCE(stream->fin));
    if (kthread_should_stop() || stream->fin) break;
}
tx_bytes_sent += len;       /* single writer: the TX kthread */
/* ... encode + post ... */
```

This is the whole behavioural change on the send side: **a real block** instead
of send-anyway. It removes the flood, the RNR storm, and the FIN-drop
non-completion in one move. Because the TX kthread is the sole writer of
`tx_bytes_sent` and reads `tx_bytes_acked` written by the RX path, the gate needs
only `READ_ONCE`/`WRITE_ONCE` and a wakeup — no lock (the existing single-writer
discipline, `urp_credit.c` header comment).

### Receiver grant (extends `urp_rdma.c:698-717`)

After each successful `urp_rx_send_uds` delivers `len` to the local UDS, advance a
per-stream **cumulative absolute** `rx_bytes_delivered += len`. Piggyback that
**absolute** value back to the sender, batched to avoid a control frame per data
frame: emit when `rx_bytes_delivered - last_granted ≥ window_bytes / 4` (mirrors
today's `threshold = initial/4`). The sender applies:

```c
tx_bytes_acked = max(tx_bytes_acked, granted_absolute);   /* RX path is sole writer */
wake_up_interruptible(&stream->credit_wq);
```

**Why cumulative/absolute + `max()`:** a lost or reordered grant cannot
permanently shrink the window — the next grant carries the true running total and
`max()` makes duplicates/reorderings idempotent. This is the same self-healing
discipline as design 32's credit-routing fix, now at byte granularity.

### Wire format — a new CONTROL sub-type (interop-gated)

The header's grant field is **`u16` `credits_granted [14..16)`** — it cannot carry
a `u64` cumulative byte count. Options considered:

- *Reuse the u16 as a byte-delta* — rejected: deltas are not self-healing (35.3)
  and 64 KB > 65535 so a single frame can exceed one delta unit.
- **Chosen: a new CONTROL/CREDIT-BYTES sub-type** that carries the `u64`
  `rx_bytes_delivered` in the frame **payload** (CONTROL frames already carry a
  payload region; `urp_emit_credit_frame` today posts header-only). Add a flag
  bit alongside `URP_CTRL_FLAG_CREDIT` (e.g. `URP_CTRL_FLAG_CREDIT_BYTES`) so a
  peer that doesn't understand it ignores the frame (it still reposts —
  `urp_rdma.c:562` default `goto repost`).

**Interop / versioning.** Byte-windowing is only safe when *both* peers speak it —
a byte-gated sender talking to a frame-credit receiver would block forever (no
byte-grants arrive). Gate it on a capability exchanged at connect: extend the
existing connection handshake (design 33 bring-up, `urp_rdma.c` CM established
path) with a version/feature word; enable byte-windowing only when both sides
advertise it, else fall back to today's frame credits. The bench twins
(`tools/urp-bench-core.*`, `crates/urp-bench/`) already round-trip the frame
header via shared hex vectors + the differential fuzzer — the new CONTROL
sub-type gets the same treatment so C and Rust stay byte-identical.

### Sizing

```
window_bytes = clamp(sysctl urp.window_bytes, WINDOW_MIN, WINDOW_MAX)
default      = clamp(BDP × factor, WINDOW_MIN, WINDOW_MAX)
BDP          = per-QP rtt_ewma_ns (urp_rdma.c:610) × link_rate
factor       ≈ 8–16   (headroom for scheduling jitter / batch bursts)
```

At 37 µs RTT the default lands ~1 MB (≫ the 116 KB BDP, ≪ the old 8 MB flood) —
deep enough to never idle the pipe, shallow enough that loss doesn't cost a
flood. A **new sysctl** in `kernel/urp_sysctl.c` (which today exposes only the
three connect-retry knobs, `urp_sysctl.c:42-70`) makes it live-tunable; follow the
exact `register_sysctl("urp", …)` pattern there.

### Mandatory reorder coupling

The per-stream reorder buffer is **hard-coded to 256 frames**
(`urp_reorder_alloc(0, 256)`, `urp_stream.c:169`). A byte window must never admit
more in-flight frames than reorder can hold, or the receiver drops in-window
frames (the qps=4 failure mode measured in §34.5.1: 1.37 M `reorder_drops`). Size
it from the window:

```
max_buffered = ceil(window_bytes / min_frame_payload)
s->reorder   = urp_reorder_alloc(0, max_buffered);
```

With 64 KB frames a 1 MB window is only ~16 frames — trivially safe; the formula
guards the small-frame case (a 1 MB window at 4 KB is ~256 frames, exactly today's
cap, which is why 256 was "enough" until multi-QP skew — §34.5.1 finding 2).

## 35.4 Where the speed actually comes from — coupling to the pump (Option B)

Windowing tells the pump *how much* to keep in flight; the pump must be fast
enough to keep that much moving. The single change that unifies them:

> **Replace the 1 ms pool-empty poll with a completion waitqueue.** Today
> `urp_buf_alloc_send` → NULL → `schedule_timeout_interruptible(1 ms)`
> (`urp_pump.c:229-232`); `urp_send_done` (`urp_rdma.c:404-419`) silently returns
> the buffer. Instead, have `urp_send_done` **`wake_up`** the TX kthread, and have
> the kthread `wait_event` on "a send buffer is free **or** the credit window has
> room." That is the *same* `credit_wq` the sender gate blocks on — grants and
> completions both wake it. This removes the ~1 ms stall that pins large-frame
> throughput near 1000 fps.

Three more pump changes compound it (design 13, still unbuilt):

1. **Selective signaling** — signal every Nth send (`IB_SEND_SIGNALED` only on
   1-in-N, `urp_pump.c:44`), reclaim the batch in `urp_send_done`. Cuts CQE
   traffic ~N×; the window's in-flight bound guarantees ≥ N sends outstanding so
   the batch never self-stalls.
2. **Multi-WR posts** — build a linked `ib_send_wr` list and post many frames per
   `ib_post_send` doorbell instead of one call per frame (`urp_post_frame`,
   `urp_pump.c:28-50`).
3. **Coalesced reads** — loop `kernel_recvmsg` to fill a large frame from many
   small UDS writes (`urp_pump.c:236-238`), so bytes-per-frame (already the
   strongest lever, §34.5.1) rises without the app having to write big.

**Budget.** A 64 KB frame copy at ~10 GB/s memory bandwidth is ~6.5 µs; if
per-frame overhead (encode + a share of a batched doorbell + a share of a batched
CQE) is held well under that, a single optimized kthread can plausibly reach
**several GB/s** for large frames — already a large fraction of the 3.125 GB/s
line. That is the Option-B target; windowing is what keeps it from re-flooding.

## 35.5 Scale-out (Option F2) — the last mile to line rate

One kthread, even optimized, is one core. **F2 stripes one logical transfer
across N independent streams**, each with its own TX kthread, QP, reorder buffer,
and byte window — no shared ordering, no cross-stream sequence races (unlike F1,
which multiplies TX threads on *one* ordered stream and fights `tx_seq`). This
maps naturally onto Redpanda/Kafka: partitions are already independent streams.
Scaling is near-linear until the NIC or PCIe is the wall, so **Option B's
per-stream GB/s × 4–8 streams → line rate.** It reuses the existing multistream
machinery (per-stream kthread/reorder/credit already exist); the byte window
designed here is per-stream, so F2 needs no new flow-control — it just runs N
copies.

**This also fixes the qps>1 collapse** (§34.5.1 finding 2): don't stripe one
ordered stream across QPs (arrival skew overflows reorder); give each stream its
own QP.

## 35.6 The line-rate budget (why B + C + F2 gets close)

| stage | mechanism | single-stream 64 KB goodput (est.) | % of 25 GbE |
|-------|-----------|-----------------------------------|-------------|
| today | serial pump, 1 ms poll, flood | ~53 MB/s (measured) | ~1.7 % |
| + B   | completion wq, selective signal, multi-WR, coalesce | several GB/s | tens of % |
| + C   | byte window (this doc) | same peak, now **reliable + waste-free** | — |
| + F2  | 4–8 streams × B, one QP each | → NIC-bound | **~line rate** |
| + D/E | one-sided WRITE / zero-copy io_uring | removes residual copies | line rate, lower CPU |

C does not add a row of its own throughput — it is the correctness/efficiency
layer that makes B's peak *sustainable* and B+F2's aggregate *not collapse* into
reorder-drops. D/E (design 31) remain the endgame once copies, not frame rate,
are the measured wall.

## 35.7 Phasing & verification

Phasing (each measured by the `urp-bench --pattern stream` harness + `urp-bw-matrix`
runner from design 34, which already report goodput in MB/s + Mb/s + % of 25 GbE,
`reorder_drops`, `credit_stalls`, and delivered `rx-frames`):

1. **Pump completion waitqueue** (Option B core) — kill the 1 ms poll; expect the
   large-frame frame rate to jump well above ~1000 fps. *No wire change.*
   **DONE (2026-08-18) — see §35.7.1;** poll floor removed (fresh 4 KB hit 3631
   fps), but the win is regime-specific and it exposed the flood/drop instability
   that argues for reordering C ahead of the rest of B.
2. **Selective signaling + multi-WR + coalesced reads** — the rest of B. *No wire
   change.*
3. **Byte windowing** (this doc §35.3) — the blocking gate, cumulative-absolute
   grants, the new CONTROL sub-type + capability gate, the sysctl, the reorder
   coupling. *Wire change → interop-gated.* Success = bulk transfers **always
   complete** (no FIN-drop hangs) and `credit_stalls`/`reorder_drops` stay ~0
   under sustained load. **DONE (2026-08-24) — see §35.7.2;** the blocking gate
   took the multi-QP flood to 0 drops and made the reorder-matrix 6/9 GREEN (all
   realistic frame sizes across `num_qps ∈ {1,4,8}`). Motivated as status.md
   gap #6 Problem B; shipped as PR #60 (connect race) + PR #61 (wire, default-off)
   + PR3 (gate, behaviour-on).
4. **Scale-out F2** — N streams, one QP each; aggregate toward line rate.

Verification gates:

- **KUnit** for the byte-accounting core (mirror the existing `urp_credit`
  KUnit/Rust diff, `kernel/urp_test.c`): sender-gate arithmetic, cumulative-absolute
  `max()` idempotence under duplicate/reordered/lost grants, reorder-sizing formula.
- **Differential fuzzer** (`crates/urp-bench` ↔ `tools/urp-bench-core`) extended to
  the new CONTROL sub-type so C and Rust stay byte-identical on the wire.
- **`nix run .#ci-local` green** (11 build targets + fuzz-smoke).
- **Hardware** (hp1↔hp3): each phase re-runs the design-34 sweep; the phase-3 gate
  is *reliable completion at 0 drops*, and the phase-4 gate is *aggregate goodput
  as a % of 25 GbE and of `ib_write_bw`* once that baseline is available.

### 35.7.1 Phase 1 measured results (2026-08-18)

Phase 1 (the completion waitqueue, §35.4's headline change) is implemented and
hardware-verified on hp1 (sink) ↔ hp3 (source), 25 GbE RoCEv2, `qps=1`, 10 s per
point, sink-measured goodput, `reorder_drops=0` throughout. Two runs (buffer-size
order varied to decouple the endpoint-churn wedge of §34.5.1):

| frame | Phase 0 (poll) | Phase 1 (waitqueue) | notes |
|-------|----------------|---------------------|-------|
| 65516 | ~980 fps / 52.7 MB/s | 981–990 fps / 51.2–51.3 MB/s | flat |
| 16384 | ~900–1240 fps / 9.4–10.9 MB/s | 704–708 fps / 9.2–9.3 MB/s | stable |
| 4096  | ~900–1240 fps / 3.7–9.7 MB/s | **3631 fps / 12.4 MB/s** (fresh) → 1085 fps / 1.9 MB/s (churned) | high variance |

**Verdict — Phase 1 does its scoped job and no more:**

1. **The 1 ms poll floor is removed — proven.** A fresh 4 KB run reached **3631
   fps**, physically impossible under the old design (a 1 ms sleep on every
   pool-empty caps iteration near ~1000/s). The prediction in §35.7 step 1 ("jump
   well above ~1000 fps") holds — but *only* where the pump was actually idling on
   an empty pool.
2. **It does not raise sustained goodput by itself.** Large frames (64 KB) are
   flat at ~51 MB/s: at that size the pump rarely sits pool-empty, so the poll was
   never the limiter. The large-frame wall is per-frame copy (`cpu_us_per_msg`
   16.98 @64 KB vs 2.23 @4 KB) + single-kthread serialization + per-frame CQE —
   i.e. §35.4 steps 1–3 (selective signaling, multi-WR, coalesced reads) + §35.5
   (F2), not the poll.
3. **Removing the poll exposed the flood/drop instability underneath.** The 4 KB
   point swings 1.9 ↔ 12.4 MB/s purely on endpoint freshness, with `credit_stalls`
   384–762 and ~3 % of frames dropped (reassembled 96.6 %). Without real windowing
   the source floods, credits stall, buffers churn, delivered goodput collapses.

**Consequence for phasing:** finding 3 strengthens the §35.1 correctness argument
and suggests **pulling Option C (byte windowing, phase 3) ahead of the rest of
Option B (phase 2)** — the missing flow control, not the large-frame copy wall, is
now the dominant source of instability and small-frame goodput loss. The large-frame
copy/serialization work (phase 2) and F2 (phase 4) remain the levers for raw peak,
but they are pointless until the flood/drop is fenced by C.

### 35.7.2 Phase 3 (byte windowing) measured results (2026-08-24)

Phase 3 — the blocking sender gate (§35.3), cumulative-absolute CREDIT-BYTES grants,
the 5-byte capability trailer + `urp.window_bytes`/`urp.window_bytes_advertise`
sysctls, and the window→reorder-depth coupling — is implemented and hardware-verified
on hp1 (sink) ↔ hp3 (source) over 25 GbE RoCEv2, driven by `.#urp-reorder-matrix`
(sweeps `buffer_size ∈ {64, 4096, 65516}` × `num_qps ∈ {1, 4, 8}`, `verify=full`).

**Result: reorder-matrix 9/9 GREEN** (68 / 4096 / 65516 × `num_qps ∈ {1,4,8}`).
Every cell passes `BENCH_OK verify=full` with `reorder_drops=0`,
`buffer_alloc_fails=0`, no dmesg WARN, and no deadlock. The byte window took the
multi-QP flood from **millions of `reorder_drops` → 0** and gave ~150× delivered
throughput on the cells that previously wedged.

Two additional hardware-only bugs surfaced under sustained multi-QP load and were
fixed as part of this phase (both required for the GREEN result):

1. **SYN-race** (`urp_stream_rx_dispatch`): under multi-QP striping a DATA frame
   routinely arrives at the acceptor *before* its stream's SYN (they ride different
   QPs and complete out of order). The old code returned `-ENOENT` and **dropped**
   the frame → a permanent reorder gap the windowed sender cannot refill (no
   retransmit) → hard deadlock (`rx_delivered=1`, "window stalled" every 5 s).
   Fixed by creating the stream implicitly on the first frame regardless of SYN
   (SYN is advisory; the reorder buffer keyed by seq delivers seq 0 first; a real
   SYN is idempotent). Diagnosed via per-second `urp stats` sampling on both boxes.
2. **Reorder depth for tiny frames** (`urp_window.h`): a 64-byte `buffer_size`
   yields ~44-byte payloads, so `window/64=16384` under-sized the reorder cap below
   the ~24000 frames a 1 MiB window admits → drops. `URP_REORDER_MIN_FRAME` lowered
   64→16 so the cap (`window/16=65536`) covers any realistic frame.

A third, **pre-existing** bug surfaced at the smallest sweep size and was fixed to
reach 9/9: a QP-health **PONG is 68 bytes** (`URP_FRAME_HEADER_SIZE 20 +
URP_PONG_PAYLOAD_SIZE 48`) but `buffer_size=64` posts 64-byte recv buffers, so the
first PONG overran the buffer → ib `local length error` → NAK → `remote invalid
request` → QP crash-loop. It reproduced with windowing OFF (`advertise=0`) and per-QP
(so it failed even at `num_qps=1`) — orthogonal to the windowing work. Fixed by
raising `URP_BUFFER_SIZE_MIN` to `URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE`
(= 68): the netlink policy now rejects a sub-68 `buffer_size` with ERANGE instead of
letting it crash-loop. The matrix's smallest sweep size moved 64 → 68 accordingly.

## 35.8 Risks

- **Deadlock on the blocking gate.** A sender blocked in `wait_event` must wake on
  every terminal event: grant, `kthread_should_stop`, FIN/RST, and QP
  teardown/drain. Miss one and a stream hangs on shutdown. The existing teardown
  ordering (the KASAN drain-order fix, [[kasan-uaf-buf-free-send-teardown]]) is the
  reference for what must fence.
- **Grant starvation vs. batching.** Batching grants at `window_bytes/4` bounds
  control traffic but must never let the sender stall with the receiver idle;
  the cumulative-absolute scheme plus a "grant on FIN / on idle" flush covers the
  tail (the same tail that drops the FIN today).
- **Interop half-upgrade.** A byte-gated sender must never block waiting for a
  peer that only speaks frame credits — the capability gate (§35.3) is
  load-bearing; default off until both peers advertise support.
- **Reorder/window mis-coupling.** If the sysctl raises `window_bytes` without
  re-sizing reorder, small frames overflow it (the measured qps=4 failure). The
  coupling formula must run on every window change, not just at stream create.

## 35.9 Relation to other docs

- [design 36](36-congestion-control-cubic.md) — **EXPERIMENTAL** congestion-control
  layer. This doc's `window_bytes` is the *flow-control* window (**rwnd**); design 36
  adds a loss-based CUBIC congestion window (**cwnd**) on top, so the sender gate's
  limit becomes `min(cwnd, rwnd)`. Design 36 is design-only and default-off; it only
  *binds* after §35.4 (pump, done) + §35.5 (F2) make us congestion-bound.
- [design 34](34-bulk-throughput.md) — the measurement that motivates this; §34.6
  is the summary this doc expands. The option ladder (§34.3) names B/C/D/E/F.
- [design 33](33-connection-bringup.md) — the connection handshake this doc
  extends with a capability word for interop gating.
- [design 31](31-urp-fast-zero-copy.md) — zero-copy io_uring (Option E); removes
  the copies that remain after B+C+F2, the endgame for line rate at low CPU.
- [design 13](13-performance.md) — the historical throughput roadmap (send
  batching, signal-every-16, inline) that Option B / §35.4 finally implement.
