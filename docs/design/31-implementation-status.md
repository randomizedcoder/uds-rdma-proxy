# Design 31 Implementation Tracker — urp-fast Zero-Copy (io_uring_cmd)

Progress tracker for [31. urp-fast: End-to-End Zero-Copy](31-urp-fast-zero-copy.md)
and its client sub-document [31a. urp-fast in C++/Seastar](31a-seastar-cpp-demo.md).

**Last updated**: 2026-08-18. PR1 (ABI + `/dev/urp` char device + `uring_cmd` fop +
trust-boundary validators + `pin_user_pages` REGISTER/UNREGISTER) **merged**. PR2
(REGISTER binds the pinned pool to a connected endpoint + per-page `ib_dma_map_page`
→ `local_dma_lkey`) **merged**. **The entire data plane is still design-only**:
`URP_CMD_SEND`/`RECV` validate then return `-ENOSYS` (`kernel/urp_cmd.c:305-324`).
This doc plans the remaining phases and tracks their state.

---

## Why now — and how this connects to the throughput work

Design 34's option ladder (§34.3) names **Option E = zero-copy io_uring = this
design** as the *endgame* for line rate at low CPU. Design 34 §34.5.1 measured the
`AF_UNIX` path as post/serialization-bound at small frames and **copy-dominated at
large frames** (`cpu_us_per_msg` 16.98 @64 KB — memcpy of the payload into a
registered slot). urp-fast removes exactly that copy: the NIC DMAs straight into/out
of the app's pinned pages, so at large messages the ceiling becomes *link* bandwidth,
not *memory* bandwidth. This is why "zero copy should make the system a lot faster"
is specifically true for the bulk-replication (Redpanda/Kafka) target.

Ordering vs the pump/windowing track: Option B (pump, [design 35 §35.4](35-windowing-flow-control.md), phase 1 done) and F2 (scale-out) raise the frame
rate of the *existing* `AF_UNIX` path; urp-fast is the *parallel* track that removes
the copy for *aware* apps. They are independent — unmodified apps keep the one-copy
`AF_UNIX` endpoint; aware apps link the fast client. Flow control differs too (see
"Flow control" below), so designs 35/36 (windowing/CC) are primarily about the pump
path, not the fast path.

---

## Status at a glance

| PR | Scope | Status |
|----|-------|--------|
| **PR1** | `/dev/urp` + `uring_cmd` fop; uapi ABI; dual-compiled validators; REGISTER/UNREGISTER = `pin_user_pages(FOLL_LONGTERM)`/unpin; SEND/RECV validate→`-ENOSYS` | **Done — merged (#36)** |
| **PR2** | REGISTER binds pool to a connected endpoint; per-page `ib_dma_map_page(BIDIRECTIONAL)` → `local_dma_lkey`; endpoint kref | **Done — merged (#37)** |
| **PR3** | `fast` endpoint kind + async-completion scaffold (`-EIOCBQUEUED` + `io_uring_cmd_done`) + per-buffer ownership SM + **SEND** data path | **Not started — NEXT** |
| **PR4** | **RECV** data path (`ib_post_recv` app page as sink, completion → app CQE with stream demux) + RX-starvation backpressure | Not started |
| **PR5** | Client libraries (C + Rust `crates/urp-fast`); `urp-bench` topology **T3** + a `uring-cmd` mode (§31.8) | Not started |
| **PR6** | `uring_cmd`-decoder fuzz target (design 27); VM pair-test + hp1/hp3 hardware E2E; the measured copy-elimination write-up | Not started |
| **PR7** | C++/Seastar client ([design 31a](31a-seastar-cpp-demo.md)) — the real-target demo | Not started (separate track) |
| *Deferred* | One-sided `IB_WR_RDMA_WRITE` + `rkey`/addr exchange (design 34 **Option D**) — removes the far-side recv-post too | Out of scope (own design) |

Legend: **Not started** / **In progress** / **Done** / **Blocked**.

---

## Cross-cutting design decisions

These resolve design 31 §31.10's open questions for the implementation. They apply
across PR3–PR6.

### D1. Two-sided first (`IB_WR_SEND`/`RECV`), not one-sided

PR2 already chose the PD's `local_dma_lkey` over a full `ib_reg_user_mr`
(§31.9.1) — enough to post the app's pages on a two-sided `SEND`/`RECV`, which needs
no remote `rkey`. So the fast path posts the **same `IB_WR_SEND` frame the existing
pump does** (`urp_post_frame`, `urp_pump.c:36`). Consequence — a strong, free
property: **the fast path is wire-compatible with the `uds` pump**, so a fast sender
interoperates with an existing `uds` receiver and vice versa. That makes each half
independently testable against the existing pair-test acceptor. One-sided
`RDMA_WRITE` (which also removes the far-side recv-post and needs an `rkey`/addr
ring exchange) is **design 34 Option D — a separate later track**, not a prerequisite
for the copy-elimination win.

### D2. Async completion — the core new plumbing

Today `urp_uring_cmd` returns an `int` synchronously (`urp_cmd.c:326-350`). A data op
cannot: it completes only when the RDMA CQE arrives, on the CQ workqueue. So
`SEND`/`RECV` will:

1. Validate + claim the buffer (ownership SM), build the WR with `wr_cqe` pointing at
   a per-op completion struct that **stashes the `io_uring_cmd *`**, `ib_post_send`/
   `recv`, and **return `-EIOCBQUEUED`** (tell io_uring the CQE comes later).
2. In the RDMA completion handler (mirroring `urp_send_done`/`urp_recv_done`), flip
   ownership back to APP and call **`io_uring_cmd_done(ioucmd, res, 0, issue_flags)`**
   with `res` = bytes sent / received (or negative errno). `res` and, for RECV, the
   `buf_index`/`stream_id`, ride back in the CQE (CQE32/big-CQE for the extra
   fields — §D5).

The per-op struct is drawn from a fixed per-fd slab sized to the pool depth (no
per-op alloc — the flat-pressure invariant, §31.4). This is the single most
load-bearing new mechanism and gets its own KUnit-able pure core where possible
(slot lifecycle) plus the POC as the integration proof.

### D3. Frame-header placement — **DECIDED: in-place header in the frame buffer**

The wire frame is a 20-byte header + payload (`urp_frame.h`); the pump keeps both in
one buffer and posts `num_sge=1`. **Decision (2026-08-18): the app's pool buffer is a
full frame slot** — `buf_size = URP_FRAME_HEADER_SIZE + max_payload`; the app writes
payload at `+URP_FRAME_HEADER_SIZE`; on SEND the kernel encodes the 20-byte header
**into the app page** (`buf[0..20]`) and posts `num_sge=1` over `[dma[idx], 20+len,
lkey]`, reusing `urp_post_frame` almost verbatim. This is still zero *payload* copy —
the kernel writes only the 20 header bytes; the payload is DMA'd off the app page
untouched. Rationale: simplest, matches the pump exactly, one SGE, no kernel scratch
pool.

Implications the client libraries (PR5) and Seastar (PR7) must honour:
- The client sees a usable payload region of `buf_size − 20` starting at offset 20;
  the pool geometry / `urp-bench` size sweep must account for the 20-byte reservation
  (a 4096 slot carries 4076 payload — the same `max_payload` the tunnel already uses,
  `urp_ep_max_payload`, so this is consistent with design 21/34 geometry).
- On **RECV** (PR4) the symmetric rule holds: the frame DMAs into the app page
  header-first, so the app reads its payload at offset 20 and the header fields
  (stream_id, len) are available in-place for demux without a copy.
- Seastar (31a) frames its own records *inside* that payload region; it does not see
  or manage the 20 transport bytes beyond leaving them reserved. The alternative
  considered — a 2-SGE gather with a kernel-owned header scratch keeping app pages
  pure payload — was **rejected** for the extra plumbing; revisit only if a client
  genuinely cannot cede the 20-byte prefix.

### D4. Per-buffer ownership state machine (§31.2)

Each pool index is `APP_OWNED` or `KERNEL_OWNED`. `SEND`/`RECV` submit requires
`APP_OWNED`, flips to `KERNEL_OWNED` (validated — a double-submit of the same index
is `-EBUSY`); completion flips back to `APP_OWNED`. Teardown (`UNREGISTER`/release)
**quiesces**: refuse unregister while any index is `KERNEL_OWNED` in-flight (or drain
first) so `unpin_user_pages` never races the NIC — closes the PR2 comment race
(`urp_cmd.c:149-154`). The ownership bitmap + transitions are a pure core (KUnit +
userspace units, the design-30 discipline).

### D5. RX completion demux (§31.10 Q3)

A RECV completion must tell the app *which* buffer and *which* stream. `buf_index`
(u32, ≤ 65536 so index fits u16) + `stream_id` (from the received frame header) +
`byte_count` ride back in a **CQE32**. The stream id comes from the same frame header
the pump's RX classifier already decodes (`urp_classify_frame`,
`urp_frame.h:203`) — reuse it.

### D6. `fast` endpoint kind (§31.9.1 PR3)

A `fast` endpoint's QP is driven by the app via `uring_cmd`, **not** by a UDS pump
kthread — so no `urp_stream_tx_fn`, no `--listen-path`/`--connect-path` UDS. Add an
endpoint-kind field (there is none today; endpoints are implicitly `uds`), a
`urp add … --kind fast` control-plane path (netlink + CLI), and gate the pump/UDS
setup on `kind == uds`. Until this lands, PR3 SEND can be exercised binding to any
established endpoint's PD (as PR2 does against `pair_acceptor`) — but a real fast
endpoint must suppress the pump or the two contend for the QP.

### D7. SQPOLL / mm safety (§31.10 Q4) and D8. Security (§31.10 Q6)

The `FOLL_LONGTERM` pin owns the registering task's `mm`; posting happens in kernel
context off the pinned pages, so a SQPOLL kernel submitter (task ≠ registrant) is
safe as long as we never assume `current` owns the pool. Every `buf_index`/`len`/
offset is bounds-checked against the registered geometry by the existing pure
validators (`urp_cmd_validate_data`); PR6 adds a libFuzzer target on the `uring_cmd`
decoder (the new trust boundary, design 17/27).

### Flow control on the fast path (reconciling with designs 35/36)

The fast path's flow control is **the buffer pool itself**: a receiver can only
absorb what it has *donated* as RECV landing space; under-donation surfaces as RNR /
a completion error (§31.6), which is explicit, app-visible backpressure. This is a
*different, simpler* mechanism than the pump's byte window
([design 35](35-windowing-flow-control.md)) — the app's donate-to-receive discipline
*is* the rwnd. So the byte-window (design 35) and CUBIC cwnd (design 36) work is
primarily about the `AF_UNIX` pump path; the fast path gets flow control for free
from the ownership/donation protocol. (A future one-sided Option D path would need an
explicit ring-tail window — noted in design 35 §35 option D.)

---

## Phase detail

### PR3 — `fast` kind + async scaffold + SEND

**Goal.** First real zero-copy transfer: an app SENDs from its pinned pool, the frame
lands on a peer (interop: an existing `uds` acceptor delivers it to its UDS sink,
proving the wire-compat property D1).

**Scope.**
- D6 `fast` endpoint kind (control plane + pump suppression).
- D2 async-completion scaffold: per-fd in-flight slot slab, `wr_cqe` → stash `ioucmd`,
  `-EIOCBQUEUED`, `io_uring_cmd_done` from a `urp_fast_send_done` CQE handler.
- D3 header (per the confirmed option) + `ib_post_send` on the endpoint QP from the
  app page (`dma[idx]`, `len`, `lkey`), reusing `urp_frame_encode` + the seq/stream
  bookkeeping the pump uses.
- D4 ownership SM (SEND half) + `URP_CMD_F_FIN` handling (last frame of a message).

**Definition of done.** POC extended: REGISTER → SEND N frames → the pair-test
acceptor's UDS sink receives the exact bytes (wire-identical to a `uds`-sourced
frame); `scan_splat` clean; ownership SM KUnit + userspace units green; the 6.1/6.6
compile stubs stay green; `nix run .#ci-local` green.

**Verification.** KUnit (ownership SM, slot lifecycle); `urp-fast-validate-units`;
`nix run .#urp-fast-poc` (extended); VM pair-test **Phase 10i** (fast SEND → uds
acceptor); then hp1/hp3.

### PR4 — RECV + backpressure

**Goal.** Fast receiver: donate pages, NIC DMAs inbound frames into them, app reaps.

**Scope.** `URP_CMD_RECV` → `ib_post_recv` with the app page as the SGE sink; a
`urp_fast_recv_done` CQE handler that demuxes `stream_id` (D5), flips ownership, and
`io_uring_cmd_done` with `res = byte_count` + `buf_index`/`stream_id` in the CQE32.
RX-starvation signalling (§31.6): surface "RX pool low / RNR" as a completion error
or an event rather than a silent drop. Ownership SM (RECV half) + teardown quiesce
(D4) fully closed.

**DoD.** POC + VM **Phase 10j** (uds sender → fast receiver) and **10k** (fast↔fast,
both ends aware) deliver correct bytes with zero transport copies; RX under-donation
produces a clean, observable backpressure signal, not a hang or a splat.

### PR5 — client libraries + bench T3

**Scope.** A C client lib (pool mgmt, ring setup, `submit_send`/`post_recv`/`reap`)
and a Rust `crates/urp-fast` (the crate the uapi header already names but that does
not exist yet). Wire `urp-bench` topology **T3 (fast)** + a `uring-cmd` mode (§31.8)
— core/deframer/tracker/twin all reused unchanged; only a new transport backend.

**DoD.** `urp-bench --pattern stream` over T3 runs C↔C and C↔Rust; the differential
fuzzer still agrees on the shared frame codec; `ci-local` green.

### PR6 — fuzz + E2E + the measurement

**Scope.** libFuzzer target on the `uring_cmd` decoder (design 27 pattern). Wire the
POC/bench into the VM (`test-vm.nix`) and run the hp1/hp3 sweep: T3 fast vs the
`AF_UNIX` `blocking`/`uring-*` curves and the `--memcpy-baseline` ceiling (§31.8).
The deliverable is the **measured** copy-elimination — the number this whole design
exists to earn, expected largest at 64 KB where the `AF_UNIX` path is memcpy-bound.

**DoD.** Fuzz target in `ci-local` fuzz-smoke; a results section (here or a sibling,
mirroring `32-performance-results.md`) showing T3 goodput as a % of `ib_write_bw` and
its delta over `AF_UNIX` at the design-34 message sizes.

### PR7 — C++/Seastar client (design 31a) — separate track

Per [design 31a](31a-seastar-cpp-demo.md): the per-shard `urp_fast_shard` service,
the reactor bridge, the three test layers, the nix integration env. Depends on PR3–PR5
(a working kernel data path + a reference C client). Tracked separately; the
[31a §31a.12](31a-seastar-cpp-demo.md) test layers are its DoD.

---

## Verification strategy (inherited discipline)

- **Pure cores, dual-compiled** — the ownership SM and any new decoder join
  `urp_cmd_validate.c` as sources compiled into both `urp.ko` (KUnit) and a userspace
  nix check, so a boundary bug is caught in a fast sandbox run, not only slow
  KUnit-in-VM (design 30 discipline; [[pairtest-build-gotchas]]).
- **POC** (`nix run .#urp-fast-poc`) grows an assertion per phase (PR1 asserts the
  `SEND -ENOSYS` stub today — that assertion flips to a real transfer in PR3).
- **VM pair-test phases** — 10h (REGISTER/UNREGISTER) done; 10i SEND, 10j RECV, 10k
  fast↔fast added per PR. KUnit only builds under the debug/sanitizer microVM.
- **Hardware** hp1↔hp3 — the bench T3 sweep vs `AF_UNIX` + `ib_write_bw`.
- **Fuzz** — the `uring_cmd` decoder target in `ci-local` fuzz-smoke.

## Relation to other docs

- [design 31](31-urp-fast-zero-copy.md) / [31a](31a-seastar-cpp-demo.md) — the design.
- [design 34](34-bulk-throughput.md) §34.3 — Option E; the copy wall this removes.
- [design 35](35-windowing-flow-control.md) / [36](36-congestion-control-cubic.md) —
  the pump-path flow/congestion control; the fast path's donate-to-receive is its own
  rwnd (see "Flow control" above).
- [design 30](30-urp-bench-io-uring.md) — the bench harness T3 slots into.
- [design 17](17-security.md) / [27](27-fuzz-testing.md) — the new trust boundary + its fuzz target.
