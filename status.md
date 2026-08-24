# Project Status

_Last updated: 2026-08-23_

## Current state

`main` is current through **PR #58**. The core thesis — UDS traffic tunneled
over RoCEv2 through an in-kernel module with no application changes — is fully
delivered and **proven on real ConnectX-4 Lx 25GbE hardware** (not just soft-RoCE):
a real Redpanda produce/consume round-trip and a **128/128 `BENCH_OK`
`verify=full`** benchmark matrix both ride the tunnel end-to-end.

Since the kernel-module plan (Phases 0–5) completed, work has been organised by
numbered design doc rather than the original linear phase plan. The major
post-plan workstreams — comprehensive fuzzing (27), the io_uring bench harness
(30), the zero-copy fast path (31), real-hardware integration (32), initiator
connection bring-up & recovery (33), and the bulk-throughput / windowing /
congestion-control designs (34–36) — are summarised below.

Highlights:

- **Real hardware (design 32)** — off emulated `rdma_rxe`: hp1↔hp3 over RoCEv2,
  128/128 bench cells green, full C/Rust interop matrix. Results in
  `docs/design/32-performance-results.md`.
- **Zero-copy fast path (design 31)** — `/dev/urp` `uring_cmd` ABI → pinned-pool
  REGISTER → `fast` endpoint kind → async SEND → zero-copy RECV, plus a
  `uring-cmd` bench backend and measured copy-vs-zero-copy numbers. PR1–PR5b
  merged and hardware-validated.
- **Connection bring-up & recovery (design 33)** — bounded connect-retry with
  capped backoff, reconnect-on-drop, silent-drop probe→retry, and lazy
  connect-on-first-accept; all hardware-verified. A userland gRPC rendezvous
  control plane is code-complete (cold-boot hardware verify pending).
- **Redpanda over UDS-over-RDMA** — metadata **and** full Kafka produce/consume
  round-trip, payload byte-verified, KASAN/KMEMLEAK/lockdep clean.
- **Fuzzing (design 27)** — every attack surface fuzzed; three real
  memory-safety bugs found and fixed.

## Where we are — kernel-module plan phases

| Phase | Plan doc | Status |
|---|---|---|
| **0** — Skeleton | `docs/KERNEL-MODULE-PLAN.md` §0 | ✅ Committed (`3a32ffc`) |
| **1 — k0** RDMA echo data path | `docs/KERNEL-MODULE-PLAN.md` §1 | ✅ Committed (`d440794`, `a986fe7`, `a600b17`) |
| **2** — GENL control plane | `KERNEL-MODULE-IMPLEMENTATION.md` §Phase 2 | ✅ Committed (`067829e`); 19/19 integration tests |
| **3a** — k1 data path (multi-QP, SRQ, credit FC, reorder, streams) | `KERNEL-MODULE-PLAN.md` §3 + IMPL §3a | ✅ Steps 1–10 + initiator multi-stream (`74df057`); KASAN-clean 12-stream burst |
| **3b** — probes / PSK | `KERNEL-MODULE-PLAN.md` §3.6–§3.7 + IMPL §3b | ✅ Probes (PING/PONG/RTT EWMA/state machine) + PSK SHA-256 in `rdma_cm` `private_data` |
| 3c — KUnit hardening + soak | (deferred) | superseded: 34 KUnit cases in `kernel/urp_test.c`; 1-hour soak landed in Phase 4 |
| **4** — k2 optimized | `KERNEL-MODULE-PLAN.md` §4 + IMPL §4 | ✅ rxe scope + 1-hour soak PASS (1240 cycles, 120 churn add/remove, 0 errors). §4.2–4.5 (zero-copy in copy path, adaptive CQ, kthread NUMA bind, hw bench) folded into the design-31/32 hardware work |
| **5** — MicroVM integration | `KERNEL-MODULE-PLAN.md` §5 + IMPL §5 | ✅ x86_64 + aarch64 (TCG) pair tests, kernel matrix 6.1/6.6/6.12/7.1, cross-arch, CI + nightly |
| 6 | — | superseded by the design-doc-driven workstreams below |

## Major workstreams (post-plan, by design doc)

| Design | Workstream | Status |
|---|---|---|
| 27 | Fuzzing program | ✅ Complete — F0–F3 automated; 3 real bugs fixed |
| 30 | `urp-bench` io_uring UDS benchmark (C + Rust twins) | ✅ Complete — 7 modes, differential fuzz, matrix |
| 31 | urp-fast zero-copy path | ✅ PR1–PR5b merged + hardware-validated. PR6 (Rust backend) / PR7 (Seastar) roadmap |
| 31a | C++/Seastar demo client | ⏳ Deferred (roadmap PR7) |
| 32 | Real-hardware RoCEv2 integration | ✅ Matrix 128/128; extra probes + Results-caveat flip pending |
| 33 | Initiator connection bring-up & recovery | 🟢 Phases 0–2 hardware-verified; Phase 3 (gRPC) code-complete, cold-boot verify pending |
| 34 | Bulk throughput | ✅ Measured on hardware; copy-vs-zero-copy captured |
| 35 | Windowing / flow control | 🟢 Phase 1 (pump completion waitqueue) merged; further windowing spec'd |
| 36 | CUBIC congestion control | 📝 Design-only, LATER (fabric near-lossless, path is pump-bound) |

## Redpanda over UDS-over-RDMA

Real `rpk` ↔ real Redpanda broker, Kafka bytes over the URP RDMA data path
(single host, soft-RoCE, RDMA loopback). Broker + `rpk` built from the redpanda
fork flake (`randomizedcoder/redpanda@d4b44629`, carrying the native Kafka UDS
listener from PR #30240).

- **Metadata round-trip** (`nix run .#test-redpanda-uds`, root): 9/9 pass; `rpk
  cluster info` returns the real cluster id + broker list over RDMA.
- **Full produce/consume** (`nix run .#test-redpanda-produce-consume`, root):
  11/11 pass; `rpk topic create` + produce + consume, consumed payload matches
  byte-for-byte. The broker advertises a client-local bridge (`127.0.0.2:9092`)
  so create/produce/fetch all ride RDMA (UDS is non-advertisable).

## Real-hardware RoCEv2 integration (design 32)

First validation off emulated `rdma_rxe`: **hp1 (acceptor) ↔ hp3 (initiator)**,
ConnectX-4 Lx 25GbE, RoCEv2, both kernel 7.1.8, PTP time sync (`ptp4l`/`phc2sys`,
HW timestamping confirmed).

- `nix/nixos-module.nix` → `nixosModules.urp` (per-endpoint systemd units) and
  `nix/urp-hw-matrix.nix` → `urp-hw-matrix` runner (ssh-driven, `nix copy` bench
  closures, per-cell `BENCH_OK` assert, 4-combo interop + RTT p50/p99).
- **Matrix: 128/128 `BENCH_OK` `verify=full`, 0 fail, 0 skip** across c↔c /
  c↔rust / rust↔c / rust↔rust × {blocking, uring-rw, uring-fixed, uring-bufring}
  × {24, 1024, 4076, 65516} × {1, 16} batch. Full numbers, methodology, and the
  interop table in **`docs/design/32-performance-results.md`**.

Five first-real-HW bugs were found and fixed to get here — the headline one being
the **credit-grant stall** (`bd133dd`): the stream flow-control handshake never
exchanged a credit grant on real hardware (remote credits stuck at `r0`), which
the emulated-rxe path had masked. Also fixed: stale-UDS-socket unlink, acceptor
QP-slot leak, an `ib_free_cq` cleanup WARN, and establishment order sensitivity.

Still open in design 32: the extra single-purpose probes (`urp-test-client`
scenario sweep, `urp-fast-poc` on hardware) are not yet run, and the design-32
Results-caveat flip / `status` refresh for those probes is pending.

## Initiator connection bring-up & recovery (design 33)

Layered plan (fix compounding bugs → bounded retry → lazy connect → gRPC
rendezvous), tracked in `docs/design/33-implementation-status.md`.

- **Phase 0 — compounding recovery bugs** ✅ hardware-verified (`5bfcd37`):
  acceptor QP-slot leak on half-open child, and the initiator's stale UDS listen
  socket never being unlinked. Both were prerequisites for any auto-recovery.
- **Phase 1 — bounded connect-retry + backoff** ✅ hardware-verified: capped
  exponential backoff, reconnect-on-drop, runtime `/proc/sys/urp/` tunables
  (`connect_max_attempts` / `connect_backoff_base_ms` / `connect_backoff_ceil_ms`).
  **Phase 1.5** closes the silent-drop gap (hard peer reboot delivers no CM
  event): a single-QP initiator now emits liveness PINGs; on missed PONGs it
  demotes the QP and schedules the same reconnect. Verified via SysRq-b hard
  drop → hp3 self-heals with no manual restart.
- **Phase 2 — lazy connect on first UDS accept** ✅ hardware-verified: an idle
  endpoint holds no RDMA resources; the dial fires on first accept (one-shot
  `connect_started`). All 6 scenarios pass incl. 128/128 through the lazy path,
  clean drain of a never-connected endpoint, boot-race safety-net, fail-fast
  after exhaustion.
- **Phase 3 — userland gRPC control plane** 🟢 code-complete + sandbox-verified
  (PRs #46 netlink lib, #47 proto+daemon, #48 auth-before-slot, #49 NixOS units).
  `urp-control` (tonic/prost) serves `Rendezvous` + `Heartbeat`; the initiator
  gates the app on peer-ready via `sd_notify READY=1`. gRPC-OK is a hint, not a
  RoCEv2 guarantee, so the Phase-1 kernel retry stays the safety net.
  **Open:** the hp1/hp3 cold simultaneous-boot integration verify (rendezvous →
  gate → lazy connect → `established` → `BENCH_OK`, zero manual steps) is still
  pending (Task #42).

## urp-fast zero-copy (design 31 + 31a)

The opt-in fast path (`docs/design/31-urp-fast-zero-copy.md`): an aware app hands
its own buffer pool to `urp.ko` over `IORING_OP_URING_CMD` into a `/dev/urp` char
device, the pool is registered against a connected endpoint's PD, and the NIC
DMAs straight into/out of the app's pinned pages — the last software copy the
`AF_UNIX` path (design 30) provably can't remove.

**PR1–PR5b merged (#36 … #58) and hardware-validated:**

- **PR1** — `/dev/urp` misc device + `->uring_cmd` fop; shared ABI
  (`include/uapi/linux/urp_cmd.h`), trust-boundary validators as a dual-compile
  pure core (KUnit + `urp-fast-validate-units`).
- **PR2** — `REGISTER` binds to a named connected endpoint, DMA-maps every pinned
  page against its RDMA device, shares the endpoint's PD (`ib_dma_map_page` +
  `local_dma_lkey`).
- **PR3a/PR3b** — `fast` endpoint kind (`CONFIG_URP_FAST`) + zero-copy SEND data
  path with a per-buffer ownership state machine.
- **PR4** — zero-copy RECV data path + teardown quiesce.
- **PR5a** — a `uring-cmd` (fast) backend for `urp-bench` (C twin), driving the
  same bench core over the zero-copy path; VM Phase 10j proves exact-byte
  delivery over rxe. Also fixed a uds→fast probe-liveness interop gap.
- **PR5b** — fast bw + RTT matrices, hp1/hp3 run, and the measured
  copy-vs-zero-copy comparison written into design 34.
- Fixed post-merge: the fast REGISTER pinned the pool with bare
  `pin_user_pages()` from an io-wq context holding no `mmap_lock`, tripping a
  `rwsem.h:81` WARN; switched to `pin_user_pages_fast()` (PR #58, `c52f110`),
  validated on hp1/hp3 (WARN count 0 across a ~2-day soak).

**Roadmap:** PR6 Rust `crates/urp-fast` backend (C-vs-Rust differential parity /
fuzz) + `uring_cmd` decoder fuzz target; PR7 C++/Seastar client (design 31a).
One-sided `RDMA_WRITE`+rkey (design 34 Option D) stays deferred.

## io_uring UDS benchmark (design 30)

`urp-bench`: a symmetric userland benchmark pair (C + liburing and Rust + the
`io-uring` crate, same CLI and wire framing) driving the UDS side with io_uring.
**Complete** — pure cores table-tested in both languages (877 C checks / 15
mirrored Rust suites, identical cross-language hex vectors, miri); **all 7
io_uring modes live** in both twins and interop C↔Rust; `fuzz-bench-deframe` +
cargo-fuzz differential (C-vs-Rust) in CI; first userland C static analysis
(clang-tidy + cppcheck). The `uring-sendzc` probe answered the design's core
question: on 7.1.6 **AF_UNIX has no zero-copy send** (`result=eopnotsupp`) — the
copy-path win is syscall batching (0.9–1.4 → 0.01–0.09 syscalls/msg), which is
exactly what motivated the design-31 zero-copy path.

## Fuzzing program (design 27 — complete)

Tracks F0–F3 all implemented and automated:

- **F0/F1 hermetic harnesses** (libFuzzer + ASAN/UBSan, compiling the REAL kernel
  C): `fuzz-classify` (RX frame classifier), `fuzz-rx-seq` (classify → dispatch →
  stream state machine), `fuzz-reorder` (C rbtree backend vs a spec model). Plus
  5 cargo-fuzz targets on the shared Rust crate.
- **F2 live-VM fuzzers** (phases of the sanitizer microVM pair test, KASAN +
  KMEMLEAK + lockdep + KCOV kernel): blind + KCOV coverage-guided netlink
  fuzzers, a multi-threaded netlink racer, and a hostile-peer RDMA **wire** fuzzer
  — each followed by an inline `dmesg` `scan_splat` oracle.
- **F3 CI**: every push runs `fuzz-smoke`; nightly runs `fuzz-long` + the
  sanitizer pair test with all F2 phases. Reproducers under `fuzz/regressions/`.

Real bugs found or closed: `SET_ENDPOINT` num_qps/buffer_count OOB teardown
(coverage-guided fuzzer → `-EBUSY` on active endpoints); endpoint lookup-vs-DEL
UAF (kref + release-via-RCU); verbose-GET-vs-DEL sub-object UAF (`ep->lock` across
the verbose fill).

## Static analysis / upstream readiness (design 26)

`nix build .#analysis-all -L` runs sparse, smatch, checkpatch --strict, W=1/W=2,
coccicheck, clippy, rustfmt — all **0** except a small documented checkpatch
residual (24, every item justified in `docs/design/26-upstream-readiness.md`
§26.4). Three real bugs found and fixed in the first pass (NLA_POLICY_RANGE
s16-truncating `URP_BUFFER_SIZE_MAX`; a 5.9 KiB stack frame in
`urp_new_endpoint_doit`; a QP-slot leak on the `urp_cm_accept_one` OOM path).
Manual-run only; not wired into CI.

## Known gaps

Previously-listed functional gaps that are now **closed** (were open in the
2026-08-12 status):

1. **Per-stream reorder buffer** — wired into the RX path
   (`urp_rx_deliver_stream`, `kernel/urp_rdma.c:520–544`) and now covered by
   comprehensive **table-driven unit tests** (positive / negative / boundary /
   corner + both `-ENOMEM` paths). The shared op-script table
   `kernel/urp_reorder_cases.h` drives both the in-kernel KUnit suite
   (`test_reorder_scenarios` / `test_reorder_arg_guards`) and the userspace
   `urp-reorder-units` check (compiles the *real* backend under ASAN/UBSan; in
   `ci-local`), mirrored in the Rust twin (`reorder.rs`). **The reorder *code* is
   verified.** The real-hardware exercise under multi-QP arrival skew remains
   **blocked — not by the reorder buffer, but by gap #6**: the first multi-QP data
   run (2026-08-23, `.#urp-reorder-matrix`) never reaches clean delivery, so the
   buffer cannot yet be exercised end-to-end on real skew.
2. **`buffer_count` / `buffer_size` endpoint config** — now drives live pool
   geometry: `urp_endpoint.c:284–285` resolves `ep->num_bufs` / `ep->buf_size`
   from the config (via `urp_resolve_num_bufs` / `urp_resolve_buf_size`), feeding
   pool depth, CQ/SRQ/SQ sizing, credit window, and DMA slot bytes. **Minor
   drift:** a stale comment at `kernel/urp_netlink.c:575–577` still claims
   `buffer_count` does not size the pool (references "design doc 29 Gap 2") — the
   code contradicts it; the comment should be corrected.

Genuinely open:

3. **gRPC control-plane cold-boot verify (design 33 Phase 3).** Code merged and
   sandbox-verified; the hp1/hp3 cold simultaneous-boot end-to-end run (zero
   manual steps) is still pending (Task #42).
4. **Design 32 extra hardware probes.** The `urp-test-client` scenario sweep and
   an on-hardware `urp-fast-poc` run are not yet done, and the design-32 Results
   caveat has not yet been flipped for them.
5. **Second concurrent initiator endpoint (design 30 Phase 10g finding).** A
   2026-08-12 finding that a second concurrent *initiator* endpoint never started
   its CM machinery. The design-33 initiator bring-up rework (lazy connect /
   retry) likely supersedes it, but this specific multi-initiator-in-one-module
   case has not been explicitly re-verified.
6. **Multi-QP data path — FIXED for realistic frames (found 2026-08-23, resolved
   2026-08-24).** The first multi-QP transfer on hp1↔hp3 (`.#urp-reorder-matrix`,
   sweeping `num_qps ∈ {1,4,8}`) wedged on **two independent root causes**, both
   now fixed: (A) a **connect-handshake race** — the acceptor allocated QP slots
   from a bare monotonic counter with no per-QP identity, so concurrent per-QP
   retries collided (`rejecting extra CONNECT_REQUEST (8 >= 8 QPs)` / retry storm);
   fixed by identity-based slot alloc carrying `qp_index` in the CM private_data
   (PR #60). (B) **no real flow control** — the credit gate was best-effort (posted
   at zero credits), so N QPs flooded the shared SRQ → RNR → `transport retry
   counter exceeded` → permanent sequence gaps → `reorder_drops` explode; fixed by
   **byte-windowing** (design 35 §35.3): a blocking per-stream sender gate bounding
   in-flight bytes + cumulative-absolute CREDIT-BYTES grants, both peers negotiating
   the capability in the CM trailer (PR #61 wire/default-off, PR3 gate/behaviour-on).
   Two further HW-only bugs surfaced and fixed under PR3: a **SYN-race** (a DATA
   frame arriving before its stream's SYN on a different QP was dropped → gap the
   windowed sender can't refill → deadlock; fixed by implicit stream creation) and
   the **reorder depth coupling** for tiny frames (MIN_FRAME 64→16). A third,
   **pre-existing** bug then surfaced at the smallest sweep size: a QP-health
   **PONG frame is 68 bytes** (`URP_FRAME_HEADER_SIZE 20 + URP_PONG_PAYLOAD_SIZE
   48`) but `buffer_size=64` posts 64-byte recv buffers, so the first PONG overran
   the buffer → ib `local length error` → NAK → `remote invalid request` → QP
   crash-loop (windowing-independent: it reproduced with `advertise=0`, and per-QP
   so it failed even at `num_qps=1`). Fixed by raising `URP_BUFFER_SIZE_MIN` to
   `URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE` (= 68) — the netlink policy now
   rejects sub-68 buffers with ERANGE rather than letting them crash-loop.
   **HW result: reorder-matrix 9/9 GREEN** — every cell (68 / 4096 / 65516 ×
   `num_qps ∈ {1,4,8}`) passes `BENCH_OK verify=full`, `reorder_drops=0`,
   `buffer_alloc_fails=0`, no dmesg WARN, no deadlock (byte window took drops
   millions→0). Repro: `nix run .#urp-reorder-matrix -- hp1 hp3 10.10.2.1`.
7. **Roadmap (not gaps, deferred by design).** urp-fast PR6 (Rust backend) / PR7
   (Seastar); design 35 further windowing; design 36 CUBIC congestion control
   (now motivated by gap #6 — multi-QP bursts exhaust RC transport retries, so the
   fabric is *not* near-lossless under multi-QP load); one-sided `RDMA_WRITE`+rkey
   (design 34 Option D).

## Working-tree hygiene notes

- `result*` Nix build-output symlinks are gitignored (`result`, `result-*`) and
  must not be committed.
- `.claude/` — Claude Code session state; not committed.
- `fuzz/Cargo.lock` **is committed intentionally** (reproducible fuzz-target
  builds, same rationale as the workspace `Cargo.lock`).
