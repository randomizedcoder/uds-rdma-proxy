# 31. urp-fast: End-to-End Zero-Copy via an io_uring Command Interface into `urp.ko`

Status: **implementation in progress** — see [§31.9.1](#3191-implementation-status).
PR1 (the `uring_cmd` char device, the command ABI, the trust-boundary
validators, and the `REGISTER`/`UNREGISTER` buffer-pool pin path) is landing;
the RDMA MR path and the `SEND`/`RECV` data path follow. This doc specifies the
opt-in fast path that [design 20 §20.2](20-future-work.md#202-shared-memory-fast-path)
sketches in one paragraph and that [design 30](30-urp-bench-io-uring.md)'s
measurements motivate. It is the answer to *"can an application hand its own
memory to `urp.ko` and get true software-zero-copy in and out over RDMA?"* —
which the transparent `AF_UNIX` path provably cannot deliver.

## 31.1 Motivation — why the `AF_UNIX` path has a floor we can't cross

[Design 30](30-urp-bench-io-uring.md) built `urp-bench` to measure exactly one
thing: what io_uring buys on the UDS side of the tunnel. Its headline result,
confirmed on kernel 7.1.6 (`IORING_OP_SEND_ZC` → `-EOPNOTSUPP` on `AF_UNIX`):

> On `AF_UNIX`, io_uring removes essentially all *per-message syscall and
> bookkeeping* overhead (2 → ~0.01–0.09 syscalls/msg via batching), but it
> **cannot remove the user↔skb byte copy**. `write(2)` copies the user buffer
> into the socket skb; `read(2)` copies it back out. That copy is the transport
> semantics of a stream socket, and no io_uring flag changes it.

So the transparent path — any unmodified app pointed at a UDS — has a hard
floor: **one app-side copy per direction, per message**, on top of the two
kernel-side copies the module already pays ([design 21 §21.1](21-kernel-module.md)).
For small messages the copy is cheap and batching wins; for large messages the
copy *is* the cost and the curves converge on memory bandwidth
([design 30 §30.7](30-urp-bench-io-uring.md#307-the-matrix)).

To go below that floor the app must stop treating the proxy as a socket and
start treating it as a **DMA peer**: the app's own buffer becomes the exact
memory the NIC reads from on send and writes into on receive, with no socket,
no skb, and no copy in between. That is this design. It trades transparency
(the app now links a small client library and drives a ring) for the last
copy — an opt-in fast path for aware applications, exactly the tradeoff
[design 20 §20.1](20-future-work.md#201-kernel-bypass-for-uds-side) anticipated.

## 31.2 The core idea — one buffer, two registrations, one owner at a time

The whole design rests on a single object: an **application-owned buffer that
is registered simultaneously with io_uring and with the RDMA device**, so the
same physical pages serve as (a) an io_uring *fixed buffer* the app addresses
by index and (b) an RDMA *memory region* (MR) the NIC DMAs to/from.

```
                 one pool buffer = one set of physical pages
        +---------------------------------------------------------+
        |  app virtual memory  (mmap'd once at startup)           |
        +---------------------------------------------------------+
             ^                                     ^
             | registered with io_uring            | registered with the RDMA
             | (IORING_REGISTER_BUFFERS -> index)   | device (MR: lkey/rkey)
             |                                     |
        app addresses it                      NIC DMAs into/out of it
        by fixed-buffer index                 with no bounce buffer
```

This is the userspace-era "dual-registered buffer"
([design 06 §6.2](06-uds-io-uring.md#62-integration-with-rdma-buffers)) — but
resurrected for the **kernel-module** architecture and, crucially, owned by an
*arbitrary application* rather than by the proxy. The module never allocates
the data buffer; it *borrows* the app's pages, pins them, maps them for DMA,
and hands them back.

**Ownership is the safety model.** At every instant a buffer is in exactly one
of two states, and the io_uring submission/completion pair is what transfers
ownership between them:

```
        submit SQE (app -> kernel)
   APP_OWNED  ─────────────────────────►  KERNEL_OWNED
   app may       reap CQE (kernel -> app)   NIC may DMA;
   read/write ◄─────────────────────────   app must NOT touch
```

No locks, no reference counts on the hot path: the ring *is* the ownership
protocol. This is the identical discipline `urp-bench` already enforces with
its outstanding-window accounting ([design 30 §30.5, §30.6](30-urp-bench-io-uring.md#305-message-framing--the-testable-fuzzable-core)) —
the same rule that fixed the two matrix-caught bugs (single outstanding recv;
never touch a buffer the kernel still owns).

## 31.3 Interface choice — `IORING_OP_URING_CMD` into a `urp.ko` char device

The app must be able to say to the module, over io_uring, *"send buffer #i (n
bytes) on this stream"* and *"here is buffer #j as receive space"*, and get
completions back on the **same** completion queue it already polls. The
mechanism that expresses this natively is `IORING_OP_URING_CMD` — the
passthrough opcode that ublk, io_uring NVMe passthrough, and ublk-zc are built
on. The app opens a `urp.ko` character device (or an endpoint fd handed out by
the CLI/GENL layer), and every data-plane operation is a `uring_cmd` on that fd.

| Option | Verdict |
|---|---|
| **`IORING_OP_URING_CMD` into `/dev/urp`** | **chosen** — the app keeps *one* io_uring for submit + completion; registered fixed buffers pass by index (no per-op `get_user_pages`); the SQE/CQE pair is the ownership handoff the user asked for verbatim ("put completion on the io_uring CQ, userland reads the queue and takes possession"). The exact ublk/nvme-passthrough zero-copy pattern. |
| Page-flipping across `AF_UNIX` ([design 21.5](21-kernel-module.md#215-zero-copy-potential)) | rejected for this path — kernel-internal page-table manipulation on the *still-transparent* socket; reaches only 0–1 copy, strict alignment constraints, and does not give the app a memory pool it controls. Complementary, not this. |
| `AF_XDP` umem | rejected — umem is the NIC packet path (XDP/AF_XDP frames), not the RDMA verbs path; wrong DMA engine for a RoCEv2 RC QP. |
| New `AF_RDMA_PROXY` socket family ([design 21](21-kernel-module.md) line 230) | rejected for the fast path — a socket read/write API cannot cleanly express "donate this specific pinned buffer as the DMA sink" without re-inventing a descriptor ring on top of it; that ring already exists and is called io_uring. |
| Bespoke char-device with its own mmap'd ring | rejected — reinvents io_uring's SQ/CQ, registered buffers, and SQPOLL. `uring_cmd` gives all of it for free and composes with the app's other io_uring work. |

The transparent `AF_UNIX` endpoints ([design 21 §21.3](21-kernel-module.md))
stay exactly as they are for unmodified apps. `urp-fast` is a second,
parallel front-end to the same RDMA back-end — an endpoint is provisioned as
either `uds` (socket) or `fast` (uring_cmd), via the existing
`urp add` CLI/GENL surface ([design 22](22-genl-interface.md),
[design 23](23-cli-tool.md)).

## 31.4 The buffer pool and its lifecycle

The user's central observation — *malloc at startup, then stabilize at low,
flat memory pressure* — is the pool's whole reason to exist. Buffers are
allocated and registered **once**; thereafter they only change *ownership*,
never *existence*.

```
  STARTUP (once)                         STEADY STATE (per message)          TEARDOWN (once)
  ───────────────                        ─────────────────────────           ───────────────
  mmap(MAP_HUGETLB) N buffers            take buffer from free list          drain in-flight
        │                                submit SQE (RECV or SEND)           unregister buffers
  io_uring_register_buffers  ──►  pool   reap CQE                     ──►     ib_dereg_mr (module)
        │                       (idle)   consume / fill in place             munmap
  URP_CMD_REGISTER (uring_cmd)           return buffer to free list          
        │  └─ module pin_user_pages +                                        
        │     ib_reg_mr  → lkey/rkey     NO malloc, NO pin, NO map here      
  warm: post initial RECV buffers        → memory pressure is flat           
```

- **Allocation**: `mmap(MAP_HUGETLB | MAP_ANONYMOUS)` for a pool of `N`
  buffers, each `buffer_size` (naturally the tunnel's `buffer_size`, so one
  pool buffer is exactly one RDMA frame — reuses the geometry of
  [design 21](21-kernel-module.md) and the `urp-bench` matrix boundaries at
  4076 / 65516). Huge pages give the TLB win and keep the pin cheap.
- **io_uring registration**: `io_uring_register_buffers()` → each buffer gets a
  fixed-buffer **index**. The app now addresses buffers by index in
  `READ_FIXED`/`WRITE_FIXED`-style ops and in our `uring_cmd`s.
- **Module registration** (`URP_CMD_REGISTER`, a `uring_cmd`): the module calls
  `pin_user_pages(FOLL_LONGTERM)` on the pool (pinned **once**, not per-op —
  this is precisely the per-op `get_user_pages` that
  [design 06 §6.1](06-uds-io-uring.md#61-why-io_uring) called out as the cost to
  avoid), then `ib_reg_user_mr()` / `ib_dma_map_sg()` to produce the MR
  `lkey` (local, for our own posts) and optionally `rkey`. From here the pages
  are dual-owned metadata-wise but **single-owner at runtime** per §31.2.
- **Steady state**: the free list is a plain `list_head` (app side: a ring or
  stack of free indices). A message costs one list pop, one SQE, one CQE, one
  list push. **No allocation, no pinning, no mapping, no registration** — the
  flat-pressure regime the user described. Under-supply of RX buffers is the
  only backpressure knob (below).
- **Teardown**: quiesce in-flight ops, `URP_CMD_UNREGISTER` (module
  `ib_dereg_mr` + `unpin_user_pages`), `io_uring_unregister_buffers`, `munmap`.

**Pool sizing** sets the pipeline depth exactly like `urp-bench`'s `batch`
dimension: `N` buffers split between an RX reserve (donated as receive space)
and a TX working set. `N ≈ 2 × batch × (RX + TX)` keeps both directions full
without unbounded in-flight bytes — the same deadlock-freedom invariant as
[design 30 §30.5](30-urp-bench-io-uring.md#305-message-framing--the-testable-fuzzable-core)
(receives always posted before new sends).

## 31.5 SEND path — app buffer → RDMA, zero software copy

```
  APP (userland)                         urp.ko                         RDMA / NIC
  ──────────────                         ──────                         ──────────
  1. idx = pool.pop()          APP_OWNED
  2. fill buf[idx] with payload  (write directly into the pinned page — the
     (+ app framing if any)       ONLY "copy", and it's the app producing data,
                                   not a transport copy)
  3. submit SQE:               ─────────►
     URING_CMD  op=SEND
       buf_index=idx  len=n    ownership: APP → KERNEL
       stream_id=s                 │
                                4. translate idx → pinned page + MR lkey
                                5. ib_post_send(qp[s], sge={addr,len,lkey})  ────────►
                                     (RDMA WRITE_IMM / SEND, zero-copy DMA        NIC reads
                                      straight from the app's page)              the app page
                                        │                                        by DMA
                                6. RDMA send completion (ib_poll_cq) ◄───────────  CQE
                                7. post CQE to the app's io_uring CQ
     8. reap CQE  ◄─────────────    ownership: KERNEL → APP
        (res = bytes sent /
         negative errno)
  9. pool.push(idx)            APP_OWNED again → reuse for next SEND or donate as RX
```

Copies on this path: **zero transport copies.** Step 2 is the application
*materializing its own data*; from there the bytes are DMA'd off the app's page
by the NIC. Contrast [design 30 §30.2](30-urp-bench-io-uring.md#302-where-we-are-today--copy-and-syscall-inventory)
row **C1** (`write(2)`: user buf → UDS skb) — that copy is *gone*, because there
is no skb. Syscalls are amortized by io_uring exactly as in design 30 (batch
`k` sends into one `io_uring_enter`, or zero with SQPOLL).

The far side is symmetric: `urp.ko` on the peer host receives the RDMA frame
into *its* app's donated RX buffer (§31.6) and completes that app's CQE. The
message never lands in a socket on either end.

## 31.6 RECEIVE path — RDMA → app buffer, zero software copy

The receive side is where "the userland donates memory and later takes
possession" is most literal. The app *pre-posts empty buffers as landing
space*; the NIC DMAs an inbound RDMA frame directly into one; the module hands
that buffer to the app by completion.

```
  APP (userland)                         urp.ko                         RDMA / NIC
  ──────────────                         ──────                         ──────────
  0. (startup) donate R empty buffers as RX space:
     for each: idx=pool.pop(); submit SQE URING_CMD op=RECV buf_index=idx
                               ─────────►
                               ownership: APP → KERNEL (buffer is now DMA sink)
                               1. ib_post_recv(qp[s], sge={page,len,lkey}) ───────►
                                    (buffer armed as the RDMA receive target)   armed
                                        │
                                        ⋮  (peer sends; §31.5 far side)
                                        │
                               2. inbound RDMA frame DMA'd INTO the app page ◄──── NIC DMA
                               3. recv completion (ib_poll_cq): which buffer,   CQE
                                  how many bytes
                               4. post CQE to app io_uring CQ (res=byte_count,
                                  buf_index=idx)
     5. reap CQE  ◄─────────────    ownership: KERNEL → APP
        data is already in buf[idx] — NOTHING copied it there; the app owns it
     6. consume buf[idx] in place
        (parse, hand to app logic, echo, …)
     7a. re-donate: submit RECV with idx  (keep RX pool replenished)
     7b. or pool.push(idx) then re-donate a different buffer
                               ─────────►  ownership: APP → KERNEL again
```

Copies on this path: **zero transport copies.** The NIC's DMA engine is the
only thing that moved the bytes, and it moved them *once*, into the final
resting place the app will read from. Contrast design 30 row **S2** (`read(2)`:
UDS skb → user buf) — gone, no skb, no `copy_to_iter`. This is the receive-side
analogue of [design 21 §21.5](21-kernel-module.md#215-zero-copy-potential)'s
page-flip, but achieved without any page-table surgery: the app's page *was
already* the RDMA sink.

**RX starvation is the one hazard and it is explicit.** If the app fails to
keep `RECV` buffers posted, the QP runs out of receive WRs (RNR). The module
surfaces this as a completion error / credit stall rather than silently
dropping — mirroring the per-QP credit loop of
[design 08a](08a-qp-health-probes.md). The client library's job is to re-donate
in step 7 as fast as it consumes, keeping `R` buffers always armed. This is the
same "receives before sends" invariant as the send path, viewed from the RX
side.

## 31.7 What actually got eliminated — the payoff table

Placing this path next to design 30's inventory
([§30.2](30-urp-bench-io-uring.md#302-where-we-are-today--copy-and-syscall-inventory))
and the module's own path ([design 21 §21.1](21-kernel-module.md)):

| Cost, one message, one direction | `AF_UNIX` + io_uring (design 30) | `urp-fast` (this doc) |
|---|---|---|
| App-side syscalls | ~2/N with batching, → 0 with SQPOLL | same (io_uring, unchanged win) |
| **App-side copy (user ↔ skb)** | **1 copy, unavoidable** (`AF_UNIX` no ZC) | **0 — DMA in/out of the app page** |
| Per-op buffer import (`get_user_pages`) | avoided via registered buffers | avoided — pinned **once** at pool registration |
| Kernel-side UDS↔DMA copy (module) | 1 (module reads skb into DMA slot) | **0 — no skb; app page *is* the DMA slot** |
| RDMA segment | zero-copy (NIC DMA) | zero-copy (NIC DMA), same |
| Steady-state allocation | per-connection buffers | **none — fixed pool, flat pressure** |

End to end, `App A → urp.ko → RDMA → urp.ko → App B` drops from **2 copies**
(the kernel-module baseline: skb→DMA on TX, DMA→skb on RX) plus **2 app copies**
(the `AF_UNIX` bookends) to **0 software copies on either host** — the NIC DMA
is the only data movement. This is the "zero copy from a software perspective"
the user described, made precise.

The honest caveats, stated up front so the design doc doesn't oversell (same
discipline as design 30's `sendzc` evidence mode):

- **Pinning cost is real but one-time.** `pin_user_pages(FOLL_LONGTERM)` on the
  pool has a startup cost and holds pages resident (accounted against
  `RLIMIT_MEMLOCK` / cgroup) — acceptable for a bounded pool, unacceptable for
  unbounded buffers. This is *why* the pool is fixed-size.
- **Alignment / frame geometry.** The buffer size must respect the tunnel's
  `max_payload` (design 21) or a message spans frames and the app must reassemble
  in place — the same straddle the design-30 matrix probes at 4076/4096/65516.
- **Not transparent.** The app links a client library and manages the pool.
  Unmodified apps keep using the `AF_UNIX` endpoint and its one-copy floor.

## 31.8 Relation to `urp-bench` (design 30) — the same harness measures the win

Design 30 already built the measurement apparatus; `urp-fast` slots into it as a
third topology and a mode, so the copy-elimination claim is *measured*, not
asserted:

- **New topology T3 — fast (aware app, through `urp.ko`):** the `urp-bench`
  binary gains a backend that drives the `uring_cmd` interface instead of
  `AF_UNIX`. Everything else — the 24-byte framing
  ([§30.5](30-urp-bench-io-uring.md#305-message-framing--the-testable-fuzzable-core)),
  the symmetric full-duplex roles, the deframer, the tracker, the C/Rust twin,
  the differential fuzzer — is reused unchanged, because they are all transport-
  agnostic pure core.
- **New mode `uring-cmd`** (or a sibling `urp-fast-bench` if the ring setup
  differs enough): its delta against `blocking`/`uring-rw`/`uring-fixed` on the
  same message-size sweep is the direct, quantified answer to "how much did the
  last copy cost?" — the number this whole design exists to earn. For large
  messages (memcpy-dominated in design 30) the expected win is the largest.
- **The `--memcpy-baseline` yardstick** already in design 30
  ([§30.3](30-urp-bench-io-uring.md#303-experiment-design--separating-the-effects))
  becomes the ceiling: a true zero-copy path should approach *link* bandwidth,
  not *memory* bandwidth, at large sizes — visibly beating the `AF_UNIX` curves
  that top out at `memcpy` MB/s.

The C-vs-Rust conformance race and the shared wire format carry over verbatim,
so `urp-fast` inherits design 30's cross-language honesty check for free.

## 31.9 Implementation sketch (for a future phased plan)

Not a work plan yet — the shape a plan would take, so the doc is actionable:

| Piece | Where | Notes |
|---|---|---|
| `urp.ko` char device + `uring_cmd` handler | `kernel/urp_cmd.c` (new) | `URP_CMD_{REGISTER,UNREGISTER,SEND,RECV}`; `->uring_cmd` fop; validates buf_index against the registered set |
| Buffer pin + MR | `kernel/urp_rdma.c` | `pin_user_pages(FOLL_LONGTERM)` + `ib_reg_user_mr`/`ib_dma_map_sg`; unpin/dereg on unregister; `RLIMIT_MEMLOCK` accounting |
| Endpoint kind `fast` | `kernel/urp_genl.c`, `urp` CLI | `urp add … --kind fast` provisions a uring_cmd endpoint sharing the RDMA back-end with the existing CM/QP machinery |
| Client library (C + Rust) | `crates/urp-fast` + `tools/` | pool mgmt, ring setup, `submit_send`/`post_recv`/`reap`; the reusable frame codec is already `urp_bench` core / `uds-rdma-protocol` ([design 21.7](21-kernel-module.md#217-code-sharing-strategy)) |
| Client library (C++/Seastar) | see [31a](31a-seastar-cpp-demo.md) | the Seastar `urp_fast_shard` service — the client for the real target (Redpanda is a Seastar app); designed in the sub-document below |
| Bench backend T3 | `tools/urp-bench.c`, `crates/urp-bench` | new mode wiring only; core untouched |
| Fuzz | design 27 pattern | the `uring_cmd` argument decoder is a new trust-adjacent parser → new libFuzzer target, like design 30's deframer |

Ordering mirrors design 30: pure/library pieces and the codec first (already
exist), then the kernel char-device + MR path, then the client library, then
the bench backend that measures it, then fuzz/analysis wiring.

### 31.9.1 Implementation status

Kernel-first phasing (so the client has a real target to integration-test
against). Each PR is independently green and microVM-verified.

| Phase | Scope | State |
|---|---|---|
| **PR1** | `/dev/urp` char device + `->uring_cmd` fop; the shared ABI (`include/uapi/linux/urp_cmd.h`); the pure trust-boundary validators (`kernel/urp_cmd_validate.c`, dual-compiled into `urp.ko` and a userspace check); `REGISTER`/`UNREGISTER` = `pin_user_pages(FOLL_LONGTERM)` / `unpin`. `SEND`/`RECV` validate then `-ENOSYS`. | **merged** |
| **PR2** | `REGISTER` binds the pinned pool to a named connected endpoint and DMA-maps every page against its RDMA device (`ib_dma_map_page`), producing the local `lkey` the data path posts with; the pool shares that endpoint's PD. `UNREGISTER` unmaps + releases the endpoint ref. | **landing** |
| PR3 | `SEND`/`RECV` data path: `ib_post_send`/`ib_post_recv` on the endpoint QP, completion → app CQE; the per-buffer ownership state machine (§31.2); endpoint kind `fast` so the app (not a UDS pump) drives the QP | planned |
| PR4 | client libraries (C + Rust, then C++/Seastar per [31a](31a-seastar-cpp-demo.md)); bench topology **T3** (§31.8); `uring_cmd`-decoder fuzz target | planned |

PR2 chose `ib_dma_map_page` + the PD's `local_dma_lkey` over `ib_reg_user_mr`
(§31.10 Q1): the module already registers *its own* buffers exactly this way
([design 21](21-kernel-module.md), `urp_bufs_init`), the two-sided `SEND`/`RECV`
model needs only a local `lkey` (no remote `rkey`), and it keeps the app pool
in the *same* PD as the QP the data path will post on. A full user MR (for a
remote `rkey` / one-sided RDMA) can come later if a one-sided mode is added.
The endpoint kind `fast` moves to PR3, where posting on the QP would otherwise
collide with a `uds` endpoint's pump — for the PR2 DMA-map milestone, binding
to any established endpoint's PD is sufficient and is exercised against the
pair-test `pair_acceptor`.

PR1 specifics worth recording:

- **The command decoder is a dual-compile pure core.** `urp_cmd_validate.c`
  has no kernel-subsystem dependencies, so the *same source* is compiled into
  the module (KUnit: `test_cmd_validate_data` / `test_cmd_validate_reg`) and
  into a userspace binary (`urp-fast-validate-units` nix check, 55 table
  cases). A boundary bug is caught by a fast sandboxed run, not only a slow
  KUnit-in-VM pass — the design-30 "one contract, two builds that must agree"
  discipline applied to the app→kernel boundary.
- **The pin is once, at REGISTER**, exactly as §31.4 requires — not per-op.
  `urp-fast-poc` (`nix run .#urp-fast-poc`) and pair-test **Phase 10h** prove
  the pool really pins/unpins against a live module, with a `scan_splat`
  memory-safety gate.
- **Kernel floor is 6.8** for the fast path (modern `<linux/io_uring/cmd.h>`
  + 4-arg `pin_user_pages`); the device is stubbed out on older LTS so the
  6.1/6.6 compile gates stay green. Unmodified `AF_UNIX` apps are unaffected
  on every kernel.

## 31.10 Open questions

1. **`ib_reg_user_mr` vs `ib_dma_map_sg` + `rdma_rw`.** Registering a user MR
   per pool is simplest; for very large pools the `rdma_rw` API with
   scatter-gather may map more cheaply. Benchmark both at implementation time.
2. **One MR for the whole pool, or one per buffer?** A single pool-wide MR with
   per-op offsets is fewer registrations and one `lkey`; per-buffer MRs give
   tighter fault isolation. Lean pool-wide (matches the fixed-buffer index
   model), revisit if a bad app corrupts a neighbor.
3. **RX completion demux.** With multiple streams multiplexed on shared QPs
   ([design 09](09-connection-multiplexing.md)), the RECV completion must carry
   `stream_id` so the app routes the buffer. Reuse the frame header's stream id,
   or return it in the `uring_cmd` CQE `res`/big-CQE.
4. **Interplay with SQPOLL.** SQPOLL means a kernel thread submits without the
   app's `io_uring_enter`; the `uring_cmd` path must be SQPOLL-safe (no
   assumption the submitting task == the registering task). Verify against the
   `FOLL_LONGTERM` pin's owning mm.
5. **Backpressure signalling.** How the module tells the app "slow down / RX
   pool low" without a copy — a completion flag vs a separate event stream (the
   GENL `events` multicast group of [design 22](22-genl-interface.md) is a
   candidate for the slow path).
6. **Security.** A malicious/ buggy app hands page indices to the kernel; the
   handler must bounds-check every `buf_index` and length against the registered
   set and never trust an offset. This is a new trust boundary
   ([design 17](17-security.md)) and gets a fuzz target (§31.9).

## 31.11 Relation to other docs

- [30-urp-bench-io-uring.md](30-urp-bench-io-uring.md) — measures the `AF_UNIX`
  ceiling this design breaks; becomes the harness that quantifies the win (§31.8).
- [20-future-work.md §20.1/§20.2](20-future-work.md) — the one-paragraph
  "kernel bypass" and "shared-memory fast path" sketches this doc fills in.
- [06-uds-io-uring.md §6.2](06-uds-io-uring.md#62-integration-with-rdma-buffers)
  — the historical userspace-era dual-registered-buffer idea, here revived for
  the kernel-module architecture and app ownership.
- [21-kernel-module.md §21.3, §21.5](21-kernel-module.md) — the RDMA consumer
  API (`ib_dma_map_page`, `ib_reg_mr`), the buffer-management patterns, and the
  page-flip zero-copy alternative this design supersedes for aware apps.
- [21-kernel-module.md §21.7](21-kernel-module.md#217-code-sharing-strategy) —
  the `uds-rdma-protocol` / frame-codec crate the client library reuses.
- [08a-qp-health-probes.md](08a-qp-health-probes.md),
  [09-connection-multiplexing.md](09-connection-multiplexing.md) — the credit
  loop and stream multiplexing the RX path and completion demux ride on.
- [17-security.md](17-security.md) — the new app→kernel trust boundary (§31.10).
- [22-genl-interface.md](22-genl-interface.md),
  [23-cli-tool.md](23-cli-tool.md) — how a `fast` endpoint is provisioned
  alongside the existing `uds` endpoints.
- [31a-seastar-cpp-demo.md](31a-seastar-cpp-demo.md) — the C++/Seastar client of
  this design: a zero-copy demonstration in the framework Redpanda is built on.
