# 34. Bulk Throughput & Windowing

Status: **Design + measurement harness.** The one-way streaming benchmark
(§34.4) and the Phase-0 measurement (§34.5) are being built now; the data-path
changes (§34.3 options B–F) are designed here and phased as follow-ups, each
gated on what the measurement shows. No data-path logic changes land with this
doc.

## 34.1 Motivation — bandwidth is now a first-class goal

The design 32 real-hardware matrix ([32-performance-results.md](32-performance-results.md))
peaks at **~165 MB/s (~1.3 Gbps) — about 5 % of the 25 GbE link**. That is *by
design*: `urp-bench` ([design 30](30-urp-bench-io-uring.md)) is a **symmetric
RTT echo** — a message completes only when its echo returns, so throughput is
hard-capped at `batch × msg_size / RTT`. It measures **latency**, and the
number quoted there is explicitly "well under 25 GbE by design — latency, not
bandwidth, is what this tests."

But the target workload has a second regime. **Redpanda/Kafka inter-node
replication** synchronizes large volumes of log data between cluster nodes:
sustained, large, one-directional transfers where the figure of merit is
**goodput**, not tail latency. urp is meant to carry that traffic, so "how
close to line rate can a bulk stream get, and what limits it?" is a
first-class question this doc answers — with (a) an instrument that measures
one-way bulk goodput and (b) a phased design for the data-path changes that
raise it.

This is a *different optimization target* from the low-latency small-message
path, and the two can conflict (batching/coalescing trade latency for
bandwidth). The design keeps both regimes explicit.

## 34.2 Diagnosis — urp is frame-rate-bound, not byte-bound

Tracing one payload from an app `write()` to the peer app `read()` in the
default MULTISTREAM mode:

- **TX:** app `write()` → AF_UNIX sockbuf (copy 1) → the **per-stream TX
  kthread** `urp_stream_tx_fn` (`kernel/urp_pump.c:210-299`) does one
  `kernel_recvmsg` into a DMA send slot (copy 2), `urp_frame_encode` writes the
  20-byte header in place, `urp_qp_select_round_robin` picks a QP, `urp_post_frame`
  posts `IB_WR_SEND` with `IB_SEND_SIGNALED` (`urp_pump.c:28-50`).
- **Wire:** two-sided RDMA SEND into a peer SRQ recv slot (copy 3).
- **RX:** `urp_recv_done` (`kernel/urp_rdma.c:510-734`, on an `IB_POLL_WORKQUEUE`)
  decodes, `urp_reorder_insert` copies out/in (copy 3b), `urp_rx_send_uds`
  `kernel_sendmsg` into the peer AF_UNIX sockbuf (copy 4) → peer `read()` (copy 5).

Five key facts bound throughput:

1. **One serial TX kthread per stream.** All the per-frame work — copy, encode,
   QP select, post — is serial on a single thread. `num_qps` only stripes on the
   *wire* (round-robin), not host-side copy/post.
2. **Every send is signaled** (`IB_SEND_SIGNALED`, `urp_pump.c:44`) → **one CQE
   per frame**, reaped on a workqueue in `urp_send_done` (`urp_rdma.c:404-419`).
   A send slot cannot be reused until its CQE is reaped, so completion latency
   gates the frame rate.
3. **Credits do not block.** The window is `num_bufs/2` **frames**
   (`urp_credit_init`, `kernel/urp_credit.c`); `urp_credit_consume` returns
   `-EAGAIN` at 0 but the pump **sends anyway** (`urp_pump.c:279-280`) — credits
   are a *stat*, not a gate. There is **no true windowing**; the RC
   `rnr_retry_count=7` plus send-pool exhaustion are the only backstops.
4. **1 ms poll on pool-empty.** When the send pool drains, the kthread
   `schedule_timeout_interruptible(1ms)` (`urp_pump.c:231`) and retries — a
   coarse stall floor.
5. **Two-sided SEND only** (never one-sided WRITE), so every payload is copied
   into a registered recv slot (copy 3). Defaults: **1 QP**, **buffer_size 4096**
   (4076-byte payload; max 65516), buffer_count 1024 split **static 50/50** →
   512 send + 512 recv buffers.

**The arithmetic:** the symmetric-echo peak of 165 MB/s ÷ 4076 B ≈ **~40k
frames/s** put a first bound on the serial kthread. But the one-way stream
measurement (§34.5.1) came in **far lower — ~1000 frames/s for 64 KB frames,
~2000–2900 for 4 KB** — a ~40× gap from that estimate. The echo peak is a
pipelined RTT number; the one-way blocking pump hits a much lower ceiling, and
~1000 fps for large frames is suspiciously close to the **1 ms pool-empty poll**
(fact 4): once the 512-slot send pool drains faster than per-frame CQEs are
reaped off the workqueue (fact 2), the kthread sleeps 1 ms and trickles. So the
dominant one-way limiter is **completion/poll latency, not copy bandwidth** — the
wire (25 GbE ≈ 3.125 GB/s) is <2 % used. Two levers dominate:

- **Bytes per frame** — amortize the fixed per-frame cost (bigger `buffer_size`,
  coalesced reads). Confirmed: 4 KB→64 KB lifts goodput ~8× (§34.5.1).
- **Remove serialization** — the single kthread, the per-frame CQE, and
  especially the **1 ms poll** (replace with a completion-driven waitqueue).

§34.5.1 measured this: goodput scales with frame size while frame rate does not,
i.e. **post/serialization-bound** — so pump (Option B) + scale-out (F2) come
before copy-elimination (D/E). The windowing that keeps that faster pump from
re-flooding is §34.6.

## 34.3 Option ladder — raising bulk throughput

Each option: mechanism · expected ceiling · effort · risk · pros · cons ·
correctness interactions.

### Option A — Config / tuning only (effort S, risk low)

Drive the existing netlink SET knobs to better points; no logic change:
`buffer_size 4096→65536` (16× bytes/frame → projects toward ~2.5 GB/s at the
same frame rate *if the app delivers large writes*), `buffer_count 1024→8192`
(deeper pool/SRQ/CQ), `num_qps 1→4-8` (wire striping). One S-effort logic tweak
is worth pulling forward: an **asymmetric send/recv pool split** — the static
50/50 at `urp_rdma.c:149` strands half the pool for a one-way sender.

- **Ceiling:** ~2–3 GB/s for large writes; near-zero for small writes (a 64 KB
  slot carrying 4 KB wastes 15/16 of the amortization).
- **Pros:** immediate, near-zero code, reversible, quantifies the headroom for
  every other option.
- **Cons:** doesn't fix the serial kthread or per-frame CQE; **both ends must be
  reconfigured together** — the recv slot is posted at `ep->buf_size`, so a
  64 KB frame into a 4 KB slot triggers `URP_RX_DROP_PAYLOAD_OVERRUN`. Footprint
  is `num_bufs × buf_size` (8192 × 64 KB = **512 MB/endpoint**) — choose
  deliberately. `buffer_size=65536` → order-4 compound pages in the page_pool;
  high-order allocs can fail under fragmentation (already fails activation
  cleanly).
- **Interactions:** no protocol change; bigger frames make the existing
  256-frame reorder window cover far more bytes (a free win).

### Option B — Optimize the two-sided SEND pump in place (effort M, risk medium)

Implements the still-unbuilt [design 13](13-performance.md) throughput roadmap,
four coupled changes in `urp_pump.c` + `urp_rdma.c`:

1. **Selective signaling** — signal every N (e.g. 16) instead of every WR; on the
   signaled CQE, `urp_send_done` frees the whole batch of N slots (tracked in
   post order per QP). Cuts CQE traffic and workqueue wakeups ~16×.
2. **Multi-WR `ib_post_send` lists** — chain `wr->next` and post many frames in
   one doorbell instead of one `ib_post_send` per frame (`urp_pump.c:282`).
3. **Coalesce many bytes per wakeup into large frames** — loop `kernel_recvmsg`
   while data is available, fill big slots, post as one WR list. With Option A's
   64 KB slot each syscall/copy moves far more bytes.
4. **Completion-driven waitqueue** — replace the 1 ms poll (`urp_pump.c:231`)
   with a waitqueue the send-completion path (`urp_send_done`, `urp_rdma.c:418`)
   wakes when slots free. Removes the stall floor.

- **Ceiling:** several-× frame rate; target **8–15+ GB/s** for large writes,
  until a single kthread's copy bandwidth becomes the next wall (→ Option F).
- **Pros:** no wire change, peer-compatible, attacks the actual bottleneck.
- **Cons:** batch-reclaim correctness under teardown/flush; a per-QP batch ring;
  effective in-flight depth per QP must be ≥ N or the sender self-stalls.
- **Interactions:** reorder eased by fewer/larger frames; credit orthogonal
  (still best-effort until Option C); teardown must force-signal/flush the
  trailing partial batch (the existing "always free on flush" invariant,
  `urp_rdma.c:413-418`, covers flush completions — keep it).

### Option C — True windowing (effort M, risk medium-high)

Replace best-effort credits with a **byte-aware blocking window** (§34.6). Makes
the sender wait when the peer is behind, instead of oversending into a drained
SRQ (which today causes RNR-retry storms — the exact hazard documented at
`urp_rdma.c:568-571`).

- **Ceiling:** does not raise peak — makes it **sustainable** (removes the
  RNR-storm sawtooth, bounds reorder pressure). This is what lets B's peak hold.
- **Pros:** smooth sustained rate; an operator BDP knob (sysctl).
- **Cons:** real flow-control protocol work; deadlock risk if a grant is lost and
  grants aren't cumulative; **must be co-designed with reorder sizing**.
- **Interactions:** the **critical constraint** — the reorder window (256 frames,
  `urp_stream.c:169`) must be ≥ the in-flight window in frames, or bulk traffic
  hits `reorder_drops`. §34.6 couples the two.

### Option D — One-sided RDMA WRITE into a peer-registered ring (effort L, risk high)

Add an `IB_WR_RDMA_WRITE(_WITH_IMM)` data path alongside SEND (SEND stays for
control/credit/probe). The receiver pre-registers a ring of large buffers and
exchanges `{remote_addr, rkey, geometry}` at connect; the sender writes straight
into a peer ring slot — **no SRQ recv buffer, no recv-side decode-into-buffer
copy** (removes copy 3/3b). The ring tail **is** the byte window (subsumes C).

- **Ceiling:** highest in-kernel option — **10–20 GB/s** for large transfers;
  residual limiter becomes the two UDS copies (copies 1/2 on TX, 4 on RX).
- **Pros:** removes the fundamental recv-copy + SRQ ceiling; unifies flow control
  with the ring.
- **Cons:** MR lifecycle, rkey exchange, slot-ownership races, `WITH_IMM`
  completion accounting, interop negotiation (must fall back to two-sided),
  teardown fencing of in-flight WRITEs before `ib_dereg_mr`.
- **Interactions:** `WITH_IMM` gives in-order per-QP delivery; multi-QP still
  reorders by seq (keyed off imm data). Window = ring tail (byte-aware natively).

### Option E — Zero-copy `io_uring_cmd` fast path (effort L, largest) = design 31

As specified in [31-urp-fast-zero-copy.md](31-urp-fast-zero-copy.md): the app
registers its own buffer into `urp.ko` via `io_uring_cmd`; urp pins/maps it as an
MR and RDMA-WRITEs **app memory → peer app memory** — no AF_UNIX, no
`kernel_recvmsg`/`kernel_sendmsg`, **no software copy at all**. Already in flight
(design 31 PR1 merged, PR2 landing).

- **Ceiling:** true zero-copy — **line rate / NIC-bound**.
- **Pros:** the ultimate ceiling; removes every payload copy.
- **Cons:** needs app-side io_uring integration (Redpanda is a Seastar app — see
  [31a-seastar-cpp-demo.md](31a-seastar-cpp-demo.md)); pinned-memory lifetime and
  security surface ([appendix-rocev2-security.md](appendix-rocev2-security.md)).
- **Interactions:** bypasses AF_UNIX entirely; reorder/credit reframed around
  registered app buffers. Builds naturally on Option D's ring + windowing.

### Option F — Host-side parallelism

- **F1 (multiple TX kthreads per stream)** fights stream ordering: concurrent
  `kernel_recvmsg` on one AF_UNIX socket reorders bytes, and `stream->tx_seq++`
  (`urp_pump.c:266`) races. A locking critical section around recv+seq-assign
  serializes the copy (the actual bottleneck), defeating the purpose. **Low value
  for an ordered stream.**
- **F2 (stripe one transfer across N streams)** — N UDS connections = N streams,
  each with its own kthread/reorder/credit state; the app shards across them.
  **Kafka partitions already are independent streams**, so this maps naturally.
  Near-linear scaling, reuses existing multistream machinery, per-stream state
  already isolated (no split-brain).
- **Ceiling:** F2 multiplies B's per-kthread ceiling by N → plausibly line rate
  at 4–8 streams. **Effort:** F1 M (not worth it); F2 S–M on the urp side (mostly
  workload/harness sharding). **Risk:** F2 low; F1 high.

## 34.4 The measurement instrument — one-way streaming in `urp-bench`

The bench's io_uring *mechanism* (`blocking`/`uring-rw`/`uring-fixed`/
`uring-bufring`/…) is orthogonal to its app *protocol* (echo vs one-way stream),
so throughput measurement is added as a new **pattern**, not a new mode:

**`--pattern {echo,stream}`** (default `echo` = zero behavior change). Composes
with every io_uring mode, so we learn which mechanism best fills the pipe. It
reuses the 24-byte header ([§30.5](30-urp-bench-io-uring.md)) as-is — stream
frames simply never set the ECHO bit.

- **connect side = source:** blast originals (ECHO clear) as fast as backpressure
  allows for `--duration`/`--count`; **no RTT tracking**; send a header-only FIN
  at the end.
- **listen side = sink:** drain and count bytes, **never echo**, complete on
  peer-FIN or `peer_closed`, and report **goodput = bytes_received / receive
  window** (first byte → FIN). Goodput is **sink-measured** because urp
  backpressure is implicit — the source `write()` blocks when the AF_UNIX sockbuf
  fills (`urp_pump.c:231`), so source-side timing overcounts buffered-but-
  undelivered data.
- The one real logic change is the completion path: stream drops the
  `own_fin_echoed` requirement (there is no echo). The pure `bench_run_done`/
  `done_core` predicate gains a stream variant; the classifier
  (`on_msg`/`classify_msg`) skips echo emission on the sink and the tracker/RTT
  path on the source.
- **Output:** the same `BENCH_OK key=value` line; in stream mode `mbps`/
  `msgs_per_s` mean sink-received bytes/msgs and the RTT fields are empty (already
  tolerated under `--duration`). The key set is unchanged, so existing harnesses
  keep parsing.

Both twins move in lockstep (shared hex vectors + differential fuzzer): C
(`tools/urp-bench-core.{h,c}`, `urp-bench.c`, `urp-bench-test.c`) and Rust
(`crates/urp-bench/src/{config,shell,uring,main}.rs`). The new pure logic —
pattern parse, one-way completion predicate, sink-goodput math — gets
table-driven P/N/B/C tests in both.

This is the harness [design 31 §31.8](31-urp-fast-zero-copy.md) anticipated: the
same tool measures the AF_UNIX path today and the zero-copy fast path later, so
every option in §34.3 is scored on one yardstick.

## 34.5 Phase-0 measurement (before any kernel change)

A new runner `nix run .#urp-bw-matrix -- hp1 hp3 10.10.2.1` (modeled on
`nix/urp-hw-matrix.nix`, driven over SSH from `l`) sweeps the Option-A knobs and
records the bulk ceiling:

1. Reconfigure **both** endpoints to the swept `buffer_size`/`buffer_count`/
   `num_qps` (buffer_size is add-time → stop the endpoint unit, `urp remove` +
   `urp add`, on hp1 **and** hp3 together; restore after). Sweep write size and
   `--pattern stream` msg-size; optional N-stream (Option F2) fan-out.
2. Start the sink on hp1 **first** and confirm its socket is bound (in
   `/proc/net/unix`) **before** running the source on hp3 — the acceptor
   connects its connect-path UDS lazily on the stream's first frame and does not
   retry (§34.5.1 finding 3), so a source that starts before the sink loses the
   stream. Then **scrape the sink's goodput**, filtered to the run's systemd
   invocation id (unit names repeat across sweeps, so a plain `-u <unit>` scrape
   can grab a stale prior-run `BENCH_OK`).
3. Capture counters per run and report **per-run deltas** (the counters are
   cumulative with no reset). `/proc/urp/<ep>/stats` exposes only
   `tx_bytes`/`rx_bytes`/`tx_frames`/`rx_frames`; the `credit-stalls` and
   `reorder-drops` counters live in the netlink CLI (`urp stats <ep>`), so the
   runner snapshots that before/after each run. `reorder_drops>0` or an acceptor
   `rx-frames` that fails to advance marks a run as non-clean (oversend / window
   mis-size / non-delivery).
4. Report goodput, **achieved source frame rate** (`Δtx-frames`/s), delivered
   frames (`Δrx-frames`), and % of the baselines.

**Baselines** (same hosts/link/MTU, back-to-back): **`iperf2`** (TCP) = the
"link saturates" reference, staged by nix-copy and started as a transient unit
(poll-until-listening before the client connects). **`ib_write_bw`** (the
upstream `perftest` project) = the fabric ceiling and the direct target for
options D/E — it is **not packaged in nixpkgs**, so the runner uses whatever
`ib_write_bw` is on the host PATH and skips the RDMA baseline when absent (as it
is on hp1/hp3 today). The gap between urp goodput and `ib_write_bw` is the
software overhead urp must close.

**The decisive verdict:** if frame rate stays flat across the `buffer_size`
sweep, urp is **frame-rate/serialization-bound** → prioritize B/F. If frame rate
scales with size but goodput stays flat, it is **copy-bound** → prioritize D/E.
This verdict steers Phase 1.

## 34.5.1 Phase-0 results (hp1 ↔ hp3, 25 GbE RoCEv2)

`urp-bench --pattern stream --mode blocking`, sink-measured goodput. `fps` =
achieved source frame rate (Δ`tx-frames`/s). A run is *clean* when
`reorder-drops == 0` and the acceptor's `rx-frames` (successful UDS deliveries)
advances by roughly the sink's message count.

Goodput is shown in both MB/s (bytes) and Mb/s (bits) — the link is rated in
bits, so the **% of 25 GbE** column is the one to watch. 25 GbE line rate =
**25 000 Mb/s = 3125 MB/s**.

| buffer_size | num_qps | goodput (MB/s) | goodput (Mb/s) | % of 25 GbE | fps      | reorder-drops | reassembled | notes |
|-------------|---------|----------------|----------------|-------------|----------|---------------|-------------|-------|
| 4096        | 1       | 6.6–9.7        | 53–78          | 0.21–0.31 % | 1900–2900| 0             | 97 %        | clean |
| 16384       | 1       | 9.4–9.6        | 75–77          | 0.30–0.31 % | 700–900  | 0             | 99 %        | clean |
| 65516       | 1       | **52.7** (best)| **421.6**      | **1.69 %**  | ~1040    | 0             | 100 %       | clean when it completes (see finding 1) |
| 4096/16384/65516 | 4  | — did not complete — | | | 170k+ | 1.37 M | — | single-stream reorder overflow (finding 2) |

Ranges are real run-to-run spread, not measurement slop — see finding 1.

**Baselines (same link, back-to-back):** `iperf2` single-stream TCP =
**~1900 MB/s = 15.2 Gb/s = ~61 % of 25 GbE** (single flow; jumbo / multi-stream
would push higher). `ib_write_bw` unavailable (not in nixpkgs / not on the
hosts), so the raw-RDMA fabric ceiling is not yet measured.

**Verdict — post/serialization-bound, not copy-bound.** Goodput scales strongly
with bytes-per-frame (4 KB→64 KB lifts it ~6.6→52.7 MB/s ≈ 8×) while the
single-kthread frame rate stays in the ~700–2900 fps band and does **not** grow
with frame size (large frames run ~1000 fps). The path is limited by the fixed
per-frame cost of the serial TX pump (§34.2), not by copy bandwidth. urp's best
one-way goodput (52.7 MB/s = **421.6 Mb/s = 1.69 % of the 25 Gb/s link**) is also
only **~2.8 % of single-stream iperf2**. → **Phase 1 = Option B (pump), then F2
(scale-out); C makes it sustainable *and reliable* (finding 1); D/E (copy
elimination) stay deferred until the copy wall is the measured limiter.**

Three findings sharpen the phasing:

1. **No backpressure → unreliable completion and noisy goodput; this is a
   *correctness* case for Option C, not just a throughput one.** With best-effort
   credits (send-anyway, §34.2) the sender never blocks: in a probe the *source*
   printed `mbps = 2849, msgs_per_s = 43492` while the acceptor received **zero**
   — it was clocking its own local-UDS write speed and flooded ~25 GB that the
   ~1000 fps wire could never drain. The excess (including the header-only **FIN**)
   is dropped, so a bulk transfer **flakily fails to complete** — 65516/q1 hit
   52.7 MB/s on one pass and delivered-but-never-finished on the next (the sink
   saw thousands of frames, never the FIN). The goodput ranges above are that
   instability. **Option C (true byte-blocking windowing, §34.6) removes the
   flood**: it is the difference between "bulk transfer sometimes hangs" and
   "bulk transfer always completes," on top of smoothing throughput.

2. **num_qps > 1 breaks a single ordered stream — reorder-window overflow.**
   Every qps=4 point failed to deliver (`rx-frames` advanced by ~1; `reorder-drops`
   ≈ 1.37 M): 4-QP arrival skew blows past the 256-frame reorder window
   (`urp_stream.c:169`) so almost nothing drains in order. Raw multi-QP striping
   of one ordered stream is a dead end at the current window size — this is direct
   evidence for §34.6's **mandatory reorder-coupling** and for **F2 (one QP per
   independent stream, e.g. per Kafka partition)** over striping. (An earlier pass
   showed a 30 MB/s "qps=4" number; it was a stale-journal artifact — the fixed
   runner now scopes the scrape to the run's systemd invocation id.)

3. **There is no cold-boot kernel bug — the requirement is "consumer up before
   first frame."** A reboot to get pristine numbers first looked like broken
   delivery (streams in `syn-received`, `connect … failed: -2`, `rx-frames = 0`,
   QP flap). Reading the code (and re-testing) showed otherwise: `syn-received`
   is normal (SYN is a one-shot flag on the first data frame; delivery works from
   that state); the QP flap was leftover churn from repeated `urp remove/add`;
   and — the real point — the acceptor connects its connect-path UDS **lazily on a
   stream's first frame with no retry** (`urp_rdma.c:658` → `urp_socket.c:166`), so
   any frame that arrives before the local consumer is listening is dropped as
   `buffer_alloc_fails` (**not** `rx-frames`, which is why it read 0). With a
   fresh endpoint (no stale streams) and the sink **confirmed bound before the
   source starts**, delivery is clean (`reorder-drops = 0`, `buffer_alloc_fails =
   0`, 97–100 % reassembled). The runner now enforces this ordering (fresh
   endpoints per point → sink bound-confirmed in `/proc/net/unix` → then source),
   scopes the journal scrape to the run's invocation id, and checks `rx-frames`
   actually advanced. A real deployment (an app listening on the connect-path from
   boot) satisfies the ordering naturally; the one robustness gap worth a small
   fix is **adding retry/backoff to the acceptor's backend-UDS connect** so a
   late-starting consumer recovers instead of losing a stream's frames.

## 34.5.2 Zero-copy fast-path measurement — copy vs zero-copy (design 31 PR5)

The Phase-0 numbers above (§34.5.1) are the **copy path** (`AF_UNIX` pump, 4
copies/frame). Design 31's zero-copy `io_uring_cmd` fast path (Option E) is now
built and merged (PR1–PR4), and `urp-bench` grew a `--mode uring-cmd` backend
(PR5a) so the *same* bench core drives both paths. This section is the measured
answer to the founding question — *how different is zero-copy vs copy on
throughput, latency, syscalls, and CPU/memory pressure?*

Same boxes, same 25 GbE RoCEv2 link, back-to-back (hp1 acceptor ↔ hp3 initiator,
2026-08-19). Copy path = `--mode uring-rw`; zero-copy = `--mode uring-cmd`
against a dedicated `--kind fast` endpoint pair (NIC DMAs straight into/out of
the app's pinned pool — 0 software copies). Runners: `.#urp-fast-bw-matrix`
(goodput) and `.#urp-fast-hw-matrix` (RTT).

**Performance summary — copy vs zero-copy (headline table).** One row per axis,
worst→best frame size; higher is better for goodput, lower for the rest:

| metric | frame | copy path | zero-copy | improvement |
|--------|-------|-----------|-----------|-------------|
| one-way goodput  | 4 KB  | 6.6–9.7 MB/s        | **47.5 MB/s**   | ~5–7×  |
| one-way goodput  | 16 KB | 9.4–9.6 MB/s        | **585 MB/s**    | **~62×** |
| one-way goodput  | 64 KB | 52.7 MB/s (1.7% line)| **2133 MB/s (68% line)** | **~40×** |
| RTT p50          | 24 B  | 34.7 µs             | **22.9 µs**     | 1.5×   |
| RTT p50          | 64 KB | 321 µs              | **78 µs**       | **4.1×** |
| RTT p99          | 64 KB | 408 µs              | **95 µs**       | 4.3×   |
| syscalls / msg   | 64 KB | 9.14                | **1.00**        | 9×     |
| CPU µs / msg     | 64 KB | 93.0                | **4.2**         | **~22×** |

The single takeaway: **at 64 KB the fast path moves ~40× the bytes at ~4× lower
latency for ~22× less CPU per message** — and the goodput advantage grows with
frame size (4 copies → 0, §31.7). Full per-size tables and method below.

**One-way bulk goodput (`--pattern stream`, batch=16, sink-measured).** Copy
column is §34.5.1's `--mode blocking` best; the win grows with frame size exactly
as design 31 §31.7 predicts (4 copies → 0):

| msg_size | copy MB/s (§34.5.1) | zero-copy MB/s | zero-copy Mb/s | % of 25 GbE | **speedup** | fast syscalls/msg | fast cpu µs/msg |
|----------|---------------------|----------------|----------------|-------------|-------------|-------------------|-----------------|
| 4 KB     | 6.6–9.7             | **47.5**       | 380            | 1.5 %       | ~5–7×       | 0.19              | 0.91            |
| 16 KB    | 9.4–9.6             | **585.2**      | 4 682          | 18.7 %      | **~62×**    | 0.48              | 1.91            |
| 64 KB    | 52.7 (best)         | **2133.2**     | 17 066         | **68.3 %**  | **~40×**    | 0.50              | 2.00            |

At 64 KB zero-copy reaches **68 % of line rate** (17.1 Gb/s) vs the copy path's
1.69 % — and vs the §34.5.1 single-stream iperf2 baseline (~1900 MB/s), the fast
path is now **~112 %** of TCP goodput on a single flow. Small frames (4 KB) stay
low: with the copy eliminated the residual cost is per-frame post/CQE (§34.2), so
the zero-copy 4 KB point is still post-bound, not copy-bound — the fast path wins
where the copy actually dominated (large frames), as expected.

**Round-trip latency + syscall/CPU cost (`--pattern echo`, batch=1, C↔C,
single-clock; PTP offset −25.7 µs bounds only the RTT/2 one-way estimate).**
Both paths measured this session:

| msg_size | copy p50/p99 µs | zero-copy p50/p99 µs | **RTT speedup** | copy syscalls·cpuµs /msg | zero-copy syscalls·cpuµs /msg |
|----------|-----------------|----------------------|-----------------|--------------------------|-------------------------------|
| 24 B     | 34.7 / 57.1     | **22.9 / 30.7**      | 1.5×            | 1.04 · 3.07              | 1.00 · 3.32                   |
| 1 KB     | 56.3 / 64.8     | **25.9 / 31.1**      | 2.2×            | 1.04 · 4.44              | 1.00 · 4.04                   |
| 4 KB     | 52.5 / 75.6     | **28.9 / 34.5**      | 1.8×            | 1.40 · 7.57              | 1.00 · 4.09                   |
| 64 KB    | 321.3 / 408.0   | **78.4 / 94.5**      | **4.1×**        | **9.14 · 93.02**         | 1.00 · **4.22**               |

**Verdict — zero-copy lifts the ceiling the copy path was pinned under, and the
gap is a function of frame size.** Three measured stories:

1. **Throughput:** 64 KB goodput jumps **52.7 → 2133 MB/s (~40×)**, from 1.69 %
   to 68 % of 25 GbE. The copy path was post/serialization-bound (§34.5.1) *and*
   copy-bound at large frames; removing the copies uncorks the large-frame case.
2. **Latency:** zero-copy RTT is lower at every size and the advantage widens
   with bytes — **4.1× at 64 KB** (78 µs vs 321 µs) — because the copy path pays
   memcpy + AF_UNIX datagram fragmentation on both legs of the round trip.
3. **Syscalls & CPU/memory pressure:** the killer number. At 64 KB the copy path
   fragments a message across **9.14 syscalls** and spends **93 µs of CPU per
   message**; the zero-copy path holds a flat **1 syscall / 4.2 µs per message
   (~22× less CPU)** at any size, and its buffer pool is pinned **once** at
   REGISTER rather than churned per message — the "flat-memory" story design 31
   §31.7 promised, now measured.

Caveats: single flow, single QP, `verify=none` for goodput / `verify=full` for
the RTT sweep (copy 64 KB echo shows 74.6 % reassembled under the no-backpressure
flood — §34.5.1 finding 1 — a copy-path artifact, not a fast-path one; the fast
path delivered cleanly). `maxrss` not scraped; `cpu_us/msg` is the CPU/memory-
pressure proxy here. These validate Option E as the decisive copy-elimination
lever for the large-frame (Redpanda/Kafka replication) workload — while windowing
(§34.6 / Option C) remains the orthogonal fix for *reliable completion* on both
paths.

## 34.6 The windowing function (designed; built in a later phase)

> **Implementation-ready spec: [design 35](35-windowing-flow-control.md).** That
> doc expands this summary with the wire format + interop gate, the pump
> completion-waitqueue coupling (where the throughput actually comes from), the
> BDP sizing math (window need is only ~116 KB at this link — windowing is a
> correctness/efficiency layer, not the raw-speed lever), F2 scale-out, and the
> phasing/verification. This section is the short version.

Replaces the 4×u16 **frame** counters in `struct urp_credit`
(`kernel/urp_credit.h:19`) with per-stream **byte** accounting:

- `u64 window_bytes` — max unacknowledged bytes in flight (the tunable limit).
- `u64 tx_bytes_sent` — cumulative bytes handed to `ib_post_send`.
- `u64 tx_bytes_acked` — cumulative bytes the peer confirms delivered to its UDS.
- `in_flight = tx_bytes_sent − tx_bytes_acked`.

**Sender gate** (replaces the best-effort consume at `urp_pump.c:279-280`):

```
avail = window_bytes − (tx_bytes_sent − tx_bytes_acked)
if len > avail:
    wait_event(stream->credit_wq, avail >= len || stop || fin)
tx_bytes_sent += len
post frame
```

The `wait_event` removes both the fake credit stat and the send-anyway behavior
that causes RNR storms. A blocked sender must wake on FIN/RST/`kthread_should_stop`.

**Receiver grant** (extends `urp_rdma.c:562-589`): after each successful
`urp_rx_send_uds` delivers `len`, advance a per-stream **cumulative absolute**
`rx_bytes_delivered`; piggyback that absolute value on the next reverse frame,
batched when `rx_bytes_delivered − last_granted ≥ window_bytes/4` (mirroring
today's threshold). The sender applies `tx_bytes_acked = max(tx_bytes_acked,
granted)` and `wake_up(credit_wq)`.

**Why cumulative/absolute:** a lost CREDIT frame must not permanently shrink the
window (the split-brain hazard). Absolute counters are self-healing — the next
grant carries the true total; a monotonic `max()` makes duplicates/reorderings
harmless. Incremental deltas do not have this property.

**Sizing:** `window_bytes = clamp(sysctl urp.window_bytes, MIN, MAX)`, default
`BDP × factor`, where BDP = per-QP `rtt_ewma_ns` (`urp_rdma.c:610`) × link rate,
factor ~8–16 → ~1 MB. A **new sysctl** in `kernel/urp_sysctl.c` (today only
connect-retry knobs) exposes it.

**Mandatory reorder coupling:** size `urp_reorder_alloc(0, max_buffered)`
(`urp_stream.c:169`, currently 256) from `ceil(window_bytes / min_frame_payload)`
so the window can never overflow reorder. With 64 KB frames a 1–4 MB window is
only 16–64 frames — safe; the formula guards the small-frame danger.

**Concurrency:** the credit path stays single-writer-per-direction (sender writes
`tx_bytes_sent`, RX writes grants), so it remains lock-light — only the `avail`
read + waitqueue need `READ_ONCE`/a barrier plus the wake. Atomics are needed
only if F1 (multi-TX-per-stream) is ever pursued — another reason F1 is
unattractive.

## 34.7 Recommended phased path

0. **Measure & tune** — the harness (§34.4) + Option A sweep + the asymmetric
   pool-split tweak (§34.5). Decisive: copy-bound vs post-bound. *(This doc's
   deliverable.)*
1. **Pump optimization** (Option B) — biggest structural win, no wire change.
2. **True windowing** (Option C, §34.6) — makes B's peak sustainable, co-designed
   with reorder sizing.
3. **Scale-out** (Option F2) — multiply across streams/cores; cheap given the
   existing multistream machinery.
4. **One-sided WRITE ring** (Option D) — when the recv-copy/SRQ is the measured
   wall; fold windowing into the ring tail.
5. **Zero-copy io_uring** (Option E / design 31) — the endgame for true line
   rate, once an app-side integration is justified.

A and B are pure host-side wins with no protocol/interop cost and attack the
actual (frame-rate/serialization) bottleneck; C makes them sustainable; D/E chase
the remaining copies but carry protocol/interop/security cost and app changes, so
they come after measurement proves the copy wall is what's left.

## 34.8 Relation to other docs

- [design 35](35-windowing-flow-control.md) — the implementation-ready windowing
  + pump + scale-out spec that §34.6 summarizes; the path from this doc's
  measurement to line rate.
- [design 30](30-urp-bench-io-uring.md) — the bench and its symmetric-echo RTT
  protocol; this doc adds the orthogonal one-way `stream` pattern (§34.4) as a
  second measurement regime.
- [design 31](31-urp-fast-zero-copy.md) — the zero-copy fast path (Option E); it
  removes *copies*, this doc's windowing/pump work removes *serialization/flow-
  control* limits. They compose: even zero-copy won't saturate 25 GbE with a
  512-frame, per-frame-signaled, single-kthread pipeline.
- [design 13](13-performance.md) — the historical throughput roadmap (send
  batching, signal-every-16, inline) that Option B finally implements.
- [design 05](05-rdma-transport.md) — the current credit state machine that
  §34.6 replaces with byte-aware blocking windows.
- [design 32](32-performance-results.md) — the latency baseline (~165 MB/s = ~5 %
  of 25 GbE) that motivates this work.
