# Kernel Module Implementation Tracker

Progress tracker for the [Kernel Module Implementation Plan](KERNEL-MODULE-PLAN.md).

**Last updated**: 2026-05-24 (Phase 5 Step 1-7 done; HEAD `705ae8c` on `phase5-vm-pair`. microvm.nix VM-pair harness + CM self-deadlock + pump half-close + drain ordering + expect-eof + kthread parking. URP-to-URP smoke test PASS in ~38 s (was ~330 s, 9x faster).)

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [Prerequisites](#phase-0-prerequisites) | Complete | 6/7 |
| 1 | [k0 -- Proof of Concept](#phase-1-k0----proof-of-concept) | Complete | 7/9 (sanitizer items deferred) |
| 2 | [urp CLI + GENL](#phase-2-urp-cli--genl-interface) | Complete (`067829e`) | 8/9 |
| 3a | [k1 Data Path](#phase-3a-k1-data-path) | Complete (7d pending) | 9/9 main + 5/6 sub-steps; 7d (test-client multi-stream) pending |
| 3b | [Probes + PSK Auth](#phase-3b-probes--psk-auth) | Complete (`dd6bab0`) | 10/10 |
| 3 | [k1 -- Functional](#phase-3-k1----functional) | In Progress (via 3a) | 0/14 |
| 4 | [k2 -- Optimized](#phase-4-k2----optimized) | rxe-testable scope complete (`c41bd61`) + 1-hour soak PASS | 2/8 main + 3 deferred-hardware; soak harness + on-reconnect-leak fix added |
| 5 | [MicroVM Integration](#phase-5-microvm-integration) | In Progress (`705ae8c`) | 1/8 (x86_64 pair PASS in ~38 s) |

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

**Status**: Data-path scaffold complete -- on branch `phase3a-k1-data-path`
(cut from `c2eea2e`). Steps 1, 2, 2b, 3, 4, 5, 6, 7, 8, 9 committed and
green; Steps 4b, 5b, 7b deferred to follow-up commits pending peer / test
fixture work that they depend on. `test-kmod-k0` passes 23/23 against
HEAD (existing 19 from Phase 2 + 4 new multi-QP smoke tests).

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
| 2b | Actual N-QP multi-cm-id allocation | `f9f49b4` | Per-QP `rdma_cm_id`s via `struct urp_cm_ctx`. Initiator loops `num_qps` resolves; acceptor's listener fans out to N CONNECT_REQUESTs via slot allocator. Shared PD + CQs sized by `URP_CQ_ENTRIES * num_qps`. Test 12-15 (connect/disconnect/reconnect) exposed a latent flush-completion buffer leak that was masked in k0 by reinit-on-every-CONNECT_REQUEST; fixed by always returning buffers to the pool from send/recv_done. |
| 3 | Shared Receive Queue (SRQ) | `0fba325` | `kernel/urp_srq.c`. Per-endpoint SRQ shared by all QPs; per-QP RQ collapsed (`max_recv_wr = 0`, `qp_init_attr.srq = ep->srq`). Initial fill + repost-on-completion both go through `ib_post_srq_recv`. |
| 4 | Per-QP credit flow control (scaffold) | `728db70` | `kernel/urp_credit.{c,h}` 1:1 C port of `uds_rdma_protocol::credit::CreditState`. `struct urp_qp.credit` initialized in `urp_qps_init` with `URP_NUM_BUFS/2` initial credits. |
| 4b | Wire credit gate into TX/RX paths | `b7caab2` | TX consume on `ib_post_send` (best-effort: stalls counted in `URP_STATS_A_CREDIT_STALLS`, send proceeds so the existing test client keeps working); RX `record_recv` + threshold-driven `urp_emit_credit_frame`; RX of `URP_FRAME_TYPE_CONTROL` + `URP_CTRL_FLAG_CREDIT` -> `urp_credit_grant`. CREDIT-frame emission gated on `stream_id != 0` so the legacy stream_id=0 test-client path isn't disturbed. |
| 5 | Reorder buffer (C rbtree default) | `3737132` | `kernel/urp_reorder.{c,h}` rbtree-backed implementation. Opaque handle + copy-in/copy-out semantics matching the Rust FFI surface in `kernel/include/urp_ffi.h`, so Step 5b's Rust backend is a thin cast-and-forward shim. |
| 5b | Rust-backed reorder buffer wiring | `83570af` | `kernel/urp_reorder_rust.c` shim + `nix/checks.nix buildUrpKoWith` + `nix build .#urp-ko-rust` extracts `liburp_protocol_ffi.a` into `kernel/rust_ffi/` and rebuilds with `CONFIG_URP_REORDER_RUST=y`. **Known limitation**: on `CONFIG_X86_KERNEL_IBT=y` kernels (Linux 7.0.3 default) objtool runs on the linked module object and rejects a `compiler_builtins::math::libm_math::arch::x86::fma::fma_with_fma4` Rust helper that the staticlib pulls in but the reorder buffer doesn't use; resolution is upstream Rust-for-Linux work, tracked in the variation note in this section. The default `.#urp-ko` build (C rbtree) is unaffected. |
| 6 | Stream multiplexing core (scaffold) | `e2ea525` | `struct urp_stream` + `ep->streams` rhashtable + stream-id allocator (initiator=odd, acceptor=even). `kernel/urp_stream.c` has create / lookup / destroy / destroy_all. RCU-deferred free. Data path still uses single `ep->conn` -- Step 7 wires per-stream lifecycle. |
| 7 | Stream lifecycle handlers (SYN/FIN/RST + half-close) | `f3f9903` | `urp_stream_rx_syn / _rx_fin / _rx_rst / _tx_fin / _tx_rst / _rx_dispatch`. Half-close via `kernel_sock_shutdown(SHUT_WR)`; abort via `SHUT_RDWR` + RCU destroy. Wire-path call site lands in Step 7b alongside the multi-stream test. |
| 7b | Wire stream_id dispatch into RX path | `9075f57` | `urp_recv_done` now decodes `stream_id` + `flags` and routes: `stream_id == 0` -> `ep->conn` (k0 compat); non-zero -> `urp_stream_rx_dispatch` under RCU, with SYN auto-creating the stream and FIN/RST running the lifecycle handlers from Step 7. The dispatch entry point is now load-bearing. |
| 7c | TX + UDS multi-stream wiring (acceptor) | `f14a107` | Per-stream TX kthread (`urp_stream_tx_fn`); `urp_stream_connect_uds(stream, path)` opens a backend UDS per stream; `urp_stream_rx_syn` on the acceptor side now opens the per-stream UDS + starts the pump. Initiator-side accept-loop streaming + multi-stream integration test (50 concurrent UDS, half-close, RST) land with the test-client `--stream-id` extension (Step 7d). |
| 7d | Test-client `--stream-id` + multi-stream integration test | pending | Extend `tools/urp-test-client.c` with `--stream-id N`; add `test-kmod-k0` cases for two streams in parallel through one endpoint. Unblocks the multi-stream half of the §3 DoD. |
| 8 | GENL emitters wire up real state | `70fafc6` | `urp_fill_endpoint` iterates `ep->qps[]` and `ep->streams` rhashtable, emitting real per-QP / per-stream nested blocks. New per-QP `atomic64_t tx/rx_{bytes,frames}` counters bumped from TX/RX paths. Synthetic stream_id=0 emitted for legacy `ep->conn` traffic until Step 7b retires it. New aggregate counters (credit_stalls, reorder_insertions/drops, buffer_alloc_fails) report 0 -- Steps 4b/5b/7b light them up. RTT and auth_failures stay 0 (3b). |
| 9 | KUnit suites + multi-QP integration smoke | `336e7e0` | 16 new KUnit cases (credit 8, reorder 5, qp_select 3) mirror the Rust unit-test counts; 4 new integration tests bring `test-kmod-k0` to 23/23. Bench harness (`urp-bench.nix`) + multi-stream tests pair with Step 7b/5b. |
| 10 | Tracker polish + DoD checklist | (this commit) | Phase 3a Definition of Done checklist; `test-kmod-k0` banner renamed to "Phase 3a Test Results"; tracker reflects 9/9 + 3 deferred. C-vs-Rust benchmark table lands with Step 5b. |

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
4. **Flush-completion buffer-pool leak** -- the k0 send/recv done
   callbacks bailed early on `IB_WC_WR_FLUSH_ERR` without returning the
   buffer to the free list. k0 papered over this by re-initing the
   entire buffer pool inside the CONNECT_REQUEST handler (so each new
   client got a fresh pool). Step 2b's one-shot
   `urp_endpoint_setup_shared` exposed the leak after the first
   disconnect/reconnect cycle (Test 13 of `test-kmod-k0`). Fixed by
   always returning the buffer to the pool, including on FLUSH_ERR.

### Phase 3a Definition of Done (subset of plan §3)

- [x] Multi-QP: 1-32 QPs per endpoint via `urp add --num-qps` (`9dd0a70`, `f9f49b4`)
- [x] SRQ prevents per-QP receive starvation (`0fba325`)
- [x] Per-QP credit-state scaffold (`728db70`); TX consume + RX grant + CREDIT-frame emit/receive (`b7caab2`)
- [x] Reorder buffer (C rbtree backend) (`3737132`); Rust backend shim + Nix `urp-ko-rust` wiring (`83570af`, currently blocked end-to-end by upstream objtool/Rust on `CONFIG_X86_KERNEL_IBT=y` kernels)
- [x] Per-stream rhashtable + id allocator (`e2ea525`); SYN/FIN/RST/half-close handlers (`f3f9903`); RX dispatch wired (`9075f57`); acceptor-side TX + per-stream UDS connect (`f14a107`). Initiator-side accept-loop streaming + multi-stream integration test deferred to **Step 7d** (needs test-client `--stream-id`).
- [x] `urp show` displays real per-QP + per-stream + aggregate stats (`70fafc6`)
- [x] KUnit suites for credit / reorder / qp_select (`336e7e0`) -- compile in default urp.ko build; runtime gating via the sanitizer/debug VM is blocked on the same VIRTIO_BLK / 9P_FS issue carried from Phase 1 (Variation #9) and lands in **Phase 5**.
- [x] Multi-QP integration smoke (`urp add --num-qps 2` + `urp show --json | jq '.qps | length == 2'`) -- 23/23 PASS in `test-kmod-k0` (`336e7e0`)
- [ ] Multi-stream integration tests (50 concurrent UDS, half-close, RST, gap timeout, multi-stream throughput) -- depend on **Step 7b**

Deferred follow-ups still inside Phase 3a:

| Sub-step | Subject | Status |
|----------|---------|--------|
| 7d | Test-client `--stream-id` + multi-stream integration test | Pending. Extend `tools/urp-test-client.c` with `--stream-id N`; add `test-kmod-k0` cases that exercise two streams in parallel through one endpoint. With this, the §3 DoD multi-stream items become testable end-to-end. |
| 5b objtool follow-up | Get `nix build .#urp-ko-rust` clean on `CONFIG_X86_KERNEL_IBT=y` kernels | Pending. Either strip `compiler_builtins::math::libm_math::arch::x86::fma::*` from the staticlib or wait for an upstream Rust-for-Linux pattern that suppresses objtool on the `multi-obj-m` rule. The wiring committed in `83570af` covers everything else. |

Deferred to **Phase 3b** (per scope decision in `floofy-stirring-donut.md`):
QP health probes (RTT EWMA, qualifying/draining states), PSK auth
(SHA-256 + rdma_cm `private_data`), per-QP RTT in `urp show`, remaining
KUnit tests (probe, PSK, stream-id allocation).

Deferred to **Phase 3c**: 1-hour soak with KASAN / KMEMLEAK / KCSAN
(the sanitizer-VM blocker carries forward to Phase 5).

---

## Phase 3b: Probes + PSK Auth

**Status**: In Progress -- on branch `phase3b-probes-psk` (cut from Phase 3a HEAD `22f98fa`). Scope: §3.6 QP health probes (PING/PONG, RTT EWMA, qualifying/draining state machine) + §3.7 Tier 0.5 PSK auth (SHA-256 in `rdma_cm` `private_data`).

### Step Status

| Step | Subject | Commit | Notes |
|------|---------|--------|-------|
| 1 | Probe state + PROBE wire format + KUnit | `0d5c077` | `struct urp_qp` gains `probe_seq` / `consecutive_misses` / `last_ping_ns` / `rtt_ewma_ns`. `urp.h` adds `urp_ping_encode/decode_*` + `urp_pong_encode/decode_*` (matches `uds_rdma_protocol::probe` byte-for-byte). 3 KUnit cases pin the wire format. |
| 2 | Probe emit via per-endpoint delayed_work | `fa58e55` | `urp_probe_work_fn` reschedules every 250ms; `urp_emit_ping_on` encodes a `URP_FRAME_TYPE_PROBE` frame with a 32B PING payload + `ktime_get_ns`/`ktime_get_real_ns` and `ib_post_send`s on every established QP. Gated on `num_qps > 1` so the single-QP test-client scenario stays probe-free. `urp_recv_done` now silently reposts PROBE frames (no UDS delivery). |
| 3 | RX of PING -> emit PONG | `fc426a7` | `urp_emit_pong_on(ep, qp, ping_payload)` echoes the PING fields + adds `t_recv_real / t_pong_mono / t_pong_real` and `ib_post_send`s back on the same QP. Best-effort -- drops PONG on buffer / post failure. |
| 4 | RX of PONG -> RTT EWMA | `bc25de8` | `urp_recv_done` PONG branch reads `t_send_mono` from the echoed payload, computes RTT, integer-EWMA (alpha = 0.2: `new = old*4/5 + rtt/5`, first sample seeds directly), and resets `consecutive_misses`. |
| 5 | QP health state machine | `2cb06d3` | `enum urp_qp_state` values stored in `q->health` (QUALIFYING / ACTIVE / DRAINING / REMOVED). `RDMA_CM_EVENT_ESTABLISHED` promotes to ACTIVE; URP_QP_MISS_THRESHOLD (3) consecutive missed PONGs demotes to DRAINING; `urp_qp_select_round_robin` skips DRAINING / REMOVED. |
| 6 | GENL emits real per-QP RTT + health | `9b6189f` | `URP_QP_A_RTT_NS` reads `q->rtt_ewma_ns`; `URP_QP_A_STATE` reads `q->health` instead of the Phase 3a placeholder. |
| 7 | PSK SHA-256 hashing on add | `a820d92` | `urp_endpoint_create` runs `sha256(cfg->password, URP_PASSWORD_MAX, ep->password_hash)`; raw input zeroed via `memzero_explicit`. New `u8 password_hash[URP_PSK_HASH_LEN=32]` + `u8 auth_priv[1+32]` (pre-built rdma_cm private_data). |
| 8 | PSK in `rdma_cm` `private_data` | `1115590` | Initiator: `rdma_connect` carries `auth_priv` in `private_data` when `has_password`. Acceptor: `urp_cm_accept_one` validates incoming `private_data` against `ep->auth_priv`; mismatch -> `rdma_reject` before any QP allocation. Acceptor's `rdma_accept` reply echoes its own `auth_priv` so a future initiator-side validate (8b candidate) can use it. |
| 9 | Auth-failure stats + GENL event | `dd6bab0` | `urp_stats.auth_failures` (atomic64_t) incremented on PSK reject; `URP_STATS_A_AUTH_FAILURES` GENL emit reads it; `urp_send_event(ep)` multicasts on failure so `urp monitor` users see it. |
| 10 | Tracker polish + DoD | (this commit) | Phase 3b complete in tracker; DoD checklist added. |

### Phase 3b Definition of Done (subset of plan §3.6 + §3.7)

- [x] PROBE wire format + per-QP probe state (`0d5c077`) — KUnit pins it byte-for-byte against the Rust reference impl.
- [x] PING emission via per-endpoint `delayed_work` at 250ms (`fa58e55`); gated on `num_qps > 1` so the single-QP test-client path is undisturbed.
- [x] PONG response on PING reception (`fc426a7`).
- [x] RTT EWMA on PONG reception (`bc25de8`); alpha = 0.2 in integer math; first sample seeds directly.
- [x] QP health state machine (`2cb06d3`); URP_QP_MISS_THRESHOLD (3) consecutive missed PONGs demotes ACTIVE → DRAINING; `urp_qp_select_round_robin` skips DRAINING/REMOVED.
- [x] GENL emits real per-QP `URP_QP_A_RTT_NS` + `URP_QP_A_STATE` (`9b6189f`).
- [x] PSK SHA-256 hashing on add (`a820d92`).
- [x] PSK validated via `rdma_cm` `private_data` (`1115590`) — acceptor-validates-initiator end-to-end; acceptor echoes its own hash in `rdma_accept` reply for a future initiator-side check.
- [x] Auth-failure counter + GENL multicast event (`dd6bab0`).

Deferred follow-ups inside Phase 3b:

| Sub-step | Subject | Status |
|----------|---------|--------|
| 8b | Initiator validates acceptor's hash in `RDMA_CM_EVENT_ESTABLISHED` | Pending. Acceptor already echoes `auth_priv` in `rdma_accept` reply; just need to read `event->param.conn.private_data` on the initiator side and compare. |
| 3b/test | URP-to-URP integration test with matching + mismatched PSKs and a multi-QP probe-driven scenario | Pending. Pairs with Phase 3a's deferred 7d (test-client `--stream-id`) — a URP-to-URP harness covers both at once. |

Deferred to Phase 3c (unchanged): 1-hour soak with KASAN / KMEMLEAK / KCSAN.

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

**Status**: rxe-testable scope complete -- on branch `phase4-k2-optimized`
(cut from Phase 3b HEAD `99ac7ff`). Hardware-gated items (zero-copy
send, adaptive CQ polling, kthread NUMA binding, real-NIC benchmarks)
explicitly deferred: their wins are not measurable on rxe / siw, and
their primary risk surface is hardware-specific. The 23/23
`test-kmod-k0` regression suite passes on every commit.

### Step Status

| Step | Subject | Commit | Notes |
|------|---------|--------|-------|
| 1 | page_pool buffer management | `ded84a7` | `urp_bufs_init` calls `page_pool_create` + N x `page_pool_dev_alloc_pages`; pages get DMA-mapped via `ib_dma_map_page`. PP_FLAG_DMA_MAP is intentionally *not* set -- rxe/siw `ib_device->dma_device == NULL` would NULL-deref `dev_to_node` inside `page_pool_create`. Documented as the plan's "primary k2 engineering risk" (§4.1). For hardware NICs the ib_dma path is equivalent to dma_map_page so the choice carries no perf cost. |
| 2 | NUMA-aware page_pool | `78967e5` | `page_pool_params.nid` reads `dev_to_node(ib_dev->dev.parent)` when a real parent exists; falls back to `NUMA_NO_NODE` for software RDMA. |
| 3 | tracker polish + DoD | `c535c7a` | Phase 4 scope settlement; deferred items below. |
| 4 | 1-hour soak harness | `c41bd61` | `nix/soak-1h.nix`: load loop + churn add/remove every 30s; baseline-deltas slab/dmesg/stats; OVERALL PASS/FAIL with leak budget. Wired as `nix run .#urp-vm -- ssh soak-1h`. |
| 5 | Fix on-reconnect leak found by soak | `c41bd61` | `urp_socket_conn_cleanup` + call from DISCONNECT branch + defensive call at top of `urp_cm_accept_one`. The acceptor was orphaning one `urp-tx` kthread + one UDS `struct socket` per test-client reconnect (overwriting `ep->conn.uds_sock` / `ep->conn.tx_thread` without releasing the previous ones). Pre-fix soak: ~210 kB/cycle leak + TX freeze at first churn + 12 orphan kthreads after 30 cycles. Post-fix soak: see results below. |

### 1-Hour Soak Result (post-fix)

Command: `nix run .#urp-vm -- ssh "sudo soak-1h"`

| Metric | Value | Budget |
|---|---|---|
| Duration | 3600 s | |
| Load cycles | 1240 | |
| Churn add/remove cycles | 120 | |
| CLI failures | 0 | 0 |
| dmesg `urp:` errors / warnings | 0 | 0 |
| Slab Δ in-loop | +1956 kB (1.91 MB) | |
| Slab Δ post-rmmod | +1828 kB (1.79 MB) | 2048 kB (PASS) |
| Frames TX | 3,314,520 | matched RX exactly |
| Frames RX | 3,314,520 | |

Drain + rmmod freed 128 kB of slab beyond the in-loop measurement -- that's the endpoint struct + page_pool teardown returning memory cleanly. The residual 1.83 MB plateaued repeatedly during the run (multiple `slab_delta=X` ticks identical to the prior tick), strongly suggesting kernel-slab cache growth rather than a per-cycle urp leak; further attribution would need `/proc/slabinfo` deltas (deferred).

### Deferred to hardware-validation (no measurable change on rxe)

| Sub-step | Subject | Why deferred |
|----------|---------|--------------|
| 4.2 | Zero-copy send (`get_user_pages_fast` + post user pages directly) | High-risk pin-page lifecycle; the win is bypassing `copy_from_iter`, which costs nothing measurable when DMA is emulated. Needs a real NIC + a benchmark that exercises 4KB-page-aligned writes. |
| 4.3 | Adaptive CQ polling (event-driven <-> busy-poll switching) | Current `IB_POLL_WORKQUEUE` is already a managed mode; switching to manual polling is a substantial refactor whose only payoff is reducing workqueue scheduling jitter at >1k CQE/sec rates -- not reachable on rxe in the test VM. |
| 4.4-kthread | `kthread_bind` of TX pumps to the NIC's NUMA node | The page_pool half (`.nid` in page_pool_params) shipped in Step 2; the kthread half is a no-op when the NIC has no NUMA binding (rxe / siw). |
| 4.5 | Performance benchmarks on hardware | rxe gives ~3.6 MB/s throughput and ~1.07 ms p50 latency regardless of optimisation -- those numbers measure the QEMU emulation cost, not the urp module. Real numbers need a hardware RDMA NIC. |

### Definition of Done (rxe-testable subset)

- [x] page_pool integrated: allocation uses `page_pool_dev_alloc_pages()`, recycle uses `page_pool_put_page()` (`ded84a7`)
- [x] NUMA: page_pool reads `ib_dev->dev.parent`'s `numa_node` when available (`78967e5`)
- [x] All Phase 3 tests still pass (regression) -- 23/23 `test-kmod-k0` on each Phase 4 commit
- [ ] Zero-copy send -- deferred to 4.2 hardware-validation pass
- [ ] Adaptive CQ polling -- deferred to 4.3 hardware-validation pass
- [ ] TX kthread NUMA binding -- deferred to 4.4 hardware-validation pass
- [ ] Performance benchmarks documented on real hardware -- deferred to 4.5

### Performance Benchmark Results (rxe baseline; not meaningful for hardware comparison)

| Benchmark | Metric | rxe-in-QEMU | Notes |
|-----------|--------|-------------|-------|
| Latency (64B echo) | p50 ns | ~1,067,000 | Dominated by rxe + virtio-net loopback, not the urp module. |
| Throughput (4KB bulk) | GB/s | ~0.0036 | Same -- bench harness should land alongside the hardware test plan. |

### Notes


---

## Phase 5: MicroVM Integration

**Status**: In Progress (Steps 1-4 done; `662b0af` on `phase5-vm-pair`)

### Deliverables

- [x] MicroVM x86_64 pair test passes end-to-end (boot -> load -> test -> clean -> shutdown) -- `662b0af`, `nix run .#urp-microvm-pair-test` -> "PASS: URP-to-URP echo round-trip succeeded"
- [ ] MicroVM aarch64 pair test passes
- [ ] MicroVM riscv64 pair test passes
- [ ] KASAN/KMEMLEAK clean in all VM tests
- [ ] CI pipeline runs on every push (shared crate + CLI + namespace integration)
- [ ] Nightly CI runs MicroVM tests + soak test
- [ ] Kernel version matrix: module builds and tests pass on 6.1, 6.6, 6.12, latest
- [ ] Redpanda cluster test: produce/consume works through kernel module proxy

### Step Status (x86_64 microvm harness)

| Step | Commit | Subject |
|------|--------|---------|
| 1 | `042982a` | microvm.nix-based VM-pair test harness (replaces hand-rolled qemu-vm.nix orchestrator) |
| 2 | `1703d72` | Finer-grained verification phases (5b/6b/8b/9b + pre/post echo diag) |
| 3 | `2e8b5ce` | Defer rdma_connect() to fix CM self-deadlock (qp stuck in INIT under multi-cm-id) |
| 4 | `662b0af` | Keep socat stdin alive across UDS handshake (`hello-pair` round-trip PASS, harness workaround) |
| 5 | `6dbac33` | Kernel-side fixes: pump half-close keeps conn alive for RX, drain order fix. Removes Step 4 workaround. |
| 6 | `36236f1` | Drop the trailing `expect eof` wait in vm-expect.exp -- 20-44x speedup per vm_run; total test 330s -> 120s. |
| 7 | `705ae8c` | Park TX pump on EOF instead of voluntary return (avoids Linux 7.0.3 kthread_stop WARN+oops). Phase 11 teardown 75s -> 2.2s, Phase 12 shutdown 41s -> 3.5s. Total test ~38s. |

### Variations from Plan

1. **Replaced hand-rolled qemu-vm.nix harness with microvm.nix** -- The Phase 5 plan
   implied building a custom QEMU orchestrator. Adopted xdp2/pcp patterns instead
   (declarative microvm runner, dual TCP consoles, expect-driven interaction,
   trap cleanup, `pgrep -f process=<hostname>`). Eliminated entire classes of bugs
   the hand-rolled version had (silent SSH-poll timeouts, output-buffering loss,
   orphaned QEMU on wrapper exit, vermagic drift).

2. **Exposed a real urp kernel bug during smoke testing** -- The migration's
   finer-grained lifecycle phases caught a CM self-deadlock in the multi-cm-id
   path: `rdma_connect()` called inline from the CM event handler took
   `id->qp_mutex` while the rdma_cm core already held it, hanging the
   `cma_work_handler` kworker. Step 3 fixed via deferred work item.

3. **Pump half-close handling** -- The kernel's TX pump was setting
   `conn->active = false` on read-side EOF, which caused the RX completion
   handler to drop incoming frames. With request/response patterns like
   `echo X | socat`, the response from the remote side would arrive AFTER
   the local pump's recvmsg returned 0 and be silently discarded. Step 5
   fixes the pump to leave conn alive on half-close so RX can still forward
   the response. Removes Step 4's harness workaround.

4. **urp_endpoint_drain order** -- Standalone `urp_pump_stop()` ran BEFORE
   `urp_socket_cleanup()` which led to `kthread_stop` blocking on a kthread
   asleep in `kernel_recvmsg` against a socket that hadn't been shut down
   yet. Step 5 removes the standalone call; `urp_socket_cleanup ->
   urp_socket_conn_cleanup` already does the shutdown-then-stop dance.

5. **Test harness teardown was slow** (fixed in Steps 6 + 7) -- Kernel
   `urp_endpoint_drain` itself completes in ~14 ms (measured via
   instrumented build), but Phase 11 in the orchestrator initially took
   ~75 s. Two compounding bugs: (a) every vm_run paid a `expect eof` tax
   waiting for an EOF that NixOS auto-login getty never produced, and
   (b) on Linux 7.0.3 `kthread_stop` on a kthread that exited via
   `return 0` from its fn WARNs + NULL-derefs, hanging subsequent
   `urp remove` / `rmmod` invocations. Step 6 dropped the expect-eof
   wait; Step 7 parked the pump on a `kthread_should_stop()` loop
   instead of voluntary exit. Full pair test now runs in ~38 s.

### Cross-Architecture Results

### Cross-Architecture Results

| Architecture | Emulation | Status | Duration | Notes |
|-------------|-----------|--------|----------|-------|
| x86_64 | KVM (native) | PASS (`705ae8c`) | ~38 s full pair test | smoke + 12-phase lifecycle, no harness workarounds |
| aarch64 | QEMU TCG | Not started | | deferred |
| riscv64 | QEMU TCG | Not started | | deferred |

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
