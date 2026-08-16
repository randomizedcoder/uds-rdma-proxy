# 31a. urp-fast in C++/Seastar — a Zero-Copy Demonstration (toward Redpanda)

Status: **design / future work** — sub-document of
[Section 31 — urp-fast](31-urp-fast-zero-copy.md). Where design 31 specifies
the `urp.ko` `io_uring` command interface and its C/Rust reference clients,
this doc specifies a **third client written in C++ on the Seastar framework**,
because the real target application — **Redpanda** — is a Seastar program. The
goal is a small, self-contained Seastar app that demonstrates end-to-end
software-zero-copy over `urp-fast` using Seastar's own idioms, so the path to a
genuine Redpanda integration is a straight line rather than a leap.

## 31a.1 Why Seastar is already shaped like design 31

The central reason to target Seastar first (before touching Redpanda) is that
**Seastar's architecture already embodies every invariant design 31 needs**.
The demonstration is therefore less "build a zero-copy runtime" and more "let
two systems that already agree talk to each other."

| design 31 requirement | Seastar's native equivalent |
|---|---|
| App owns a buffer pool, `malloc` once at startup ([§31.4](31-urp-fast-zero-copy.md#314-the-buffer-pool-and-its-lifecycle)) | Seastar reserves its whole heap (hugepages) at startup and sub-allocates per shard from a fixed arena — allocation-in-the-hot-loop is already an anti-pattern the framework is built to avoid |
| Flat, low steady-state memory pressure | Seastar's design goal verbatim: pre-allocate, recycle, never churn the allocator on the data path |
| `APP_OWNED` / `KERNEL_OWNED`, submit/complete ownership handoff ([§31.2](31-urp-fast-zero-copy.md#312-the-core-idea--one-buffer-two-registrations-one-owner-at-a-time)) | `temporary_buffer<char>` move-only ownership + `future`/`continuation` completion — the buffer is "owned by the pending op" until the future resolves |
| Buffer returns to the pool on completion | `temporary_buffer`'s **custom deleter** — the exact idiom Seastar's own network stack uses to hand a received packet up the stack zero-copy and recycle it when the last reference drops |
| Submit SQE / reap CQE | `future<> send(...)` / `future<temporary_buffer> recv(...)` — the SQE is submitted under the hood, the CQE resolves the future |
| io_uring as the submission mechanism | Seastar **already ships an `io_uring` reactor backend** (`--reactor-backend=io_uring`); io_uring is a first-class citizen, not a foreign body |
| One owner, no locks on the hot path | **shard-per-core, share-nothing** — each core is a single-threaded reactor that owns its data exclusively |
| One pool + ring + endpoint per owner | **one per shard** falls out for free (below) |
| Many RDMA QPs for parallelism ([design 08](08-multi-qp-ecmp.md)) | Seastar's shard count *is* the natural QP count → **one QP per shard**, ECMP across them |

The one thing Seastar does **not** have is an RDMA transport — its net stack is
POSIX or (optionally) DPDK, neither of which is verbs. That gap is precisely
what `urp.ko` + `urp-fast` fill: `urp-fast` is a **side-channel transport** that
bypasses Seastar's network stack entirely and moves bytes over RDMA, while
presenting Seastar-shaped (`future` + `temporary_buffer`) APIs to the app.

**Aside — this is not a DPDK dependency.** Seastar and DPDK share a *philosophy*
(busy-poll reactor, pre-allocated pools, share-nothing per core), which is why
the fit feels familiar, but the four hooks above — shard-per-core ownership, the
startup arena, `temporary_buffer` recycling, and the io_uring reactor backend —
are Seastar application-architecture properties, independent of whether Seastar's
net stack is POSIX or DPDK. `urp-fast` uses RDMA verbs (a different DMA engine
than DPDK's poll-mode NIC path), so the demo runs fine with Seastar's plain
POSIX stack; DPDK is orthogonal (§31a.11).

## 31a.2 The target: Redpanda's UDS transport, made aware

Today the repo tunnels Redpanda transparently: Redpanda writes its inter-broker
RPC to a UDS (`/var/run/redpanda/rpc.sock`), and the proxy tunnels it over RDMA
([design 02 §2](02-architecture.md), [design 09](09-connection-multiplexing.md)).
That path pays the `AF_UNIX` copy floor design 30 measured
([§30.2](30-urp-bench-io-uring.md#302-where-we-are-today--copy-and-syscall-inventory)).

Because Redpanda is Seastar, making it an *aware* `urp-fast` client is far more
tractable than for a generic app: it already thinks in `temporary_buffer`s,
futures, and per-shard sharding, and it already has an internal buffer type
(`iobuf` — a chain of `temporary_buffer`s) that a pooled zero-copy buffer slots
into without a copy. This demonstration builds the reusable per-shard service
that a Redpanda transport would later sit on; §31a.10 sketches that final step.

## 31a.3 Per-shard architecture (share-nothing)

Each Seastar shard owns a complete, independent `urp-fast` stack. Nothing on the
data path crosses a core boundary, so there are no locks and no atomics in the
hot loop — the same property that makes design 31's ownership model lock-free,
now enforced by Seastar's threading model rather than by discipline.

```
  Seastar process (N shards = N cores)
  ┌───────────────────────────────────────────────────────────────────────┐
  │  shard 0 (core 0)            shard 1 (core 1)        …   shard N-1      │
  │  ┌───────────────────┐       ┌───────────────────┐                     │
  │  │ reactor (busy-poll)│       │ reactor           │                     │
  │  │  ├ urp-fast poller │       │  ├ urp-fast poller │                     │
  │  │  ├ buffer pool  ───┼──┐    │  ├ buffer pool     │                     │
  │  │  ├ uring_cmd ring  │  │    │  ├ uring_cmd ring  │                     │
  │  │  └ endpoint/QP     │  │    │  └ endpoint/QP     │                     │
  │  └───────────────────┘  │    └───────────────────┘                     │
  └─────────────────────────┼───────────────────────────────────────────────┘
                            │  (per-shard pool region:
                            │   pinned once, MR-registered once)
                            ▼
                       urp.ko  ── one QP per shard ──►  RDMA / RoCEv2  ──► peer
```

- **One pool per shard**: a dedicated aligned memory region (§31a.4), pinned and
  RDMA-registered **once** at shard startup via `urp-fast`'s `URP_CMD_REGISTER`.
- **One `uring_cmd` ring per shard**: a *separate* `io_uring` from Seastar's own
  reactor backend ring (Seastar does not expose raw SQE submission for arbitrary
  opcodes, so `urp-fast` runs its own ring and integrates it as a poller —
  §31a.5). Two rings per shard is a small, honest cost; teaching Seastar's
  backend about `urp_cmd` natively is future upstream work (§31a.11).
- **One endpoint / QP per shard**: provisioned `--kind fast` (design 31 §31.9),
  giving the one-QP-per-core layout [design 08](08-multi-qp-ecmp.md) already
  wants for ECMP. No cross-shard stream multiplexing
  ([design 09](09-connection-multiplexing.md)) is needed on the fast path —
  sharding replaces it.

## 31a.4 The buffer pool in Seastar memory

The pool must yield buffers that are simultaneously (a) page-aligned, (b)
io_uring fixed buffers, and (c) covered by one RDMA MR. Seastar supplies exactly
the right primitive: `temporary_buffer<char>::aligned(alignment, size)` over a
region carved from the shard's hugepage arena.

```
  shard startup:
    region = allocate_aligned(page_size, N * buffer_size)   // hugepage-backed
    io_uring_register_buffers(ring, region, N)              // fixed-buffer indices
    URP_CMD_REGISTER(ring, region)                          // urp.ko: pin + ib_reg_mr → one MR
    free_indices = {0 .. N-1}                                // per-shard stack, no lock

  steady state (per op): pop index → build temporary_buffer view → submit → on
    completion the buffer's deleter pushes the index back. No malloc, no pin,
    no map — matches design 31 §31.4 flat-pressure regime exactly.
```

`buffer_size` is the tunnel's frame size (design 21), so one pool buffer is one
RDMA frame — the same geometry the design-30 matrix probes at 4076 / 65516
([§30.7](30-urp-bench-io-uring.md#307-the-matrix)). Messages larger than a frame
reassemble in place across pooled buffers (the design-30 deframer, reused).

**The deleter is the zero-copy handoff.** When a receive completes, the pooled
buffer is presented to application code as an ordinary `temporary_buffer` whose
deleter returns it to the pool (and re-arms it as RX space):

```cpp
// sketch — a received frame handed up zero-copy, auto-recycled when consumed
seastar::temporary_buffer<char> make_pool_buffer(pool& p, unsigned idx, size_t len) {
    char* data = p.addr_of(idx);
    return seastar::temporary_buffer<char>(
        data, len,
        seastar::make_deleter([&p, idx] { p.return_and_repost(idx); }));
}
```

The application holds the `temporary_buffer` for as long as it needs the data
and simply lets it drop; nothing is ever copied out of the pooled page. This is
identical to how Seastar's stack recycles network packet buffers — the demo
inherits a battle-tested idiom rather than inventing one.

## 31a.5 Bridging the `urp-fast` ring into the reactor

The per-shard `uring_cmd` ring is drained cooperatively so the Seastar reactor
never blocks. Two integration modes, matching design 31's SQPOLL-vs-eventfd
tradeoff:

| Mode | Mechanism | Use |
|---|---|---|
| **Busy-poll poller** (default for the perf demo) | register a `reactor::poller` that drains the ring's CQ every reactor tick and fulfills the matching promises | lowest latency; pairs with `urp-fast` SQPOLL; matches Seastar's busy-polling reactor philosophy |
| **eventfd-driven** (efficiency mode) | `io_uring_register_eventfd(ring, efd)`, wrap `efd` in a `seastar::pollable_fd`, `readable().then(drain_cq)` | lets the reactor sleep when idle; lower CPU at the cost of wakeup latency |

Either way the app-facing surface is a per-shard service exposing futures:

```cpp
// sketch — the per-shard urp-fast service
class urp_fast_shard {
public:
    // take a pooled buffer to fill for TX (ready future unless pool exhausted)
    seastar::future<seastar::temporary_buffer<char>> alloc();
    // submit a filled buffer; future resolves when the RDMA send CQE returns
    seastar::future<> send(seastar::temporary_buffer<char> payload, stream_id s);
    // future resolves with the buffer the NIC DMA'd into (zero-copy)
    seastar::future<seastar::temporary_buffer<char>> recv(stream_id s);
};
```

Internally each `send`/`recv` allocates a promise, stows it keyed by the
`uring_cmd` user-data, submits the SQE, and the poller (or eventfd handler)
resolves the promise when the CQE arrives — the standard "callback → future"
bridge Seastar programs already use to wrap foreign async APIs.

## 31a.6 SEND flow (future-based)

```
  app coroutine/continuation                 urp_fast_shard            urp.ko / NIC
  ──────────────────────────                 ──────────────           ────────────
  auto buf = co_await svc.alloc();     pop pool index, view as temporary_buffer
  fill(buf);   // produce data directly into the pinned pool page (no copy)
  co_await svc.send(std::move(buf),s); ── submit uring_cmd SEND(idx,len,stream) ──►
                                          promise stored by user-data          ib_post_send
                                              ⋮   (reactor keeps running)      (DMA off the
                                          poller reaps SEND CQE ◄────────────── app's page)
                                          deleter → index back to pool
  // future resolves here → buffer already recycled; loop for next message
```

Zero transport copies: `fill(buf)` writes into the pinned page; the NIC DMAs
from it. Design 30's `write→skb` copy (row C1) is gone. Multiple `send`s in
flight batch into one `io_uring_enter` exactly as in design 30.

## 31a.7 RECEIVE flow (temporary_buffer with pool-returning deleter)

```
  app                                        urp_fast_shard            urp.ko / NIC
  ───                                        ──────────────           ────────────
  (startup) post R recv buffers: for each  submit uring_cmd RECV(idx,stream) ──► ib_post_recv
                                             promise stored                     (arm app page
                                                 ⋮                               as DMA sink)
                                          inbound frame DMA'd INTO the page ◄──── NIC DMA
                                          poller reaps RECV CQE (idx, nbytes)
  auto buf = co_await svc.recv(s);  ◄─────  resolve promise with make_pool_buffer(idx,nbytes)
  handle(buf);      // consume in place — the bytes were never copied here
  // buf drops → deleter re-posts idx as RX space (keeps R buffers armed)
```

Zero transport copies: the NIC's DMA is the only data movement, landing the
bytes in the page the app reads from. Design 30's `skb→read` copy (row S2) is
gone. RX starvation is the one hazard (design 31 §31.6) — the service keeps `R`
buffers armed by re-posting in the deleter, so consumption and re-arming stay
balanced without app effort.

## 31a.8 The demonstration app

A standalone `seastar::app_template` — **not** Redpanda — that runs the
design-30 symmetric full-duplex benchmark over `urp-fast` instead of `AF_UNIX`:

- Each shard generates its own message stream **and** echoes the peer's, using
  pooled `temporary_buffer`s throughout (design 30 §30.4 symmetric roles).
- Reuses the **24-byte framing** ([design 30 §30.5](30-urp-bench-io-uring.md#305-message-framing--the-testable-fuzzable-core))
  — link the existing C core (`tools/urp-bench-core.c`, already dual-compile
  clean) or a thin C++ transliteration; the differential fuzzer keeps all three
  languages honest.
- Emits the **same `BENCH_OK …` line grammar**
  ([§30.8](30-urp-bench-io-uring.md#308-measurement-methodology)) so results
  drop straight into the design-30 matrix runner and tables.

What it proves, concretely:

1. **Zero software copies** inside a production framework's real idioms —
   verified the same way design 30 verifies its claims (the copy inventory of
   §31.7, plus `--memcpy-baseline` as the ceiling the fast path should *beat*,
   approaching link rather than memory bandwidth at large sizes).
2. **Flat memory** — Seastar's built-in `seastar::metrics` (allocator stats,
   `reactor` memory counters) show no data-path allocation churn: the pool is
   allocated once and only cycles ownership.
3. **The future/`temporary_buffer` model composes with zero-copy naturally** —
   no bespoke buffer management leaks into application code; `co_await
   svc.recv()` yields a buffer, and dropping it recycles it.
4. A **direct three-way comparison** in one table: `AF_UNIX` (design 30) vs
   `urp-fast` C/Rust (design 31) vs `urp-fast` Seastar (this doc) — the same
   matrix, the same units.

## 31a.9 Measuring and comparing

Run the Seastar demo in design 30's topologies: **T1 direct** (two Seastar
processes, one host — the clean substrate for the copy-elimination delta) and
**T2/T3 tunnelled** (through `urp.ko` over RDMA, the pair-test microVM, Phase
10g's successor). Because the output grammar matches, the design-30 matrix
runner aggregates all languages side by side; the expected reading is:

- Seastar `urp-fast` ≈ C/Rust `urp-fast` within noise (same transport, same
  syscall profile) — any gap is a Seastar-integration defect, the same
  conformance logic as design 30 §30.1.
- All three `urp-fast` clients beat the `AF_UNIX` curves by the last copy,
  widening with message size (memcpy-dominated regime).

## 31a.10 Path to real Redpanda integration (out of scope for the demo)

The demo builds `urp_fast_shard`; a real Redpanda transport is the next layer:

- Redpanda inter-broker RPC rides Seastar's `rpc` framework over
  `seastar::connected_socket`. A `urp-fast`-backed transport would present the
  buffer-oriented interface Redpanda's RPC serializes into/out of, replacing the
  socket for peers reachable over RDMA.
- Redpanda's `iobuf` is a chain of `temporary_buffer`s; a pooled zero-copy
  buffer is appended to an `iobuf` **without a copy**, so the send path can hand
  Redpanda's serialized frames to `urp-fast` and the receive path can deliver
  pooled buffers up as `iobuf` fragments.
- This is genuinely more work than the demo (RPC handshake, flow control,
  reconnection, coexisting with the POSIX transport for non-RDMA peers) and gets
  its own design when the demo has proven the numbers. The demo de-risks it.

## 31a.11 Risks and open questions

1. **Two rings per shard.** Seastar's reactor backend ring vs the `urp-fast`
   `uring_cmd` ring. Independent, so no correctness issue, but a busy-poll
   poller adds a little CPU. Long-term: upstream a `urp_cmd`-aware submission
   path into Seastar's io_uring backend so there is one ring — noted, not
   required.
2. **Registering Seastar-managed memory as a long-term-pinned MR.** The pool
   region must be non-reclaimable for its lifetime (`FOLL_LONGTERM` pin,
   `RLIMIT_MEMLOCK` accounting). Carve a **dedicated** per-shard region rather
   than pinning Seastar's general arena, so the allocator never tries to reclaim
   pinned pages. (design 31 §31.10 Q2 — one MR per pool.)
3. **DPDK / userspace net mode is orthogonal.** If Seastar runs its DPDK net
   stack, `urp-fast` still bypasses it — verbs, not Ethernet frames. The demo
   should run with the POSIX stack to keep the two concerns separate.
4. **Cross-shard delivery.** If an inbound message must be handled on a
   different shard than the QP that received it, use
   `foreign_ptr<temporary_buffer<char>>` + `smp::submit_to` — correct but off
   the fast path; the per-shard endpoint layout (§31a.3) is designed to make it
   rare.
5. **Build weight.** Seastar is a heavy C++ dependency (its own toolchain
   expectations). Package the demo behind its own nix devshell/derivation,
   likely gated out of the default `nix flake` build like the redpanda-gated
   targets ([flake `apps` are already `hasRedpanda`-gated](../DESIGN.md)).
6. **`uring_cmd` from C++.** liburing's `io_uring_prep_cmd`/`sqe->cmd` surface is
   C; a thin C++ RAII wrapper around it lives in the demo (the same wrapper a
   Redpanda transport reuses).

## 31a.12 Table-driven unit tests

Same discipline as [design 28](28-testability.md) and
[design 30 §30.11](30-urp-bench-io-uring.md#3011-unit-tests--table-driven-both-languages):
`static const struct { … } cases[]` arrays with positive / negative / boundary /
corner rows, one assertion loop per suite. The Seastar client splits cleanly into
three test layers by what each needs to run — which is *also* what keeps most of
the logic testable at all: the parts that decide correctness are pure, and the
async plumbing is driven by a fake ring, so **no real io_uring or RDMA is needed
to prove the client correct**.

### Layer 1 — pure-core suites (no Seastar, no ring; sandbox-safe)

Plain C++ table binary, run as a nix check (§31a.13 L0). Reuses the design-30
framing/deframer/tracker tables verbatim (link the C core or a C++ twin, kept
honest by the differential), plus the Seastar-client-specific pure units:

| Suite | Positive cases | Negative / boundary / corner cases |
|---|---|---|
| pool index allocator | pop then push restores the full free set; N buffers cycle in LIFO order; `alloc_count == free_count` after a full cycle | pop from an empty pool → *exhausted* sentinel (never a bogus index); **double-return** of the same index → detected, free list uncorrupted; push an out-of-range index → rejected; `N = 1`; `N = max` |
| buffer view / `addr_of(idx)` | idx `0` and `N-1` map to `base + idx*buffer_size`; `len ≤ buffer_size` | idx `== N` (out of bounds) → reject; **`len > buffer_size`** → reject (RX-overflow guard, see safety suite) |
| CQE → promise demux | in-order resolve; `res ≥ 0` → value; `res < 0` → exception; drain K CQEs in one pass | **unknown `user_data`** (spurious CQE) → ignored, no crash; **duplicate `user_data`** (double completion) → second ignored, promise never resolved twice; `user_data` recycle after completion |
| ownership state machine | `APP→KERNEL` on submit; `KERNEL→APP` on completion | submit an already-`KERNEL` buffer → reject; return an already-`APP` buffer (double free) → reject; touch a `KERNEL`-owned buffer → debug assert |
| framing (reuse §30.5) | 24-byte encode↔decode roundtrip across field extremes; `ECHO`/`FIN` combinations | bad magic / version / reserved-flag bit; `payload_len = cap+1`; short header (len 0/1/23) — the design-30 tables, **plus a C++↔C differential row** so the third language can't drift |
| config validate | valid `{shards, pool_size, buffer_size}` combinations | `pool_size < 2×batch` (can't keep both directions full); `buffer_size ≠` tunnel frame size; `shards = 0`; `buffer_size` not page-aligned (breaks the MR/fixed-buffer registration) |

### Layer 2 — fake-ring async suites (Seastar, no real io_uring/RDMA)

A `fake_ring` test double records submitted SQEs and lets the test inject CQEs on
demand, so `send()`/`recv()` futures resolve deterministically under
`seastar::testing::thread_test_case` with a manual poll. This proves the
future/promise/`temporary_buffer` bridge without touching the kernel:

| Suite | Drives | Asserts |
|---|---|---|
| send happy path | fill a pooled buffer → `send()` → inject SEND CQE | future resolves; deleter fired; index back in the free list |
| recv happy path | pre-post RX buffer → inject RECV CQE(idx, nbytes) → `recv()` | future yields a `temporary_buffer` viewing the pooled page; dropping it re-posts the index |
| send/recv error | inject an **error CQE** (`res < 0`, e.g. RNR/QP error) | future resolves *exceptional*; **buffer is still recycled** — no leak on the error path |
| out-of-order completion | submit A,B,C; complete C,A,B | each future resolves with its own result, no cross-talk |
| batched drain | inject 8 CQEs before one poll | a single poll pass resolves all 8 (batching, design 30 §30.6) |
| RX starvation | drain the RX pool → post another recv | `alloc()` future **pends**, then resolves when a buffer frees — never deadlocks (design 31 §31.6) |
| **conservation** | run K full send+recv cycles | **all N indices back in the free list; `allocations == frees`** — the unit-test proof of "no memory pressure," mirrored at runtime by §31a.13's memory oracle |

### Layer 3 — safety / negative suites ("prove it does *not* have errors")

The cases whose entire point is that a wrong implementation would corrupt memory
or leak. Several overlap the tables above but are called out because they are the
security-relevant ones ([design 17](17-security.md), [design 27](27-fuzz-testing.md)):

| Hazard | Test → required outcome |
| **RX length overflow** — a completion claims more bytes than the buffer holds | inject RECV CQE with `nbytes > buffer_size` → **rejected before the buffer is exposed**; no stale-DMA read past the frame. This is exactly the [design 27 §27.8 seed bug](27-fuzz-testing.md) (unchecked `payload_len` vs `wc->byte_len`) — the Seastar client must never repeat it |
| double completion (duplicate/spurious CQE) | second completion is dropped; the promise (and the `temporary_buffer` it would build) is never produced twice → no use-after-free |
| double pool return | detected; free list stays a set, never gains a duplicate index that would hand two owners the same page |
| out-of-bounds `buf_index` on submit | rejected in the C++ `uring_cmd` wrapper before the SQE reaches `urp.ko` (defense in depth — the kernel bounds-checks too, §31.10 Q6) |
| malformed frame mid-stream | hard error, cell fails (`BENCH_FAIL`), buffer recycled, **no resync** — the design-30 §30.5 "trusted stream, no magic-scanning" semantics |
| pool exhaustion under sustained load | `alloc()` back-pressures via a pending future; the shard never deadlocks or busy-spins to OOM |
| leak-under-fault | after a run that injects error CQEs on a fraction of ops, **conservation still holds** — the error path recycles as reliably as the success path |

Harness: Layer 1 is a standalone C++ table binary (no Seastar → cheap, sandboxed
check); Layer 2/3 use `seastar::testing::thread_test_case` + the `fake_ring`
double. The C++ framing joins the design-30 **differential fuzzer** as a third
oracle, so C, Rust, and C++ decoders must agree byte-for-byte or a test fails by
construction.

## 31a.13 Nix integration test environment

Four claims the environment must prove, in increasing cost:

1. **Correctness** — two Seastar `urp-fast` apps exchange byte-exact messages.
2. **High speed** — throughput/latency competitive with the C/Rust clients and
   far above the `AF_UNIX` baseline.
3. **Zero copy** — no software copy on the data path.
4. **No memory pressure after init** — allocation and RSS flat in steady state.

**Sandbox decision (inherited from [design 30 §30.14](30-urp-bench-io-uring.md#3014-integration--targets-and-oracles)):**
io_uring and RDMA are not guaranteed inside the nix build sandbox, so *nothing
that touches the ring or the NIC is a nix `check`* — the checks cover pure logic
only; everything live is a `nix run` app on the host or a phase inside the
microVM (whose kernel and soft-RoCE `rxe` device we control). Seastar is a heavy
dependency, so its build sits in its own derivation (`nix/urp-fast-seastar.nix`,
`pkgs.seastar`, `meta.mainProgram`), gated out of the default `nix flake` build
the way the redpanda targets already are.

### Layers

| Layer | nix target | Substrate | Proves |
|---|---|---|---|
| **L0** pure units | `checks.urp-fast-seastar-units` (Layer-1 table binary; the Seastar fake-ring suites build behind the gated derivation) | sandbox | correctness of logic + the conservation invariant (claims 1, 4-in-unit) |
| **L1** host smoke | `nix run .#urp-fast-seastar-local` (`writeShellApplication`) | one host, `rxe` soft-RoCE over loopback (or `urp.ko` on a single host), **T1 direct**, 1–2 shards, `--verify full` | apps talk (claim 1); memory-stability gate (claim 4) |
| **L2** microVM pair | pair-test **Phase 10h** (successor to 10g) *or* a dedicated `urp-fast-pair-test` | two VMs, `urp.ko` over emulated `rxe` RDMA (**T2/T3 tunnelled**) | cross-host zero-copy talk; tunnelled `BENCH_OK`; `scan_splat` clean (claims 1–3) |
| **L3** full matrix | `nix run .#urp-fast-seastar-matrix` (nightly/manual) | host, msg × batch × shard sweep + `--memcpy-baseline` | speed matrix + three-way delta table (claims 2, 3) |

All four layers reuse the design-30 framing, the `BENCH_OK …` line grammar, and
the matrix runner, so Seastar results aggregate into the **same table** as the
`AF_UNIX` and C/Rust `urp-fast` numbers — a single, directly comparable artifact.

### The four oracles

**Correctness** — design-30 `--verify full`: byte-exact xorshift payload check on
every echoed message; the harness greps `BENCH_OK … verify=full`, exactly as
Phase 10g does today (`vm_run`/`pass`/`fail`/`scan_splat` helpers,
[memory: pairtest gotchas]).

**Speed** — `BENCH_OK` `msgs_per_s`/`mbps`/`p50_us`/`p99_us` aggregated by the
matrix runner into the AF_UNIX-vs-C/Rust-vs-Seastar table. Expected reading:
Seastar ≈ C/Rust within noise (same transport, same syscall profile — a sustained
gap is a Seastar-integration defect, the design-30 §30.1 conformance logic), and
all three `urp-fast` clients pull away from `AF_UNIX` as message size grows.

**Zero copy** — direct copy-counting of an RDMA DMA from userland is not simple,
so the proof is **layered** rather than a single number, and the doc says so
plainly:

1. *Architectural* — no `memcpy` on the data path, enforced by the pool/deleter
   design and the copy inventory ([§31.7](31-urp-fast-zero-copy.md#317-what-actually-got-eliminated--the-payoff-table)); a code-review gate, not a runtime claim.
2. *memcpy-baseline ceiling* — the fast path should approach **link** bandwidth,
   not **memory** bandwidth, at large messages, and beat the `--memcpy-baseline`
   MB/s that the `AF_UNIX` curves top out at (design 30 §30.3). A hidden
   per-message copy would cap throughput at memory bandwidth — visible in the
   table.
3. *Memory-stability* (below) — a per-message copy into fresh buffers would show
   up as allocation churn; a flat allocator is corroborating evidence of in-place
   reuse.
4. *Optional deep proof (nightly)* — an ftrace/eBPF kprobe on `copy_to_iter` /
   `skb_*copy*` shows ~zero data-path hits for `urp-fast` versus many for the
   `AF_UNIX` control: a copy-count **differential**, the most direct evidence,
   run in the microVM where we own the kernel.

**No memory pressure after init** — the strongest and most direct claim, and the
one the user cares about, gets a dedicated runtime oracle:

- Seastar exposes `seastar::memory::stats()` (cumulative allocations, frees, live
  bytes) and a metrics endpoint; sample at `t_warmup` and `t_end`.
- A `writeShellApplication` **`mem-stable`** helper: launch the app, warm up `W`
  seconds, snapshot (`memory::stats()` **and** `/proc/self/status`
  VmRSS/RssAnon), run `R` seconds under load, snapshot again, compute deltas.
- **PASS iff** data-path `allocations(t_end) − allocations(t_warmup) ≈ 0` (only
  pool cycling, no new allocations), live bytes flat, and RSS delta within a
  small slack (logging/metrics only). This is the runtime twin of the Layer-2
  **conservation** unit test — together they prove "malloc at startup, then flat"
  from both the inside (buffer bookkeeping) and the outside (kernel RSS).

### CI wiring

L0 joins the explicit-attr build list in `ci.yml` (the repo avoids
`nix flake check`); L1's host smoke runs nightly where an io_uring-capable runner
exists (self-hosted); L2 rides the existing self-hosted-kvm `microvm-pair` job
like Phase 10g; L3 is nightly/manual. The `scan_splat` dmesg oracle stays armed
after every live phase, and per-cell retry ×3 absorbs `rxe` stream-setup races
(the design-30 Phase 10g lessons carry over unchanged).

## 31a.14 Relation to other docs

- [31-urp-fast-zero-copy.md](31-urp-fast-zero-copy.md) — the parent design; this
  doc is its C++/Seastar client (alongside the C and Rust clients of §31.9).
- [30-urp-bench-io-uring.md](30-urp-bench-io-uring.md) — the benchmark whose
  framing, symmetric roles, matrix, and `BENCH_OK` grammar the demo reuses; the
  `AF_UNIX` baseline it beats.
- [02-architecture.md](02-architecture.md),
  [09-connection-multiplexing.md](09-connection-multiplexing.md) — how Redpanda
  is tunnelled transparently today, the floor this fast path lifts.
- [08-multi-qp-ecmp.md](08-multi-qp-ecmp.md) — the one-QP-per-shard layout that
  Seastar's sharding produces for free.
- [21-kernel-module.md §21.7](21-kernel-module.md#217-code-sharing-strategy) —
  the shared frame codec the demo links rather than re-implements.
- [20-future-work.md §20.2](20-future-work.md#202-shared-memory-fast-path) — the
  "requires a client library" note; `urp_fast_shard` is that library for
  Seastar apps.
- [28-testability.md](28-testability.md) — the table-driven case-array idiom and
  pure-core extraction discipline §31a.12's unit tests follow.
- [27-fuzz-testing.md §27.8](27-fuzz-testing.md) — the RX length-overflow seed
  bug the §31a.12 safety suite exists to prevent recurring in the C++ client;
  the differential harness the C++ framing joins as a third oracle.
- [17-security.md](17-security.md) — the app→kernel trust boundary the negative
  suites and the kernel-side bounds checks (§31.10 Q6) guard.
