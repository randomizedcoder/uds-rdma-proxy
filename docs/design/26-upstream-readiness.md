# 26. Upstream Readiness: Static Analysis + Kernel-Maintainer Review

Status: **living document** — first pass 2026-08-09.

This document tracks what stands between `kernel/` and a serious upstream
(netdev/RDMA) review: the static-analysis tooling now wired into the flake,
the baseline it found, what was fixed, what is deliberately kept, and the
deeper items that remain as follow-ups.

## 26.1 Tooling

`nix/analysis/` (patterned on xdp2's analysis framework) provides
report-only derivations — findings never fail the build; each target emits
`report.txt` (findings in our files only), `count.txt`, and the full
`build.log`:

| target | tool | notes |
|---|---|---|
| `analysis-sparse` | sparse (upstream master, pinned) | `make C=2 CHECK=sparse`. nixpkgs' 2024 sparse predates `__typeof_unqual__`, which kernels >= 7.x force on under `__CHECKER__` — hence the pinned-master override in `nix/analysis/sparse.nix`. |
| `analysis-smatch` | smatch 1.74 (nixpkgs) | `make C=2 CHECK="smatch -p=kernel"`; no cross-function DB (fine for out-of-tree). |
| `analysis-checkpatch` | checkpatch.pl from the target kernel's own source tree | `--no-tree --strict --terse --show-types --file` over every kernel source file. Fully hermetic: the nixpkgs kernel `.dev` output ships the whole source tree, including `scripts/`. |
| `analysis-w1` / `analysis-w2` | gcc via kbuild `W=1` / `W=12` | |
| `analysis-coccicheck` | coccinelle 1.3.1 + the kernel's `scripts/coccinelle/` suite | `make M=... coccicheck MODE=report`. |
| `analysis-clippy` / `analysis-rustfmt` | pinned nightly toolchain | clippy per-crate (the no_std FFI staticlib needs `--lib --release`; `--all-targets` would link std into its test harness and collide with its `#[panic_handler]`). |
| `analysis-all` | aggregate | symlink farm + `summary.txt` count table. |

Usage: `nix build .#analysis-all -L && cat result/summary.txt`. All targets
are manual-run by design — none gate `checks`/CI until the residual counts
are argued down to ~0 (candidates to promote first: checkpatch, clippy,
rustfmt — cheap, no kbuild).

Everything runs against `linuxPackages_latest` (the same kernel the
`kernel-module-build` CI gate compiles), overridable via the
`kernelPackages` argument of `nix/analysis/default.nix`.

## 26.2 Baseline -> after first fix pass

| tool | baseline | after | residual is |
|---|---|---|---|
| sparse | tree unparseable (see 26.3 item 5) | **0** | — |
| smatch | 5 | **0** | — |
| checkpatch | 79 | **24** | all intentional, see 26.4 |
| gcc W=1 | 2 | **0** | — |
| coccicheck | 0 | **0** | — |
| clippy | 8 + a broken invocation | **0** | — |
| rustfmt | 14 files drifted | **0** | — |

## 26.3 Real bugs the tools found (fixed in this pass)

These are the reason the tooling exists — none were style:

1. **`NLA_POLICY_RANGE` u16 truncation** (`urp_netlink.c`): the netlink
   policy for `URP_ENDPOINT_A_BUFFER_SIZE` used
   `NLA_POLICY_RANGE(NLA_U32, ..., URP_BUFFER_SIZE_MAX)`. That macro packs
   its bounds into `s16`; 65536 silently became **0**, breaking buffer-size
   validation. Fixed with `NLA_POLICY_FULL_RANGE` +
   `struct netlink_range_validation`. (Found by W=1 `-Woverflow`.)
2. **5.9 KiB stack frame in `urp_new_endpoint_doit`** (`urp_netlink.c`): a
   whole `struct urp_endpoint` (it embeds the 64-entry buffer array) was
   used as on-stack config scratch — nearly 3x the kernel's 2 KiB frame
   budget. Now heap-allocated and released with `kfree_sensitive` (it
   carries the raw PSK). (Found by W=1 `-Wframe-larger-than`.)
3. **QP-slot leak on OOM in `urp_cm_accept_one`** (`urp_rdma.c`): the
   `child_ctx` kzalloc-failure path returned without decrementing
   `qps_accepted`, permanently leaking the accepted-QP slot so later
   legitimate CONNECT_REQUESTs would be rejected as "extra". Now unwinds
   through `err_release_slot`. (Found by smatch "missing unwind goto".)
4. **Dead code**: `urp_recvs_per_qp()` (`urp_rdma.c`) — obsoleted by the
   SRQ path, deleted. **Missing prototype**: `urp_recv_done_for_srq` was
   declared `extern` inside `urp_srq.c` (checkpatch ERROR class) — moved
   to `urp.h`. (gcc `-Wmissing-prototypes` / `-Wunused-function`.)
5. **sparse couldn't parse the tree at all** — the combined
   `#if defined(__has_include) && __has_include(<...>)` line is unlexable
   by sparse, and its fallback branch then hit the nonexistent pre-6.6
   header. Restructured with an explicit `__CHECKER__` version-gate branch
   so the whole module is now sparse-clean.

## 26.4 Intentional / accepted checkpatch residual (24)

| type | count | why kept |
|---|---|---|
| `ALLOC_WITH_SIZEOF` (prefer `kzalloc_obj`) | 5 | `kzalloc_obj` does not exist on the supported 6.1/6.6/6.12 LTS matrix. Flip when the floor rises (or on actual upstreaming, where LTS compat shims drop out anyway). |
| `LINUX_VERSION_CODE` + `CONSTANT_COMPARISON` | 8 | The deliberate compat gates: `urp_sockaddr_t` (7.0 unsized-sockaddr rework), `urp_strscpy` (6.8 `sized_strscpy`), and the page_pool split gate's sparse branch (26.3 item 5). All vanish on upstreaming. |
| `NEW_TYPEDEFS` | 2 | Same `urp_sockaddr_t` shim — a typedef is the point (the *type name* changes across versions). |
| `SPACING` on `__has_include(<...>)` | 4 | checkpatch false positive on the angle-bracket operand (`urp_rdma.c`). |
| `BIT_MACRO` (uapi) | 4 | `BIT()` is not available to UAPI headers; plain shifts are the uapi norm (kernel-side uses of `BIT()` were converted). |
| `AVOID_BUG` | 1 | `urp_panic_abort()` is the Rust `#[panic_handler]` sink and must not return; it now `WARN()`s first, then `BUG()` to satisfy the noreturn contract (documented at the call site). |

## 26.5 Mechanical hygiene applied in this pass

- `#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt` in every TU that logs;
  the hand-rolled `"urp: "` prefixes (76 sites) stripped.
- Hot-path printks rate-limited (`pr_err_ratelimited` in kthread RX/TX
  loops and CQ completion paths); per-connection/per-stream lifecycle
  chatter demoted to `pr_debug`.
- SPDX lines on `kernel/Kbuild` + `kernel/Makefile`; `urp-objs` →
  `urp-y` (was mixing legacy and modern spellings).
- `BUG()` in the Rust panic sink prefixed with `WARN()` + rationale.
- `char ip_buf[16]` → `INET_ADDRSTRLEN`; `(1 << 0)` → `BIT(0)` (kernel
  side only — the UAPI header keeps plain shifts, `BIT()` isn't uapi).
- `__attribute__((noreturn))` → `__noreturn` (urp_ffi.h).
- Block-comment closers on their own line (36 sites), parenthesis
  alignment, single-statement braces, declaration spacing, spinlock
  member comments, long uapi comment lines wrapped, non-ASCII (em-dash,
  section sign) comments converted to ASCII.
- The placeholder-URL comment in `urp_pump.c` replaced with the real
  explanation (kthread task_struct not pinned; see follow-up below).
- `MODULE_AUTHOR` now carries a real contact. `MODULE_VERSION` retained
  (drop if upstreaming — in-tree modules version with the kernel).
- Rust: `cargo fmt` across the workspace; `# Safety` sections on all 7
  `unsafe extern "C"` FFI functions; clippy clean.

## 26.6 Deeper follow-ups (documented, NOT fixed here)

Ordered by what a maintainer would flag first:

1. **Endpoint lifetime / kref** — **FIXED.** Was: no refcounting anywhere;
   callers in `urp_netlink.c` dereferenced (and `mutex_lock`ed) the endpoint
   *after* `rcu_read_unlock()`, and two concurrent removes were racy (a UAF /
   double-free window; targeted by the design-27 concurrent racer). Now
   `struct urp_endpoint` carries a `struct kref`: `urp_endpoint_get()` takes a
   reference under RCU (`kref_get_unless_zero`) so a looked-up pointer stays
   valid after the RCU section; `urp_endpoint_remove()` unpublishes from the
   rhashtable and drops the *table* reference exactly once (the
   `rhashtable_remove_fast` winner drains — no double-drain, no double-free for
   concurrent DELs); the object is freed via `call_rcu` only when the last
   reference is dropped. Every genl handler (`NEW`/`DEL`/`SET`/`GET`) now holds
   a ref across its use and `urp_endpoint_put()`s it. Validated: KASAN +
   KMEMLEAK clean under ~18.7k concurrent NEW/DEL/SET/GET ops (the racer).
   Follow-on (also **FIXED**): kref makes the endpoint *object* safe, but its
   sub-objects (`ep->qps[]`, the `ep->streams` table) are freed by
   `urp_endpoint_drain()` under `ep->lock`, while the *verbose* `GET` fill
   (`urp_fill_endpoint`, unprivileged) read them with no lock — an unprivileged
   GET racing an admin DEL read freed memory. Fixed by holding `ep->lock`
   across the verbose fill (the only verbose caller, `GET` doit, runs in
   sleepable context; dumpit fills scalars only and stays lock-free inside its
   RCU walk), with a `lockdep_assert_held(&ep->lock)` guarding the contract.
2. **netns awareness** — every `sock_create_kern(&init_net, ...)` and
   `rdma_create_id(&init_net, ...)`, plus `.netnsok = false`. Design and
   scope decision already recorded in
   [24-network-namespaces.md](24-network-namespaces.md) (deferred Phase 7).
3. **`ep->lock` held across blocking teardown** —
   `urp_endpoint_drain()` holds the mutex across
   `cancel_delayed_work_sync()` / `kthread_stop()` / RDMA teardown. Safe
   only because the probe work never takes `ep->lock` — an undocumented
   invariant lockdep can't see. Narrow the lock scope.
4. **1 ms poll loops in the pumps** — buffer-exhaustion and QP-selection
   backoff spin on `schedule_timeout_interruptible(1ms)`. Should be a
   waitqueue woken from `urp_send_done`.
5. **kthread lifecycle** — pumps park forever after EOF because the
   `task_struct` handed to `kthread_stop()` isn't pinned. Proper fix:
   `get_task_struct()` at start, let the pump self-exit, `put` after
   `kthread_stop()`. Also name threads per-endpoint/stream
   (`"urp-tx/%s"`) so `ps` output is usable.
6. **`urp.h` monolith** (600+ lines, every struct + prototype for 11 TUs)
   — split (`urp_frame.h` for the codec inlines is the natural first
   cut); replace open-coded frame-offset arithmetic with a
   `struct urp_frame_hdr` of `__le32`/`__le64` members.
7. **Annotations** — no `__read_mostly` / `__ro_after_init` anywhere;
   `urp_endpoints`/`urp_endpoints_inited` are non-static module globals.
8. **CI promotion** — once residuals hold at ~0, add
   checkpatch/clippy/rustfmt as cheap per-push gates and sparse/smatch to
   nightly.

## 26.7 Relation to other documents

- [12-testing.md](12-testing.md) — runtime/test matrix (this doc covers
  *static* analysis only).
- [24-network-namespaces.md](24-network-namespaces.md) — the netns design
  referenced by follow-up 2.
- `status.md` — current counts are mirrored in the status file.
