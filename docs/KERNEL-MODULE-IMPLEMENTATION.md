# Kernel Module Implementation Tracker

Progress tracker for the [Kernel Module Implementation Plan](KERNEL-MODULE-PLAN.md).

**Last updated**: 2026-05-23 (Phase 3a Step 2 committed as `9dd0a70`)

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [Prerequisites](#phase-0-prerequisites) | Complete | 6/7 |
| 1 | [k0 -- Proof of Concept](#phase-1-k0----proof-of-concept) | Complete | 7/9 (sanitizer items deferred) |
| 2 | [urp CLI + GENL](#phase-2-urp-cli--genl-interface) | Complete (`067829e`) | 8/9 |
| 3a | [k1 Data Path](#phase-3a-k1-data-path) | In Progress | 1/9 |
| 3 | [k1 -- Functional](#phase-3-k1----functional) | In Progress (via 3a) | 0/14 |
| 4 | [k2 -- Optimized](#phase-4-k2----optimized) | Not Started | 0/8 |
| 5 | [MicroVM Integration](#phase-5-microvm-integration) | Not Started | 0/8 |

---

## Phase 0: Prerequisites

**Status**: Complete

### Deliverables

- [x] `uds-rdma-protocol` crate compiles with `--no-default-features` (no_std + alloc)
- [x] `cargo test` passes: frame roundtrip, credit state, reorder buffer (20+ test cases)
- [x] `cargo +nightly miri test` passes (no UB)
- [x] `cargo fuzz run frame_decode` runs for 60s with no crashes
- [x] `make -C kernel` produces `urp.ko` (empty module: init prints, exit prints)
- [x] `insmod urp.ko && rmmod urp` succeeds (in NixOS test VM with matching kernel)
- [ ] ~~MicroVM kernel boots with `CONFIG_KUNIT=y`~~ (deferred to Phase 5)

### Variations from Plan

1. **Not an extraction** -- Plan says "extract protocol logic from the userspace proxy design." No userspace code exists. This is a clean-room implementation from design docs.
2. **Phase 0.3 deferred** -- KUnit/MicroVM infrastructure does not exist yet. Deferred to Phase 5.
3. **FrameHeader is a regular Rust struct** -- NOT `#[repr(C, packed)]`. Manual byte-level encode/decode via `to_le_bytes`/`from_le_bytes` avoids UB. Packed repr deferred to Phase 3 FFI.
4. **Multi-module crate** -- 8 modules (frame, credit, reorder, probe, mtu, qp, constants, error) instead of single `lib.rs`.
5. **63 test cases** -- Well above the 20+ target (frame: 22, credit: 8, reorder: 8, probe: 6, mtu: 12, qp: 4, plus extras).
6. **FFI exports deferred** to Phase 3.
7. **Kernel module built against 6.12.68 headers** -- Running kernel is 6.18.22 (NixOS). Build validated, insmod not possible without matching headers. Nix flake devshell will provide correct kernel-dev for the running kernel.
8. **Miri/fuzz require nightly** -- Not available outside Nix devshell. Stable Rust (1.94.0) is from Nix system profile.
9. **Single nightly toolchain in devshell** -- Plan implied separate stable/nightly. Using nightly as sole toolchain in devshell avoids PATH conflicts with system rustup proxies in `~/.cargo/bin`. `CARGO_HOME` set to `.cargo-nix/` to isolate from system cargo-miri rustup proxy.
10. **Miri: 63/63 tests pass** -- Zero UB detected. All modules clean.
11. **Fuzz: 58.7M runs in 60s** -- frame_decode target, no crashes.

### Notes


---

## Phase 1: k0 -- Proof of Concept

**Status**: Complete (sanitizer runtime testing deferred to Phase 5)

### Deliverables

- [x] Module loads, creates UDS socket, accepts connections, pumps data over RDMA
- [x] Echo test: 1000 roundtrips, all data matches (via `urp-test-client` over rdma_rxe)
- [x] Throughput test: sustained 100MB transfer, no hangs or crashes (3.6 MB/s over rdma_rxe in QEMU)
- [x] `/proc/urp/stats` shows correct byte/frame counters
- [x] Clean insmod/rmmod cycle with no kernel errors in dmesg
- [x] KUnit tests written: 12 tests (frame codec roundtrip, endianness, boundary values, buffer lifecycle)
- [ ] **TODO (deferred to Phase 5)**: KASAN clean -- no memory errors during test suite. Sanitizer kernel builds successfully but panics at boot ("No filesystem could mount root fs"); root cause is `kernel.override { kernelPatches = ... }` dropping NixOS-injected configs needed by the qemu-vm rootfs (likely VIRTIO_BLK / 9P_FS). Fix path: restructure `nix/test-vm.nix` to use `boot.kernelPatches` inside the NixOS config (canonical pattern), breaking the urpKo/kernelPackages cycle via two-pass eval. Defer to Phase 5 with broader MicroVM/test-infra rework.
- [ ] **TODO (deferred to Phase 5)**: KMEMLEAK clean -- no leaks after `rmmod`. Same blocker as KASAN.
- [x] Latency measured: p50=1.07ms, p99=1.22ms (rdma_rxe in QEMU -- not representative of hardware)
- [ ] **Decision gate**: No userspace proxy exists yet to compare against. Deferred to after userspace v2 implementation.

### Source Files Implemented

| File | Lines | Purpose |
|------|-------|---------|
| `kernel/urp.h` | 226 | Internal header: structs, inline frame encode/decode, function decls |
| `kernel/urp_main.c` | 129 | `module_init`/`module_exit`, `module_param` (listen_path, connect_path, peer_address, peer_port, bind_port) |
| `kernel/urp_socket.c` | 207 | Virtual UDS endpoint: `sock_create_kern`, `kernel_bind`, `kernel_listen`, UDS connect |
| `kernel/urp_rdma.c` | 520 | RDMA CM: `rdma_create_id`, address/route resolution, QP creation, buffer pool (`alloc_page` + `ib_dma_map_page`), CQ done callbacks, CM event handling with acceptor data path |
| `kernel/urp_pump.c` | 137 | TX kthread: `kernel_recvmsg` -> frame encode -> `ib_post_send`. RX inline in CQ callback |
| `kernel/urp_proc.c` | 77 | `/proc/urp/stats`: tx/rx bytes/frames, connections, connected/active state |
| `kernel/urp_test.c` | 175 | KUnit tests: frame codec roundtrips (all types), endianness, boundary values, buffer free-list lifecycle |
| `kernel/include/uapi/linux/urp.h` | 31 | Frame constants, default port |
| `tools/urp-test-client.c` | 560 | Userspace RDMA CM client: echo (payload verification), throughput (MB/s), latency (p50/p99 RTT) modes |
| `nix/test-kmod-k0.nix` | 290 | `writeShellApplication`: integration test with echo/throughput/latency/KASAN/KMEMLEAK checks |
| `nix/urp-test-client.nix` | 23 | Build test client with `rdma-core` (`-lrdmacm -libverbs`) |
| `nix/checks.nix` | 73 | `buildUrpKo` function, `protocol-tests` and `kernel-module-build` checks |
| `nix/test-vm.nix` | 110 | NixOS QEMU VM: SSH root access, rdma_rxe, optional KASAN/KMEMLEAK/KUnit debug kernel |
| `nix/urp-vm.nix` | 141 | `writeShellApplication`: VM management (start/ssh/stop/console/status) |

### Variations from Plan

1. **Frame encode/decode in C inline** -- Plan says "call shared crate FFI." FFI exports deferred to Phase 3. k0 uses inline C functions matching the Rust wire format (little-endian, same byte layout). Wire compatibility verified by matching `to_le_bytes`/`from_le_bytes` field offsets.
2. **CQ callbacks via `ib_cqe`** -- Plan says "CQ completion -> frame decode." Using modern kernel `ib_cqe` per-work-completion callbacks (`wc->wr_cqe` + `container_of`) instead of `wr_id` casting. This is the correct pattern for `ib_alloc_cq` with `IB_POLL_WORKQUEUE`.
3. **RX handled in CQ callback** -- Plan mentions a separate RX kthread. k0 handles RX inline in `urp_recv_done` CQ callback (workqueue context). Simpler, avoids extra kthread. Can be refactored to dedicated kthread if needed for k1.
4. **Kernel module built against nixpkgs kernel** -- Using `pkgs.linuxPackages.kernel.dev` (6.18.24) for build. Running kernel is 6.18.22. Nix check target provides repeatable builds.
5. **Nix `checks` outputs added** -- `protocol-tests`, `kernel-module-build`. Miri incompatible with Nix sandbox (needs network for sysroot). Package output `urp-ko` produces built module.
6. **NixOS test VM** -- Plan defers MicroVM to Phase 5. Added early as QEMU VM (`nix run .#urp-vm`) for kernel module testing without host sudo. VM kernel matches flake-pinned nixpkgs, so urp.ko loads without vermagic mismatch.
7. **Userspace RDMA test client** -- Plan assumes dual module instances for testing. Kernel modules are global -- can't load twice. Created a minimal userspace RDMA CM client (`tools/urp-test-client.c`) that connects to the module's listener and sends/receives URP DATA frames.
8. **Acceptor data path setup before `rdma_accept`** -- UDS connect and pump start happen in the `CONNECT_REQUEST` CM handler, before `rdma_accept()` transitions the QP to RTR/RTS. This prevents a race where recv completions fire before the UDS forwarding path is ready.
9. **Debug VM with sanitizers** -- Separate debug VM variant (`nix run .#urp-vm-debug`) with KASAN, KMEMLEAK, and KUnit enabled. Requires full kernel rebuild (~30 min first time, cached after). Standard VM stays fast for iteration. **TODO**: debug kernel currently panics at boot due to missing virtio/9p config; runtime sanitizer testing deferred to Phase 5 (see Deliverables section).
10. **Decision gate deferred** -- No userspace proxy v2 exists to compare latency against. The kernel module latency numbers (p50=1.07ms rdma_rxe in QEMU) are not meaningful for the comparison. Decision gate moves to after userspace v2.

### Latency Results

| Test | p50 (ns) | p99 (ns) | Notes |
|------|----------|----------|-------|
| Kernel module RTT | 1,074,154 | 1,221,992 | rdma_rxe (soft-RoCE) in QEMU VM |
| Userspace proxy RTT | -- | -- | No userspace proxy exists yet |
| Raw RDMA RTT | -- | -- | Not measured yet |

### k0 Integration Test Results

| Test | Status | Notes |
|------|--------|-------|
| insmod | PASS | Module loads in acceptor mode |
| /proc/urp/stats | PASS | Readable, correct format |
| Basic RDMA echo (1 roundtrip) | PASS | urp-test-client -> module -> socat echo -> back |
| 1000 echo roundtrips | PASS | 1000/1000, payload verified |
| 100 MB throughput | PASS | 3.6 MB/s, 25726 frames, no hangs |
| Latency (1000 x 64B) | PASS | p50=1.07ms, p99=1.22ms |
| Stats verification | PASS | tx/rx bytes and frames match (104,936,257 bytes, 27,737 frames) |
| rmmod | PASS | Clean unload, no kernel errors |
| dmesg check | PASS | No urp errors/panics/warnings |

### Notes

- **VM-based testing** (recommended):
  ```
  nix run .#urp-vm -- start    # Start QEMU VM with matching kernel
  nix run .#urp-vm -- ssh test-kmod-k0   # Run integration tests
  nix run .#urp-vm -- stop     # Stop VM
  ```
- **Debug VM with sanitizers** (TODO -- currently panics at boot; deferred to Phase 5):
  ```
  nix run .#urp-vm-debug -- start    # Start VM with KASAN/KMEMLEAK/KUnit kernel
  nix run .#urp-vm-debug -- ssh test-kmod-k0   # Tests include sanitizer checks
  nix run .#urp-vm-debug -- stop
  ```
  Boot panic: kernel reaches userspace setup but cannot mount root fs (VIRTIO_BLK / 9P_FS missing from override-built kernel). Fix path: switch `nix/test-vm.nix` from `kernel.override { kernelPatches = ... }` to `boot.kernelPatches` inside the NixOS config so NixOS-injected qemu-vm configs are preserved.
- **Host testing** (requires `--impure` for kernel match):
  ```
  nix build --impure --expr \
    'let f = builtins.getFlake (toString ./.);
         p = import <nixpkgs> {};
     in f.lib.x86_64-linux.buildUrpKo p.linuxPackages'
  sudo test-kmod-k0 result/lib/modules/$(uname -r)/urp.ko
  ```
- All test scripts are Nix `writeShellApplication` with dependencies via `runtimeInputs`.
- Uses `rdma_rxe` (soft-RoCE) for RDMA testing without hardware.


---

## Phase 2: urp CLI + GENL Interface

**Status**: Complete -- kernel module, CLI, Nix package, and integration
test all build and run clean against Linux 7.0.3. The 19-test integration
suite (`test-kmod-k0`) passes 19/19 end-to-end inside the QEMU VM
including 1000-roundtrip echo, 100 MB throughput, 1000-sample latency,
EEXIST/ENOENT/EINVAL error paths, drain, remove, rmmod, and a clean
dmesg.

### Deliverables

- [x] `urp add/remove/show/stats/drain/set/monitor` all defined; clap+netlink wiring builds and unit-tests pass
- [x] `urp show --json` produces valid, parseable JSON with all fields (`format_json_smoke` test)
- [x] Multiple endpoints supported via per-name rhashtable (lookup is RCU; create/destroy mutex-serialized)
- [x] Multicast events emitted on every state transition (`urp_send_event` called from CREATING/ACTIVE/DRAINING/STOPPED)
- [x] Error cases return meaningful messages (EEXIST/ENOENT/EINVAL/EPERM mapped in `error.rs`; extack threaded through)
- [x] `urp add` with no module loaded -> "urp kernel module not loaded" error (CTRL_CMD_GETFAMILY ENOENT path)
- [x] Module unload with active endpoints -> drain-all-then-unregister-GENL ordering in `urp_exit`
- [x] `cargo test -p urp-cli` passes (8/8: attr roundtrip, sockaddr v4-mapped, format human/json, clap validation x2, kernel-header consistency)
- [ ] ~~KASAN/KMEMLEAK clean through full CLI exercise cycle~~ -- deferred to Phase 5 (sanitizer VM blocker carried over from Phase 1)

### Source Files Implemented

| File | Status | Notes |
|------|--------|-------|
| `kernel/include/uapi/linux/urp.h` | Done | UAPI: 5 attribute enums, 3 state enums, length limits, defaults, GENL family/version/mcgrp constants |
| `kernel/urp.h` | Done | Per-endpoint struct: name (rhashtable key), state, mutex, ht_node, rcu, sockaddr_in6 peer/bind, mutable num_qps/buffer_count/password |
| `kernel/urp_endpoint.c` | Done | NEW. rhashtable + create / activate / drain / destroy / lookup / drain_all. State machine CREATING -> ACTIVE -> DRAINING -> STOPPED. Teardown via call_rcu |
| `kernel/urp_proc.c` | Done | Refactored to per-endpoint subdirs: `/proc/urp/<name>/stats`, endpoint pointer attached as pde_data |
| `kernel/urp_netlink.c` | Done | NEW. GENL family "urp" v1: NEW/DEL/SET/GET (GET supports both doit + dumpit), `urp_fill_endpoint` shared serializer, "events" mcgrp on state changes |
| `kernel/urp_main.c` | Done | Rewrote: module loads idle, GENL-only configuration, drains all endpoints on unload before unregistering family |
| `kernel/Kbuild` | Done | Added urp_endpoint.o + urp_netlink.o |
| `crates/urp-cli/Cargo.toml` | Done | clap 4 derive, anyhow, thiserror, serde, serde_json, libc (no neli) |
| `crates/urp-cli/src/main.rs` | Done | Clap entry, exit-code mapping |
| `crates/urp-cli/src/uapi.rs` | Done | Hard-coded UAPI mirror + kernel-header re-parse test |
| `crates/urp-cli/src/attr.rs` | Done | Hand-rolled TLV encoder + iterator (~200 LOC) |
| `crates/urp-cli/src/netlink.rs` | Done | Raw AF_NETLINK / NETLINK_GENERIC: family resolution, request/reply, dump multipart, mcgrp subscribe (~290 LOC) |
| `crates/urp-cli/src/error.rs` | Done | UrpError + from_errno mapping |
| `crates/urp-cli/src/format.rs` | Done | Endpoint parse + format_human / format_oneline / format_json + sockaddr_in6 v4-mapped encode |
| `crates/urp-cli/src/commands/{add,remove,set,show,stats,drain,monitor}.rs` | Done | Subcommand wiring |
| `nix/urp-cli.nix` | Done | NEW. rustPlatform.buildRustPackage |
| `flake.nix` | Done | Exposes `urp-cli` as a flake package; threads it into testKmodK0 + testVm |
| `nix/test-vm.nix` | Done | Adds `urpCli` to VM environment.systemPackages; switched kernel to `linuxPackages_latest` (7.0.3) |
| `nix/checks.nix` | Done | `kernel-module-build` switched to `linuxPackages_latest` |
| `nix/test-kmod-k0.nix` | Done | Rewrote for the new flow (idle insmod -> `urp add` -> `/proc/urp/test/stats`); added EEXIST/ENOENT/EINVAL/no-module error tests |

### Variations from Plan

12. **Kernel target bumped to `pkgs.linuxPackages_latest` (7.0.3)** -- the
    plan used the nixpkgs default kernel (6.18.x). Switched because this
    code is intended for upstream review by the Linux kernel network dev
    team, who expect new code to compile against current mainline.
    Required two source-level adjustments to `urp_socket.c`:
    - `kernel_bind` / `kernel_connect` now take `struct sockaddr_unsized *`
      (kernel commit deprecating `struct sockaddr` for in-kernel callers).
    - `strscpy()` enforces a compile-time cstring check on both args via
      `__must_be_cstr`; pointer-typed `const char *path` parameters fail
      the check, so the relevant call sites drop down to the underlying
      `sized_strscpy()`.
    `kmod-local.nix` still uses the running system kernel (unchanged).
13. **Streams attribute returns single-entry array** reflecting the k0
    connection; real multi-stream emission lands in Phase 3 alongside the
    actual stream mux.
14. **`URP_ENDPOINT_A_PASSWORD` is stored raw, not hashed**, and no auth
    is enforced. SHA-256 + auth check lands in Phase 3 (PSK auth).
15. **`URP_ENDPOINT_A_RDMA_DEVICE` is parsed and stored but ignored** by
    the rdma_cm path (which still auto-picks). Wired through in Phase 3.
16. **`URP_QP_A_RTT_NS` reports 0** -- probes are Phase 3.
17. **`ep->uds_path` field removed** -- the Phase 1 acceptor stored the
    path in this dedicated field for the rdma-cm ESTABLISHED handler to
    read back. Phase 2 already stores both `listen_path` and
    `connect_path` on the endpoint, so the duplicate field was deleted
    and the ESTABLISHED handler now reads `ep->connect_path` directly.
18. **CLI uses hand-rolled netlink, not `neli`** -- the original plan
    called for `neli 0.7 + neli-proc-macros`. Switched to libc + a small
    in-tree TLV encoder to keep the dep tree shallow for upstream review
    and avoid neli's API churn. Total netlink layer is ~290 LOC.
19. **`urp set` is restricted to `num_qps`, `buffer_count`, `password`,
    plus the `state=DRAINING` form** -- matches the UAPI mutability
    contract. Other fields are silently ignored on SET (immutable).
20. **CLI subcommands take endpoint name as a positional argument**
    (`urp add NAME ...`, `urp show NAME`, `urp drain NAME`,
    `urp remove NAME`) -- matches the design-doc 23 examples. `add` and
    `set` were initially `--name <NAME>`; switched to positional after
    integration testing flagged the inconsistency.
21. **`urp_endpoints_params` uses fixed-key hashing only** -- the initial
    implementation set `obj_hashfn` / `obj_cmpfn` to hash only the
    NUL-terminated prefix of `name[16]`. `rhashtable_lookup_insert_fast()`
    `BUG()`s if `obj_hashfn` is set (see `include/linux/rhashtable.h`
    line 968 in 7.0.3). Switched to default fixed-key hashing on the full
    16-byte zero-padded name; `urp_endpoint_lookup()` zero-pads
    caller-supplied names into a stack buffer before lookup so that hash
    inputs are always identical for matching names.

### Source Files Implemented (kernel side)

| File | Status | Notes |
|------|--------|-------|
| `kernel/include/uapi/linux/urp.h` | Done | Full UAPI -- 5 enums (cmd/attr/endpoint/qp/stream/stats), 3 state enums, length limits, defaults |
| `kernel/urp.h` | Done | Per-endpoint struct: name (rhashtable key), state, mutex, ht_node, rcu, sockaddr_in6 peer/bind, mutable num_qps/buffer_count/password |
| `kernel/urp_endpoint.c` | Done | NEW. rhashtable + create / activate / drain / destroy / lookup / drain_all. State machine: CREATING -> ACTIVE -> DRAINING -> STOPPED. Teardown via call_rcu |
| `kernel/urp_proc.c` | Done | Refactored to per-endpoint subdirs: `/proc/urp/<name>/stats` |
| `kernel/urp_netlink.c` | Done | NEW. GENL family "urp" v1, 4 commands (NEW/DEL/SET/GET), GET supports both doit + dumpit, "events" multicast group on state changes |
| `kernel/urp_main.c` | Done | Rewrote: module loads idle, GENL-only configuration, drains all endpoints on unload before unregistering family |
| `kernel/Kbuild` | Done | Added urp_endpoint.o + urp_netlink.o |

### Variations from Plan

12. **Kernel target bumped to `pkgs.linuxPackages_latest` (7.0.3)** -- plan
    used the nixpkgs default kernel (6.18.x). Switched because this code
    is intended for upstream review by the Linux kernel network dev team.
    Required two source-level adjustments to `urp_socket.c`:
    - `kernel_bind` / `kernel_connect` now take `struct sockaddr_unsized *`
      (kernel commit deprecating `struct sockaddr` for in-kernel callers).
    - `strscpy()` enforces a compile-time cstring check on both args via
      `__must_be_cstr`; pointer-typed `const char *path` parameters fail
      the check, so we drop down to the underlying `sized_strscpy()`.
    `kmod-local.nix` still uses the running system kernel (unchanged).
13. **Streams attribute returns single-entry array** reflecting k0 connection;
    real multi-stream emission lands in Phase 3 alongside actual stream mux.
14. **`URP_ENDPOINT_A_PASSWORD` is stored raw, not hashed**, and no auth is
    enforced. SHA-256 + auth check lands in Phase 3 (PSK auth).
15. **`URP_ENDPOINT_A_RDMA_DEVICE` is parsed and stored but ignored** by the
    rdma_cm path (which still auto-picks). Wired through in Phase 3.
16. **`URP_QP_A_RTT_NS` reports 0** -- probes are Phase 3.
17. **`ep->uds_path` field removed** -- the Phase 1 acceptor stored the
    path in this dedicated field for the rdma-cm ESTABLISHED handler to
    read back. Phase 2 already stores both `listen_path` and `connect_path`
    on the endpoint, so the duplicate field was deleted and the ESTABLISHED
    handler now reads `ep->connect_path` directly.

### Notes

Kernel build verification:
```
$ nix build .#urp-ko
$ modinfo result/lib/modules/7.0.3/urp.ko
filename:       .../result/lib/modules/7.0.3/urp.ko
version:        0.0.2
description:    UDS-RDMA Proxy kernel module
license:        GPL
depends:        rdma_cm,ib_core
vermagic:       7.0.3 SMP preempt mod_unload
```

CLI build + tests:
```
$ nix build .#urp-cli
$ result/bin/urp --help
UDS-RDMA Proxy control CLI
Usage: urp <COMMAND>
Commands:
  add      Create a new endpoint
  remove   Remove an endpoint by name
  set      Mutate live endpoint config
  show     Show endpoint(s)
  stats    Print stats for endpoint(s)
  drain    Drain an endpoint (no new streams, finish existing)
  monitor  Subscribe to state-change events
$ cargo test -p urp-cli
test result: ok. 8 passed; 0 failed; 0 ignored
```

Test script + VM image:
```
$ nix build .#test-kmod-k0
$ nix build .#urp-vm
$ nix run .#urp-vm -- start
$ nix run .#urp-vm -- ssh sudo test-kmod-k0
...
========================================
  Phase 2 Test Results
========================================
  Passed: 19
  Failed: 0
========================================
```

Coverage exercised by the run:
- Module loads idle (no `module_param`); `/proc/urp` is empty.
- `urp add test --connect-path ... --bind 0.0.0.0:4791` creates the
  endpoint; `/proc/urp/test/stats` becomes readable.
- `urp show`, `urp show test`, `urp show test --json | jq` all work.
- Error paths: `urp add test ...` (duplicate) -> EEXIST,
  `urp remove nonexistent_ep` -> ENOENT,
  `urp add bad --num-qps 99` -> EINVAL.
- Data path via rdma_rxe over loopback eth0: 1/1 echo, 1000/1000 echo
  roundtrips, 100 MB throughput (~3.6 MB/s on rxe), 1000 x 64-byte
  latency (p50 ~1.07 ms, p99 ~1.15 ms over rxe).
- `urp drain test` -> draining, `urp remove test` -> stopped, `rmmod
  urp` succeeds. Final dmesg has no `urp:.*error|panic|bug|warn`
  entries.

Test fixtures (cosmetic) noted: the post-test cleanup trap leaves the
SSH session in a state where `nix run .#urp-vm -- ssh` returns 255 even
though the on-VM script itself reports 0/19 failures. Doesn't affect
the result; tracked for a follow-up cleanup pass.

---

## Phase 3a: k1 Data Path

**Status**: In Progress -- on branch `phase3a-k1-data-path` (cut from `c2eea2e`).

Phase 3 from `KERNEL-MODULE-PLAN.md` covers k1 functional in one big bucket
(14 deliverables). For execution we split it: **3a = data path** (multi-QP,
SRQ, credits, reorder, stream mux, lifecycle, GENL emitters); **3b** = probes
+ PSK auth + extended observability; **3c** = full KUnit + soak. The detailed
sub-plan lives in `~/.claude/profiles/siden/plans/floofy-stirring-donut.md`.

### Step Status

| Step | Subject | Commit | Notes |
|------|---------|--------|-------|
| 1 | Rust->kernel FFI staticlib prerequisite | `c2eea2e` | staticlib + `kernel/include/urp_ffi.h` + `nix/urp-protocol-ffi.nix`. Consumed by Step 5 (Rust reorder backend). |
| 2 | Multi-QP scaffold + round-robin selection | `9dd0a70` | `struct urp_qp`, `urp_qps_init/destroy/select_round_robin/index_of` in new `kernel/urp_qp.c`. Endpoint owns `ep->qps[]`. Single-cm-id flow preserved; `num_qps > 1` returns `-EOPNOTSUPP` until Step 2b. 19/19 `test-kmod-k0` still pass. |
| 2b | Actual N-QP multi-cm-id allocation | pending | Replaces the `-EOPNOTSUPP` guard. N parallel `rdma_cm_id`s; data-path-ready gate fires when all reach `ESTABLISHED`. |
| 3 | Shared Receive Queue (SRQ) | pending | `kernel/urp_srq.c`. |
| 4 | Per-QP credit flow control | pending | `kernel/urp_credit.{c,h}`, 1:1 with `uds_rdma_protocol::credit::CreditState`. |
| 5 | Reorder buffer (C rbtree default + Rust opt-in) | pending | `kernel/urp_reorder.{c,h}` + `urp_reorder_rust.c` + `kernel/Kconfig`. |
| 6 | Stream multiplexing core | pending | `kernel/urp_stream.c`, per-stream rhashtable. |
| 7 | Stream lifecycle (SYN/FIN/RST + half-close) | pending | Flag handling + UDS half-close. |
| 8 | GENL emitters wire up real state | pending | Real per-QP / per-stream nested blocks; aggregate stats. |
| 9 | Integration tests + bench harness | pending | Tests grow from 19 to ~30; new `nix/urp-bench.nix` for C-vs-Rust reorder comparison. |
| 10 | Tracker polish + benchmark table | pending | Final docs pass + C-vs-Rust numbers. |

### Variations from Plan

1. **Step 2 split into 2a (scaffold, this commit) and 2b (actual multi-cm-id)**
   to keep the diff size and review surface manageable per the user's
   "one commit per step" preference. The plan's `urp_qp_alloc_all`
   signature is preserved structurally; Step 2b will iterate `ep->qps[]`
   and create one `rdma_cm_id` per QP.
2. **`struct urp_qp_state` renamed to `struct urp_qp`** -- the UAPI
   already defines `enum urp_qp_state` (qualifying/active/draining/
   removed), which is a different concept and shares the C tag namespace.
   Naming the per-QP runtime struct `urp_qp` avoids the collision.
3. **`nix/urp-cli.nix` source filter** -- the c2eea2e commit added the
   `uds-rdma-protocol-ffi` crate but only updated `nix/checks.nix`'s
   filter, not `nix/urp-cli.nix`'s. `urp-cli` (and transitively
   `urp-vm` / `test-kmod-k0`) failed to build until the filter was
   extended in `e63db81`. Logged here so a future Phase 3a Step 5 doesn't
   re-trip the same issue when wiring up the FFI staticlib.

---

## Phase 3: k1 -- Functional

**Status**: In Progress (tracked under Phase 3a above for the data-path subset)

### Deliverables

- [ ] Multi-QP: 1-32 QPs per endpoint, configurable via `urp add --num-qps`
- [ ] Reorder buffer delivers frames strictly in sequence order across QPs
- [ ] Credit-based flow control: no RNR errors, sender blocks when out of credits
- [ ] Stream multiplexing: 100+ concurrent UDS connections over shared QP set
- [ ] Stream lifecycle: SYN/FIN/RST work correctly, half-close supported
- [ ] SRQ: shared receive queue prevents per-QP starvation
- [ ] QP health probes: PING/PONG measured RTT visible in `urp show`
- [ ] QP state machine: qualifying -> active transition works, draining removes QP from selection
- [ ] PSK authentication: matching passwords connect, mismatches reject
- [ ] `urp show` displays per-QP health, per-stream state, aggregate stats
- [ ] All KUnit tests pass (16+ test cases)
- [ ] All integration tests pass (15+ test cases)
- [ ] 1-hour soak test: no leaks (KMEMLEAK), no crashes (KASAN), no races (KCSAN)
- [ ] `rmmod` after all tests: clean teardown, KMEMLEAK reports zero leaks

### KUnit Test Results

| Test | Status | Notes |
|------|--------|-------|
| `test_frame_roundtrip_all_types` | | |
| `test_frame_max_payload` | | |
| `test_frame_zero_payload` | | |
| `test_credit_initial` | | |
| `test_credit_exhaust_and_grant` | | |
| `test_credit_threshold_batching` | | |
| `test_reorder_in_order` | | |
| `test_reorder_out_of_order` | | |
| `test_reorder_duplicate` | | |
| `test_reorder_gap_timeout` | | |
| `test_reorder_max_buffered` | | |
| `test_buffer_pool_alloc_free` | | |
| `test_buffer_pool_exhaustion` | | |
| `test_endpoint_state_machine` | | |
| `test_stream_id_allocation` | | |
| `test_psk_hash_match` | | |
| `test_psk_hash_mismatch` | | |

### Integration Test Results

| Test | Status | Notes |
|------|--------|-------|
| `test_basic_echo` | | |
| `test_large_transfer` | | |
| `test_bidirectional` | | |
| `test_multi_connection` | | |
| `test_half_close` | | |
| `test_abrupt_close` | | |
| `test_multi_qp_reorder` | | |
| `test_credit_backpressure` | | |
| `test_buffer_exhaustion` | | |
| `test_psk_auth_success` | | |
| `test_psk_auth_failure` | | |
| `test_psk_no_auth` | | |
| `test_endpoint_drain` | | |
| `test_module_unload` | | |
| `test_qp_health_probes` | | |

### Stress Test Results

| Test | Duration | Status | Notes |
|------|----------|--------|-------|
| `soak_16conn_4k` | 1 hour | | |
| `connection_churn` | 30 min | | |
| `oom_pressure` | 15 min | | |

### Notes


---

## Phase 4: k2 -- Optimized

**Status**: Not Started

### Deliverables

- [ ] page_pool integrated: allocation uses `page_pool_dev_alloc_pages()`, recycle uses `page_pool_put_page()`
- [ ] Zero-copy send: page-aligned 4KB writes bypass `copy_from_iter` (verified via stats counter)
- [ ] Adaptive CQ polling: transitions between event-driven and busy-poll (verified via tracepoints)
- [ ] NUMA: buffer allocation and kthreads on same NUMA node as RDMA device
- [ ] All Phase 3 tests still pass (regression)
- [ ] Performance benchmarks documented: latency p50/p99, throughput, message rate
- [ ] page_pool recycle rate > 95% under sustained load (verify via page_pool stats)
- [ ] Hardware RDMA tests pass (if hardware available)

### Performance Benchmark Results

| Benchmark | Metric | Result | Notes |
|-----------|--------|--------|-------|
| Latency (64B echo) | p50/p99 ns | | |
| Throughput (4KB bulk) | GB/s | | |
| Small message storm | msg/sec | | |
| page_pool vs free list | alloc/free ns | | |
| Zero-copy vs copy | latency delta | | |
| NUMA cross-node penalty | latency increase | | |

### Notes


---

## Phase 5: MicroVM Integration

**Status**: Not Started

### Deliverables

- [ ] MicroVM x86_64 pair test passes end-to-end (boot -> load -> test -> clean -> shutdown)
- [ ] MicroVM aarch64 pair test passes
- [ ] MicroVM riscv64 pair test passes
- [ ] KASAN/KMEMLEAK clean in all VM tests
- [ ] CI pipeline runs on every push (shared crate + CLI + namespace integration)
- [ ] Nightly CI runs MicroVM tests + soak test
- [ ] Kernel version matrix: module builds and tests pass on 6.1, 6.6, 6.12, latest
- [ ] Redpanda cluster test: produce/consume works through kernel module proxy

### Cross-Architecture Results

| Architecture | Emulation | Status | Duration | Notes |
|-------------|-----------|--------|----------|-------|
| x86_64 | KVM (native) | | | |
| aarch64 | QEMU TCG | | | |
| riscv64 | QEMU TCG | | | |

### Kernel Version Matrix

| Kernel | Build | Tests | Notes |
|--------|-------|-------|-------|
| 6.1 (LTS) | | | |
| 6.6 (LTS) | | | |
| 6.12 (LTS) | | | |
| Latest stable | | | |

### Notes


---

[Back to Plan](KERNEL-MODULE-PLAN.md) | [Back to Design Overview](DESIGN.md)
