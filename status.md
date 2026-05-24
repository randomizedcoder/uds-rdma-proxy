# Project Status

_Last updated: 2026-05-23_

## Current branch

`phase3a-k1-data-path` — cut from `c2eea2e` (the prior `phase1-k0-kernel-module`
HEAD). The historical branch `phase1-k0-kernel-module` still exists locally and
carries phases 0, 1, 2, and 3a-Step-1 commits despite its name.

## Where we are

| Phase | Plan doc | Status |
|---|---|---|
| **0** — Skeleton | `docs/KERNEL-MODULE-PLAN.md` §0 | ✅ Committed (`3a32ffc`) |
| **1 — k0** RDMA echo data path | `docs/KERNEL-MODULE-PLAN.md` §1 | ✅ Committed (`d440794`, `a986fe7`, `a600b17`) |
| **2** — GENL control plane | `docs/KERNEL-MODULE-IMPLEMENTATION.md` §Phase 2 | ✅ Committed (`067829e`) |
| **3a** — k1 data path | `~/.claude/profiles/siden/plans/floofy-stirring-donut.md` + `~/.claude/profiles/runpod/plans/this-repo-has-a-abundant-fern.md` | 🚧 **Steps 1, 2, 2b, 3, 4, 5 done (`c2eea2e`, `9dd0a70`, `f9f49b4`, `0fba325`, `728db70`, `3737132`); Steps 4b/5b/6–10 pending** |
| 3b — probes / PSK / extended observability | (deferred) | not started |
| 3c — KUnit hardening + soak | (deferred) | not started |
| 4 / 5 / 6 | per `docs/KERNEL-MODULE-PLAN.md` | not started |

## Phase 2 deliverables (now committed in `067829e`)

GENL control plane: `urp add / show / remove / drain / set / monitor / stats` CLI driving the kernel module via a generic netlink family. 19/19 integration tests pass in the QEMU VM.

Working-tree files attributable to Phase 2:

- `crates/urp-cli/` — clap-driven CLI, neli-based netlink client
- `kernel/urp_netlink.c`, `kernel/urp_endpoint.c` — GENL family + endpoint rhashtable
- `kernel/include/uapi/linux/urp.h` — UAPI for GENL attrs
- `kernel/urp.h`, `kernel/urp_main.c`, `kernel/urp_proc.c`, `kernel/urp_rdma.c`, `kernel/urp_socket.c`, `kernel/Kbuild` — endpoint lifecycle wiring
- `nix/urp-cli.nix` — CLI build
- `nix/test-kmod-k0.nix`, `nix/test-vm.nix` — integration test harness expansions
- `docs/KERNEL-MODULE-IMPLEMENTATION.md` — Phase 2 status row, variations #20–21 (positional CLI names, rhashtable fixed-key fix)

Two notable bugfixes captured in IMPLEMENTATION.md:
1. CLI segfault → kernel `BUG_ON` at `rhashtable.h:968`. `rhashtable_lookup_insert_fast` BUGs when `obj_hashfn` is set; converted to fixed-key default hashing across the full 16-byte zero-padded `name` field.
2. CLI `--name` mismatch with test invocation pattern. Removed `#[arg(long)]` from `name` in `add`/`set` so the test script's positional invocation works.

## Phase 3a Step 1 deliverables (now committed in `c2eea2e`)

Rust→kernel FFI prerequisite. Sets up the staticlib build path that Step 5 will consume when wiring up the optional Rust-backed reorder buffer (`CONFIG_URP_REORDER_RUST=y`).

New files:
- `crates/uds-rdma-protocol-ffi/{Cargo.toml, src/lib.rs, src/ffi.rs, src/kernel_runtime.rs}` — sibling staticlib crate. Kept separate from `uds-rdma-protocol` to avoid duplicate-`panic_impl` lang-item collisions when libtest pulls in `std`.
- `kernel/include/urp_ffi.h` — C prototypes for the seven `urp_rust_reorder_*` exports + the three kernel-supplied callbacks (`urp_kalloc`, `urp_kfree`, `urp_panic_abort`). Gated on `CONFIG_URP_REORDER_RUST`.
- `nix/urp-protocol-ffi.nix` — `rustPlatform.buildRustPackage` derivation with `cargoLock.lockFile` for offline (sandboxed) Nix builds.

Modified:
- `Cargo.toml` (workspace) — adds the new member + workspace-level `[profile.release] panic = "abort"` (Cargo refuses package-level profiles in workspaces).
- `flake.nix` — exposes `packages.urp-protocol-ffi`.
- `nix/checks.nix` — `protocol-tests` switched from raw `cargo test` to `rustPlatform.buildRustPackage` (raw `cargo test` can't reach crates.io from the Nix sandbox); src filter widened to include the new crate dirs.

Verified via Nix (no system Rust used):
- `nix build .#urp-protocol-ffi` → produces `result/lib/liburp_protocol_ffi.a` (2.2 MB after LTO+strip) + `result/include/urp_ffi.h`. All 7 expected `T urp_rust_reorder_*` symbols are defined; only the 3 expected `U urp_kalloc / urp_kfree / urp_panic_abort` are undefined (resolved by the kernel module side in Step 5).
- `nix build .#checks.x86_64-linux.protocol-tests` → passes.
- `nix build .#checks.x86_64-linux.kernel-module-build` → passes (urp.ko unchanged in default config).

## Phase 3a Steps 2–10 — progress

| Step | Subject | Commit | Status |
|------|---------|--------|--------|
| 2 | Multi-QP scaffold + round-robin selection | `9dd0a70` | done |
| 2b | Actual N-QP multi-cm-id allocation | `f9f49b4` | done |
| 3 | Shared Receive Queue (SRQ) | `0fba325` | done |
| 4 | Per-QP credit flow control (scaffold) | `728db70` | done |
| 4b | Wire credit gate into TX/RX paths | — | pending (needs CREDIT-aware peer) |
| 5 | Reorder buffer (C rbtree backend) | `3737132` | done |
| 5b | Rust reorder backend wiring | — | pending |
| 6 | Stream multiplexing core | — | pending |
| 7 | Stream lifecycle (SYN/FIN/RST + half-close) | — | pending |
| 8 | GENL emitters wire up real state | — | pending |
| 9 | Integration test expansion + bench harness | — | pending |
| 10 | Tracker polish + benchmark table | — | pending |

User-confirmed cadence: one commit per step (so Step 2's commit is not fully
e2e runnable on its own with `num_qps > 1` — Step 2b lifts the
`-EOPNOTSUPP` guard). The current `phase3a-k1-data-path` branch's HEAD
(Step 2) still passes the full 19/19 `test-kmod-k0` suite because every
existing test uses the default `num_qps = 1`.

The full step-by-step file-level plan remains at
`~/.claude/profiles/siden/plans/floofy-stirring-donut.md`; the active
session plan is `~/.claude/profiles/runpod/plans/this-repo-has-a-abundant-fern.md`.

## Working-tree hygiene notes

The following files are in the working tree but should NOT be committed:
- `result-dev`, `result-local`, `result-vm` — Nix build output symlinks
- `.claude/scheduled_tasks.lock` — Claude Code session state
- `fuzz/Cargo.lock` — generated lockfile for the fuzz target

These will be skipped at commit time.
