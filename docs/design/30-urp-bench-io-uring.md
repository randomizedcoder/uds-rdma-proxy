# 30. urp-bench: io_uring UDS Benchmark — Copy Cost vs Syscall Batching (C + Rust)

Status: **design / not started** — 2026-08-12.

A symmetric userland benchmark app pair (`urp-bench`, implemented twice: C +
liburing, Rust + the `io-uring` crate) that drives the UDS side of the tunnel
with io_uring — registered buffers, batched submissions — and sweeps
message size × batch size × io_uring mode to produce a performance matrix.
The goal is to answer, with data, the open question from
[design 20 §20.1](20-future-work.md#201-kernel-bypass-for-uds-side): can
io_uring remove the copying overhead on the UDS side, and how much does
syscall batching buy?

## 30.1 Motivation and hypothesis

The kernel module already eliminated the proxy-side copies (4 → 2, see
[design 21](21-kernel-module.md)): `urp.ko` reads the UDS straight into
DMA-mapped buffers and posts them to the NIC. What remains on each host is the
**application's own copy** — `write(2)` copies the user buffer into the UDS
skb, `read(2)` copies it back out — plus two syscalls per message per side.

io_uring is the obvious candidate to attack both, and the historical
userspace-era docs already analysed it
([06](06-uds-io-uring.md), [13](13-performance.md),
[21 §21.5](21-kernel-module.md), [02](02-architecture.md)). Their conclusion,
which this design treats as the **hypothesis to be tested rather than folklore
to be repeated**:

| Cost | io_uring mechanism | Removable? |
|---|---|---|
| syscalls (2/msg/side) | batch N SQEs per `io_uring_enter`; SQPOLL → ~0 | **yes** — this is the headline win |
| per-op buffer import/validation | registered (fixed) buffers, registered files | **yes** (small, measurable) |
| RX wakeup/re-arm per message | multishot recv + provided buffer rings | **yes** (RX-side overhead, not the copy) |
| the user↔skb byte copy itself | `IORING_OP_SEND_ZC` / `MSG_ZEROCOPY` | **no** — not supported on `AF_UNIX`; the kernel copies anyway |

So the honest expectation is: io_uring **does not** "pass the buffer to the
kernel module" on `AF_UNIX` — the copy stays — but it removes essentially all
of the *per-message syscall and bookkeeping* overhead around the copy. For
small messages (syscall-dominated) the win should be large; for large messages
(memcpy-dominated) the curves should converge on memory bandwidth. The matrix
in §30.7 is designed so each effect is isolated and quantified separately
(§30.3), including a `sendzc` probe mode whose only job is to *record the
evidence* of what the kernel actually does with zero-copy sends on `AF_UNIX`
on the kernel under test.

Secondary hypothesis: the C and Rust implementations, driving identical
io_uring feature sets over an identical wire protocol, should be
**performance-identical within noise**. Any sustained gap is a defect in the
slower implementation (missed feature flag, extra copy, allocation in the hot
loop) — the benchmark doubles as a cross-language conformance test, and
C↔Rust on-the-wire interop (§30.14) doubles as a live differential test of
the framing code, in the spirit of the F0 track of
[design 27](27-fuzz-testing.md).

## 30.2 Where we are today — copy and syscall inventory

End-to-end path of one application message, initiator → acceptor
(`App A → urp.ko → RDMA → urp.ko → App B`), with every copy and syscall:

| # | What | Where | Removable by this work? |
|---|---|---|---|
| S1 | `write(2)` syscall, App A | app | **yes** — amortized by batching (this doc) |
| C1 | user buf → UDS skb | inside App A's `write(2)` | **no** (`AF_UNIX` has no zero-copy TX; §30.1) |
| C2 | UDS skb → DMA-mapped slot | `kernel/urp_pump.c:85` (legacy) / `kernel/urp_pump.c:237` (stream) `kernel_recvmsg` | out of scope (kernel side; already optimal — lands directly in the RDMA buffer) |
| C3/C4 | reorder-buffer memcpys (insert + drain) | `kernel/urp_reorder.c:206` / `kernel/urp_reorder.c:238` | out of scope — and currently **inert**: the reorder buffer is not yet wired into the RX path (design-29 gap, see [status.md](../../status.md)) |
| C5 | DMA slot → UDS skb | `kernel/urp_rdma.c:442` `kernel_sendmsg` | out of scope (kernel side) |
| S2 | `read(2)` syscall, App B | app | **yes** — amortized by batching (this doc) |

Symmetric costs apply on the echo leg. Today's UDS traffic generators —
`socat` in the microVM pair test (`nix/microvms/lib.nix`, Phase 7) and
plain blocking I/O — pay S1/S2 in full, one syscall per message per
direction, and give us **no instrumentation** of that cost.

One tunnel interaction the matrix must exercise: the TX pump reads at most
`max_payload = buffer_size − 20` bytes per `kernel_recvmsg`
(default **4076**, ceiling **65516** — `kernel/urp.h`
`urp_ep_max_payload()`), so an application message larger than `max_payload`
is split across multiple RDMA frames and reassembles as arbitrary chunk
boundaries at the far UDS. The message sizes in §30.7 deliberately straddle
both boundaries.

## 30.3 Experiment design — separating the effects

Seven modes, chosen so that pairwise subtraction isolates one effect at a
time. `blocking` is the non-io_uring control that today's tools effectively
use; every claim about "what io_uring buys" is a delta against it.

| Mode | Mechanism | syscalls/msg | per-op import | copy | Isolates (vs previous row) |
|---|---|---|---|---|---|
| `blocking` | `read(2)`/`write(2)` loop | 2 | yes | yes | control |
| `uring-rw` batch=1 | `IORING_OP_RECV`/`SEND`, 1 SQE per enter | ~2 | yes | yes | io_uring fixed overhead (should be ≈ control or slightly worse) |
| `uring-rw` batch=N | same, N×2 SQEs per enter | ~2/N | yes | yes | **syscall batching** |
| `uring-fixed` | `READ_FIXED`/`WRITE_FIXED` on `IORING_REGISTER_BUFFERS` | ~2/N | **no** | yes | buffer import/validation cost |
| `uring-bufring` | multishot `RECV` + `IORING_REGISTER_PBUF_RING` provided buffers | lowest RX | no | yes | RX re-arm/wakeup cost |
| `uring-sqpoll` | `IORING_SETUP_SQPOLL` kernel submitter | → 0 | no | yes | the syscall floor (at the cost of a spinning kthread) |
| `uring-sendzc` | `IORING_OP_SEND_ZC` probe | n/a | n/a | **records it** | evidence: `-EOPNOTSUPP`, or completion with the copied-fallback notification (`IORING_NOTIF_USAGE_ZC_COPIED` in the notif CQE `res`) — either way `AF_UNIX` still copied |

Cross-cutting toggles (not full matrix dimensions, to contain the cell
count):

- `--defer-taskrun` — sets `IORING_SETUP_DEFER_TASKRUN |
  IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN` (kernels ≥ 6.1);
  run the smoke subset once with and once without, report the delta.
- Registered files (`io_uring_register_files`) are used in **all** uring
  modes — the fd-lookup saving is uninteresting to isolate.
- `--memcpy-baseline` — a self-test that measures raw `memcpy` MB/s at every
  message size in the matrix on the same machine, printed as
  `BENCH_MEMCPY msg_size=… mbps=…`. This is the yardstick that lets a
  reviewer attribute cpu-time-per-byte to "the copy" vs "everything else".

Modes that the running kernel cannot support (PBUF_RING needs ≥ 5.19,
multishot recv ≥ 6.0, DEFER_TASKRUN ≥ 6.1, SQPOLL may be restricted by the
`kernel.io_uring_disabled` sysctl) are **skipped loudly**:
`BENCH_SKIP mode=… reason=…` — never silently downgraded, so a matrix cell
is either measured honestly or visibly absent.

## 30.4 Topologies

Same binary, role chosen by flags; both topologies use the identical app.

**T1 — direct (baseline, one host, no urp.ko):**

```
 urp-bench --id 1 --connect /tmp/urp-bench.sock        urp-bench --id 2 --listen /tmp/urp-bench.sock
        |                                                        |
        +----------------- AF_UNIX SOCK_STREAM ------------------+
```

Clean substrate: no RDMA, no VM, no kernel module — pure UDS + io_uring.
This is where the C-vs-Rust comparison and the mode deltas are measured
without confounders, and where CI smoke runs.

**T2 — tunneled (two VMs, through urp.ko over RDMA):**

```
 VM1                                                VM2
 urp-bench --id 1                                   urp-bench --id 2
   --connect /tmp/urp-pair.sock                       --listen /tmp/urp-pair-echo.sock
        |                                                  ^
        v                                                  | (urp.ko connects out per stream)
   urp.ko (listen_path) <==== RDMA/RoCEv2 RC ====> urp.ko (connect_path)
```

`urp-bench --listen` takes over the echo-backend role that `socat …
EXEC:cat` plays today in the pair test (`nix/microvms/lib.nix` Phase 7) —
but unlike `socat` it also *generates* its own traffic and instruments
everything.

**Symmetric full-duplex roles.** Both instances simultaneously (a) generate
their own message stream and (b) echo every non-echo message they receive.
There is no "client" and "server" in the data plane — only in who
connects/listens. Each side therefore measures RTT on its own messages while
serving as the reflector for the peer's, and both directions of the tunnel
are loaded at once (which is exactly the regime the per-QP credit loop and
the TX pumps see in production, and which one-directional tools never
exercise).

## 30.5 Message framing — the testable, fuzzable core

The UDS is a transparent byte stream: `urp.ko` adds nothing app-visible
(framing is created by the TX pump's read size and destroyed before
`kernel_sendmsg` — [design 04](04-framing-protocol.md)). The bench therefore
needs its own minimal framing to delimit messages on a stream, tell originals
from echoes, and carry timing. This little protocol — encode, decode,
incremental deframing, echo tracking — is deliberately designed as the pure,
side-effect-free core that unit tests (§30.11) and fuzzers (§30.12) chew on.

**`bench_hdr` — 24 bytes, little-endian** (LE to match the urp wire header
convention, [design 04 §4.2](04-framing-protocol.md)):

```
 off len field        notes
  0   4  magic        "URPB" = 0x42505255 (LE u32); hard error on mismatch
  4   1  version      1; hard error on mismatch
  5   1  flags        bit0 ECHO, bit1 FIN, bits 2–7 reserved, must be zero
  6   2  origin_id    u16, unique per instance (--id, assigned by the harness)
  8   4  payload_len  u32, bytes following the header; cap = msg_size − 24,
                      absolute cap 1 MiB (BENCH_PAYLOAD_MAX) — reject above
 12   4  seq          u32, per-origin monotonically increasing
 16   8  t_send_ns    u64, sender's CLOCK_MONOTONIC at submit; opaque to the
                      echoer, returned unchanged → RTT needs no cross-host
                      clock sync (same trick as the urp PING/PONG probes,
                      design 08a)
```

`msg_size` in the matrix always means **total wire bytes including the
header**; the minimum cell, 24, is a header-only message with
`payload_len = 0` — a deliberate boundary case.

Semantics:

- **Payload fill/verify.** Deterministic xorshift32 stream seeded by
  `(origin_id << 16) ^ seq` — the receiver can verify any message without the
  sender storing copies. `--verify {none,header,full}`: perf cells run
  `header` (magic/version/flags/caps/len checks only); smoke cells run `full`
  (byte-exact payload check). Stated policy: **`full` is never used in a cell
  whose numbers are reported** — verification cost must not contaminate the
  copy measurement.
- **Echo.** On receiving a complete message with ECHO clear: set the ECHO bit
  in place and transmit the same bytes back **out of the receive buffer** —
  no userspace copy on the reflect path (this is precisely where fixed and
  provided buffers genuinely earn their keep). On receiving a message with
  ECHO set: match `(origin_id, seq)` in the tracker, record
  `now − t_send_ns` as an RTT sample, recycle the buffer.
- **Deframing/reassembly.** Stream chunking is arbitrary — especially through
  the tunnel, where the TX pump re-chunks at `max_payload` (§30.2). The
  deframer is an incremental state machine (`WANT_HDR` → `WANT_PAYLOAD`) that
  accepts arbitrary chunk boundaries; a message spanning receive buffers is
  copied into an assembly buffer and **counted** — reported as
  `reassembled_pct` so this real cost is visible in the results rather than
  silently folded in.
- **No resync.** A malformed header (bad magic/version/reserved flags/cap
  violation) is a hard error that fails the cell (`BENCH_FAIL`). The
  transport is a trusted, reliable byte stream; scanning for the next magic
  would only mask real corruption (contrast with the hostile-wire posture of
  design 27 — the *bench protocol* is not a trust boundary, but its parser
  gets fuzzed anyway because it is cheap to do so).
- **Shutdown.** After its last original message each side sends a header-only
  FIN. A side exits cleanly when: its own FIN echo has returned, AND the
  peer's FIN has been seen (and echoed), AND its echo tracker is empty. A
  `--duration`-scaled timeout guard turns a wedged run into `BENCH_FAIL
  reason=timeout` rather than a hang.
- **Deadlock freedom.** Both sides write concurrently, so both sockets' buffers
  can fill. Invariant: **receives are always posted/served before new writes
  are queued**, and writes are submitted with the ring's async backpressure
  (never a blocking loop that starves the read side). The window accounting
  (§30.6) caps outstanding originals so the sum of in-flight bytes stays
  bounded.

## 30.6 The app

One binary, ~flat CLI:

```
urp-bench (--listen PATH | --connect PATH) --id N
          --mode {blocking,uring-rw,uring-fixed,uring-bufring,uring-sqpoll,uring-sendzc}
          --msg-size BYTES --batch N
          (--count N | --duration SECS)
          [--verify {none,header,full}] [--defer-taskrun] [--memcpy-baseline]
```

Event loop (uring modes), per iteration:

```
 1. harvest CQEs:
      recv completions  -> feed bytes to deframer
                           -> complete originals: queue echo write (in place)
                           -> echoes: tracker match, RTT sample, recycle buffer
      send completions  -> release/recycle send slots
 2. top up own originals to window W = batch (respecting --count/--duration)
 3. re-arm receives (not needed in multishot/bufring mode)
 4. ONE io_uring_enter submitting everything queued (<= 2*batch SQEs)
```

SQ ring sized `4 × batch` (own sends + echoes + receives + slack); CQ ring
`2 ×` that. The single submit point is wrapped in `bench_submit()`, which is
also where the syscall counter lives (§30.8).

**The matrix runner is an outer script, not in-app.** Each matrix cell runs a
fresh pair of processes with exactly one configuration — process isolation
means no warm-state bleed between cells, a crashed cell fails only itself,
and `bench_config` stays a pure, table-testable value. Per-cell startup cost
(~tens of ms) is noise against the ≥ 1 s cell duration. This matches the
Phase 10f geometry-sweep precedent in [BENCHMARKING.md](../BENCHMARKING.md).

## 30.7 The matrix

| Dimension | Values | Why these |
|---|---|---|
| `msg_size` | 24, 64, 256, 1024, **4076**, 4096, 16384, **65516**, 131072 | 24 = header-only floor; 4076 = default per-frame `max_payload` (fits one RDMA frame exactly); 4096 = just over it (forces a 20-byte second frame when tunneled); 65516 = the absolute frame ceiling; 131072 = always multi-frame |
| `batch` | 1, 4, 16, 64, 256 | 1 = no batching (isolates io_uring fixed cost); 256 = deep pipeline |
| `mode` | the 7 modes of §30.3 | effects separation |
| `lang` | c, rust | conformance race |

- **Smoke subset (CI, per topology):** `msg_size ∈ {24, 4076, 65516}` ×
  `batch ∈ {1, 32}` × `mode ∈ {blocking, uring-rw, uring-fixed}`,
  `--verify full`, ~1 s/cell → 18 cells per language, ~40 s per language.
- **Full matrix (nightly / manual):** all cells, 2 s/cell, `--verify header`
  → 9 × 5 × 7 = 315 cells per language, ~20 min per language in the direct
  topology. For the tunneled topology the mode set is trimmed to
  `{blocking, uring-rw, uring-fixed}` (recommended default, §30.16 Q1) since
  the tunnel adds ~fixed cost per cell and the mode deltas are already
  measured in T1.

Unsupported cells produce `BENCH_SKIP` lines (§30.3), so every matrix
position is accounted for in the output — measured, failed, or skipped, never
missing.

## 30.8 Measurement methodology

Per cell (one process pair, one config):

| Metric | How |
|---|---|
| `msgs_per_s`, `mbps` | completed own-originals over wall clock (echo leg counted once — a message is "done" when its echo returns) |
| `p50_us`, `p99_us`, `min_us`, `max_us` | RTT samples from `t_send_ns`; fixed-size sample array, qsort + index — the exact house idiom of `tools/urp-test-client.c:377` (latency mode) |
| `syscalls_per_msg` | in-code counter around `bench_submit()` (and around `read`/`write` in blocking mode); denominator = own msgs + echoed msgs. liburing may issue extra enters for CQ waits — the counter counts *actual* `io_uring_enter` invocations, and validation runs cross-check externally with `perf stat -e raw_syscalls:sys_enter` |
| `cpu_us_per_msg` | `getrusage(RUSAGE_SELF)` utime+stime delta over the cell / messages |
| `reassembled_pct` | deframer counter: messages that spanned receive buffers (§30.5) |

**Output grammar** — one machine-parseable line per cell on stdout, the house
convention consumed by expect/grep harnesses (cf. `BIGFRAME_OK` scraping in
`nix/microvms/lib.nix`):

```
BENCH_OK lang=c mode=uring-fixed msg_size=4076 batch=32 msgs=100000 \
  mbps=812.4 msgs_per_s=209000 p50_us=9.8 p99_us=22.1 min_us=7.9 max_us=310.0 \
  syscalls_per_msg=0.13 cpu_us_per_msg=1.9 reassembled_pct=0.4 verify=header
BENCH_FAIL lang=… mode=… msg_size=… batch=… reason=…
BENCH_SKIP lang=… mode=… reason=no_pbuf_ring
BENCH_MEMCPY msg_size=4076 mbps=18234.0
```

The matrix runner aggregates `BENCH_OK` lines into two artifacts: the
msg_size × batch table per mode, and a C-vs-Rust delta table
(`(rust − c)/c` per cell) whose expected value is **≈ 0**; sustained deltas
beyond noise fail the conformance expectation and get investigated as bugs
(§30.1). Results land in [BENCHMARKING.md](../BENCHMARKING.md) next to the
buffer-geometry numbers — including, prominently, the **big caveat** that
emulated-VM numbers are not real performance; the direct topology on a real
host is the meaningful substrate for the io_uring deltas.

## 30.9 C implementation (work items B1, B3)

Two-layer split, same dual-compile trick as `kernel/urp_frame.h` (compiled
into module, fuzzers, and test client alike):

**B1 — pure core, `tools/urp-bench-core.{c,h}`** — compiles **without
liburing** (no `#include <liburing.h>` anywhere in it), so unit tests and
fuzzers stay hermetic and sandbox-safe. All functions small,
side-effect-free, table-testable:

| Function | Contract | Test notes |
|---|---|---|
| `void bench_hdr_encode(const struct bench_hdr *h, uint8_t out[BENCH_HDR_SIZE])` | fixed 24-byte LE layout | roundtrip vs decode; shared hex vectors (§30.11) |
| `int bench_hdr_decode(const uint8_t *buf, size_t len, struct bench_hdr *out)` | 0 or `-BENCH_ESHORT/-BENCH_EMAGIC/-BENCH_EVERSION/-BENCH_EFLAGS/-BENCH_ECAP` | negative/boundary table |
| `void bench_fill_payload(uint8_t *dst, size_t len, uint16_t origin, uint32_t seq)` | xorshift32, seed `(origin<<16)^seq` | determinism, seed independence |
| `int bench_verify_payload(const uint8_t *p, size_t len, uint16_t origin, uint32_t seq)` | 0 / `-BENCH_ECORRUPT` at first bad byte | flip-one-byte cases |
| `int bench_deframe_feed(struct bench_deframer *d, const uint8_t *chunk, size_t len, bench_msg_cb cb, void *ctx)` | incremental WANT_HDR/WANT_PAYLOAD state machine; partial-header staging + assembly buffer; hard error, no resync | **primary fuzz surface**; chunk-split tables |
| `int64_t bench_track_echo(struct bench_tracker *t, uint16_t origin, uint32_t seq, uint64_t now_ns)` | RTT ns, or `-BENCH_EUNKNOWN/-BENCH_EDUP`; outstanding-window bitmap; u32 seq wrap correct | wrap/dup/unknown tables |
| `unsigned bench_batch_plan(const struct bench_batch *b, unsigned inflight, unsigned remaining)` | how many originals to queue now; never exceeds window or remaining | end-of-run boundary tables |
| `int bench_config_validate(const struct bench_config *c)` | full negative surface: msg_size < 24 / > cap, batch 0 / > 1024, mode string, listen+connect exclusivity | negative tables |
| `int bench_bufring_take(struct bench_bufring *r)` / `void bench_bufring_recycle(struct bench_bufring *r, unsigned idx)` | provided-buffer bookkeeping over an index array (note: PBUF_RING buffers are echoed via address-based SEND — the send side has no "fixed index") | exhaustion/double-recycle tables |
| `void bench_stats_add(struct bench_stats *s, uint64_t rtt_ns)` / `bench_stats_finalize` | fixed sample array, qsort percentiles | known-distribution tables |
| `int bench_format_result(const struct bench_stats *, const struct bench_config *, char *buf, size_t n)` | emits the `BENCH_OK …` line, `-ENOSPC` on truncation | golden-string cases |

**B3 — io_uring shell, `tools/urp-bench.c`** — ring setup per mode, the
event loop of §30.6, `bench_submit()` wrapper, feature probing
(`io_uring_get_probe`, opcode checks → `BENCH_SKIP`), blocking-mode fallback
loop. Thin by design; covered by integration runs, not unit tests.

**Build — `nix/urp-bench.nix`**, modeled line-for-line on
`nix/urp-test-client.nix` (lib.fileset source closure, `$CC -Wall -Wextra
-O2`, `meta.mainProgram`), package attr `urp-bench-c`:

```nix
{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "urp-bench";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [ ../tools/urp-bench.c ../tools/urp-bench-core.c ../tools/urp-bench-core.h ];
  };
  buildInputs = [ pkgs.liburing ];        # NEW dependency, first use in the repo
  buildPhase = ''
    $CC -Wall -Wextra -O2 -o urp-bench tools/urp-bench.c tools/urp-bench-core.c -luring
  '';
  installPhase = ''mkdir -p $out/bin; cp urp-bench $out/bin/'';
  meta.mainProgram = "urp-bench";
}
```

## 30.10 Rust implementation (work items B2, B4)

New workspace crate **`crates/urp-bench`** (lib + bin). Library modules
mirror the C core one-to-one so the test tables and fuzz targets pair up:
`frame.rs`, `deframe.rs`, `tracker.rs`, `stats.rs`, `batch.rs`, `config.rs`,
`report.rs` — all safe Rust, no io_uring imports, miri-clean (they join the
existing `run-miri` devshell job). The unsafe backend lives in `uring.rs` +
`main.rs` only.

**Binding choice: the tokio-rs `io-uring` crate** (the low-level binding, not
a runtime). Rationale:

| Option | Verdict |
|---|---|
| `io-uring` (tokio-rs) | **chosen** — raw SQE/CQE control, `REGISTER_BUFFERS`, PBUF_RING, multishot, SQPOLL, DEFER_TASKRUN all exposed; feature parity with liburing, which the fairness of the C-vs-Rust race requires; already short-listed in the historical [design 14](14-dependencies.md) |
| `tokio-uring`, `glommio`, `monoio` | rejected — an executor/runtime between the benchmark and the ring confounds the comparison with C |
| `rustix::io_uring` | rejected — raw syscall surface, hand-rolled ring management; a subtle ring bug would invalidate the benchmark |

**Mandatory companion edits** (cargo hard-errors on missing workspace
members, and the nix source filters are allowlists — forgetting any of these
breaks existing builds):

| File | Edit |
|---|---|
| `Cargo.toml` (workspace) | `members += "crates/urp-bench"` |
| `nix/checks.nix:20-27` | add `urp-bench` to the crate-dir filter |
| `nix/urp-cli.nix:20-28` | same |
| `nix/urp-protocol-ffi.nix:26-38` | same |
| `nix/urp-bench-rs.nix` (new) | `rustPlatform.buildRustPackage`, package attr `urp-bench-rs`, `meta.mainProgram = "urp-bench"` |

Both binaries install as `urp-bench` with identical CLIs, so the harness
swaps languages by swapping the nix package — and C↔Rust cross-runs need no
special-casing.

## 30.11 Unit tests — table-driven, both languages

Idioms are the repo standards from [design 28](28-testability.md): C uses the
`static const struct { … } cases[]` + `ARRAY_SIZE` loop of
`kernel/urp_test.c` (e.g. `:574`); Rust uses the slice-of-structs pattern of
`crates/urp-cli/src/uapi.rs:261`.

**C — `tools/urp-bench-test.c`**, a standalone binary over the pure core
(no liburing), run as a **sandboxed nix check** `urp-bench-units`. Case
tables (positive / negative / boundary / corner per the standing rule):

| Suite | Cases |
|---|---|
| hdr decode | len 0 / 1 / 23 (short); bad magic; bad version; each reserved flag bit set; payload_len = cap / cap+1 / u32 max; valid minimal (24 B) and maximal; ECHO, FIN, ECHO\|FIN combinations |
| hdr roundtrip | encode→decode identity across field extremes (origin 0/0xffff, seq 0/u32 max, t_send_ns u64 max) |
| deframer | 1-byte drip feed; header split 23+1; payload split at every quarter; two messages in one chunk; message spanning 3 chunks (assembly path + counter); garbage first byte (hard error, no resync); payload_len cap violation mid-stream; FIN header-only; empty chunk |
| tracker | in-order echo; out-of-order; duplicate (−EDUP); unknown seq (−EUNKNOWN); u32 seq wrap around 0xffffffff; window-full behavior |
| batch plan | remaining < batch; inflight == window; remaining == 0; window 1; window 256 |
| config | msg_size 23 / 24 / cap / cap+1; batch 0 / 1 / 1024 / 1025; bad mode string; both listen+connect; neither; count and duration both set |
| stats | known distribution → exact p50/p99; single sample; sample-array saturation |
| format | golden `BENCH_OK` string; truncation → −ENOSPC |

**Rust — `#[cfg(test)]` in each module**, the *same tables* transliterated.
A shared **hex test-vector table** (raw 24-byte header ↔ decoded fields)
appears verbatim in both suites as the cross-language oracle — if the tables
ever disagree, one implementation is wrong by construction.

Runner: `checks.urp-bench-units` builds+runs the C test binary and
`cargo test -p urp-bench` (extending the `protocol-tests` pattern in
`nix/checks.nix:122`, `touch $out/passed` sentinel). Pure computation only —
sandbox-safe (§30.14).

## 30.12 Fuzzing extension (design 27 house pattern)

| Target | Kind | Harness | Corpus |
|---|---|---|---|
| `fuzz-bench-deframe` | `mkCFuzzer` (libFuzzer + ASAN/UBSan, hermetic) over the real `tools/urp-bench-core.c` | `nix/fuzz/bench_deframe_fuzz.c`: first bytes of input derive a chunk-split schedule, rest is fed through `bench_deframe_feed` in those chunks — exercises decode + reassembly + tracker | `fuzz/regressions/bench-deframe/` |
| `bench_frame_decode` | cargo-fuzz | arbitrary bytes → `frame.rs` decode; plus encode→decode roundtrip property | `fuzz/regressions/bench-frame/` |
| `bench_differential` | cargo-fuzz, `cc`-compiled C core linked in (the F0 differential pattern, [design 27 §27.4](27-fuzz-testing.md)) | byte-for-byte: C `bench_hdr_decode` vs Rust decode must agree on accept/reject and every field | shared with above |

Wiring: three new attrs in `nix/fuzz/default.nix` (the `fuzzSrc` filter
already admits all `tools/*.c`), `inherit` lines in `flake.nix`, regression
dirs with `.gitkeep`, `fuzz-bench-deframe` added to the ci.yml `fuzz-smoke`
replay+run loop, and all three to the nightly.yml `fuzz-long` matrix.

## 30.13 Static analysis extension

The `nix/analysis/` suite is kernel-only today (sparse, smatch, checkpatch,
W=1/W=2, coccicheck) plus Rust clippy/fmt — **no userland C linting exists**.
This feature adds the first:

- `nix/analysis/clang-tidy.nix` — checks `bugprone-*, clang-analyzer-*,
  cert-*, performance-*` over `tools/*.c` (the bench core, the shell, and the
  existing `urp-test-client.c` comes along for free).
- `nix/analysis/cppcheck.nix` — `--enable=warning,portability,performance`.

Both honor the house report-only contract —
`$out/{report.txt,count.txt,build.log}`, findings never fail the build —
and register in `nix/analysis/default.nix` (`analysis-clang-tidy`,
`analysis-cppcheck`, plus the `all` aggregate). `mkKbuildReport` is
kbuild-specific, so these get a small sibling helper. The Rust side needs
nothing: workspace membership puts `crates/urp-bench` under the existing
`analysis-clippy`/`analysis-rustfmt` automatically.

## 30.14 Integration — targets and oracles

| Target | What it runs | Oracle |
|---|---|---|
| `nix build .#checks.…urp-bench-units` | C table tests + `cargo test -p urp-bench` (sandboxed, no io_uring) | exit 0; sentinel `$out/passed` |
| `nix run .#urp-bench-local` | host-run direct-topology smoke: C↔C, Rust↔Rust, and **C↔Rust interop both ways**, smoke cells, `--verify full` | every cell `BENCH_OK … verify=full`; interop cells present |
| `nix run .#urp-bench-matrix` | full direct-topology sweep, both languages + memcpy baseline; writes the matrix + C-vs-Rust delta tables | all cells OK/SKIP; delta table printed |
| pair test **Phase 13** (`nix run .#urp-microvm-pair-test`) | tunneled smoke cells through RDMA, urp-bench replacing socat on a dedicated socket pair | expect scrapes `BENCH_OK` lines; existing `scan_splat` dmesg oracle stays armed |
| `URP_BENCH_FULL=1 nix run .#urp-microvm-pair-test` | full tunneled matrix (nightly/manual, trimmed mode set §30.7) | same |

**Sandbox decision, stated explicitly:** io_uring inside the nix build
sandbox is not guaranteed — `kernel.io_uring_disabled` may be set on the
builder, and seccomp policies vary. Therefore *nothing that touches io_uring
is a nix check*: the sandboxed check covers the pure core only, and all
ring-using runs are `nix run` apps on the host or phases inside the microVM
(whose kernel we control). The binary itself degrades gracefully — if ring
setup fails it prints `BENCH_SKIP reason=no_io_uring` and exits 0 in probe
mode, so a harness can distinguish "environment can't" from "benchmark
failed".

CI: `urp-bench-units` joins the explicit-attr build list in
`.github/workflows/ci.yml` (the repo deliberately avoids `nix flake check`);
`fuzz-bench-deframe` joins `fuzz-smoke`; nightly adds the fuzz targets and
(on the self-hosted kvm runner) the Phase-13 smoke cells ride along with the
existing `microvm-pair` job.

## 30.15 Phased plan

| Item | Deliverable | Status |
|---|---|---|
| **B1** | C pure core + header spec (`tools/urp-bench-core.{c,h}`) | *not started* |
| **B2** | Rust twin core (`crates/urp-bench` lib) + workspace/nix-filter wiring (§30.10 table) | *not started* |
| **B3** | C io_uring shell (`tools/urp-bench.c`) + `nix/urp-bench.nix` (liburing) | *not started* |
| **B4** | Rust io_uring backend (`uring.rs`, `main.rs`) + `nix/urp-bench-rs.nix` | *not started* |
| **B5** | unit-test suites both languages + `urp-bench-units` check + shared hex vectors | *not started* |
| **B6** | fuzz targets + regression dirs + ci/nightly wiring | *not started* |
| **B7** | `analysis-clang-tidy` + `analysis-cppcheck` | *not started* |
| **B8** | `.#urp-bench-local` / `.#urp-bench-matrix` apps, pair-test Phase 13, results section in BENCHMARKING.md | *not started* |

Sequencing: **B1 → B5(C half) first** — the core is table-tested before any
io_uring code exists. Then B2, B3, B6, B7 in parallel; B4 after B2; B8 last.
The C↔Rust interop cell in B8 is the acceptance gate for B2/B4.

## 30.16 Open questions

1. **Nightly time budget.** Full matrix ≈ 315 cells × 2 s × 2 languages ≈
   21 min direct; tunneled adds VM boot + per-cell overhead. Recommended
   default (assumed by §30.7): full matrix direct-only nightly; tunneled runs
   the trimmed 3-mode set. Revisit if the nightly wall clock allows more.
2. **C↔Rust wire interop as a hard requirement.** Recommended **yes**
   (assumed throughout): it is a free live differential test and forces the
   two implementations to stay honest. Cost is two extra smoke runs.
3. **Keep the `uring-sendzc` probe?** Recommended **yes**: one cheap mode
   whose entire output is the evidence line answering "does AF_UNIX zero-copy
   exist on this kernel" — the exact question that motivated this doc. Drop
   only if the mode set must shrink.

## 30.17 Relation to other docs

- [02-architecture.md](02-architecture.md) — the historical 4-copy analysis;
  §30.2 is its kernel-module-era successor for the app-side costs.
- [06-uds-io-uring.md](06-uds-io-uring.md) — the userspace-era io_uring
  design (historical). This doc takes its mechanisms (registered buffers,
  SQPOLL, batching) and finally *runs the experiment* it could not.
- [13-performance.md](13-performance.md) — historical copy/syscall
  optimization survey; source of the SQPOLL and batching expectations.
- [14-dependencies.md](14-dependencies.md) — the `io-uring` crate
  short-listing that §30.10 confirms.
- [20-future-work.md §20.1](20-future-work.md) — the open question this doc
  answers with data.
- [21-kernel-module.md](21-kernel-module.md) — why the kernel side already
  has no removable copy (§21.5's page-flip idea remains future work and is
  untouched here).
- [27-fuzz-testing.md](27-fuzz-testing.md) — the F0/F1 patterns §30.12
  extends to the bench protocol.
- [28-testability.md](28-testability.md) — the table-driven idioms and
  pure-core extraction discipline §30.9/§30.11 follow.
- [../BENCHMARKING.md](../BENCHMARKING.md) — where results land, next to the
  buffer-geometry sweep, under the same emulated-numbers caveat.
