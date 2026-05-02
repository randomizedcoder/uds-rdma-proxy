# Kernel Module Implementation Tracker

Progress tracker for the [Kernel Module Implementation Plan](KERNEL-MODULE-PLAN.md).

**Last updated**: 2026-05-02

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [Prerequisites](#phase-0-prerequisites) | Complete | 6/7 |
| 1 | [k0 -- Proof of Concept](#phase-1-k0----proof-of-concept) | In Progress | 7/9 |
| 2 | [urp CLI + GENL](#phase-2-urp-cli--genl-interface) | Not Started | 0/9 |
| 3 | [k1 -- Functional](#phase-3-k1----functional) | Not Started | 0/14 |
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

**Status**: Not Started

### Deliverables

- [ ] `urp add/remove/show/stats/drain/set/monitor` all work end-to-end
- [ ] `urp show --json` produces valid, parseable JSON with all fields
- [ ] Multiple endpoints can be created and managed simultaneously
- [ ] Multicast events fire on state transitions (verified via `urp monitor`)
- [ ] Error cases return meaningful messages (EEXIST, ENOENT, EINVAL)
- [ ] `urp add` with no module loaded -> "urp kernel module not loaded" error
- [ ] Module unload with active endpoints -> all endpoints drained and cleaned up
- [ ] `cargo test -p urp-cli` passes (unit tests for encoding, formatting, validation)
- [ ] KASAN/KMEMLEAK clean through full CLI exercise cycle

### Notes


---

## Phase 3: k1 -- Functional

**Status**: Not Started

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
