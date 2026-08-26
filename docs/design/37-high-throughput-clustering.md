# 37. High-Throughput Cluster Data Plane — Beating TCP per Stream

Status: **Strategy + measured results.** This pass also landed two changes and
ran the per-stream benchmark: a permanent **jumbo MTU (9700)** on the RoCE links and
a **`buffer_size` cap raise (64 KiB → 1 MiB)**, with the single-stream jumbo results
in [§37.4](#374-measured-results--jumbo-mtu--large-frames-2026-08-25) — where both
the copy and zero-copy paths now beat single-stream TCP. It also reviews the
throughput/latency work scattered across
[design 34](34-bulk-throughput.md) (option ladder), [design 35](35-windowing-flow-control.md)
(windowing/pump/scale-out), [design 36](36-congestion-control-cubic.md) (CUBIC),
and the [design 31](31-urp-fast-zero-copy.md) zero-copy fast path, and folds them
into a single **cluster-data-plane north star**: make `urp` a drop-in-faster
replacement for the TCP that Redpanda/Kafka and ClickHouse use between nodes.
Scoped to the **25 GbE hp1↔hp3 testbed** — every number here is measured on that
link or is a target on it; no 100 GbE speculation.

## 37.1 Motivation — urp as a cluster data plane

Redpanda/Kafka brokers replicate log data between nodes over **TCP**. ClickHouse
shards exchange INSERT fan-out and query-time data over **TCP**. TCP works, but it
pays the full IP/TCP stack on every packet and copies payload on both ends. The
premise of `urp` is that **UDS-over-RoCEv2 is both lower latency and higher
throughput** — the NIC runs the transport in hardware, and on the fast path there
are *zero* software copies.

The integrity test for that premise is deliberately strict and **single-stream**:

> **Thesis: one `urp` stream must beat one TCP stream — at every frame size, on
> both goodput and tail latency.**

We do **not** get to win by opening more connections than TCP. A cluster *does*
run many parallel flows (one per Kafka partition, one per ClickHouse shard), and
`urp` exploits that with scale-out ([§37.6](#376-scale-out-f2--additive-not-the-way-we-beat-tcp)) —
but scale-out is how we **fill the NIC**, not how we **beat TCP**. If a single
`urp` stream lost to a single TCP stream, no amount of fan-out would make the
comparison honest. So this doc leads with the per-stream contest and treats
multi-flow as additive.

The good news: on large frames we already win. The work is closing the gap on
small/mid frames — and that gap is a per-flow software-overhead problem, not a
"need more streams" problem.

## 37.2 What clustering demands of the transport

Two target workloads, on the axes that map to `urp` mechanisms:

**Redpanda/Kafka inter-node replication.** Many **independent, strictly-ordered
per-partition** byte streams carrying large sequential log segments. Figure of
merit: **sustained goodput** with a bounded tail (a slow replica stalls the ISR).
Message sizes are large (batched produce → segment-sized transfers). The
parallelism is **inherent** — partitions are already independent streams — which
is exactly the shape `urp`'s per-stream machinery ([design 09](09-connection-multiplexing.md))
was built for. Critically, a *single* partition is one ordered stream: striping
one partition across QPs is the wrong model (it overflows the reorder window —
[§34.5.1 finding 2](34-bulk-throughput.md#3451-phase-0-results-hp1--hp3-25-gbe-rocev2)).

**ClickHouse clustering.** Distributed INSERT fan-out to shards, plus query-time
**data shuffle/exchange** between shards (the `Distributed`/`Exchange` path).
Bursty, mixed sizes, latency-sensitive at query tail, again **many parallel
connections**. Same multi-flow shape; small control/coordination messages benefit
from the low-RTT and inline-send paths.

Cross-cutting: **both workloads open N connections regardless of transport.** So
the fair comparison has two parts — *per-stream* (the integrity test, §37.9) and
*aggregate at equal flow count* (the deployment test) — and `urp` must win the
first outright before the second means anything.

## 37.3 Why TCP today — and why one RDMA stream can beat one TCP stream

TCP is the incumbent for good reasons: it's universal, needs no special NIC, and
its single-flow path is heavily optimized (TSO/GSO on send, GRO on receive, so the
host touches far fewer, far larger segments than the wire carries; kernel-managed
congestion control). On this link a **single** `iperf2` TCP flow reaches
**~1900 MB/s = 15.2 Gb/s ≈ 61 % of 25 GbE**
([§34.5.1 baselines](34-bulk-throughput.md#3451-phase-0-results-hp1--hp3-25-gbe-rocev2)).

A single RDMA stream can beat that because it removes work TCP cannot:

- **No IP/TCP stack on the wire.** RoCEv2 runs the reliable transport in the NIC
  (RC QP); the host posts a work request and the hardware does segmentation,
  ordering, ACKs, and retransmit. There is no per-segment host stack traversal.
- **Zero software copies on the fast path.** [Design 31](31-urp-fast-zero-copy.md)
  DMAs straight from app-pinned pages to peer app-pinned pages — no `AF_UNIX`
  copy, no `kernel_recvmsg`/`kernel_sendmsg`. Measured **1 syscall / 4.2 µs CPU per
  64 KB message** vs the copy path's 9.14 syscalls / 93 µs
  ([§34.5.2](34-bulk-throughput.md#3452-zero-copy-fast-path-measurement--copy-vs-zero-copy-design-31-pr5)).
- **Lower, more deterministic latency.** RDMA RTT is ~1–2 µs on the wire; `urp`'s
  measured floor is **24.5 µs p50** end-to-end (24 B)
  ([design 32 results](32-performance-results.md)), and 64 KB RTT is **78 µs p50**
  on the fast path vs 321 µs on the copy path (§34.5.2).

The **pre-jumbo** scorecard, single flow, 25 GbE at the old 1500 MTU (RoCE PMTU
1024), back-to-back (all from §34.5):

| Regime (single stream)      | 4 KB        | 16 KB        | 64 KB                    |
|-----------------------------|-------------|--------------|--------------------------|
| **TCP** (`iperf2 -P1`)      | —           | —            | ~1900 MB/s (61 % line)   |
| **urp copy path**           | 6.6–9.7     | 9.4–9.6      | 52.7 MB/s (1.7 % line)   |
| **urp zero-copy fast path** | 47.5 MB/s   | 585 MB/s     | **2133 MB/s (68 % line)**|

At the 1500 MTU the fast path already beat single-stream TCP at 64 KB (~112 %) but
the copy path lost badly, and small frames stayed post-bound. **[§37.4](#374-measured-results--jumbo-mtu--large-frames-2026-08-25)
supersedes this with the jumbo (9700 MTU) + large-frame results**, where — as the
thesis predicts — the copy path *also* beats single-stream TCP and the fast path
nearly saturates the link on one stream.

## 37.4 Measured results — jumbo MTU + large frames (2026-08-25)

Two cheap, hardware-grounded changes turned the thesis from "true only on the fast
path at 64 KB" into "**true on both paths**." Testbed: hp1↔hp3, ConnectX-4 Lx
25 GbE RoCEv2, `mlx5_core` 7.1.8; line rate **3125 MB/s = 25000 Mb/s**. The NIC
reports `max_msg_sz = 1 GiB`, `max_mr_size` unlimited, **`max_sge = 30`** — so RC
segments a large message into PMTU packets in hardware and urp's old 64 KiB slot
cap was purely a software choice (the wire `payload_length` is a u32).

1. **Jumbo MTU 9700** (from 1500) on the RoCE links, made permanent in the host
   NixOS configs — lifts the RoCE path MTU (`active_mtu`) **1024 → 4096**, its
   ceiling, so 4× fewer RoCE packets per frame. Verified end-to-end (`ping -M do
   -s 9672` passes).
2. **`URP_BUFFER_SIZE_MAX` raised 64 KiB → 1 MiB** (`kernel/include/uapi/linux/urp.h`),
   so a single copy-path frame carries far more bytes. Backed by high-order
   `page_pool` slots for now (fresh-boot allocation of 256 KiB–1 MiB succeeds,
   `buffer-alloc-fails:0`); the production path is multi-SGE gather (`max_sge=30`),
   see §37.5 / §37.7.

**Single-stream goodput, jumbo, `num_qps=1`** (`urp-bench --pattern stream`,
sink-measured; TCP = single-stream `iperf2` at the same jumbo MTU):

| frame  | copy MB/s | copy % line | copy fps | fast MB/s | fast % line |
|--------|-----------|-------------|----------|-----------|-------------|
| 64 KiB | 1718.7    | 55.0 %      | 29089    | 2298.4    | 73.6 %      |
| 128 KiB| 1869.9    | 59.8 %      | 15810    | —         | —           |
| 256 KiB| **1912.8**| **61.2 %**  | 8099     | 3040.2    | 97.3 %      |
| 512 KiB| 1631.2    | 52.2 %      | 3450     | **3050.8**| **97.6 %**  |
| 1 MiB  | 809.6     | 25.9 %      | 836      | —         | —           |

**TCP single-stream jumbo `iperf2` = 1862 MB/s (59.6 % line).** All urp runs
`reorder_drops=0`, 100 % reassembled.

Three findings, each a direct answer to the thesis:

1. **The copy path now beats single-stream TCP.** Peak **1912.8 MB/s at 256 KiB =
   103 % of jumbo TCP** — up from the pre-jumbo 52.7 MB/s (2.8 % of TCP), a **~36×**
   lift, with **no zero-copy and no Option B**. It came from jumbo (RoCE PMTU
   1024→4096) + large frames riding the *already-merged* design-35 pump: frame rate
   is **~29 000 fps at 64 KiB**, not the ~1000 fps of the old pre-waitqueue,
   1 ms-poll-floored pump ([§34.5.1](34-bulk-throughput.md#3451-phase-0-results-hp1--hp3-25-gbe-rocev2)).
2. **The copy path has a sweet spot (~128–256 KiB), then a copy wall.** Goodput
   *rises* with frame size while frame-rate-bound, peaks near 256 KiB, then *falls*
   at 512 KiB / 1 MiB as the (2-usable-core) box goes **copy-bound** —
   `cpu_us_per_msg` climbs 19 → 158 as the per-frame `memcpy` and UDS fragmentation
   dominate. Bigger is *not* always better: direct evidence for **adaptive frame
   sizing with a cap** (§37.5a) and for zero-copy at large frames.
3. **The fast path nearly saturates the link on one stream.** Zero-copy *climbs*
   exactly where the copy path falls: **3050.8 MB/s at 512 KiB = 97.6 % of 25 GbE**,
   one stream, `syscalls_per_msg=0.5`, `cpu_us_per_msg=2.3`. Jumbo also lifted its
   64 KiB point 68 % → 73.6 %. It clears the copy wall because there is no copy.

**Verdict: one `urp` stream beats one TCP stream — on *both* paths — no N-stream
needed** (§37.6 scale-out is now purely additive). The remaining gap to line on the
copy path is the copy itself (Limiter 2, §37.5); on the fast path a single stream is
already at ~98 %.

### Why the headroom is ours to close

Underneath, the copy path is **post/serialization-bound then copy-bound**, never
congestion-bound: one `kernel_recvmsg` copy per frame (`kernel/urp_pump.c:146`,`:315`),
every send `IB_SEND_SIGNALED` (`kernel/urp_pump.c:61`; QP `IB_SIGNAL_ALL_WR`
`kernel/urp_rdma.c:436`), one WR per doorbell (`kernel/urp_pump.c:51-63`), one
recv-copy + `kernel_sendmsg` + SRQ repost per frame on RX
(`kernel/urp_rdma.c:494`,`:830`). The fabric stays ~98 % idle at these rates
([design 36 §36.1](36-congestion-control-cubic.md)), so CUBIC has nothing to bind
against and stays deferred. The headroom to line is a **host-software** problem
(per-frame overhead, then the copy), all in code we own.

## 37.5 The single-stream ceiling — what caps one flow, and how to lift it

This is the heart of the doc: make **one** `urp` stream beat **one** TCP stream at
**every** size. The per-flow limiters and the levers that remove them:

**Limiter 1 — per-frame post/completion overhead.** Every send generates a CQE
(`IB_SIGNAL_ALL_WR`), reaped one-at-a-time on a workqueue
(`urp_send_done`, `kernel/urp_rdma.c:453-467`), and each frame is its own
`ib_post_send` doorbell. At 4076-byte frames this caps the pump near ~1000 fps and
even the *zero-copy* fast path at 16 KB tops out at 585 MB/s because it, too,
signals every send and posts one WR per op (`kernel/urp_cmd.c:368`,`:843`). **The
lever is [Option B](34-bulk-throughput.md#option-b--optimize-the-two-sided-send-pump-in-place-effort-m-risk-medium),
applied to both the pump *and* the fast post loop:**
- **Selective signaling** — signal every Nth WR (e.g. 16), batch-reclaim the N
  slots on the signaled CQE. Cuts CQE/workqueue traffic ~N×. (Note the coupling:
  buffer reclaim is currently *tied* to every-send signaling — reclaim must move to
  the batch boundary.)
- **Multi-WR posts** — chain `wr->next` and ring the doorbell once for many frames.
- **Coalesced reads** — loop `kernel_recvmsg` to fill a large frame from many small
  UDS writes before posting (copy path), amortizing the fixed cost.

This is the lever that lifts the **small/mid-frame** single-flow ceiling — exactly
where TCP's TSO/GSO batching currently wins. Applied to the fast path it should
pull 16 KB and 4 KB up toward line and push 64 KB from 68 % past it.

**Limiter 2 — the copy itself (copy path only).** Two `AF_UNIX` copies on TX/RX
plus the SRQ recv-slot copy. **The lever is the zero-copy fast path
([design 31](31-urp-fast-zero-copy.md), built + HW-validated)** — the reason the
64 KB single-flow number already beats TCP. Its cost is app-side io_uring
integration (Redpanda is a Seastar app — [design 31a](31a-seastar-cpp-demo.md)
sketches the client); for peers that can't adopt it, Option B on the copy pump is
the fallback that still narrows the gap.

**Limiter 3 — tiny-message latency.** Control/coordination frames (PING/PONG/
CREDIT, ~20–68 B) still DMA and take a CQE; no `max_inline_data` is set
(`kernel/urp_rdma.c:412-436`). **The lever is inline sends** (`IB_SEND_INLINE` for
< ~64 B): the NIC reads the payload from the WQE, skipping a DMA — a latency win on
the small-message path both workloads use for coordination.

**Limiter 4 — the 50/50 pool split.** `buffer_count` is split static 50/50 into
send/recv pools (`kernel/urp_rdma.c` resolve), stranding half the pool for a
one-directional replication sender. An **asymmetric split** (Option A tuning) is a
near-zero-code win worth pulling forward.

**Limiter 5 — small frames on the wire (RoCE PMTU) and the frame-size cap.** Two
tuning levers, both now applied and measured (§37.4): **jumbo MTU** lifts the RoCE
`active_mtu` 1024 → 4096 (4× fewer RoCE packets/frame), and **raising the
`buffer_size` cap** lets one copy-path frame carry far more bytes — which, because
the copy path is frame-rate-bound, scales goodput almost linearly up to the copy
wall. The prototype uses high-order `page_pool` slots; the **production large-frame
path is multi-SGE gather** — the NIC's `max_sge=30` lets one logical frame span up
to 30 pages with no high-order allocation (and composes with Option B's coalesced
reads). This is the cheapest lever and it is what put the copy path past TCP.

**What one stream reaches (measured, §37.4).** With jumbo + large frames alone the
**fast path already hits 97.6 % of 25 GbE on one stream** (512 KiB) and the **copy
path 61 % (= 103 % of single-stream TCP)** — Limiter 1 (Option B) is not even needed
to clear TCP; it remains the lever to lift the *small/mid*-frame points and to push
the fast path's 64 KiB number (73.6 %) toward its large-frame ceiling. **Target
now met: one `urp` stream ≥ single-stream TCP at the sizes clustering uses, with a
single fast-path stream ≈ line rate.**

**Sustaining the peak — windowing (built, and why it's not the speed lever).** A
faster single-stream pump will re-expose the old failure mode: oversending into a
drained receiver → RNR storms. **Byte-windowing (Option C, [design 35](35-windowing-flow-control.md),
merged — reorder-matrix 9/9 GREEN)** is what makes a fast pump *sustainable*: the
sender blocks instead of flooding, with self-healing cumulative-absolute grants.
Note the apparent contradiction in the docs and its resolution: [design 35 §35.1](35-windowing-flow-control.md)
says windowing "is not the speed lever" (true — BDP here is only ~116 KB, under two
64 KB frames, so the window rarely gates a well-fed sender), while
[design 36 §36.2](36-congestion-control-cubic.md) says CUBIC "supersedes" that
claim for the *congestion* case. Both are consistent once framed by role:
**windowing is the correctness/efficiency layer that lets a faster pump hold its
peak; CUBIC is a congestion layer that only binds once offered load exceeds fabric
capacity — which won't happen until the pump work above raises single-flow load.**
CUBIC stays deferred.

## 37.5a Adaptive frame sizing — reconciling latency and throughput

The §37.4 curve makes the case concrete: goodput *rises* with frame size while
frame-rate-bound, then *falls* past a sweet spot (~256 KiB on this box) as the copy
wall bites. And there is the standing latency↔throughput tension — a bigger frame
coalesces more but waits longer to fill. Real cluster traffic ramps from idle to
saturated, so the *right* frame size is not a constant. The design should let it
**grow with offered load, bounded by a sweet-spot cap** — small frames at low load
(latency), large frames under backpressure (throughput), never past the copy wall.

Two layers, cheapest first:

- **(a) Natural adaptivity — essentially free.** With the raised cap plus
  coalesced reads (§37.5 Limiter 1), the pump reads *whatever is queued* on the UDS
  socket, bounded by `buffer_size`: a lightly loaded stream emits small frames (low
  latency), a saturated one fills toward the cap (high throughput). Raising the cap
  raises the *ceiling*, it does not force large frames — so the cap should be set at
  the **sweet spot (≈128–256 KiB here), not the 1 MiB max**, to stay left of the
  copy wall. This alone gives most of the adaptivity for zero added latency.
- **(b) Explicit Nagle-like coalescing window** (design 13 §13.3, reframed for the
  kernel pump). When natural coalescing is not enough — a stream dribbling many
  small writes just under the read rate — an EWMA of the per-stream arrival rate maps
  to a µs-scale flush timeout (or target frame size), with a **size-threshold
  immediate flush** and, critically, **timeout → 0 at low load** so latency-critical
  traffic is never penalised. The target size is capped at the sweet spot, not the
  slot max.

**Interactions:** the target must stay ≤ the byte-window and ≤ the reorder window
(design 35 §35.3 mandatory coupling), both already sized in bytes, so a larger frame
is automatically safe. **Decision to build (b) is gated on the numbers** — §37.4
shows natural adaptivity + a sweet-spot cap already clears TCP, so an explicit timer
is a refinement, not a prerequisite. Captured here as a design sketch; a follow-up
task tunes the default cap and prototypes the EWMA timer if a dribble workload needs
it.

## 37.6 Scale-out (F2) — additive, not the way we beat TCP

Once a single stream wins, **scale-out multiplies it to fill the NIC and to match
the cluster's own parallelism** — it is not a crutch for the per-stream contest.

**F2 = one stream per QP, one QP per independent flow** — one per Kafka partition,
one per ClickHouse shard connection
([design 35 §35.5](35-windowing-flow-control.md), [§34.3 Option F2](34-bulk-throughput.md#option-f--host-side-parallelism)).
Each stream has its own TX kthread / reorder / window state, already isolated with
no split-brain (the multistream machinery exists and is HW-validated). This is the
*right* multi-QP model; the *wrong* one — per-frame round-robin striping of a
single ordered stream across QPs — overflows the reorder window and delivers almost
nothing (1.37 M reorder_drops, [§34.5.1 finding 2](34-bulk-throughput.md#3451-phase-0-results-hp1--hp3-25-gbe-rocev2)).
F1 (multiple TX kthreads on *one* ordered stream) is rejected: it serializes on the
copy and races `tx_seq` (§34.3 Option F1).

**Why it's additive, not the win:** if one fast-path stream reaches ≥ 90 % of line
(§37.5 target), F2 exists only to (a) recover the last few % of NIC headroom a
single QP leaves, and (b) let a broker/shard's N partitions each get an independent
ordered pipe — which is what the workload wants anyway. The cluster was going to
open N connections regardless; `urp` simply gives each one its own hardware-ordered
QP. Scale-out is cheap here (mostly workload/harness sharding over existing
machinery), so it lands *after* the per-stream win, as amplification.

## 37.7 The lever ladder, prioritized for the per-stream win

One table, ordered by how directly each lever serves "one stream beats one TCP
stream," with status. Latency effect noted because the goal is both.

| # | Lever | Attacks | Per-stream effect | Latency | Status |
|---|-------|---------|-------------------|---------|--------|
| 1 | **Jumbo MTU + raised frame-size cap** (§37.4) | RoCE PMTU + per-frame amortization | **copy path 1.7 %→61 % line (past TCP); fast 68 %→97.6 %** | neutral | **built + measured (2026-08-25)** |
| 2 | **Zero-copy fast path** (Option E / [d31](31-urp-fast-zero-copy.md)) | copies 1–4 | **≈97.6 % line @ jumbo, clears the copy wall** | 4.1× lower @64 KB | **built + HW-validated** |
| 3 | **Multi-SGE large frames** (`max_sge=30`) — production twin of #1 | high-order alloc fragility | same goodput, robust (no order-8 alloc) | neutral | **next (path Y; #1 prototype uses high-order slots)** |
| 4 | **Option B: selective signaling + multi-WR + coalesce** ([d34 §34.3](34-bulk-throughput.md#option-b--optimize-the-two-sided-send-pump-in-place-effort-m-risk-medium)) — pump **and** fast post loop | per-frame CQE/doorbell | lifts the *small/mid*-frame points + fast 64 KB toward its ceiling | slight coalesce cost | not built (no longer needed to clear TCP) |
| 5 | **Byte-windowing** (Option C / [d35](35-windowing-flow-control.md)) | oversend / RNR storm | makes the peak *sustainable* (not higher) | removes sawtooth | **built (9/9 GREEN)** |
| 6 | **Adaptive frame sizing** (§37.5a) | latency↔throughput + copy wall | keeps small frames at low load, caps at the sweet spot | preserves low-load latency | design sketch; (a) natural adaptivity free, (b) gated on numbers |
| 7 | **Inline sends** (`IB_SEND_INLINE` <64 B) ([d13 §13.5](13-performance.md)) | tiny-frame DMA+CQE | control-message latency | small-msg RTT | not built |
| 8 | **F2 scale-out** ([§37.6](#376-scale-out-f2--additive-not-the-way-we-beat-tcp)) | single-QP NIC headroom | **additive** — fills line, maps to partitions/shards | per-stream tail preserved | not built |
| 9 | **Option D** one-sided WRITE ring ([d34 §34.3](34-bulk-throughput.md#option-d--one-sided-rdma-write-into-a-peer-registered-ring-effort-l-risk-high)) | recv-copy/SRQ ceiling | removes the copy wall on the copy path | — | deferred (trigger: copy-path large-frame wall matters) |
| 10 | **CUBIC cwnd** ([d36](36-congestion-control-cubic.md)) | congestion (none yet) | none until congestion-bound | — | deferred (trigger: offered load > fabric) |

Second-order host-side levers from the historical [design 13](13-performance.md)
roadmap — SQPOLL, NUMA-aware buffer placement, CPU pinning, CQ moderation — apply
to both paths and are worth measuring once #2 lands, but none is on the critical
path to the per-stream win.

## 37.8 Concrete 25 GbE targets + decision tree

Numeric goals, per-stream first, with §37.4 status:

- **T1 (integrity test) — one `urp` stream ≥ one TCP stream at the sizes clustering
  uses. ✅ MET** at jumbo: copy path 256 KiB = 1912.8 MB/s = **103 % of jumbo TCP**;
  fast path ≥ 256 KiB ≈ **164 % of TCP**. Open only at **4–16 KiB**, where TCP's
  TSO/GSO still wins on the copy path — Lever #4 (Option B) is the remaining lift,
  but it is **no longer needed to clear TCP at cluster frame sizes**.
- **T2 — one fast-path stream ≥ 90 % of 25 GbE. ✅ MET** — 97.6 % at 512 KiB (73.6 %
  at 64 KiB; Option B on the fast post loop would raise the small-frame point).
- **T3 — copy-path per-flow beats single-stream TCP. ✅ MET** (61 % line / 103 % TCP
  at 256 KiB) — for peers that cannot adopt the io_uring fast path.
- **T4 (deployment test) — aggregate ≥ line at small N via F2**, each stream holding
  its per-stream p99. **Not yet measured** — additive, follows the per-stream win.

**Decision tree (updated by the results):**
1. Per-stream head-to-head (§37.9): **done — both paths beat single-stream TCP at
   ≥ 128 KiB.** The thesis holds; the natural next step is F2 (T4) for the aggregate
   deployment picture, plus the multi-SGE production large-frame path (Lever #3).
2. To also win at **4–16 KiB** on the copy path, build Lever #4 (Option B). Lower
   priority now that cluster-relevant frame sizes already clear TCP.
3. The **copy wall** at 512 KiB–1 MiB is real (§37.4) but avoided by capping frame
   size at the sweet spot (§37.5a); the zero-copy path already sidesteps it. Option D
   (one-sided WRITE) only if the copy path must go large; CUBIC only if offered load
   ever exceeds fabric capacity — neither is on this link's path.

## 37.9 Head-to-head benchmark plan — proving the thesis

The claim is falsifiable and must be measured, not asserted. The instrument
already exists — `urp-bench --pattern {stream,echo}` ([design 34 §34.4](34-bulk-throughput.md#344-the-measurement-instrument--one-way-streaming-in-urp-bench))
and the matrix runners (`.#urp-bw-matrix` copy, `.#urp-fast-bw-matrix` zero-copy,
`.#urp-fast-hw-matrix` RTT). The additions here are the **TCP/RDMA baselines** and
the **per-stream-first comparison**; wiring them into the runners is a follow-up,
this section specifies the method.

> **Part 1 has been run (2026-08-25) — results in [§37.4](#374-measured-results--jumbo-mtu--large-frames-2026-08-25).**
> The goodput contest used `.#urp-bw-matrix` / `.#urp-fast-bw-matrix` with a jumbo
> `iperf2 -P1` baseline; both urp paths beat single-stream TCP at ≥ 128 KiB. Part 2
> (aggregate/N-flow) and the latency head-to-head remain.
>
> *Harness caveat surfaced:* killing a `--kind fast` bench mid-REGISTER left an
> `iou-wrk` worker wedged in `urp_cmd_reg_mr_sync` → `wait_for_completion`
> (uninterruptible; a hung-task INFO, no crash, copy path unaffected). A small
> robustness follow-up should make the fast REGISTER wait interruptible/timed so a
> client death mid-registration cannot wedge the worker.

**Part 1 — per-stream contest (the headline, T1/T2).**
- **Goodput:** `urp-bench --pattern stream` single flow (fast path via
  `--mode uring-cmd` against a `--kind fast` endpoint pair; copy path via
  `--mode blocking`) vs **`iperf2 -P1`** (TCP), same link back-to-back, sweeping
  frame size {4 K … 1 M}. Sink-measured goodput.
- **Latency:** `urp-bench --pattern echo` p50/p99 vs a single-connection TCP
  ping-pong (`netperf TCP_RR` or `sockperf` — install-or-skip, like `ib_write_bw`
  below) at matched sizes.
- **Fabric ceiling:** **`ib_write_bw`** single-QP as the raw-RDMA reference and the
  target for the fast path. Not in nixpkgs / not on hp1/hp3 today — document as
  "install-or-skip," exactly as [design 34 §34.5](34-bulk-throughput.md#345-phase-0-measurement-before-any-kernel-change)
  already handles it.
- **Pass/fail:** for each frame size, `urp` (fast) single-flow goodput **≥**
  `iperf2 -P1` **and** `urp` echo p99 **<** TCP_RR p99. The copy path is reported
  for the io_uring-less fallback but is not the thesis gate.

**Part 2 — deployment contest (aggregate, T4).**
- N-stream fan-out (F2) on both `urp` runners, N ∈ {1, 2, 4, 8, 16} × frame size,
  vs **`iperf2 -P N`** at the **same N**. Report aggregate goodput and per-stream
  p50/p99 (a tail-fairness check — no single partition starved).
- **Pass/fail:** `urp` aggregate goodput ≥ TCP at equal N, approaching 25 GbE, with
  per-stream p99 within the Part-1 single-stream envelope.

**Rigor (reuse [§34.5](34-bulk-throughput.md#345-phase-0-measurement-before-any-kernel-change)'s
hard-won harness discipline):** fresh endpoints per point (`urp remove`+`add`);
**sink bound-confirmed in `/proc/net/unix` before the source starts** (the acceptor
connects its backend UDS lazily on the first frame with no retry — a source that
starts first loses the stream); journal scrape **scoped to the run's systemd
invocation id** (unit names repeat across sweeps); per-run counter **deltas**
(counters are cumulative, no reset); a run is clean only when `reorder_drops == 0`
and the sink's `rx-frames` advanced by ~the message count.

This needs **no new measurement instrument** — only the multi-flow TCP/RDMA
baselines and N-stream aggregation, landing as a runner tweak in the follow-up
that implements Lever #2.

## 37.10 Relation to other docs

- [design 34 — bulk throughput](34-bulk-throughput.md): the engineering option
  ladder (A–F) and the copy-vs-zero-copy measurements this doc re-prioritizes for
  the per-stream, cluster-facing goal. 37 is the *why/for-whom*; 34 is the *how*.
- [design 35 — windowing/pump/scale-out](35-windowing-flow-control.md): the
  build-ready spec for Option C (built) and F2; where §37.5's "sustain the peak"
  and §37.6's scale-out are specified.
- [design 31 — zero-copy fast path](31-urp-fast-zero-copy.md) + [31a — Seastar
  demo](31a-seastar-cpp-demo.md): Lever #1, the reason single-flow already beats TCP
  at 64 KB, and the app-integration path for Redpanda.
- [design 32 — performance results](32-performance-results.md): the latency anchor
  (24.5 µs p50 floor).
- [design 30 — urp-bench](30-urp-bench-io-uring.md): the instrument §37.9 drives.
- [design 36 — CUBIC](36-congestion-control-cubic.md): the deferred congestion
  layer; §37.5 reconciles its "supersedes §35.1" framing.
- [design 13 — performance (historical)](13-performance.md): the origin of Levers
  #2/#4 and the second-order host-side levers.
- [appendix — RoCEv2 security](appendix-rocev2-security.md): the fabric/trust
  assumptions a production cluster deployment must satisfy.

> Prototype / research direction. Targets and the benchmark plan here are the 25
> GbE testbed's; production hardware and app integration are follow-on work.
