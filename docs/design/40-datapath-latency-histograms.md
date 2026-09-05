# 40. Datapath Latency & Rate Histograms

Status: **PR-A (inter-arrival histogram) implemented + live-validated on the mesh
(2026-09-05); PR-B1 (OWD plumbing/codec, hardware-free) implemented (2026-09-05);
PR-B2 (OWD datapath + full exporter/Grafana surface) implemented (2026-09-05);
PR-C (live PTP-mesh validation) pending.** Follow-up
to [39. Metrics Exporter](39-metrics-exporter.md) §39.11 ("Latency/credit gaps —
the module exposes only `rtt_ewma_ns`; no latency histograms"). Specifies two new
kernel-measured distributions, surfaced through the same generic-netlink → exporter
→ Grafana path design 39 already built. PR-A + PR-B ship both histograms end to end
(kernel sampler + netlink nest + `urp-netlink` decode + exporter family + Grafana
panels + KUnit/userspace/render tests, all hardware-free); PR-C below validates OWD
against the PTP-synced mesh.

## 40.0 Why — the gap this closes

Today the *only* latency signal `urp` exports is `urp_qp_rtt_ns` — a per-QP EWMA
(α=0.2) of the software PING/PONG probe (design 08a). We measured what that number
actually is (memory: `latency-window-vs-contention`, `impl-characterization`):

- It is a **round trip**, not one-way, and both legs ride *behind* the data on the
  same SQ, so under load it inflates with queueing — it reads node contention and
  the §38.5 fairness fan-out, not wire latency. It spiked to ~500 µs at 4 concurrent
  flows while a single flow sat at ~36 µs.
- It is an **EWMA** — a smoothed mean. It cannot answer "what is p99 / p999?", which
  is the number an SLO is written against and the number the low-latency thesis of
  this whole project lives or dies on.
- It only populates on **active senders** (initiator side), and the periodic worker
  that keeps it fresh is gated to `num_qps > 1`.

So we can graph mean throughput precisely (design 39 counters → `rate()`) but we
cannot graph the *shape* of either the rate or the latency. This doc adds that shape
as two histograms, both computed in the kernel where the timestamps are cheap and
exact, both rendered as standard Prometheus histograms so Grafana's
`histogram_quantile()` gives real percentiles.

The two are deliberately split by **cost and risk**:

| | Histogram A — inter-arrival | Histogram B — one-way delivery latency |
|---|---|---|
| Measures | RX rate / jitter / burst gaps | true end-to-end DATA latency |
| Clock | one local `ktime_get_ns` at RX | PTP-disciplined realtime, **two** nodes |
| Wire change | **none** (seq already in header) | +8-byte sampled trailer + caps bit |
| Cross-node dependency | none | PTP sync quality bounds accuracy |
| Ship | **first** (PR-A, hardware-free to unit-test) | after (PR-B, needs the testbed's PTP) |

Histogram A is nearly free and needs no protocol change, so it ships first and
de-risks B. This mirrors the user's framing: "time between each sequence number …
and between every 10th and every 100th frame … to get a sense of the rates" (that is
A), distinct from true per-frame latency (that is B).

---

## 40.1 Histogram A — inter-arrival time (rate & jitter)

**Idea.** Every received DATA frame carries a monotonically increasing
`sequence_number` (`urp_frame.h`, u64 at header bytes `[4..12)`). At RX we already
decode it (`urp_rdma.c:771` classify, and the in-order copy-elision path at `:946`).
Stamp `now = ktime_get_ns()` on arrival and bucket the delta since the previous
arrival. The *mean* rate is already available from `rx_frames_total` via `rate()`;
what A adds is the **distribution** — the credit-stall pauses, the fairness sawtooth,
the microbursts — none of which a mean can show.

**Three sampling strides**, exactly as the user framed it. For a stream we keep three
"last seen" timestamps and emit into three sub-histograms:

- **stride 1** — Δt between consecutive frames (`seq` == last + 1). Instantaneous
  inter-arrival: microburst spacing and per-frame jitter.
- **stride 10** — Δt across every 10th delivered frame. Smooths single-frame noise;
  shows short-window rate.
- **stride 100** — Δt across every 100th. Coarse sustained rate; the line that tracks
  goodput without per-frame noise.

Rendering all three lets a reader see the burst structure collapse as the stride
grows (a bursty-but-high-throughput flow has a wide stride-1 and a tight stride-100).

**Where it lives.** Per-**endpoint** aggregate by default (cheapest; fits the existing
`urp_stats` block, one histogram set per endpoint, updated from the RX completion
context which is already the single writer of `rx_frames`). A per-QP variant is a
follow-up (§40.6) — per-QP inter-arrival would directly visualize the §38.5 fan-out,
but it multiplies bucket storage by `num_qps`; start endpoint-wide.

**Bucket layout** (fixed, compile-time — only *counts* cross the wire). Log-spaced,
inter-arrival for a 25–800 GbE fabric spans sub-µs (line-rate back-to-back) to the
tens-of-ms credit-stall tail:

```
le (seconds): 250e-9, 500e-9, 1e-6, 2e-6, 5e-6, 10e-6, 25e-6, 50e-6,
              100e-6, 250e-6, 500e-6, 1e-3, 5e-3, 25e-3, +Inf         (15 buckets)
```

**Storage** — extend `struct urp_stats` (`kernel/urp.h`) with, per stride s∈{1,10,100}:

```c
struct urp_hist15 {                 /* 15-bucket cumulative-able histogram */
    atomic64_t bucket[15];          /* count in [edge[i-1], edge[i]) */
    atomic64_t sum_ns;              /* Σ Δt — feeds _sum */
    atomic64_t count;              /* Σ observations — feeds _count */
};
struct urp_interarrival {
    u64             last_ns[3];     /* last arrival ktime per stride (writer-local) */
    u64             last_seq[3];    /* seq at last sample per stride */
    struct urp_hist15 h[3];         /* stride 1 / 10 / 100 */
};
```

**Hot-path cost.** Per delivered frame: one `ktime_get_ns()` (a few ns, vDSO-class in
kernel), a branchless bucket index (`urp_hist_bucket()` — a small unrolled compare
ladder or `fls64` on the ns value, pure and unit-testable), and up to three
`atomic64_inc`. Never allocates, never logs a series, no lock (RX completion is the
sole writer; the netlink reader does a racy read of monotone counters, same contract
as every other `urp_stats` counter today). Gated by `urp.interarrival_hist` (bool,
default **on** — it is cheap enough to leave on).

**No protocol change.** `seq` is already on the wire; the timestamp is taken locally.
This is the entire reason A ships first.

---

## 40.2 Histogram B — one-way delivery latency (true latency)

**Idea.** Sample a small fraction of DATA frames (default 1 in 64), stamping the
sender's realtime clock into the frame at TX; at RX compute
`owd = t_recv_real − t_send_real` against the PTP-disciplined realtime clock and
bucket it. This is the real per-frame delivery latency on *data* frames — not the
queueing-contaminated PING/PONG round trip, and a distribution, not an EWMA.

**Wire change (backward-compatible).** The 20-byte header is full, so a sampled frame
carries an **8-byte timestamp trailer** appended *after* the payload, flagged in the
existing `flags` byte `[13]`:

```
URP_DATA_FLAG_TSTAMP   = BIT(3)     /* bits 0..2 are already SYN/FIN/RST */
trailer (8 bytes, little-endian, present iff flag set):
    [0..8)  t_send_real  u64        /* sender CLOCK_REALTIME (ns) at post */
```

> **Correction (PR-B1).** The original spec named this flag `BIT(1)` and claimed
> "BIT(0) is unused." That is wrong: the data-flags byte `[13]` already carries
> `URP_DATA_FLAG_SYN`=BIT(0), `_FIN`=BIT(1), `_RST`=BIT(2)
> (`include/uapi/linux/urp.h`), so TSTAMP takes the next free bit, **BIT(3)**.
> The caps bit is `URP_CONN_CAP_TSTAMP = (1 << 1)` (byte-windowing holds
> `(1 << 0)`). The 8-byte trailer codec is `urp_frame_tstamp_{encode,decode}` in
> `urp_frame.h`, pinned by KUnit (`test_tstamp_trailer`) and the userspace twin.

The trailer sits past `payload_length` bytes, so a receiver that does not understand
the flag (or a mis-set flag) simply never reads it — but we do not rely on that:

**Capability negotiation.** Reuse the design 35 §35.3 caps trailer already in
`urp_frame.h` (`urp_conn_priv_build_full` / `urp_conn_priv_peer_caps`, the
`URP_CONN_PRIV_CAP_TRAILER_LEN` == 5 trailer). Add `URP_CONN_CAP_TSTAMP` (a new caps
bit). A sender stamps **only** when both peers advertised it (same gate the byte-window
uses). An old peer never advertises → no trailer is ever sent → exact legacy wire.
The receiver's buffer sizing must reserve the 8 trailer bytes when the cap is
negotiated (analogous to the 20-byte header reserve).

**Sampling.** `urp.latency_sample_period` (u32, default 64, **0 = off**). TX stamps
when `seq % period == 0`. 1/64 keeps the wire and CPU cost negligible while giving
thousands of samples/sec at line rate — ample for a stable p999. Seq is already on the
wire, so a sampled frame is fully self-describing (seq for trace-matching, t_send for
the delta).

**Clock & accuracy — the honest caveat.** OWD is only as good as the two nodes' clock
alignment. The testbed has PTP (pmc); with NIC hardware timestamping the residual
offset is sub-µs, well under the ~36 µs single-flow latency we are trying to resolve.
Document prominently that **OWD requires PTP**; on an unsynced host the number is
meaningless. Two guards:

1. Export `urp_owd_clock_offset_ns` — if the module can read the local PTP servo
   offset (or we accept it via sysctl from a userspace PTP monitor), surface it so a
   dashboard can grey out OWD when sync is poor. (Kernel-reading the servo is a
   stretch; MVP accepts a sysctl-set advisory value, 0 = unknown.)
2. Clamp: a negative `owd` (clock skew / reordering across the boundary) increments
   `urp_owd_clock_anomalies_total` and is **not** bucketed, so skew never corrupts the
   histogram.

**Bucket layout** — latency-tuned, ~1 µs to ~10 ms:

```
le (seconds): 1e-6, 2e-6, 5e-6, 10e-6, 20e-6, 50e-6, 100e-6, 200e-6,
              500e-6, 1e-3, 2e-3, 5e-3, 10e-3, +Inf                   (14 buckets)
```

**Storage** — per-endpoint (default), same `urp_hist`-style block plus the anomaly and
sample counters. Per-QP OWD (§40.6) is the high-value follow-up because it makes the
fairness latch visible as a *latency* asymmetry, not just a throughput one.

**Relationship to `urp_qp_rtt_ns`.** B does not replace the PING/PONG probe (that
still drives the BDP window term, `urp_ep_repr_rtt_ns`) — it *supersedes it as the SLO
signal*. Keep both; the probe is a liveness/keepalive + window input, OWD is the
user-facing latency truth.

---

## 40.3 Netlink surface (kernel → exporter)

Both histograms ride the existing `URP_CMD_GET_ENDPOINT` verbose reply as new nested
attributes under the endpoint (mirroring `URP_ENDPOINT_A_STATS`,
`kernel/include/uapi/linux/urp.h`). Only the bucket **counts + sum + count** cross the
wire; the `le` edges are compile-time constants known to both kernel and the
`urp-netlink` decoder, so they are never serialized.

New UAPI attrs (append-only, so old `urp-netlink` decoders ignore them; ids 15/16
were already taken by `URP_ENDPOINT_A_MODE`/`_KIND`, so this starts at 17):

```c
URP_ENDPOINT_A_INTERARRIVAL   = 17,  /* NLA_NESTED: 3 × urp_hist_attr (stride 1/10/100) */
URP_ENDPOINT_A_OWD            = 18,  /* NLA_NESTED: 1 × urp_hist_attr + anomaly/offset (PR-B) */

/* nested histogram attr set (both reuse it) */
enum urp_hist_attr {
    URP_HIST_A_UNSPEC   = 0,
    URP_HIST_A_STRIDE   = 1,   /* NLA_U32: 1/10/100 (interarrival) or 0 (owd) */
    URP_HIST_A_BUCKETS  = 2,   /* NLA_BINARY: N × u64 le-count (N fixed per family) */
    URP_HIST_A_SUM_NS   = 3,   /* NLA_U64 */
    URP_HIST_A_COUNT    = 4,   /* NLA_U64 */
    __URP_HIST_A_MAX,
};
```

`URP_ENDPOINT_A_OWD` also carries `urp_owd_clock_offset_ns` and
`urp_owd_clock_anomalies_total` as sibling scalars.

**Cost note (ties to §39.4).** These nests only appear in the per-endpoint verbose
`doit` GET (never the cheap `dumpit`), so they cost bytes only on the `1 + N` fan-out
the exporter already pays, and only when the histograms are enabled. Buckets are a
fixed small blob (~15×8 = 120 bytes), so the per-endpoint reply grows by a few hundred
bytes — negligible against the scrape budget.

`crates/urp-netlink/src/format.rs`: add `Histogram { le: &'static [f64], buckets:
Vec<u64>, sum_ns: u64, count: u64 }` decoded fields to `Endpoint` (behind the new
attr ids; absent on an old module → `None`, exporter emits nothing). The `le` slice is
a const in the crate keyed by family, matching the kernel edges — one source of truth
per family, asserted equal in a unit test against the kernel header values.

---

## 40.4 Exporter surface (design 39 render.rs)

New metric families, rendered by hand into the reused buffer (design 39 §39.3 zero
steady-state alloc), `# HELP`/`# TYPE ... histogram` once per family. Prometheus
histogram triplet — cumulative `_bucket`, `_sum`, `_count`:

```
# Histogram A — labels {endpoint, device, stride}
urp_endpoint_interarrival_seconds_bucket{stride="1",le="1e-06"}  <cumsum>
urp_endpoint_interarrival_seconds_sum{stride="1"}                <Σ Δt seconds>
urp_endpoint_interarrival_seconds_count{stride="1"}              <n>
   # …repeated for stride="10", stride="100"

# Histogram B — labels {endpoint, device}
urp_endpoint_owd_seconds_bucket{le="1e-05"}   <cumsum>
urp_endpoint_owd_seconds_sum                  <Σ owd seconds>
urp_endpoint_owd_seconds_count                <n>
urp_endpoint_owd_clock_offset_seconds         <gauge; 0 = unknown/no-PTP>
urp_endpoint_owd_anomalies_total              <counter; skew-rejected samples>
```

> **Naming (PR-B2).** The offset gauge and anomaly counter ship as
> `urp_endpoint_owd_clock_offset_seconds` and `urp_endpoint_owd_anomalies_total`
> (this section's earlier sketch wrote `_clock_offset_ns` / `_clock_anomalies_total`).
> Prometheus convention is base units — **seconds**, not nanoseconds — matching the
> `_seconds` histogram families above; the kernel carries the offset in ns
> (`URP_OWD_A_CLOCK_OFFSET_NS`) and the renderer divides by 1e9.

The kernel stores **per-bucket** (non-cumulative) counts; the renderer accumulates
into `le`-ordered cumulative sums (Prometheus histogram contract) as it emits — a
running add over 14–15 entries, no allocation. `sum_ns` is divided to seconds at
render. A unit-test table asserts monotonic non-decreasing `_bucket` and
`bucket[+Inf] == _count`.

Cardinality: A adds 3×(15+2) ≈ 51 series/endpoint (behind the default-on flag); B adds
14+2+2 ≈ 18. Both fold into the existing `--max-series` cap (§39.2). Because these are
weighty, gate rendering behind exporter flags `--interarrival` (default on) and
`--owd` (default on **iff** the module reports the nest; silently skipped otherwise),
so a node without PTP/OWD simply omits family B.

---

## 40.5 Dashboard & alarms

**Grafana** (design 39 dashboard JSON, consumed from the upstream repo on `l` —
edits need merge-first, memory `design39-exporter`):

- **OWD percentiles (PR-B2, shipped)** — `histogram_quantile(0.5|0.99|0.999, sum by
  (endpoint,le) (rate(urp_endpoint_owd_seconds_bucket[$__rate_interval])))`, three
  lines; the p999 line is the low-latency SLO. Paired panels added to the design 39
  dashboard: a full-distribution OWD heatmap (a bimodal split is the fairness latch)
  and a `urp_endpoint_owd_clock_offset_seconds` + `rate(owd_anomalies_total)` panel to
  read PTP health before trusting the percentiles.
- **Inter-arrival (PR-A, shipped)** — two panels added to the design 39 dashboard:
  (1) a percentiles-by-stride timeseries — `histogram_quantile(0.5|0.99, sum by
  (endpoint,stride,le) (rate(urp_endpoint_interarrival_seconds_bucket[$__rate_interval])))`
  — putting all three strides (1/10/100) on **one panel** so the burst structure
  collapses as the stride grows; (2) a `stride="1"` heatmap of the raw bucket rates
  for the full per-frame distribution. This is the direct visualization of "trace the
  packet rates." (The OWD percentile panels above arrive with PR-B.)
- **Rate-vs-jitter overlay** — `rate(urp_endpoint_rx_frames_total)` (mean, existing)
  over the stride-100 inter-arrival p50 (should track) with the stride-1 p99 (the
  jitter envelope) shaded behind.

**Alerts** (`nix/urp-exporter-alerts.yml`, design 39 §39.9):

- `URPOwdTailHigh` (shipped, info) — `histogram_quantile(0.99, sum by (endpoint,le)
  (rate(urp_endpoint_owd_seconds_bucket[5m]))) > 0.005`. Threshold is fabric-specific
  (single-flow floor is tens of µs; the tail grows under contention / the fairness
  latch), so it ships info-level and tuneable rather than paging.
- `URPOwdClockSkew` (shipped, warning) — `rate(urp_endpoint_owd_anomalies_total[5m]) > 0`
  sustained: negative OWD ⇒ the PTP mesh drifted and the percentiles are untrustworthy.

---

## 40.6 Follow-ups (deferred out of this spec)

- **Per-QP variants** of both histograms — makes the §38.5 fairness fan-out visible as
  a per-QP latency/rate asymmetry, not just aggregate. Multiplies bucket storage by
  `num_qps` (≤32); ship endpoint-wide first, add per-QP behind a flag.
- **Native histograms** (Prometheus sparse/native) instead of classic `le` buckets —
  finer resolution at lower series cost, but needs a newer exposition format and
  Grafana support; classic buckets first for portability.
- **Kernel PTP servo read** for `owd_clock_offset_ns` — MVP takes it via sysctl from a
  userspace PTP monitor; reading the servo in-kernel is a stretch goal.
- **Fast-path (zero-copy) coverage.** The fast RX path (`urp_fast_recv_done`) has no
  pump; A/B hooks must be added to its inline disposition
  (`urp_fast_rx_disposition`) too, or the fast path silently reports no histograms.
  Scope A/B to the copy path first (where the latency spike lives), then extend.

---

## 40.7 Phasing

**PR-A — inter-arrival histogram (no protocol change). IMPLEMENTED 2026-09-04.**
`struct urp_hist15` + `struct urp_interarrival` on `urp_endpoint` (`kernel/urp.h`);
`urp_hist_bucket()` pure classifier in the shared `kernel/urp_hist.h`, table-tested
by KUnit (`test_hist_bucket`) **and** the userspace twin (`tools/urp-hist-units.c`,
nix check `urp-hist-units`, 32 checks); `urp_interarrival_sample()` RX hook in
`urp_recv_done` (once per delivered DATA frame, before the reorder-drain fan-out);
`URP_ENDPOINT_A_INTERARRIVAL` (=17) netlink nest + encoder gated by the sysctl;
`urp-netlink` `Histogram` decode + a `hist_edges_match_kernel_header` parity test;
exporter `urp_endpoint_interarrival_seconds_*` family behind `--interarrival`
(default on), zero-alloc-verified; `urp.interarrival_hist` sysctl (default on);
two Grafana panels (percentiles-by-stride + stride-1 heatmap). Kernel module builds
against mainline; `urp-netlink`/`urp-exporter` test suites + the three nix checks
green. **Still to do:** live validation on the mesh (PR-C-lite: confirm the family
populates and the exporter footprint stays within the design 39 §39.9 budget).

**PR-B1 — OWD plumbing + codec (hardware-free, zero behaviour change). IMPLEMENTED
2026-09-05.** `URP_DATA_FLAG_TSTAMP` (BIT(3)) + the 8-byte `urp_frame_tstamp_*`
trailer codec in `urp_frame.h`; `URP_CONN_CAP_TSTAMP` (1<<1) caps bit;
`urp_owd_bucket()` + `URP_OWD_NBUCKETS` (14, latency-tuned edges) in the shared
`urp_hist.h`; `struct urp_owd` on `urp_endpoint` (reuses `urp_hist15` storage);
`URP_ENDPOINT_A_OWD` (=18) UAPI attr + `enum urp_owd_attr`; `urp.latency_sample_period`
(default 64) + `urp.owd_clock_offset_ns` (advisory) sysctls. Tests: KUnit
`test_owd_bucket` / `test_tstamp_trailer` / `test_conn_priv_cap_tstamp`; userspace
twin extended (`urp-hist-units`, 57 checks). Nothing stamps or reads yet, so the
wire is byte-identical to legacy and the OWD nest is never emitted. Kernel module
builds mainline 7.2; `urp-hist-units` green. **No new fuzz harness:** the codec is a
straight-line LE64 round-trip (unit-covered); the hostile TSTAMP-parse surface lands
in PR-B2 and is already exercised by `wire_fuzz` (random-flag + truncated-frame
generation covers a set TSTAMP flag with a missing/short trailer).

**PR-B2 — OWD datapath + surface (protocol change; needs PTP to validate).
IMPLEMENTED 2026-09-05.** Wires the negotiation (`urp_local_caps` advertises
`URP_CONN_CAP_TSTAMP` when `latency_sample_period != 0`, mirroring
`window_bytes_advertise`; `urp_tstamp_negotiate` latches `ep->tstamp_negotiated` at
all-QPs-ESTABLISHED); TX stamp on sampled frames in **both** pump paths
(`urp_tx_maybe_stamp`: `seq % period == 0`, `num_chunks == 1`, cap negotiated) with
an unconditional 8-byte trailer reserve (`urp_ep_tx_max_payload`); RX delta vs
`ktime_get_real_ns()` (`urp_owd_sample`: negative → `anomalies`, else bucket; length-
guarded against a hostile flag-without-trailer); `URP_ENDPOINT_A_OWD` netlink encoder;
`urp-netlink` decode (`Owd` struct, `URP_OWD_EDGES_NS` parity test); exporter
`urp_endpoint_owd_seconds_*` + `urp_endpoint_owd_clock_offset_seconds` gauge +
`urp_endpoint_owd_anomalies_total` counter behind the default-on `--owd`/`--no-owd`
flag (self-suppresses when the module reports no nest); Grafana OWD percentile +
distribution + PTP-health panels; `URPOwdTailHigh` + `URPOwdClockSkew` alerts.
Kernel builds mainline 7.2; `urp-netlink` + `urp-exporter` render/decode tables green.

**PR-C — hardware validation.** On the PTP-synced hp1/hp2/hp3 mesh: confirm
single-flow OWD p50 ≈ the ~36 µs floor and that the p99 tail grows with concurrency
(the contention story, now measured one-way instead of via the queueing-inflated
probe); confirm the inter-arrival heatmap widens under credit-stall load; confirm
exporter goodput-delta stays within the design 39 §39.9 <1% footprint budget with both
histograms on. Append numbers here and flip Status.

---

## 40.8 How to reproduce (once implemented)

```
# unit (no hardware)
nix build -L .#checks.x86_64-linux.urp-hist-units       # owd + interarrival classifier
nix build -L .#checks.x86_64-linux.urp-exporter-tests   # render + decode tables
nix run .#ci-local
# the hostile TSTAMP-parse surface (PR-B2) rides the existing wire fuzzer, which
# already sets random flags incl. BIT(3) and truncated trailers:
#   (in the microVM tier) wire_fuzz <acceptor-ip> <port> 300

# live (module loaded, one node): inter-arrival needs no PTP
nix run .#urp-exporter -- --listen 127.0.0.1:9975
curl -s localhost:9975/metrics | grep urp_endpoint_interarrival_seconds

# OWD needs PTP across both nodes (pmc), sample period on:
sysctl urp.latency_sample_period=64
curl -s localhost:9975/metrics | grep urp_endpoint_owd_seconds
```
