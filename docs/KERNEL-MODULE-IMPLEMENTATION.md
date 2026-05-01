# Kernel Module Implementation Tracker

Progress tracker for the [Kernel Module Implementation Plan](KERNEL-MODULE-PLAN.md).

**Last updated**: 2026-05-01

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [Prerequisites](#phase-0-prerequisites) | Not Started | 0/7 |
| 1 | [k0 -- Proof of Concept](#phase-1-k0----proof-of-concept) | Not Started | 0/9 |
| 2 | [urp CLI + GENL](#phase-2-urp-cli--genl-interface) | Not Started | 0/9 |
| 3 | [k1 -- Functional](#phase-3-k1----functional) | Not Started | 0/14 |
| 4 | [k2 -- Optimized](#phase-4-k2----optimized) | Not Started | 0/8 |
| 5 | [MicroVM Integration](#phase-5-microvm-integration) | Not Started | 0/8 |

---

## Phase 0: Prerequisites

**Status**: Not Started

### Deliverables

- [ ] `uds-rdma-protocol` crate compiles with `--no-default-features` (no_std + alloc)
- [ ] `cargo test` passes: frame roundtrip, credit state, reorder buffer (20+ test cases)
- [ ] `cargo +nightly miri test` passes (no UB)
- [ ] `cargo fuzz run frame_decode` runs for 60s with no crashes
- [ ] `make -C kernel` produces `urp.ko` (empty module: init prints, exit prints)
- [ ] `sudo insmod kernel/urp.ko && sudo rmmod urp` succeeds on host
- [ ] MicroVM kernel boots with `CONFIG_KUNIT=y`

### Notes


---

## Phase 1: k0 -- Proof of Concept

**Status**: Not Started

### Deliverables

- [ ] Module loads, creates UDS socket, accepts connections, pumps data over RDMA
- [ ] Echo test: 1000 roundtrips, all data matches
- [ ] Throughput test: sustained 100MB transfer, no hangs or crashes
- [ ] `/proc/urp/stats` shows correct byte/frame counters
- [ ] KUnit tests pass: frame codec, buffer lifecycle
- [ ] KASAN clean: no memory errors during test suite
- [ ] KMEMLEAK clean: no leaks after `rmmod`
- [ ] Latency comparison measured and documented
- [ ] **Decision gate**: If latency improvement < 15% vs userspace v2, revisit whether kernel module path is justified

### Latency Results

| Test | p50 (ns) | p99 (ns) | Notes |
|------|----------|----------|-------|
| Kernel module RTT | | | |
| Userspace proxy RTT | | | |
| Raw RDMA RTT | | | |

### Notes


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
