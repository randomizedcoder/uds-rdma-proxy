# 36. Congestion & Flow Control — CUBIC cwnd + byte rwnd (EXPERIMENTAL)

Status: **EXPERIMENTAL — design only.** No code lands with this doc. This is a
forward-looking spec for adding a TCP-style congestion-control layer on top of the
byte flow-control window from [design 35](35-windowing-flow-control.md) §35.3. It is
marked experimental because (a) on our dedicated RoCEv2 fabric the classic loss
signal is largely masked by hardware (see §36.4), so the controller may rarely
leave slow-start/growth, and (b) we are currently **pump-bound, not
congestion-bound** (§36.1), so the congestion window will not *bind* until the
Option B pump work and Option F2 scale-out (design 35 §35.4–35.5) raise offered
load to where the fabric or receiver is actually the limit. The controller is
therefore built and enabled behind a default-off sysctl (§36.10), measured only
after B/F2.

## 36.1 Why this exists, and why "window" was ambiguous

Design 35 §35.3 specs a **flow-control** window: bound in-flight bytes to what the
*receiver* can absorb. That is the fix for the measured flood/drop instability
(design 35 §35.7.1 finding 3) — the receiver overran because the sender ignored
credits, not because the fabric was congested. It is a static, receiver-sized
limit. Call it **rwnd** (receive window), by analogy to TCP.

This doc adds the other half TCP has: a **congestion-control** window, **cwnd**,
that *dynamically* grows and shrinks based on a network congestion signal, searching
for the operating point that fills the path without collapse. Per the project
decision (2026-08-18) the controller is **CUBIC** and the congestion signal is
**loss** — RNR-NAK exhaustion and reorder-drops (§36.4).

As in TCP, the two windows **compose**; they are not alternatives:

```
effective_window = min(cwnd, rwnd)
```

The sender gate (design 35 §35.3, the blocking `wait_event` that replaced
send-anyway) blocks on `effective_window`, not on rwnd alone.

**Honest framing (why experimental / why later).** At the time of writing we are at
~51 MB/s, ~1.7 % of the 25 GbE line (design 35 §35.7.1), capped by the single-kthread
pump — copy + serialization + per-frame CQE. With the fabric ~98 % idle a cwnd
reacting to congestion has nothing to react to: it opens to its ceiling and sits
there, `cwnd ≫ rwnd`, so `min(cwnd, rwnd) = rwnd` and the controller is inert.
cwnd only starts to *bind* once Option B + F2 push offered load up to the fabric or
receiver limit. So:

- **rwnd is the correctness fix we need now** (design 35 §35.3, phase order).
- **cwnd is a real but later layer**, designed here so it slots in as `min(cwnd, rwnd)`
  with no rework, and measured once B/F2 make us congestion-bound.

## 36.2 Relationship to design 35

Design 35 §35.3 ("the byte-based windowing protocol") **is the rwnd half** of this
design. Nothing there changes except its framing: the per-stream `window_bytes`
becomes the *receive* window `rwnd`, and the sender gate's limit becomes
`min(cwnd, rwnd)` instead of `window_bytes`. Everything else in §35.3 —
cumulative-absolute byte grants, the self-healing `max()`, the new CONTROL
sub-type, the reorder coupling, the sysctl sizing — stands and is a prerequisite
for this doc. Design 35 §35.4 (pump completion waitqueue, **done**) and §35.5 (F2)
are also prerequisites for cwnd to *bind*.

This doc supersedes design 35 §35.1's claim that "windowing is not the speed lever."
That is true of *rwnd on a tiny BDP*. A *cwnd* is, in principle, a speed lever — but
only once the offered load reaches the congestion point, which today it does not.

## 36.3 The two windows

### 36.3.1 rwnd — flow control (design 35 §35.3, recap)

Per-stream `u64 rwnd` (= design 35 `window_bytes`), sized from receiver reorder /
buffer capacity and BDP (design 35 §35.3 "Sizing"). The receiver advances a
**cumulative-absolute** `rx_bytes_delivered` and grants it back; the sender does
`tx_bytes_acked = max(tx_bytes_acked, granted)`. `in_flight = tx_bytes_sent −
tx_bytes_acked`. This half is loss-tolerant by construction (a lost grant is healed
by the next absolute grant). **No change from design 35.**

### 36.3.2 cwnd — CUBIC congestion control (new)

Per-stream `u64 cwnd` in **bytes**, plus the CUBIC state (§36.5). Two regimes,
exactly as TCP:

- **Slow start** while `cwnd < ssthresh`: `cwnd += bytes_acked` per ACK (exponential
  per RTT).
- **Congestion avoidance** at `cwnd ≥ ssthresh`: grow along the CUBIC curve (§36.5).
- **On a loss episode**: multiplicative decrease `ssthresh = cwnd × β`,
  `cwnd = ssthresh`, remember `W_max`, restart the CUBIC epoch (§36.5).

"MSS" for the byte math is the endpoint max frame payload (`urp_ep_max_payload`,
`kernel/urp.h:109`).

### 36.3.3 The effective window

```
avail = min(cwnd, rwnd) − (tx_bytes_sent − tx_bytes_acked)
```

The design 35 §35.3 sender gate blocks until `avail ≥ len` (or stop/FIN). A byte
grant (ACK) can wake the gate by advancing `tx_bytes_acked` (rwnd side) **and** by
growing `cwnd` (cwnd side); a loss episode shrinks `cwnd` but never blocks the
gate — it only lowers the ceiling for future sends.

## 36.4 The congestion signal: loss (RNR-NAK / reorder-drops)

The project chose a **loss-based** CUBIC (as opposed to delay/`rtt_ewma` or ECN/
DCQCN). On this stack "loss" has two observation points, one per side:

**Sender-observed — send-completion error.** `urp_send_done`
(`kernel/urp_rdma.c:415`) already inspects `wc->status`; anything other than
`IB_WC_SUCCESS` / `IB_WC_WR_FLUSH_ERR` is a hard failure. On an RC QP with
`rnr_retry_count = 7` (`urp_rdma.c:278,903`) the hardware silently retries transient
RNR, so a completion *error* means retries were exhausted or the QP went to error —
a strong, rare loss signal.

**Receiver-observed — reorder-drops (the dominant signal here).** The receiver bumps
`ep->stats.reorder_drops` (`kernel/urp_rdma.c:508`) when a frame falls outside the
reorder window, and drops frames on SRQ / buffer exhaustion. On a near-lossless PFC
fabric this receiver-side drop — from path skew (design 34 §34.5.1 finding 2, the
qps>1 collapse) or receiver overflow — is the **main** thing that looks like
congestion. It is observed at the receiver, so it must be **fed back** to the sender.

**Feedback path.** Extend design 35 §35.3's CREDIT-BYTES CONTROL sub-type to carry a
second cumulative-absolute counter alongside `rx_bytes_delivered`:

```
CONTROL / CREDIT-BYTES payload (little-endian):
  [0..8)   rx_bytes_delivered  u64   (design 35 — the rwnd grant)
  [8..16)  rx_loss_count       u64   (design 36 — cumulative receiver drops)
```

The sender keeps `last_rx_loss_count`; on each control frame, `delta = rx_loss_count
− last_rx_loss_count`. `delta > 0` means the receiver dropped frames since the last
report → **one loss episode** (not one-per-drop — see dedup below).

**Loss-episode dedup (once per RTT, like TCP's "once per window").** Reacting to
every dropped frame would collapse cwnd on a single skew burst. Snapshot a
`recover` mark = `tx_bytes_sent` at the moment of a decrease; ignore further loss
signals until `tx_bytes_acked > recover` (a full window has been acked since). This
is TCP NewReno's recovery gate, in bytes.

**Honest caveat — the signal may be rare.** PFC + `rnr_retry_count = 7` make the
fabric close to lossless; DCQCN in the NIC may already be doing ECN-based congestion
control *below* us. So a loss-based cwnd may spend almost all its time growing and
never decrease — behaving like a large fixed window bounded by rwnd. That is
acceptable (it degrades to rwnd, which is correct), but it means:

- CUBIC's decrease path is **hard to exercise in normal operation** → we must test it
  with injected loss / deliberate qps>1 reorder skew (§36.11).
- A loss-based controller **stacked on hardware DCQCN** is two congestion loops on one
  path; interactions (oscillation, under-utilization) are an open risk (§36.12). A
  delay- or ECN-based controller would avoid the double loop, but the project chose
  loss for a first cut — revisit if measurement shows pathology.

## 36.5 CUBIC math (port Linux `tcp_cubic.c`, integer / fixed-point)

"Copy the TCP CUBIC style implementation" = port Linux's `net/ipv4/tcp_cubic.c`
algorithm, in bytes, with **no floating point** (kernel rule). The reference is the
canonical integer CUBIC; we mirror it field-for-field so a KUnit suite can be
diffed against known CUBIC vectors, exactly as `urp_credit` is a 1:1 Rust port
diffed in KUnit (`kernel/urp_credit.c` header).

Per-stream CUBIC state (mirrors `struct bictcp`):

```
u64 cwnd;            /* current congestion window, bytes                */
u64 ssthresh;        /* slow-start threshold, bytes                     */
u64 w_max;           /* window just before the last loss (W_max)        */
u64 w_last_max;      /* previous w_max, for fast convergence            */
u64 epoch_start_ns;  /* CLOCK_MONOTONIC at start of the current epoch   */
u64 origin_point;    /* K-origin window (bytes)                         */
u64 K_ns;            /* time to reach origin from epoch start           */
u64 tcp_cwnd;        /* Reno-friendly shadow window (bytes)             */
u64 recover;         /* tx_bytes_sent snapshot at last decrease         */
```

Constants (Linux defaults): `C = 0.4` (growth aggression), `β = 0.7`
(`beta = 717/1024`), `fast_convergence = 1`, `C` encoded via `bic_scale` /
`cube_rtt_scale` fixed-point as in the reference. Time base = the per-QP
`rtt_ewma_ns` (`urp_rdma.c`, the value fed the §36.7 clock) rather than
jiffies/`BICTCP_HZ`.

- **CUBIC growth (congestion avoidance).** `W_cubic(t) = C·(t − K)³ + W_max`, with
  `K = cbrt( W_max·(1 − β) / C )`. On each ACK compute the target from
  `t = now − epoch_start` and step `cwnd` toward it (bounded by the per-ACK
  increment, `cwnd_cnt` accumulation, as in the reference). Integer cube root via a
  Newton/bit helper (port `cubic_root()` from `tcp_cubic.c`).
- **Reno-friendly region.** Maintain `tcp_cwnd` (AIMD) and take
  `cwnd = max(cwnd_cubic, tcp_cwnd)` so CUBIC is never slower than Reno on
  short/shallow paths (the reference's `bictcp_update` TCP-friendliness).
- **Loss (decrease).** `epoch_start = 0` (force recompute); fast convergence:
  `if cwnd < w_last_max: w_last_max = cwnd; w_max = cwnd·(1 + β)/2` else
  `w_last_max = w_max = cwnd`; then `ssthresh = max(cwnd·β, 2·MSS)`,
  `cwnd = ssthresh`, snapshot `recover = tx_bytes_sent`.
- **Slow start.** While `cwnd < ssthresh`: `cwnd += bytes_acked` (clamped to
  `rwnd`), no CUBIC update.

All arithmetic in `u64` bytes; the fixed-point scaling factors come straight from
the Linux reference so the KUnit vectors match a known-good CUBIC.

## 36.6 Integration with the pump gate (design 35 §35.4)

The design 35 §35.3 gate is the only touch-point on the send path:

```c
/* design 35 gate, extended: limit is min(cwnd, rwnd) */
u64 win  = min(READ_ONCE(cc->cwnd), READ_ONCE(cc->rwnd));
u64 flt  = cc->tx_bytes_sent - cc->tx_bytes_acked;
if (len > win - flt) {
    wait_event_interruptible(stream->credit_wq,
        ({ u64 w = min(READ_ONCE(cc->cwnd), READ_ONCE(cc->rwnd));
           (w - (READ_ONCE(cc->tx_bytes_sent) - READ_ONCE(cc->tx_bytes_acked))) >= len; })
        || kthread_should_stop() || READ_ONCE(stream->fin));
    ...
}
cc->tx_bytes_sent += len;
```

The RX control-frame handler (design 35 §35.3 receiver-grant apply, extended):

```c
/* rwnd side (design 35) */
tx_bytes_acked = max(tx_bytes_acked, rx_bytes_delivered);
/* cwnd side (design 36) */
u64 acked_delta = tx_bytes_acked - prev_acked;
if (loss_delta(rx_loss_count) > 0 && tx_bytes_acked > recover)
    urp_cubic_on_loss(cc);          /* multiplicative decrease + new epoch */
else if (acked_delta)
    urp_cubic_on_ack(cc, acked_delta, now_ns);  /* slow-start or CUBIC growth */
wake_up_interruptible(&stream->credit_wq);
```

Both windows advance/shrink in the RX path; the TX path only reads them (plus writes
`tx_bytes_sent`). See §36.8 for the concurrency change this forces.

## 36.7 RTT and timing

CUBIC needs a monotonic clock for the epoch and an RTT estimate for slow-start /
TCP-friendliness. We already track a per-QP EWMA RTT (`rtt_ewma_ns`, `urp_rdma.c`,
fed by the QP health PONGs, design 08a). cwnd is **per-stream** (to match the
per-stream gate and reorder space); it samples RTT from the stream's current QP.
`epoch_start_ns` / `now_ns` use `ktime_get_mono_fast_ns()` (already used on the TX
path, `urp_pump.c` timekeeping include).

## 36.8 State placement & concurrency (differs from design 35's lockless claim)

Design 35 §35.3 argued the rwnd accounting is lockless: the TX kthread is the sole
writer of `tx_bytes_sent`, the RX path the sole writer of `tx_bytes_acked`, so
`READ_ONCE`/`WRITE_ONCE` suffice. **cwnd breaks that**: on an ACK the RX path *grows*
cwnd, and there is no TX writer of cwnd — but the CUBIC update reads several fields
and writes several (`cwnd`, `w_max`, `epoch_start`, `tcp_cwnd`), and the TX gate
reads `cwnd` concurrently. A torn read of a multi-field update could mis-gate.

Resolution: put the whole congestion/flow state in one **`struct urp_cc`** guarded by
a per-stream `spinlock_t cc_lock`, taken briefly around (a) the RX-path CUBIC update
+ grant apply and (b) the TX-path `avail` computation. This mirrors TCP doing all
cwnd work under the socket lock. The `wait_event` predicate takes the lock (or reads
a single `WRITE_ONCE`-published `effective_avail` the RX path recomputes on each
update — lighter, avoids the sleeper taking a spinlock in the predicate). The design
prefers the **published-snapshot** variant: the RX path, under `cc_lock`, writes
`WRITE_ONCE(cc->effective_avail_gen, ++gen)` and a published `min(cwnd,rwnd)`; the TX
predicate reads those with `READ_ONCE` and needs no lock. (Detail pinned at
implementation time against the real gate.)

## 36.9 Wire format & interop

One wire change beyond design 35: the CREDIT-BYTES CONTROL payload grows from 8 to
16 bytes to carry `rx_loss_count` (§36.4). It stays behind design 35's connect-time
capability gate; add a distinct feature bit (e.g. `URP_FEAT_CC_CUBIC` vs design 35's
`URP_FEAT_BYTE_WINDOW`) so three peer combinations degrade cleanly:

| local \ peer | best-effort | rwnd only | rwnd+cubic |
|--------------|-------------|-----------|------------|
| best-effort  | today       | today     | today      |
| rwnd only    | today       | rwnd      | rwnd       |
| rwnd+cubic   | today       | rwnd      | rwnd+cubic |

cwnd engages only when **both** peers advertise `URP_FEAT_CC_CUBIC`; otherwise it
falls back to rwnd-only (design 35) or best-effort (today). The extended CONTROL
sub-type gets the same bench-twin + differential-fuzzer treatment as design 35
(`tools/urp-bench-core.*` ↔ `crates/urp-bench/`, shared hex vectors) so C and Rust
stay byte-identical.

## 36.10 Configuration (experimental gating)

New sysctl `urp.cc_mode` (follow the `register_sysctl("urp", …)` pattern at
`kernel/urp_sysctl.c:42`):

| `cc_mode` | behaviour |
|-----------|-----------|
| `0` off (default) | today's best-effort credits — no change |
| `1` rwnd | design 35 byte flow control only (`cwnd = ∞`) |
| `2` cubic | **experimental** — `min(cwnd, rwnd)`, loss-based CUBIC |

Default **off** keeps the experimental path dark until explicitly enabled. Companion
knobs: `urp.rwnd_bytes` (design 35 sizing), `urp.cwnd_init` (initial cwnd, default a
few × MSS), `urp.cwnd_min` (floor, `2 × MSS`). All live-tunable.

## 36.11 Phasing & verification

1. **rwnd** — design 35 §35.3 (the correctness fix; independent of this doc).
2. **CUBIC as a pure primitive** — `kernel/urp_cubic.{c,h}`, a byte-CUBIC port of
   `tcp_cubic.c` with **no** kernel infra (like `urp_credit`), + KUnit diffed against
   known CUBIC vectors: slow-start ramp, CUBIC concave/convex growth, decrease + fast
   convergence, TCP-friendly floor, once-per-RTT loss dedup, `min(cwnd, rwnd)`. Pure,
   sandbox-testable via `nix run .#ci-local`. No wiring, no behaviour change.
3. **Loss feedback wire** — extend the CREDIT-BYTES CONTROL sub-type (+`rx_loss_count`),
   capability bit, bench-twin + fuzzer vectors. No behaviour change until step 4.
4. **Wire cwnd into the gate** — `effective = min(cwnd, rwnd)`, RX-path ACK/loss
   drives CUBIC, behind `cc_mode = cubic`. `cc_lock` / published-snapshot (§36.8).
5. **Hardware measurement — only after Option B + F2** (design 35 §35.4–35.5) raise
   offered load to the congestion point. Gates: (a) with injected loss / qps>1 skew,
   cwnd visibly backs off and recovers (CUBIC curve observable in a stat); (b) no
   collapse / no permanent stall; (c) aggregate goodput vs `ib_write_bw` improves or
   holds vs rwnd-only. Before B/F2, cwnd stays `≫ rwnd` and is inert by design.

Verification gates (mirror design 35 §35.7): KUnit for the CUBIC core + the
min()/dedup logic; differential fuzzer for the extended CONTROL sub-type;
`nix run .#ci-local` green; hardware on hp1↔hp3 with a loss-injection knob (the
qps>1 reorder skew is a ready-made loss source, design 34 §34.5.1).

## 36.12 Risks & open questions

- **Near-lossless fabric → inert controller.** PFC + `rnr_retry=7` may mean cwnd
  never decreases in normal operation (§36.4). Then CUBIC ≈ fixed window = rwnd, and
  all the machinery is dead weight until a real loss regime appears (heavy F2 skew,
  oversubscription). Mitigation: keep it experimental/default-off; only invest
  further if measurement shows loss actually occurs.
- **Double control loop with DCQCN.** A software loss-based cwnd on top of hardware
  ECN congestion control can oscillate or under-utilise. Open question whether to
  disable one; a delay/ECN-based urp controller would avoid the conflict but was not
  the chosen first cut.
- **RNR masking.** `rnr_retry_count = 7` hides the transient loss CUBIC wants to see.
  Consider exposing an RNR counter or lowering the retry count *for the experimental
  mode* so the signal is legible — but that trades away the current stability.
- **Per-stream cwnd vs shared fabric.** N streams (F2) each run an independent cwnd;
  aggregate can over-drive a shared bottleneck (the classic N-flow TCP fairness/
  synchronisation issue). May need a shared/endpoint-level cwnd or a coupling term.
- **Fixed-point cube-root precision** in the byte-domain port; validated by the KUnit
  vectors against the Linux reference.
- **Loss granularity.** `reorder_drops` is a per-endpoint counter today
  (`urp_rdma.c:508`), not per-stream; per-stream loss attribution for a per-stream
  cwnd needs the counter (or the fed-back `rx_loss_count`) scoped per stream.

## 36.13 Relation to other docs

- [Design 35](35-windowing-flow-control.md) §35.3 — the rwnd half; prerequisite.
  §35.4 (pump, done) + §35.5 (F2) — prerequisites for cwnd to *bind*.
- [Design 34](34-bulk-throughput.md) §34.5.1 — the post-bound verdict and the qps>1
  reorder-skew loss source used to exercise CUBIC.
- [Design 08a](08a-qp-health-probes.md) — the `rtt_ewma_ns` this controller clocks on.
- `kernel/urp_credit.c` — the 1:1-port + KUnit-diff pattern `urp_cubic` follows.
