# Project Status

_Last updated: 2026-05-04_

## Current branch

`phase1-k0-kernel-module` — 1 commit ahead of `origin/phase1-k0-kernel-module`.

## Where we are

| Phase | Plan doc | Status |
|---|---|---|
| **0** — Skeleton | `docs/KERNEL-MODULE-PLAN.md` §0 | ✅ Committed (`3a32ffc`) |
| **1 — k0** RDMA echo data path | `docs/KERNEL-MODULE-PLAN.md` §1 | ✅ Committed (`d440794`, `a986fe7`, `a600b17`) |
| **2** — GENL control plane | `docs/PHASE-2-PLAN.md` (per-session plan file) | ✅ **Implementation complete; uncommitted in working tree** |
| **3a** — k1 data path | `docs/.../floofy-stirring-donut.md` (active plan) | 🚧 **Step 1 of 10 complete; in working tree** |
| 3b — probes / PSK / extended observability | (deferred) | not started |
| 3c — KUnit hardening + soak | (deferred) | not started |
| 4 / 5 / 6 | per `docs/KERNEL-MODULE-PLAN.md` | not started |

## Phase 2 deliverables (currently uncommitted)

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

## Phase 3a Step 1 deliverables (currently uncommitted)

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

## Phase 3a Steps 2–10 — pending

Tracked in tasks #51–#59. Plan doc: `~/.claude/profiles/work/plans/floofy-stirring-donut.md`.

| Step | Task ID | Subject |
|---|---|---|
| 2 | #51 | Multi-QP allocation + round-robin selection |
| 3 | #52 | Shared Receive Queue (SRQ) |
| 4 | #53 | Per-QP credit flow control |
| 5 | #54 | Reorder buffer (C default + Rust opt-in) |
| 6 | #55 | Stream multiplexing core |
| 7 | #56 | Stream lifecycle (SYN/FIN/RST + half-close) |
| 8 | #57 | GENL emitters wire up real state |
| 9 | #58 | Integration test expansion + bench harness |
| 10 | #59 | Update implementation tracker |

Per the plan, Steps 2–4 will ship as one commit (multi-QP scaffold + SRQ + credits — single-stream e2e at that point), Steps 5–7 as the next (reorder + streams + lifecycle — k1 functional milestone), Step 8 wires up observability, Step 9 expands tests + bench, Step 10 updates the tracker.

## Working-tree hygiene notes

The following files are in the working tree but should NOT be committed:
- `result-dev`, `result-local`, `result-vm` — Nix build output symlinks
- `.claude/scheduled_tasks.lock` — Claude Code session state
- `fuzz/Cargo.lock` — generated lockfile for the fuzz target

These will be skipped at commit time.
