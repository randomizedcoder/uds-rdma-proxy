# Project Status

_Last updated: 2026-08-12_

## Current branch

`main` is current through PR #21. Highlights of the merged run of PRs:

- **Redpanda over UDS-over-RDMA** (PRs #1–#2 + follow-ups): metadata round-trip
  AND full Kafka produce/consume over the RDMA tunnel, KASAN/KMEMLEAK/lockdep
  validated. Initiator multi-stream (Phase 3a Step 7d) landed as part of this.
- **Static analysis / maintainer hygiene** (see section below +
  `docs/design/26-upstream-readiness.md`): hermetic `nix/analysis/` tooling,
  three real bugs fixed, checkpatch 79 → 24.
- **Comprehensive fuzzing program** (PRs #12–#21, design doc 27 — see the
  Fuzzing section below): every attack surface fuzzed, three memory-safety
  bugs found or closed (SET num_qps OOB teardown; endpoint lookup-vs-DEL UAF,
  fixed via kref; verbose-GET-vs-DEL sub-object UAF), per-push + nightly CI
  wired.

## Where we are

| Phase | Plan doc | Status |
|---|---|---|
| **0** — Skeleton | `docs/KERNEL-MODULE-PLAN.md` §0 | ✅ Committed (`3a32ffc`) |
| **1 — k0** RDMA echo data path | `docs/KERNEL-MODULE-PLAN.md` §1 | ✅ Committed (`d440794`, `a986fe7`, `a600b17`) |
| **2** — GENL control plane | `docs/KERNEL-MODULE-IMPLEMENTATION.md` §Phase 2 | ✅ Committed (`067829e`) |
| **3a** — k1 data path | `docs/KERNEL-MODULE-PLAN.md` §3 + `KERNEL-MODULE-IMPLEMENTATION.md` §Phase 3a | ✅ **Phase 3a complete -- Steps 1-10 + 2b, 4b, 5b, 7b, 7c done (`c2eea2e` through `b7caab2`); 23/23 `test-kmod-k0` PASS. Sub-step 7d (initiator multi-stream + concurrent-connection integration) **DONE (`74df057`; KASAN-clean 12-stream burst) -- see Redpanda section**. 5b's objtool-on-IBT follow-up remains pending.** |
| **3b** — probes / PSK | `docs/KERNEL-MODULE-PLAN.md` §3.6-§3.7 + `KERNEL-MODULE-IMPLEMENTATION.md` §Phase 3b | ✅ **Complete. Steps 1-10 done (`0d5c077` through `dd6bab0`). Probes (PING/PONG/RTT EWMA/state machine), PSK SHA-256 + rdma_cm `private_data` validation, auth-failure observability all wired. Test-kmod-k0 23/23 PASS. Sub-steps 8b (initiator-validates-acceptor) + 3b/test (URP-to-URP harness) pending.** |
| 3c — KUnit hardening + soak | (deferred) | largely superseded: KUnit suites landed via 3a Step 9 + design 28 extractions (34 cases in `kernel/urp_test.c`); the 1-hour soak landed in Phase 4 |
| **4** — k2 optimized | `docs/KERNEL-MODULE-PLAN.md` §4 + `KERNEL-MODULE-IMPLEMENTATION.md` §Phase 4 | ✅ **rxe-testable scope complete + 1-hour soak PASS. Steps: 1 page_pool (`ded84a7`), 2 NUMA-aware (`78967e5`), 3 tracker (`c535c7a`), 4-5 soak harness + reconnect-leak fix (`c41bd61`). 23/23 `test-kmod-k0` + 1240 cycles + 120 churn add/remove with 0 errors, 1828 kB slab leak (budget 2048). §4.2 zero-copy / §4.3 adaptive CQ / §4.4 kthread NUMA bind / §4.5 hardware benchmarks deferred to a hardware-validation pass.** |
| **5** — MicroVM integration | `docs/KERNEL-MODULE-PLAN.md` §5 + `KERNEL-MODULE-IMPLEMENTATION.md` §Phase 5 | 🟢 **Substantially complete (6/8 deliverables done, +1 partial, 1 blocked). Steps 1-9 x86_64 harness/sanitizer PASS. Step 10 kernel-version matrix (6.1.180/6.6.148/6.12.101/7.1.6 build via 4 LTS compat shims). Step 11 nixpkgs+microvm bump (latest kernel 7.1.6). Step 12 CI (`ci.yml` every-push + `nightly.yml` self-hosted KVM). Steps 13-14 cross-arch harness (localSystem/crossSystem, xdp2-aligned). Step 15 aarch64 emulated pair test FULL PASS under TCG. riscv64: urp.ko build gate (boot via same harness, not run). Redpanda: **metadata AND full produce/consume round-trip over RDMA PASS** (Stage 0 unblocked -- broker built hermetically via the redpanda fork's Nix flake; see below).** |
| 6 | per `docs/KERNEL-MODULE-PLAN.md` | not started |

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
| 4b | Wire credit gate into TX/RX paths | `b7caab2` | done (best-effort + stream-id != 0 gating) |
| 5 | Reorder buffer (C rbtree backend) | `3737132` | done |
| 5b | Rust reorder backend wiring | `83570af` | done (default build green; `urp-ko-rust` blocked by objtool on IBT kernels) |
| 6 | Stream multiplexing core (scaffold) | `e2ea525` | done |
| 7 | Stream lifecycle handlers | `f3f9903` | done |
| 7b | Wire stream_id dispatch into RX path | `9075f57` | done |
| 7c | TX + UDS multi-stream wiring (acceptor) | `f14a107` | done |
| 7d | Initiator multi-stream accept loop + concurrent-connection test | `74df057` | **done** (per-stream pump per UDS conn; KASAN-clean 12-stream burst) |
| 8 | GENL emitters wire up real state | `70fafc6` | done |
| 9 | KUnit suites + multi-QP integration smoke | `336e7e0` | done (23/23 PASS) |
| 10 | Tracker polish + DoD checklist | done (this row) | done |

User-confirmed cadence: one commit per step (so Step 2's commit is not fully
e2e runnable on its own with `num_qps > 1` — Step 2b lifts the
`-EOPNOTSUPP` guard). The current `phase3a-k1-data-path` branch's HEAD
(Step 2) still passes the full 19/19 `test-kmod-k0` suite because every
existing test uses the default `num_qps = 1`.

## Redpanda over UDS-over-RDMA

Real Kafka client (`rpk`) ↔ real Redpanda broker, Kafka bytes carried over the
URP RDMA data path. Single host, soft-RoCE (`rdma_rxe`), RDMA loopback over one
device. Broker + `rpk` are built from the redpanda fork's Nix flake (`redpanda`
flake input, pinned to `randomizedcoder/redpanda@d4b44629`, which carries the
native Kafka UDS listener from PR #30240 — unblocking the old "Stage 0"
hermetic-source gap). Two fork fixes were required: build `rpk` from local
source (it fetched an upstream rev predating the UDS client support), and build
the broker with clang 22 (the llvm 23 RC miscompiled it).

### Stage A — metadata round-trip (PASS, PR #2)

```
rpk --UDS--> urp initiator ==RDMA/rxe==> urp acceptor --UDS--> redpanda
  unix://rpk.sock                                       kafka_api unix_path (+ TCP)
```

- Harness: `nix/test-redpanda-uds.nix` (`nix run .#test-redpanda-uds`, root).
- **9/9 pass.** `rpk cluster info` returns the real cluster id + broker list;
  `urp show` confirms the 53 B request / 424 B response crossed RDMA; a negative
  check proves attribution (fails when the acceptor is removed; direct TCP fine).

### Full produce/consume over RDMA (PASS, `74df057`)

`rpk`/franz use UDS only for the metadata bootstrap; produce/consume then follow
the *advertised* address (UDS is non-advertisable). So the broker advertises a
client-local bridge (`127.0.0.2:9092`) that funnels back into the tunnel, so ALL
Kafka traffic — create, produce, fetch — rides RDMA:

```
rpk --TCP 127.0.0.2:9092--> socat --UDS--> urp initiator ==RDMA==> urp acceptor --UDS--> redpanda
```

- Harness: `nix/test-redpanda-produce-consume.nix`
  (`nix run .#test-redpanda-produce-consume`, root).
- **11/11 pass.** `rpk topic create` + `produce` + `consume` round-trip; the
  consumed payload matches byte-for-byte.

### Initiator multi-stream (Phase 3a Step 7d, `74df057`)

Full produce/consume required completing the initiator data path: franz opens a
separate connection per broker, so the initiator now opens **one stream per
accepted UDS connection** (per-stream pump emits SYN) instead of the single k0
`ep->conn`. Completing this surfaced two host-oopsing teardown bugs, now fixed:
shut the UDS **before** `kthread_stop` (was reversed → `rmmod` hang), and the
per-stream pump **parks** on `kthread_should_stop` after EOF instead of
self-returning (was a `task_struct` refcount underflow / oops).

Validated in the microVM pair test (now fires a **12-concurrent-stream burst**):
non-sanitizer PASS, and **KASAN + KMEMLEAK + lockdep CLEAN** on both VMs.
`test-kmod-k0` still 23/23 (k0 path unaffected).

Known follow-ups: (1) **reap-on-close via a FIN handshake** — the eager
reap-on-`tx_done` dropped half-close responses and was reverted, so streams reap
at drain (no per-endpoint leak, but long-lived endpoints accumulate until then);
(2) make the acceptor's eager k0 `ep->conn` lazy (it opens one idle backend
connection in the multi-stream case).

## Static analysis / upstream readiness (2026-08-09)

`nix/analysis/` (patterned on xdp2) adds hermetic, report-only analyzers:
`nix build .#analysis-all -L` runs **sparse** (pinned upstream master;
nixpkgs' is too old for 7.x headers), **smatch**, **checkpatch --strict**
(from the target kernel's own source tree inside `.dev`), **W=1/W=2**,
**coccicheck** (the kernel's coccinelle suite), **clippy**, and
**rustfmt --check**, and prints a per-tool count table. Manual-run only --
not wired into checks/CI.

First pass results (kernel 7.1.6): sparse/smatch/W=1/coccicheck/clippy/
rustfmt all **0**; checkpatch 79 -> **24**, every residual intentional and
justified in `docs/design/26-upstream-readiness.md` §26.4. Three **real
bugs** found and fixed: `NLA_POLICY_RANGE` s16-truncating
`URP_BUFFER_SIZE_MAX` (65536 -> 0, validation broken), a 5.9 KiB stack
frame in `urp_new_endpoint_doit` (whole `struct urp_endpoint` as stack
scratch), and a QP-slot leak on the `urp_cm_accept_one` OOM path. Plus the
mechanical hygiene sweep (pr_fmt, ratelimited printks, SPDX on
Kbuild/Makefile, urp-y, `__noreturn`, `# Safety` docs on the FFI, cargo
fmt, checkpatch style). Deeper maintainer items were catalogued as
follow-ups in design doc 26; of those, **kref endpoint lifetime** (incl.
the sub-object GET-vs-DEL UAF) and **kthread task_struct pinning** have
since been FIXED (see design 26 §26.6); netns, lock scope, and
waitqueue-driven pumps remain open. All regression gates re-verified
green: urp-ko (7.1.6 + 6.1/6.6/6.12), urp-cli (incl. the uapi mirror
test), protocol-tests.

## Fuzzing program (2026-08-10/11, design doc 27 — COMPLETE)

Tracks F0–F3 all implemented and automated:

- **F0/F1 hermetic harnesses** (libFuzzer + ASAN/UBSan, compile the REAL
  kernel C): `nix run .#fuzz-classify` (RX frame classifier),
  `.#fuzz-rx-seq` (classify → dispatch → stream state machine over frame
  sequences; uses the extracted dual-compile `kernel/urp_stream_sm.c`),
  `.#fuzz-reorder` (default C rbtree reorder backend against a spec-model
  differential, linked with the kernel's own `tools/lib/rbtree.c` from the
  nixpkgs-pinned kernel source). Plus 5 cargo-fuzz targets on the shared
  Rust crate (`fuzz/fuzz_targets/`).
- **F2 live-VM fuzzers**, run as phases of the sanitizer microVM pair test
  (KASAN + KMEMLEAK + lockdep + KCOV kernel): blind + KCOV coverage-guided
  netlink fuzzers (with `KCOV_TRACE_CMP` operand-dictionary feedback), a
  multi-threaded netlink racer (NEW/DEL/SET/GET churn), and a hostile-peer
  RDMA **wire** fuzzer (librdmacm RC client speaking malformed frames at a
  dedicated acceptor). Each fuzz phase is followed by an inline dmesg
  `scan_splat` oracle so a wedged VM cannot mask a KASAN splat.
- **F3 CI**: every push runs `fuzz-smoke` (45 s classify + rx-seq +
  regression-reproducer replay); nightly runs `fuzz-long` (10 min each of
  the three hermetic harnesses) and the sanitizer pair test with all F2
  fuzz phases. Crash reproducers are committed under `fuzz/regressions/`.

Real bugs found or closed by the program: (1) `SET_ENDPOINT`
num_qps/buffer_count OOB teardown — found live by the coverage-guided
netlink fuzzer, fixed with `-EBUSY` on active endpoints; (2) endpoint
lookup-vs-DEL use-after-free — closed by the kref + release-via-RCU
refactor; (3) verbose-GET-vs-DEL sub-object UAF (`ep->qps[]`/streams read
unlocked during drain) — closed by holding `ep->lock` across the verbose
fill. Remaining optional items are listed in design 27 (lifecycle/churn
soak fuzz, corpus artifact management, coverage reporting).

## io_uring UDS benchmark design (2026-08-12, design doc 30 — DESIGNED)

`docs/design/30-urp-bench-io-uring.md` designs `urp-bench`: a symmetric
userland benchmark pair (C + liburing and Rust + the `io-uring` crate, same
CLI and wire framing) that drives the UDS side with io_uring and sweeps
message size × batch size × io_uring mode (blocking control, batched
send/recv, registered buffers, provided-buffer rings + multishot recv,
SQPOLL, and a SEND_ZC evidence probe). It answers design 20 §20.1's open
question — the honest hypothesis is that AF_UNIX has no zero-copy path, so
the copy stays and syscall batching is the measurable win. Work items B1–B8
(pure table-tested cores in both languages, io_uring shells, `urp-bench-units`
check, `fuzz-bench-deframe` + cargo-fuzz + C↔Rust differential targets,
first userland C static analysis via clang-tidy/cppcheck, pair-test Phase 13)
are all **not started**. Note: design doc 29 remains referenced-but-uncommitted;
30 was numbered around it.

## Known functional gaps

Found by the 2026-08-11 docs-vs-implementation review (full analysis and
wiring plans land in `docs/design/29-code-review-refactor-plan.md`):

1. **Per-stream reorder buffer is not wired into the RX path.** Both backends
   (C rbtree default, optional Rust FFI) are implemented, KUnit-tested, and
   fuzzed — but `urp_recv_done` delivers frames in completion order and never
   calls `urp_reorder_insert`/`urp_reorder_drain_next`; the per-stream buffer
   is allocated and freed unused, and `stats.reorder_insertions`/`reorder_drops`
   are exported but never incremented. Invisible on single-QP and rxe-loopback
   paths; matters for real multi-QP/ECMP deployments.
2. **`buffer_count` / `buffer_size` endpoint config is accepted but unused.**
   Parsed, validated, reported, and change-guarded — but the buffer pool is
   sized by compile-time `URP_NUM_BUFS`/`URP_BUF_SIZE`.

## Working-tree hygiene notes

- `result*` Nix build-output symlinks are gitignored (`result`, `result-*`)
  and must not be committed.
- `.claude/` — Claude Code session state; not committed.
- `fuzz/Cargo.lock` **is committed intentionally** (reproducible fuzz-target
  builds, same rationale as the workspace `Cargo.lock`).
