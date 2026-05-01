# Testing Strategy

## 12.1 Software RDMA Setup

Linux provides software RDMA implementations that emulate RDMA hardware over regular Ethernet:

- **rdma_rxe** (`rdma_rxe` kernel module): RoCEv2 emulation. Processes RDMA operations in software using the kernel network stack. Functional fidelity is high; performance is not representative of hardware.
- **siw** (`siw` kernel module): iWARP emulation over TCP. Alternative to rxe.

Software RDMA via `rdma_rxe` is the backbone of all non-hardware testing — local development, namespace integration tests, CI, and MicroVM tests all use it. The `rdma_rxe` module processes RDMA verbs (ibv_post_send, ibv_poll_cq, rdma_cm connections, etc.) in software through the kernel network stack, so the full ibverbs code path is exercised identically to hardware. This makes `sudo modprobe rdma_rxe` the single most useful command during development.

**Ensuring rdma_rxe is available**: Most Linux distribution kernels ship `CONFIG_RDMA_RXE=m` by default. For NixOS hosts, use `boot.kernelPatches` to guarantee it — see [Section 19.2](19-project-structure.md#local-rdma-development-with-rdma_rxe) for the full NixOS configuration. Inside MicroVMs, `boot.kernelPatches` in the VM's NixOS config bakes RDMA support into the VM kernel automatically.

**Test environment setup** (single machine, two network namespaces):

```bash
# Load the software RDMA module
sudo modprobe rdma_rxe

# Create network namespaces and veth pair
ip netns add ns_a
ip netns add ns_b
ip link add veth_a type veth peer name veth_b
ip link set veth_a netns ns_a
ip link set veth_b netns ns_b

# Configure addresses
ip netns exec ns_a ip addr add 10.0.0.1/24 dev veth_a
ip netns exec ns_a ip link set veth_a up
ip netns exec ns_a ip link set lo up
ip netns exec ns_b ip addr add 10.0.0.2/24 dev veth_b
ip netns exec ns_b ip link set veth_b up
ip netns exec ns_b ip link set lo up

# Create RDMA devices on the veth interfaces
ip netns exec ns_a rdma link add rxe_a type rxe netdev veth_a
ip netns exec ns_b rdma link add rxe_b type rxe netdev veth_b

# Verify
ip netns exec ns_a rdma dev
ip netns exec ns_b rdma dev
ip netns exec ns_a ibv_devinfo
```

This gives two software RDMA devices (`rxe_a`, `rxe_b`) that communicate over a veth pair. Both proxy instances run on the same physical machine in different network namespaces. The devshell wraps all of this into a single `setup-rxe` command — see [Section 19.2](19-project-structure.md#local-rdma-development-with-rdma_rxe) for the full local development workflow.

## 12.2 Unit Tests

Unit tests use **table-driven tests** wherever possible. Each test function iterates over a table of named cases with explicit inputs, expected outputs, and a category tag (positive, negative, boundary, adversarial). This pattern makes it immediately visible which classes of input are covered, makes it trivial to add a new case (one table row), and produces clear failure messages that name the failing case.

#### Table-Driven Test Pattern

```rust
#[test]
fn test_frame_codec_roundtrip() {
    struct Case {
        name: &'static str,
        category: &'static str,  // positive | negative | boundary | adversarial
        header: FrameHeader,
        payload: &'static [u8],
        expect: Result<(), CodecError>,
    }

    let cases = &[
        // ── Positive ──────────────────────────────────────────────
        Case {
            name: "data frame with payload",
            category: "positive",
            header: FrameHeader {
                stream_id: 1,
                sequence_number: 42,
                frame_type: FRAME_TYPE_DATA,
                flags: 0,
                credits_granted: 8,
                payload_length: 5,
            },
            payload: b"hello",
            expect: Ok(()),
        },
        Case {
            name: "control frame zero payload",
            category: "positive",
            header: FrameHeader {
                stream_id: 0,
                sequence_number: 0,
                frame_type: FRAME_TYPE_CONTROL,
                flags: CTRL_FLAG_CREDIT,
                credits_granted: 16,
                payload_length: 0,
            },
            payload: b"",
            expect: Ok(()),
        },
        Case {
            name: "all data flags set",
            category: "positive",
            header: FrameHeader {
                stream_id: 7,
                sequence_number: u64::MAX,
                frame_type: FRAME_TYPE_DATA,
                flags: DATA_FLAG_SYN | DATA_FLAG_FIN | DATA_FLAG_RST,
                credits_granted: u16::MAX,
                payload_length: 3,
            },
            payload: b"abc",
            expect: Ok(()),
        },

        // ── Boundary ──────────────────────────────────────────────
        Case {
            name: "max stream_id",
            category: "boundary",
            header: FrameHeader { stream_id: u32::MAX, ..Default::default() },
            payload: b"",
            expect: Ok(()),
        },
        Case {
            name: "max sequence_number",
            category: "boundary",
            header: FrameHeader { sequence_number: u64::MAX, ..Default::default() },
            payload: b"",
            expect: Ok(()),
        },
        Case {
            name: "payload exactly max size",
            category: "boundary",
            header: FrameHeader { payload_length: MAX_PAYLOAD_SIZE as u32, ..Default::default() },
            payload: &[0xAA; MAX_PAYLOAD_SIZE],
            expect: Ok(()),
        },

        // ── Negative ─────────────────────────────────────────────
        Case {
            name: "payload_length exceeds buffer",
            category: "negative",
            header: FrameHeader { payload_length: MAX_PAYLOAD_SIZE as u32 + 1, ..Default::default() },
            payload: &[0; MAX_PAYLOAD_SIZE + 1],
            expect: Err(CodecError::PayloadTooLarge),
        },

        // ── Adversarial ──────────────────────────────────────────
        Case {
            name: "payload_length lies (claims more than actual)",
            category: "adversarial",
            header: FrameHeader { payload_length: 4096, ..Default::default() },
            payload: b"short",  // only 5 bytes, header claims 4096
            expect: Err(CodecError::PayloadLengthMismatch),
        },
        Case {
            name: "invalid frame type",
            category: "adversarial",
            header: FrameHeader { frame_type: 0xFF, flags: 0, ..Default::default() },
            payload: b"",
            expect: Err(CodecError::InvalidFrameType),
        },
        Case {
            name: "reserved flag bits set",
            category: "adversarial",
            header: FrameHeader { frame_type: FRAME_TYPE_DATA, flags: 0xFF, ..Default::default() },
            payload: b"",
            expect: Err(CodecError::ReservedFlagsSet),
        },
    ];

    for case in cases {
        let mut buf = vec![0u8; FRAME_HEADER_SIZE + case.payload.len()];
        let result = encode_frame(&case.header, case.payload, &mut buf)
            .and_then(|_| {
                let (decoded_header, decoded_payload) = decode_frame(&buf)?;
                assert_eq!(decoded_header, case.header);
                assert_eq!(decoded_payload, case.payload);
                Ok(())
            });

        assert_eq!(
            result.is_ok(),
            case.expect.is_ok(),
            "[{}] ({}) expected {:?}, got {:?}",
            case.category, case.name, case.expect, result
        );
    }
}
```

The `[category] (name)` format in the assert message makes failures immediately actionable:

```
[adversarial] (payload_length lies) expected Err(PayloadLengthMismatch), got Ok(())
```

#### Test Coverage by Component

| Component | Positive | Negative | Boundary | Adversarial |
|-----------|----------|----------|----------|-------------|
| **Frame codec** | Roundtrip encode/decode, all valid flag combinations, zero payload, piggybacked credits | Payload exceeds max, truncated header, incomplete payload | `u32::MAX` stream_id, `u64::MAX` seq, payload exactly max size, payload = 1 byte | Lying payload_length, reserved flag bits set, malformed header bytes |
| **Buffer pool** | Alloc/dealloc correctness, concurrent alloc/dealloc stress test, alignment verification | Pool exhaustion returns `None`, double-free detection | Alloc exactly `pool_size` buffers, alloc `pool_size + 1` | **Poison pattern test** — stale data from previous connection (see [Section 12.10](#1210-buffer-bleed--poisoning-tests)) |
| **Credit state machine** | Initial credits correct, decrement on send, grant on recv, threshold triggers grant message | Decrement below zero blocked, grant above max capped | Exactly-at-threshold (grant vs no-grant), zero initial credits, max `u16` credits | Peer grants more credits than possible (overflow), rapid grant/decrement oscillation |
| **Reorder buffer** | In-order delivery (passthrough, no buffering), single out-of-order frame reordered | Duplicate sequence number rejected, gap exceeds timeout | Exactly-at-gap-timeout delivery, buffer at max capacity, seq 0 and `u64::MAX` | Massive gap (seq jumps by millions), interleaved streams on same buffer |
| **QP selector** | Round-robin even distribution, hash-affinity same stream → same QP | All QPs at zero credits → backpressure signal | Single QP (degenerate case), 32 QPs (max), stream_id = 0 | Adaptive weights overflow, QP failure mid-selection |
| **Configuration** | Valid TOML parsing, CLI overrides win, all defaults valid | Missing required fields, invalid types, negative values | buffer_count = credits (minimum valid), max_payload_size = 1 | buffer_count < credits (invalid), num_qps = 0, conflicting CLI + TOML |

## 12.3 Integration Tests

End-to-end tests using software RDMA (rxe/siw):

```
 Test setup:
 +----------+     +----------+    rxe loopback    +----------+     +----------+
 | Test     | UDS | Proxy A  | <================> | Proxy B  | UDS | Test     |
 | Client   |---->| (ns_a)   |    (or veth pair)  | (ns_b)   |---->| Server   |
 +----------+     +----------+                    +----------+     +----------+
```

| Test | Description |
|------|-------------|
| **Basic transfer** | Write N bytes on one side, verify exact same bytes arrive on other side |
| **Bidirectional** | Simultaneous writes in both directions, verify correctness |
| **Large transfer** | Stream 1GB+ to verify sustained throughput and no memory leaks |
| **Half-close** | Close write end on one side, verify EOF propagates, other direction continues |
| **Abrupt close** | Kill one proxy, verify other side detects failure and cleans up |
| **Multiple connections** | 100 concurrent UDS connections, verify all data correct |
| **Multi-QP reordering** | With N QPs and artificial delay injection on some QPs, verify in-order delivery |
| **Credit exhaustion** | Fast sender, slow receiver: verify backpressure works, no data loss |
| **Buffer pool exhaustion** | More concurrent sends than buffer slots: verify graceful backpressure |
| **Reconnection** | Proxy restart: verify applications can reconnect and resume |
| **Lossy network (tc-netem)** | Inject 0.1% packet loss on veth pair using `tc qdisc add dev veth_a root netem loss 0.1%`. Validates RDMA retry mechanisms, credit flow control recovery, and reorder buffer gap_timeout tuning under imperfect fabric conditions. See [Section 12.7](#127-lossy-network-simulation-rocev2-flappiness). |
| **NUMA negative affinity** | Run proxy with buffers on NUMA node 0 but NIC and CPU threads pinned to NUMA node 1. Measures worst-case cross-NUMA latency as a baseline to demonstrate why `cpu_affinity` and `huge_pages` config matters. See [Section 12.8](#128-numa--memory-locality-validation). |
| **Redpanda integration** | Run a 3-node Redpanda cluster with inter-broker traffic forced through the proxy. Validate with `rpk topic produce/consume` and `rpk bench`. See [Section 12.9](#129-redpanda-compatibility-suite). |

## 12.4 Load Generator (`uds-rdma-bench`)

A companion binary for benchmarking and profiling:

```
uds-rdma-bench \
  --mode producer \
  --uds-path /tmp/app.sock \
  --message-size 4096 \
  --rate 100000 \
  --duration 60s \
  --report-interval 1s
```

**Modes**:

| Mode | Description |
|------|-------------|
| **Producer** | Writes data to UDS at configurable rate and message size. Measures send throughput. |
| **Consumer** | Reads data from UDS. Measures receive throughput. Optionally verifies data integrity (sequence numbers, checksums). |
| **Echo** | Reads from UDS, writes back immediately. For round-trip latency measurement. |
| **Bidirectional** | Simultaneous producer + consumer on the same UDS connection. |

**Output**: Periodic reports (configurable interval) with throughput (MB/s, msg/s), latency percentiles (p50, p99, p99.9), and CPU utilization. Final summary at completion.

**Data integrity**: The producer can embed a sequence number and CRC32 in each message. The consumer verifies sequence continuity and checksum correctness.

## 12.5 Performance Benchmarking

### 12.5.1 Criterion Microbenchmarks

Low-level benchmarks for hot-path components, run via `cargo bench`. These use [Criterion.rs](https://github.com/bheisler/criterion.rs) for statistical rigor (confidence intervals, outlier detection, automatic regression detection against saved baselines).

```
benches/
├── frame_codec.rs        # encode/decode throughput at various payload sizes
├── buffer_pool.rs        # alloc/free cycle latency, contended vs uncontended
├── reorder_buffer.rs     # insert + drain throughput, varying gap sizes
├── credit_accounting.rs  # grant/decrement/threshold-check hot loop
└── batching.rs           # AdaptiveBatcher decision overhead per message
```

| Benchmark | What it measures | Key scenarios |
|-----------|-----------------|---------------|
| `frame_encode_*` | Encode throughput (GB/s) | 64B, 1KB, 4KB, max payload; with/without credit piggyback |
| `frame_decode_*` | Decode throughput (GB/s) | Same sizes; valid frames, frames at boundary sizes |
| `buffer_alloc_free` | Alloc+free cycle (ns) | Uncontended single-thread; 4/8/16 threads contended (crossbeam ArrayQueue) |
| `buffer_alloc_exhaustion` | Behavior at pool capacity | Alloc all slots then measure alloc failure path latency |
| `reorder_insert_drain` | Insert+drain throughput (ops/s) | In-order (passthrough), 1% out-of-order, 10% out-of-order, worst-case reverse |
| `reorder_buffer_depth` | Memory pressure at depth | 100, 1K, 10K, 100K buffered frames — measures BTreeMap overhead |
| `credit_grant_decrement` | Accounting overhead (ns/op) | Hot loop: decrement, check threshold, grant; single-threaded |
| `adaptive_batcher_decision` | Per-message flush decision (ns) | EWMA update + should_flush() at low/medium/high throughput rates |

**Regression detection**: CI runs `cargo bench` nightly and compares against the `main` branch baseline. Criterion's `--save-baseline` / `--baseline` flags automate this. Regressions > 5% trigger a CI failure.

### 12.5.2 End-to-End Macro Benchmarks

Full-system benchmarks using `uds-rdma-bench` through the proxy over `rdma_rxe`. These measure real throughput and latency including all copies, syscalls, and protocol overhead.

**Benchmark matrix**:

| Variable | Values | Purpose |
|----------|--------|---------|
| **Message size** | 64B, 256B, 1KB, 4KB, 16KB, 64KB | Throughput vs. latency trade-off; find PMTU-aligned sweet spots |
| **Concurrency** | 1, 4, 16, 64, 256 connections | Scalability: lock contention, buffer pool pressure, CQ saturation |
| **QP count** | 1, 4, 8, 16, 32 | ECMP scaling: diminishing returns, reorder buffer overhead |
| **CQ poll mode** | busy, event, adaptive | Latency vs. CPU trade-off at various load levels |
| **Buffer size** | 1KB, 4KB, 8KB, 16KB, 64KB | Optimal sizing relative to PMTU and message size |
| **Batch mode** | disabled, fixed 100us, adaptive | Batching impact at various message rates |
| **Phase** | v0 (TCP), v1 (rsockets), v2 (ibverbs) | Quantify each phase's improvement |

**Key benchmark scenarios**:

| Scenario | Configuration | What it answers |
|----------|--------------|-----------------|
| **Latency floor** | 1 connection, 64B messages, echo mode, busy-poll CQ | Minimum achievable round-trip latency |
| **Throughput ceiling** | 16 connections, 64KB messages, producer mode, 32 QPs | Maximum achievable throughput (MB/s) |
| **Small message storm** | 256 connections, 64B messages, 1M msg/s target | Overhead per message; syscall and CQ bottlenecks |
| **Mixed workload** | 50% 64B + 50% 4KB, bidirectional, adaptive batching | Realistic workload behavior |
| **Scalability curve** | Sweep 1→256 connections at fixed 4KB messages | Find the connection count where throughput plateaus |
| **QP scaling curve** | Sweep 1→32 QPs at fixed 16 connections | Find where reorder buffer overhead exceeds ECMP benefit |
| **Backpressure behavior** | Fast producer, slow consumer (rate-limited), 60s | Credit stall frequency, UDS backpressure latency, recovery time |
| **Phase comparison** | Same workload across v0/v1/v2 | Quantify TCP→rsockets→ibverbs improvement |

**Output per benchmark run**:
- `bench.json`: Per-second samples with throughput (MB/s, msg/s), latency percentiles (p50, p90, p99, p99.9, max), CPU utilization
- `metrics-{a,b}.prom`: Prometheus scrape from both proxy instances (all 50+ metrics)
- `SUMMARY.md`: One-page markdown with key numbers and comparison to baseline
- `flamegraph.svg`: CPU profile from `perf record` (optional, via `--profile` flag)

### 12.5.3 Nix Experiment Factory

Each benchmark scenario is wrapped as a Nix derivation via `mkBenchExperiment` (see [Section 19.2](19-project-structure.md#nixbenchmkbenchexperimentnix-experiment-factory)), making runs fully reproducible. Example:

```bash
# Run a specific experiment
nix run .#bench-latency-floor
nix run .#bench-throughput-ceiling
nix run .#bench-phase-comparison

# Run all experiments (slow — full matrix)
nix run .#bench-all
```

Results accumulate in `bench-results/` with timestamped directories, enabling historical comparison.

### 12.5.4 Profiling Tools

| Tool | What it provides | When to use |
|------|-----------------|-------------|
| `perf stat` | CPU counters: IPC, cache misses, context switches, branch mispredicts | Quick overhead assessment per benchmark |
| `perf record` + `flamegraph` | CPU flame graph (sample-based) | Identify hot functions when throughput is unexpectedly low |
| `cargo-show-asm` | Assembly output for specific functions | Verify compiler vectorization, check for unexpected branches in hot path |
| Prometheus + Grafana | Real-time dashboards of all 50+ proxy metrics | Monitor during long benchmark runs; correlate throughput dips with flow control stalls |
| `mlx5_core` counters | NIC-level: TX/RX bytes, packet drops, PFC pause frames | Production hardware profiling (not available with rdma_rxe) |
| `valgrind --tool=cachegrind` | Cache line utilization analysis | Optimize struct layout for cache friendliness |
| `perf c2c` | False sharing detection | Identify cacheline contention between CQ poll thread and async runtime |

## 12.6 CI Pipeline

```yaml
# GitHub Actions
test-unit:
  # Standard unit tests, no RDMA needed
  runs-on: ubuntu-latest
  steps:
    - cargo test --lib

test-integration:
  # Requires rdma_rxe
  runs-on: [self-hosted, rdma-capable]  # OR Docker with --privileged
  steps:
    - modprobe rdma_rxe
    - # setup namespaces and rxe devices
    - cargo test --test integration

benchmark:
  # Nightly performance regression tests
  runs-on: [self-hosted, rdma-capable]
  steps:
    - cargo bench
    - # Compare against baseline, flag regressions > 5%
```

**Docker alternative**: Run integration tests in a privileged container that loads `rdma_rxe`:

```dockerfile
FROM rust:latest
RUN apt-get update && apt-get install -y rdma-core libibverbs-dev librdmacm-dev iproute2
# Tests load rdma_rxe inside the container (requires --privileged)
```

## 12.7 Lossy Network Simulation (RoCEv2 Flappiness)

Hardware RoCEv2 is notoriously sensitive to network configuration. Even with PFC enabled, transient packet loss can occur during PFC storms, switch reboots, or misconfigured ECN thresholds. The proxy must handle this gracefully.

**Test setup**: Use `tc-netem` on the veth pair to inject controlled impairment:

```bash
# Inject 0.1% random packet loss
ip netns exec ns_a tc qdisc add dev veth_a root netem loss 0.1%

# Inject 0.01% loss with 50us jitter (more realistic)
ip netns exec ns_a tc qdisc add dev veth_a root netem loss 0.01% delay 10us 50us

# Inject reordering (25% of packets delayed by 10ms)
ip netns exec ns_a tc qdisc add dev veth_a root netem delay 10ms reorder 25% 50%
```

**What this validates**:
- RDMA RC QP retry mechanisms (`rnr_retry` and `retry_cnt` parameters) handle dropped packets without QP failure
- Credit-based flow control recovers correctly after packet loss causes temporary credit desynchronization
- The reorder buffer's `gap_timeout` is tuned correctly -- too short and it false-positives on normal ECMP reordering; too long and it delays failure detection
- Prometheus metrics correctly report `wc_errors_total` and `reorder_gap_timeouts_total` under impairment
- No data corruption or duplication after recovery

**Test matrix**:
| Impairment | Loss % | Jitter | Duration | Expected Outcome |
|-----------|--------|--------|----------|-----------------|
| Clean baseline | 0% | 0 | 60s | No errors, baseline throughput |
| Light loss | 0.01% | 0 | 60s | Some retransmissions, minor throughput dip, no data loss |
| Moderate loss | 0.1% | 50us | 60s | Visible retransmissions, throughput reduction, recovery |
| Heavy reorder | 0% | 10ms, 25% reorder | 60s | Reorder buffer engaged, correct delivery order |
| PFC storm sim | 1% loss, 100ms burst | 0 | 30s | QP may reset; verify graceful reconnection |

## 12.8 NUMA & Memory Locality Validation

The performance optimizations in [Section 13.6](13-performance.md#136-numa-aware-buffer-placement) (NUMA-aware placement) are only valuable if they actually make a measurable difference. A **negative affinity test** provides the worst-case baseline:

**Test setup**:
```bash
# Correct affinity (NIC on NUMA node 0, buffers + threads on NUMA node 0)
numactl --cpunodebind=0 --membind=0 ./uds-rdma-proxy --config correct.toml

# Wrong affinity (NIC on NUMA node 0, buffers + threads on NUMA node 1)
numactl --cpunodebind=1 --membind=1 ./uds-rdma-proxy --config wrong.toml
```

**Metrics to compare**:
| Metric | Expected Correct | Expected Wrong | Why |
|--------|-----------------|----------------|-----|
| `frame_latency_seconds` p99 | ~5us | ~15-30us | Cross-NUMA memory access adds 50-100ns per access |
| `rdma_send_latency_seconds` | ~2us | ~5-10us | NIC DMA from remote NUMA node |
| CPU cache misses (`perf stat`) | Low | 2-5x higher | Remote NUMA accesses bypass local cache |
| Throughput (MB/s) | Baseline | 30-60% of baseline | Cross-NUMA bandwidth is lower |

This test is critical for **documentation purposes**: it provides concrete numbers showing users why the `cpu_affinity` and `huge_pages` settings in the TOML config are not optional for production deployments.

## 12.9 Redpanda Compatibility Suite

Since the project is motivated by tunneling Redpanda inter-broker traffic, integration testing with Redpanda validates the "universal compatibility" claim with a real-world, high-throughput application.

**Test setup**:

```
 Node A                         Node B                         Node C
 +----------+                   +----------+                   +----------+
 | Redpanda |                   | Redpanda |                   | Redpanda |
 | Broker 0 |                   | Broker 1 |                   | Broker 2 |
 +----+-----+                   +----+-----+                   +----+-----+
      | UDS                          | UDS                          | UDS
 +----v-----+     RDMA          +----v-----+     RDMA          +----v-----+
 | Proxy A  |<=================>| Proxy B  |<=================>| Proxy C  |
 +----------+                   +----------+                   +----------+
```

**Test procedure**:
1. Configure Redpanda brokers to use UDS for inter-broker communication (Kafka API listener on UDS path)
2. Run proxy pairs between each node pair
3. Create a topic with replication factor 3
4. Use `rpk topic produce` to write 1M messages (1KB each)
5. Use `rpk topic consume` to read all messages back, verify count and content
6. Run `rpk bench` (Redpanda's built-in benchmark) to measure:
   - Produce throughput (MB/s) via proxy vs. native TCP
   - Consume throughput (MB/s) via proxy vs. native TCP
   - End-to-end latency p50/p99 via proxy vs. native TCP

**Success criteria**:
- Zero message loss or corruption
- Produce/consume throughput within 20% of native TCP (on software RDMA; should exceed TCP on hardware)
- No Redpanda errors or leader election instability caused by the proxy
- Clean shutdown and restart of proxy does not cause permanent partition unavailability

## 12.10 Buffer Bleed / Poisoning Tests

Pre-registered buffer pools reuse memory across connections. A bug in `payload_length` handling could cause the proxy to send stale data from a previous connection to a new one ("buffer bleed"). This is both a correctness and security concern.

**Table-driven buffer bleed tests**:

```rust
#[test]
fn test_buffer_pool_bleed_protection() {
    struct Case {
        name: &'static str,
        category: &'static str,
        /// Data written by "connection 1" before freeing the slot
        stale_data: &'static [u8],
        /// Payload length claimed by the header on reuse
        claimed_length: u32,
        /// Actual data written by "connection 2"
        new_data: &'static [u8],
        /// Expected validated send length (header + actual payload)
        expected_send_len: usize,
        /// Whether stale data should be visible in the reallocated slot
        expect_stale_visible: bool,
    }

    let cases = &[
        // ── Positive ──────────────────────────────────────────────
        Case {
            name: "clean reuse after free",
            category: "positive",
            stale_data: b"CONFIDENTIAL_DATA_FROM_CONN_1",
            claimed_length: 5,
            new_data: b"hello",
            expected_send_len: FRAME_HEADER_SIZE + 5,
            expect_stale_visible: false,
        },

        // ── Boundary ──────────────────────────────────────────────
        Case {
            name: "full buffer reuse",
            category: "boundary",
            stale_data: &[0xFF; MAX_PAYLOAD_SIZE],
            claimed_length: MAX_PAYLOAD_SIZE as u32,
            new_data: &[0xAA; MAX_PAYLOAD_SIZE],
            expected_send_len: FRAME_HEADER_SIZE + MAX_PAYLOAD_SIZE,
            expect_stale_visible: false,
        },
        Case {
            name: "single byte reuse",
            category: "boundary",
            stale_data: b"SECRET",
            claimed_length: 1,
            new_data: b"X",
            expected_send_len: FRAME_HEADER_SIZE + 1,
            expect_stale_visible: false,
        },

        // ── Adversarial ──────────────────────────────────────────
        Case {
            name: "payload_length lies (claims more than written)",
            category: "adversarial",
            stale_data: b"OLD_SECRET_DATA_THAT_SHOULD_NOT_LEAK",
            claimed_length: 4096,  // claims 4KB
            new_data: &[0x42; 100],  // only writes 100 bytes
            expected_send_len: FRAME_HEADER_SIZE + 100,  // must clamp, not trust header
            expect_stale_visible: false,
        },
        Case {
            name: "zero-length reuse still zeroed",
            category: "adversarial",
            stale_data: b"SECRETS_IN_BUFFER",
            claimed_length: 0,
            new_data: b"",
            expected_send_len: FRAME_HEADER_SIZE,
            expect_stale_visible: false,
        },
    ];

    let pool = BufferPool::new(/* ... */);

    for case in cases {
        // Connection 1: write stale data, then free
        let slot = pool.alloc_tx().unwrap();
        slot.write_payload(case.stale_data);
        pool.free_tx(slot);

        // Connection 2: reallocate the same slot
        let slot2 = pool.alloc_tx().unwrap();

        // Verify no stale data visible
        if !case.expect_stale_visible {
            assert!(
                slot2.payload_region().iter().all(|&b| b == 0),
                "[{}] ({}) buffer contains stale data after realloc",
                case.category, case.name,
            );
        }

        // Write new data with a possibly-lying header
        slot2.write_payload(case.new_data);
        let mut header = FrameHeader::default();
        header.payload_length = case.claimed_length;
        slot2.set_header(header);

        // Validated send length must clamp to actual bytes written
        assert_eq!(
            slot2.validated_send_length(),
            case.expected_send_len,
            "[{}] ({}) send length mismatch",
            case.category, case.name,
        );

        pool.free_tx(slot2);
    }
}
```

**Debug-mode poisoning**: In debug builds (and optionally in release with a config flag), the buffer pool fills freed buffers with `0xDE` bytes. The send path asserts that the region beyond `payload_length` does not contain valid-looking data from a previous use. This catches buffer bleed bugs during development without impacting release performance.

## 12.11 MicroVM Integration Testing

For the highest fidelity testing short of real hardware, the project uses [microvm.nix](https://github.com/astro/microvm.nix) to run the proxy in **pairs of lightweight virtual machines** with independent kernel instances and RDMA subsystems. This complements the namespace-based testing above with full kernel isolation and cross-architecture coverage.

| Tier | Method | Isolation | Architectures | Speed |
|------|--------|-----------|---------------|-------|
| 1 | `cargo test` | Process | Host only | Seconds |
| 2 | Network namespaces + rdma_rxe | Process (shared kernel) | Host only | Seconds |
| 3 | **MicroVM pairs + rdma_rxe** | **Full kernel (separate VMs)** | **x86_64, aarch64, riscv64** | **Minutes** |

**What MicroVMs add beyond namespaces**:
- Independent kernel RDMA subsystems (catches bugs masked by shared kernel state)
- Cross-architecture testing via QEMU TCG (ARM, RISC-V) — validates the proxy runs correctly on non-x86 hardware without requiring physical machines
- Controlled kernel version (Nix-pinned, reproducible)
- Automatic rdma_rxe configuration on boot via systemd services
- Prometheus metric scraping from both VMs for post-test analysis

**Test execution**:
```bash
nix run .#microvms.test-x86_64     # KVM: ~2 minutes
nix run .#microvms.test-aarch64    # QEMU TCG: ~5 minutes
nix run .#microvms.test-riscv64    # QEMU TCG: ~10 minutes
nix run .#microvms.test-all        # All architectures sequentially
```

See [Section 19.3](19-project-structure.md#193-microvm-integration-testing) for the full Nix infrastructure design and [Section 19.4](19-project-structure.md#194-multi-architecture-support) for cross-compilation details.

## 12.12 Fuzzing and Security Testing

The proxy sits on a network boundary, processes untrusted data from UDS clients, and uses `unsafe` code for RDMA buffer management. This demands aggressive security testing beyond conventional unit and integration tests.

### 12.12.1 Fuzz Targets (cargo-fuzz / libFuzzer)

Structure-aware fuzzing using [cargo-fuzz](https://github.com/rust-fuzz/cargo-fuzz) (backed by LLVM's libFuzzer) with coverage-guided mutation. Fuzz targets live in `fuzz/`:

```
fuzz/
├── Cargo.toml
└── fuzz_targets/
    ├── frame_decode.rs           # Decode arbitrary bytes as a frame
    ├── frame_roundtrip.rs        # Encode then decode, assert equivalence
    ├── reorder_buffer.rs         # Random sequence of insert/drain operations
    ├── credit_state_machine.rs   # Random grant/decrement/query sequences
    ├── config_parse.rs           # Parse arbitrary bytes as TOML config
    └── protocol_session.rs       # Simulated session: sequence of frames with state
```

| Fuzz target | Input | What it finds |
|-------------|-------|---------------|
| `frame_decode` | Arbitrary `&[u8]` | Panics, OOB reads, integer overflows in header parsing. The decoder must never crash on any input. |
| `frame_roundtrip` | Structured `FrameHeader` + `&[u8]` payload (via `arbitrary` crate) | Encode/decode asymmetry, data corruption, off-by-one in payload length |
| `reorder_buffer` | Sequence of `(seq: u64, action: Insert|Drain|Timeout)` | Memory exhaustion, incorrect delivery order, missed frames, panic on edge sequences |
| `credit_state_machine` | Sequence of `(action: Grant(u16)|Decrement|Query)` | Underflow, overflow, deadlock state, inconsistent available count |
| `config_parse` | Arbitrary `&[u8]` as TOML | Panics in config validation, nonsensical values accepted without error |
| `protocol_session` | Sequence of `Frame` values simulating a connection lifecycle | State machine violations (FIN before SYN, RST handling, duplicate stream_id), credit desync |

**Running fuzzers**:

```bash
# Run a single target (runs indefinitely until interrupted or crash found)
cargo fuzz run frame_decode

# Run with a time limit (CI-friendly)
cargo fuzz run frame_decode -- -max_total_time=300

# Run with address sanitizer (default in cargo-fuzz)
cargo fuzz run frame_decode  # ASAN is on by default

# Minimize a crash corpus
cargo fuzz tmin frame_decode fuzz/artifacts/frame_decode/<crash-file>

# Coverage report
cargo fuzz coverage frame_decode
```

**Corpus management**: Each target has a seed corpus (`fuzz/corpus/<target>/`) with known-interesting inputs: valid frames of all types, boundary-sized payloads, frames with every flag combination, and previously-found crash inputs. The corpus is checked into git so CI builds on prior fuzzing progress.

### 12.12.2 Property-Based Testing (proptest)

[proptest](https://github.com/proptest-rs/proptest) generates random inputs from a strategy and checks invariants. Complementary to fuzzing: proptest is better for relational properties ("encode then decode equals original"), while fuzzing is better for finding crashes.

```rust
use proptest::prelude::*;

proptest! {
    /// Any valid FrameHeader roundtrips through encode/decode without loss.
    #[test]
    fn frame_header_roundtrip(
        stream_id in any::<u32>(),
        seq in any::<u64>(),
        flags in 0u16..0x3F,  // only valid flag bits
        credits in any::<u16>(),
        payload_len in 0u32..=MAX_PAYLOAD_SIZE as u32,
    ) {
        let header = FrameHeader { stream_id, sequence_number: seq, flags, credits_granted: credits, payload_length: payload_len };
        let payload = vec![0xAB; payload_len as usize];
        let mut buf = vec![0u8; FRAME_HEADER_SIZE + payload.len()];

        encode_frame(&header, &payload, &mut buf).unwrap();
        let (decoded_header, decoded_payload) = decode_frame(&buf).unwrap();

        prop_assert_eq!(header, decoded_header);
        prop_assert_eq!(&payload[..], decoded_payload);
    }

    /// Reorder buffer always delivers frames in sequence order, regardless of insertion order.
    #[test]
    fn reorder_delivers_in_order(
        // Generate a permutation of sequence numbers 0..n
        n in 1usize..500,
        seed in any::<u64>(),
    ) {
        let mut seqs: Vec<u64> = (0..n as u64).collect();
        // Shuffle using seed for reproducibility
        seqs.shuffle(&mut StdRng::seed_from_u64(seed));

        let mut reorder = ReorderBuffer::new();
        let mut delivered = Vec::new();

        for seq in seqs {
            let frame = Frame { sequence_number: seq, /* ... */ };
            for out in reorder.insert(frame) {
                delivered.push(out.sequence_number);
            }
        }
        // Drain remaining
        for out in reorder.drain_ready() {
            delivered.push(out.sequence_number);
        }

        // Must be strictly monotonic
        for w in delivered.windows(2) {
            prop_assert!(w[0] < w[1], "out-of-order delivery: {} before {}", w[0], w[1]);
        }
        prop_assert_eq!(delivered.len(), n, "frame loss: delivered {} of {}", delivered.len(), n);
    }

    /// Credit accounting never goes negative and never exceeds max.
    #[test]
    fn credit_invariants(
        initial in 1u16..=256,
        ops in prop::collection::vec(
            prop_oneof![
                Just(CreditOp::Decrement),
                (1u16..=64).prop_map(CreditOp::Grant),
            ],
            0..1000
        ),
    ) {
        let mut credits = CreditState::new(initial);
        for op in ops {
            match op {
                CreditOp::Decrement => { credits.try_decrement(); }
                CreditOp::Grant(n) => { credits.grant(n); }
            }
            prop_assert!(credits.available() <= credits.max_credits());
            // available is unsigned, so "never negative" is type-enforced
        }
    }
}
```

### 12.12.3 Memory Safety Analysis

| Tool | What it catches | How to run |
|------|----------------|------------|
| **Miri** | Undefined behavior in `unsafe` code: uninitialized reads, invalid pointer arithmetic, aliasing violations, use-after-free | `cargo +nightly miri test` — runs the full unit test suite under Miri's interpreter. Slow (~100× slower) but catches UB that sanitizers miss. Critical for buffer pool and frame codec `unsafe` blocks. |
| **AddressSanitizer** (ASAN) | Heap buffer overflow, use-after-free, double-free, stack buffer overflow, memory leaks | `RUSTFLAGS="-Zsanitizer=address" cargo +nightly test` — on by default in `cargo fuzz`. Run integration tests under ASAN periodically. |
| **MemorySanitizer** (MSAN) | Use of uninitialized memory | `RUSTFLAGS="-Zsanitizer=memory" cargo +nightly test` — critical for verifying buffer zeroing on free. Requires the entire dependency tree to be instrumented (no pre-built std). |
| **ThreadSanitizer** (TSAN) | Data races between threads | `RUSTFLAGS="-Zsanitizer=thread" cargo +nightly test` — run with multi-threaded tests (concurrent buffer pool alloc, CQ poll + send path). |
| **LeakSanitizer** (LSAN) | Memory leaks | Enabled by default with ASAN. Verify buffer pool slots are all returned after connection close. |

**CI integration**: Miri and ASAN run on every PR. MSAN and TSAN run nightly (slower, more setup).

### 12.12.4 Adversarial Protocol Testing

Simulate a malicious or buggy peer sending crafted frames to the proxy. These tests verify that the proxy handles garbage gracefully without crashing, leaking memory, or entering an inconsistent state.

| Attack | Crafted input | Expected behavior |
|--------|--------------|-------------------|
| **Truncated header** | Send 10 bytes (less than 20-byte header) | Discard frame, increment `malformed_frames_total` counter, do not crash |
| **Huge payload_length** | Header claims `payload_length = 0xFFFFFFFF` | Reject frame (exceeds max), do not attempt allocation |
| **Sequence number wraparound** | Send seq `u64::MAX` then seq `0` | Reorder buffer handles correctly (or rejects — no panic) |
| **Duplicate stream_id SYN** | Send SYN for stream_id 5 when stream 5 already exists | Reject or RST, do not corrupt existing stream |
| **FIN then data** | Send FIN for stream, then more data frames on same stream | Discard post-FIN data, do not reopen stream |
| **Credit flood** | Grant `u16::MAX` credits every frame | Credits capped at configured max, no overflow |
| **Zero-length frame storm** | 1M frames with payload_length=0, no flags | Handle without OOM, rate-limit or backpressure |
| **Reserved flags set** | Frames with bits 6-15 set | Reject or ignore reserved bits, log warning |
| **Interleaved RST** | RST mid-transfer on active stream | Clean teardown of stream, no stale state |
| **Connection exhaustion** | Open `max_connections + 1` streams via SYN flood | Reject excess with RST, existing streams unaffected |

These are implemented as integration tests that inject raw frames (bypassing the normal UDS path) directly into the RDMA receive buffer or TCP socket (v0), using a test harness that can craft arbitrary byte sequences.

### 12.12.5 `unsafe` Code Audit

The proxy uses `unsafe` in a limited number of locations. Each `unsafe` block must have a `// SAFETY:` comment justifying correctness. The audit checklist:

| `unsafe` site | Why unsafe is needed | Invariant to verify |
|---------------|---------------------|---------------------|
| Buffer pool: `MR` registration | `ibv_reg_mr` requires raw pointer to contiguous allocation | Pointer valid for lifetime of `BufferPool`, alignment matches ibverbs requirements |
| Buffer pool: slot access | Index into registered buffer by offset | Bounds checked: `offset + slot_size <= mr_length` |
| Frame header: `#[repr(C, packed)]` read/write | Direct cast from `&[u8]` to `&FrameHeader` | Buffer length ≥ `FRAME_HEADER_SIZE`, alignment handled by packed repr |
| io_uring: buffer registration | `IORING_REGISTER_BUFFERS` requires stable pointers | Buffers are `mlock`'d and never reallocated after registration |
| CQ polling: work completion access | `ibv_poll_cq` writes into caller-provided array | Array length matches `max_cq_entries`, completions validated before use |

**Tooling**:
- `cargo geiger` — counts `unsafe` usage across the dependency tree; track over time, flag unexpected increases
- `cargo audit` — checks dependencies for known vulnerabilities (run in CI on every PR)
- `cargo deny` — license and advisory checks

### 12.12.6 Stress and Chaos Testing

Long-running tests designed to surface race conditions, memory leaks, and resource exhaustion that only manifest under sustained load.

| Test | Duration | Configuration | What it catches |
|------|----------|--------------|-----------------|
| **72-hour soak test** | 72h | 16 connections, 4KB messages, steady 50% throughput | Slow memory leaks, file descriptor leaks, metric counter overflow, buffer pool fragmentation |
| **Connection churn** | 8h | Open/close 100 connections per second | Connection table leaks, stream_id exhaustion (u32 wrap), cleanup race conditions |
| **OOM pressure** | 1h | Reduce buffer pool to minimum, 64 connections at full rate | Graceful degradation under memory pressure, no panic or corruption |
| **CPU starvation** | 4h | Pin proxy to 1 CPU core, 32 connections | Starvation between CQ poll, io_uring, and async runtime; verify progress |
| **Random kill/restart** | 4h | Kill and restart one proxy every 30s during active transfer | Reconnection logic, no orphaned resources, peer detects disconnect promptly |
| **Concurrent fuzzing + load** | 2h | Fuzz one UDS connection while others run production-like load | Verify that a malicious connection doesn't impact well-behaved connections |


[Back to Design Overview](../DESIGN.md)
