# 29. Code Review + Refactor Plan

Status: **review + plan** — 2026-08-11. Product of a full
docs-vs-implementation review: three parallel audit passes (documentation,
kernel + Rust code, nix layer) over the tree at `main` (post-PR #21). The
doc-accuracy fixes landed separately (core-docs refresh + legacy-doc
banners); this document records what the *code* review found — two
functional gaps, a dead-code inventory, and a prioritized refactor backlog
for code that grew organically and is now worth restructuring for
readability and newcomer onboarding.

Scope rule for the refactor items: **structure/layout/readability only** —
behavior-preserving, validated by the existing gates (kernel build matrix,
KUnit, `test-kmod-k0`, microVM pair tests, fuzz harnesses, `analysis-all`).
The two functional gaps are the exception: they are *feature* work, each
deserving its own PR with soak/fuzz validation.

## 29.1 Functional gaps (the review's most important findings)

### Gap 1 — the reorder buffer is built, tested, fuzzed… and never called

Evidence chain:

- Each stream allocates a reorder buffer: `urp_reorder_alloc(0, 256)` at
  `urp_stream.c` (stream create), freed on stream destroy.
- The **only** read anywhere in the data path is
  `urp_reorder_gap_count(s->reorder)` for netlink display.
- There is **zero** call to `urp_reorder_insert()` /
  `urp_reorder_drain_next()` outside `urp_reorder.c`, `urp_reorder_rust.c`,
  and `urp_test.c`. `urp_recv_done` delivers frames in *completion order*
  directly to `kernel_sendmsg`.
- `ep->stats.reorder_insertions` / `reorder_drops` are declared, exported
  over netlink, and printed by the CLI — and never incremented.

So the entire reorder surface — the default C rbtree backend
(`urp_reorder.c`), the opt-in Rust backend (`CONFIG_URP_REORDER_RUST` →
`urp_reorder_rust.c` → the FFI staticlib → `reorder.rs`), their KUnit
suites, and two fuzz harnesses (`.#fuzz-reorder`, cargo-fuzz `reorder_ops`)
— currently serves code the RX path never invokes.

Why it hasn't hurt: on a single QP (and on rxe loopback generally), RC QP
completions arrive in order, so completion order == sequence order. The gap
becomes real on **multi-QP over real ECMP paths** — exactly the design-08/09
scenario the buffers were written for.

**Wiring plan** (own PR, not part of the mechanical refactors):

1. In the RX DELIVER_STREAM path, compare the frame's sequence number to the
   stream's next-expected counter; in-order frames deliver directly
   (fast path), out-of-order frames go through `urp_reorder_insert()`
   followed by a `urp_reorder_drain_next()` loop.
2. Increment `reorder_insertions` on buffered insert and `reorder_drops` on
   reject (-ENOBUFS/-EEXIST); the already-exported stats become truthful.
3. Decide the sleep-context story: `urp_recv_done` runs in CQ softirq
   context; drain delivery calls `kernel_sendmsg` (may sleep). Today the
   code already defers blocking work — the drain loop must live on the same
   deferred path.
4. Validation: the existing `.#fuzz-reorder` spec-model oracle, the KUnit
   suites, the pair test with `--num-qps > 1`, and a tc-netem reorder run
   (design 12 §12.7) that actually forces cross-QP reordering.

### Gap 2 — `buffer_count` / `buffer_size` are accepted and ignored

The endpoint attributes are parsed (`urp_netlink.c`), range-validated by
policy, defaulted (`urp_endpoint.c`), reported back over GET, and guarded
against change on an active endpoint (`-EBUSY`) — but nothing sizes off
them: the buffer pool is compile-time `URP_NUM_BUFS` / `URP_BUF_SIZE`
(`urp.h`), used by `urp_bufs_init`, the SRQ depth, and the CQ sizing. A
comment in `urp_netlink.c`'s SET handler even claimed the pool scales from
`buffer_count` (corrected in the mechanical pass).

**Wiring plan** (own PR): plumb `ep->buffer_count`/`ep->buffer_size` into
`urp_bufs_init()` + SRQ/CQ sizing with the compile-time values as
defaults/caps, or — if config-driven sizing is not wanted — remove the
attributes from the UAPI before any ABI stability promise makes that
impossible. Either way the current state (silently accepted, silently
ignored) is the worst of both.

## 29.2 Dead-code inventory (kernel)

| Symbol | Location | Disposition |
|---|---|---|
| `urp_post_recv()`, `urp_post_recv_for_qp()` | `urp_rdma.c` | **Delete** — superseded by the SRQ path; no callers. |
| `urp_stream_tx_rst()` | `urp_stream.c` | **Delete** — no callers. |
| `struct urp_endpoint.rx_work`, `.rx_wq` | `urp.h` | **Delete** — declared, never initialized or used. |
| `struct urp_stream.rx_next` | `urp.h` | **Delete** — written once, never read. |
| `URP_RQ_DEPTH` | `urp.h` | **Delete** — unreferenced (`max_recv_wr = 0` under SRQ). |
| `urp_streams_reap()` | `urp_stream.c` | **Keep** — deliberately unhooked pending the FIN-handshake reap design; move the explanatory NOTE (currently in `urp_rdma.c`, 400 lines away) to the definition. |
| `urp_reorder_drain_pending()` | both backends | **Keep** — part of the backend contract; becomes live with Gap 1. |
| `urp_qp.consecutive_pongs` | `urp.h` | **Keep** — feeds the designed (deferred) qualifying→active promotion; documented at the field. |
| `URP_QP_STATE_REMOVED` | UAPI | **Keep** — UAPI enum slot for the designed working-set management; never assigned yet. |

The "Delete" rows are executed by the kernel mechanical-refactor PR.

## 29.3 Kernel refactor backlog (prioritized; S/M/L = effort)

Executed in the mechanical pass (S):

1. **`urp_post_frame()` helper** — the ~15-line `ib_post_send` boilerplate
   (WR setup, cqe, sge, error → `urp_buf_free_send`) exists 5× in
   `urp_pump.c` (legacy TX, stream TX, PING, PONG, CREDIT emitters).
2. **`urp_cm_id_destroy()` helper** — `rdma_destroy_id` + `kfree(ctx)`
   appears 5× in `urp_rdma.c`.
3. **GENL preamble helper** — parse → NAME check → extack → `nla_strscpy` →
   `urp_endpoint_get` → extack is copy-pasted across the DEL/SET/GET
   handlers; plus one immutable-u32 guard helper for the two identical SET
   blocks (num_qps / buffer_count).
4. **`pr_fmt` uniformity** — only 6 of 15 `.c` files define it; log lines
   from the other 9 carry no `urp:` prefix.
5. **Comment truth fixes** — the false pool-scaling claim in the SET
   handler; the stale "num_qps capped at 1" claim in `urp_qp.c`.
6. **`qps` → `qp` rename** — several functions take a *single* QP through a
   parameter named `qps` (`urp_emit_ping_on`, `urp_emit_credit_frame`, the
   pump locals), right next to `ep->qps` which really is the array.

Planned follow-ups (M/L):

7. **Split `urp_rdma.c` (1,127 lines, ≥5 concerns)** into `urp_bufs.c`
   (buffer pool), `urp_cm.c` (CM handshake + accept), `urp_rx.c`
   (`urp_recv_done` + helpers), leaving init/teardown + QP creation in
   `urp_rdma.c`. The seams already exist as comment banners. (M)
8. **Split `urp_recv_done` (222 lines)** into per-action handlers
   (`urp_rx_credit`, `urp_rx_pong`, `urp_rx_deliver`, repost) so the
   classifier's action enum reads as a dispatch table. Pairs naturally
   with item 7 and with the Gap-1 wiring. (M)
9. **Finish the `urp.h` split** (528 lines): compat shims → `urp_compat.h`;
   pool constants + `struct urp_buffer` → `urp_bufs.h`; `struct urp_qp` +
   prototypes → `urp_qp.h`; `struct urp_stream` → `urp_stream.h`;
   `struct urp_endpoint` (107 lines, six concerns) → `urp_endpoint.h`;
   `urp.h` becomes a ~60-line umbrella. Precedent already in-tree:
   `urp_frame.h`, `urp_stream_sm.h`, `urp_credit.h`, `urp_reorder.h`.
   Include-order constraints (frame header last, SM header after UAPI)
   become per-header instead of prose. (L)
10. **Unify the two TX pump loops** — `urp_tx_thread_fn` and
    `urp_stream_tx_fn` (`urp_pump.c`) are structurally identical
    (recv → encode → select QP → credit → post → counters); differences
    (id/seq source, SYN-on-first, EOF action) fit a small ops struct. (M)
11. **Split `urp_fill_endpoint` (212 lines)** into `urp_fill_qps` /
    `urp_fill_streams` / `urp_fill_stats`; removes two inline
    rhashtable-walk unwinds. (S–M)
12. **rhashtable-walk helper** — 5 hand-rolled walk loops with subtly
    different error handling (`urp_netlink.c` ×2, `urp_endpoint.c`,
    `urp_stream.c` ×2). (S)
13. **Present-tense comment pass** — ~140 "Phase N Step M" comments narrate
    when code was written rather than what it does, and several are stale;
    convert load-bearing ones to present tense, drop the rest (git blame
    keeps the history). (M, mechanical but wide)

## 29.4 Wire-format single source of truth

The 20-byte frame header currently has **four independent definitions**:

1. `kernel/urp_frame.h` (+ UAPI constants) — normative.
2. `crates/uds-rdma-protocol/src/frame.rs` — Rust twin, differentially
   tested/fuzzed against (1). Acceptable duplication: it *is* the oracle.
3. `nix/fuzz/urp_fuzz_shim.h` — includes the real UAPI header + `urp_frame.h`
   but hard-codes `URP_MAX_PAYLOAD` as a manual mirror of `urp.h`.
4. `tools/urp-test-client.c` — a private re-implementation of
   `urp_frame_encode`/decode plus re-defined constants, bound to the real
   header only by a comment.

Plan: (4) is fixed in the CLI/tools mechanical PR — the test client includes
the UAPI header + `urp_frame.h` directly (both are kernel-infra-free; the
fuzz shim proves the include works). For (3), move the `URP_MAX_PAYLOAD`
definition into the UAPI header (or a shared constants header) so the shim
stops mirroring it. (2) stays, guarded by the differential tests, plus the
CLI's `uapi.rs` header-grep drift test.

## 29.5 CLI backlog (`crates/urp-cli`)

Executed in the mechanical pass (S): dedupe `align4` (defined identically in
`attr.rs` and `netlink.rs`); extract the copy-pasted "one endpoint by name,
or dump all" fetch shared by `show`/`stats`; dedupe the `--num-qps`
value_parser between `add`/`set`.

Follow-ups: split `format.rs` (467 lines) into `model.rs` (serde structs +
attribute parsers) and `render.rs` (the three output formatters) (S–M);
consider making `urp-cli` consume `uds-rdma-protocol` instead of keeping
`uapi.rs` and the protocol crate's `constants.rs` as two independent mirrors
of the same C header (M — the header-grep test makes the current state safe,
just duplicated).

## 29.6 Nix refactor backlog

Executed in the mechanical pass (S): delete `nix/kmod-local.nix` (never
imported; its `.#urp-ko-local` attr never existed); delete dead let-bindings
in `microvms/lib.nix` (`diagDir`, the mislabeled `T_DRAIN`); drop unused
re-exports (`microvms/default.nix` top-level attrs, `analysis.sparseMaster`);
unify the four hand-copied `builtins.path` source filters
(`checks.nix` / `urp-cli.nix` / `urp-protocol-ffi.nix` / `fuzz/default.nix`)
into `nix/lib/sources.nix`; finish the fuzz factory so the four hand-rolled
live-fuzzer derivations (`netlinkFuzz`/`covFuzz`/`raceFuzz`/`wireFuzz`) are
generated and get kebab-case names (`fuzz-netlink`, `fuzz-netlink-cov`,
`fuzz-netlink-race`, `fuzz-wire`); consolidate `mkVm.nix`'s four separate
`import ../fuzz` calls; share the kbuild invocation strings duplicated
between `checks.nix` and `analysis/kbuild-check.nix`.

Planned follow-ups:

1. **Split `microvms/lib.nix`** (778 lines; `fullPairTest` is a single
   655-line shell string) into per-phase script fragments under
   `microvms/phases/` or externalized `.sh` files (shellcheck-at-source,
   like `vm-expect.exp` already is); replace the 8 copies of the
   `if [ "$label" = vm1 ]` port/host lookup with a helper. (L)
2. **Shared harness prelude** — the rxe-discovery + echo-server setup block
   (~45 lines) and the `log/warn/fail/pass` + cleanup-trap boilerplate are
   copied across `test-kmod-k0.nix`, `soak-1h.nix`, both redpanda tests,
   and `lib.nix` (one copy has already drifted: the soak variant lost the
   echo-server health check). (M)
3. **Dedupe the redpanda harnesses** — `test-redpanda-uds.nix` and
   `test-redpanda-produce-consume.nix` are ~35% identical. (M)
4. **Attr naming normalization** (breaking, batch it): five conventions
   coexist (`urp-ko` / `netlinkFuzz` / `test-kmod-k0` / `soak-1h` /
   `microvm-vm1` vs `urp-microvm-vm1-serial`); `-debug` and `-aarch64`
   occupy the same suffix slot so debug×cross is inexpressible. Adopt
   `<base>[-<variant>][-<arch>]`; generate `apps` from the
   `writeShellApplication` packages instead of hand-writing 2 of ~49. (M)
5. **Move cross-arch + variant wiring out of `flake.nix`** — the 131-line
   `let` re-imports `checks.nix` under `pkgsCross` (duplicating
   `microvms/default.nix`'s `mkCrossArch`), and hand-fans-out the four
   testVm/urpVm variants. (M)
6. **Retire the legacy VM stack?** — `test-vm.nix` + `urp-vm.nix` (+
   `urp-vm`/`urp-vm-debug` attrs, 389 lines) predate `microvms/` and run in
   no CI job; the tracker last recorded the debug variant boot-panicking.
   Needs an owner decision: delete, or fix and document a use case
   (interactive single-VM debugging) the microVM pair doesn't cover. (S–M)
7. **Runtime `nix build` inside `fullPairTest`** — the pair test shells out
   to `nix build .#microvm-vm1` at *run* time (requires CWD = flake
   checkout, re-evaluates the flake); close over the store paths at eval
   time instead. (M)
8. **Sanitizer kernel config defined twice** — `test-vm.nix` and
   `microvms/mkVm.nix` carry different KASAN/KMEMLEAK config sets; share
   one definition (moot for `test-vm.nix` if item 6 deletes it). (S)

## 29.7 What a newcomer hits first (orientation notes)

The review's newcomer-confusion findings, recorded so onboarding docs can
address them:

1. **Two coexisting data paths**: the legacy k0 single-connection
   (`ep->conn`, stream_id 0, `URP_RX_DELIVER_LEGACY`) runs alongside the
   multiplexed stream path. Which is live depends on side and QP index —
   nothing states this in one place (the DESIGN.md architecture section now
   does).
2. **"Phase N Step M" comment archaeology** as primary in-code
   documentation (§29.3 item 13).
3. **The reorder buffer looks central and is inert** (§29.1 Gap 1) — hours
   of misdirected reading until discovered.
4. **Four wire-format definitions** (§29.4).
5. **Two fuzz trees** (`fuzz/` cargo-fuzz vs `nix/fuzz/` C harnesses) with
   a shared `fuzz/regressions/` convention named after the C targets —
   design 27 is the map; nothing in-tree at the top level points to it
   (README now does).

## 29.8 Relation to other documents

- [26-upstream-readiness.md](26-upstream-readiness.md) — the maintainer
  follow-ups there (netns, lock scope, waitqueue pumps, `urp.h` split,
  annotations) overlap items here; design 26 tracks the *upstreamability*
  view, this document tracks the *structure/onboarding* view and the
  functional gaps.
- [27-fuzz-testing.md](27-fuzz-testing.md) — the fuzz harnesses are the
  safety net that makes the refactors in §29.3 cheap to validate.
- [28-testability.md](28-testability.md) — extraction items here continue
  the "decide, then act" program design 28 started (E1/E2 are done; the
  `urp_recv_done` split is its natural continuation).
- `status.md` — carries the two functional gaps as the standing "Known
  functional gaps" list.
