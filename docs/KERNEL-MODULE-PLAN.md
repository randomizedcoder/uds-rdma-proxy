# Kernel Module Implementation Plan

This plan covers the full implementation of the UDS-RDMA proxy kernel module (`urp`), the `urp` CLI tool, and the shared protocol crate -- from first line of code through optimized production readiness.

The kernel module eliminates 2 of the 4 memory copies in the userspace proxy by intercepting UDS socket operations directly in kernel space, forwarding data over RDMA via in-kernel verbs without crossing the user/kernel boundary on each hop ([Section 21](design/21-kernel-module.md)). It uses Approach D (Virtual UDS Endpoint): the module creates a real UDS socket file, applications connect normally, and custom `proto_ops` route traffic to RDMA transparently.

## Reference Documents

| Doc | Covers |
|-----|--------|
| [21 - Kernel Module](design/21-kernel-module.md) | Architecture, copy analysis, socket interception, buffer management, pump loop, phased roadmap (k0/k1/k2) |
| [22 - GENL Interface](design/22-genl-interface.md) | GENL family `"urp"`, 4-command model, attribute hierarchy, UAPI header, kernel handlers |
| [23 - urp CLI Tool](design/23-cli-tool.md) | Rust CLI (neli + clap), subcommands, output formats, systemd deployment |
| [04 - Framing Protocol](design/04-framing-protocol.md) | 20-byte wire format, frame types (DATA/CONTROL/PROBE), flags, credit piggybacking |
| [05 - RDMA Transport](design/05-rdma-transport.md) | rdma_cm setup, QP config, SRQ, credit-based flow control, buffer pool, CQ polling |
| [07 - Bidirectional Pump](design/07-bidirectional-pump.md) | Per-connection task model, half-close, error propagation, backpressure |
| [08 - Multi-QP ECMP](design/08-multi-qp-ecmp.md) | QP selection strategies, reorder buffer (B-tree), ECMP path diversity |
| [08a - QP Health Probes](design/08a-qp-health-probes.md) | PING/PONG protocol, RTT measurement, QP health state machine |
| [09 - Connection Multiplexing](design/09-connection-multiplexing.md) | Stream lifecycle (SYN/FIN/RST), per-stream sequences, two-layer flow control |
| [12 - Testing Strategy](design/12-testing.md) | Unit tests, integration tests, fuzzing, sanitizers, stress/chaos, MicroVM tests |
| [17 - Security](design/17-security.md) | Tier 0.5 PSK (SHA-256 in rdma_cm private_data), auth flow |
| [19 - Project Structure](design/19-project-structure.md) | Nix flake, MicroVM infrastructure, local rdma_rxe development |

---

## Table of Contents

| # | Phase | Summary |
|---|-------|---------|
| 0 | [Prerequisites](#phase-0-prerequisites) | Extract shared crate, Kbuild integration, KUnit in MicroVMs |
| 1 | [k0 -- Proof of Concept](#phase-1-k0----proof-of-concept) | Minimal C module, single QP, virtual UDS endpoint, basic pump, latency measurement |
| 2 | [urp CLI + GENL](#phase-2-urp-cli--genl-interface) | UAPI header, GENL handlers in kernel, Rust CLI tool, endpoint CRUD |
| 3 | [k1 -- Functional](#phase-3-k1----functional) | Multi-QP, reorder buffer, credit flow control, stream multiplexing, PSK auth, QP health probes |
| 4 | [k2 -- Optimized](#phase-4-k2----optimized) | page_pool, zero-copy send, adaptive CQ polling, NUMA-aware allocation |
| 5 | [MicroVM Integration](#phase-5-microvm-integration) | Cross-architecture VM pair tests, Redpanda cluster compatibility, CI pipeline |

---

## Development Strategy

**Local first, MicroVMs second.** Every phase begins with local development using network namespaces + `rdma_rxe` (seconds per test cycle), then graduates to MicroVM pair tests for kernel isolation and cross-architecture validation.

```
Local loop (fast):   edit -> make -> insmod -> setup-rxe -> test -> rmmod
                     ~30s cycle, same host kernel, two namespaces

MicroVM loop (thorough):  nix build -> boot VM pair -> load module -> test -> collect
                          ~2-5min (x86_64 KVM), full kernel isolation
```

---

## Phase 0: Prerequisites

**Goal**: Establish the build foundation so Phase 1 can start writing kernel code immediately.

### 0.1 Extract `uds-rdma-protocol` Crate

Extract protocol logic from the userspace proxy design into a standalone crate that compiles for both `std` (userspace) and `no_std + alloc` (kernel).

**Files**: `crates/uds-rdma-protocol/src/lib.rs`

**Components to extract** (ref: [Section 21.7](design/21-kernel-module.md)):
- `FrameHeader` (`#[repr(C, packed)]`, 20 bytes) -- encode/decode
- Frame type constants: `DATA` (0x00), `CONTROL` (0x01), `PROBE` (0x02)
- Per-type flag constants: `DATA_FLAG_SYN/FIN/RST`, `CTRL_FLAG_CREDIT/QP_DISABLE/STREAM_WINDOW/AUTH`, `PROBE_FLAG_PONG`
- `CreditState` -- `can_send()`, `consume()`, `grant()`, `pending_grants()`
- `ReorderBuffer` -- `BTreeMap<u64, Vec<u8>>` with `insert()`, `drain()`, `gap_count()`
- `PingPayload` / `PongPayload` (32B / 48B, ref: [Section 8a](design/08a-qp-health-probes.md))
- QP selection functions: `qp_select_round_robin()`, `qp_select_hash()`
- MTU/payload sizing: `max_payload_for_mtu()`
- Protocol constants: `FRAME_HEADER_SIZE`, `ROCEV2_HEADER_OVERHEAD`

**Testing (Phase 0)**:
- `cargo test` -- roundtrip encode/decode, credit state transitions, reorder sequences
- `cargo test --no-default-features` -- verify `no_std + alloc` compiles
- `cargo +nightly miri test` -- catch UB in `#[repr(C, packed)]` operations
- Fuzz targets: `frame_decode`, `frame_roundtrip`, `reorder_buffer`, `credit_state_machine`

### 0.2 Kbuild Integration

Create the kernel module build system and integrate it with the Nix flake.

**Files to create**:
- `kernel/Kbuild` -- module build rules (`obj-m := urp.o`, `urp-objs := urp_main.o ...`)
- `kernel/Makefile` -- convenience wrapper (`make -C /lib/modules/$(uname -r)/build M=$(pwd) modules`)
- `nix/kernel-module.nix` -- Nix derivation for building the module against a pinned kernel

**Shell function** (add to `nix/shell-functions/build.nix`):
```bash
build-kmod() {
    make -C kernel M=$(pwd)/kernel modules
}
load-kmod() {
    sudo insmod kernel/urp.ko
}
unload-kmod() {
    sudo rmmod urp 2>/dev/null || true
}
```

### 0.3 KUnit Infrastructure

Configure MicroVM kernels with `CONFIG_KUNIT=y` and validate that KUnit tests run inside MicroVMs.

**Files**: update `nix/microvms/mkVm.nix` kernel config:
```nix
boot.kernelPatches = [{
    extraStructuredConfig = with lib.kernel; {
        KUNIT = yes;
        KUNIT_ALL_TESTS = no;  # only run our tests
    };
}];
```

### Definition of Done -- Phase 0

- [ ] `uds-rdma-protocol` crate compiles with `--no-default-features` (no_std + alloc)
- [ ] `cargo test` passes: frame roundtrip, credit state, reorder buffer (20+ test cases)
- [ ] `cargo +nightly miri test` passes (no UB)
- [ ] `cargo fuzz run frame_decode` runs for 60s with no crashes
- [ ] `make -C kernel` produces `urp.ko` (empty module: init prints, exit prints)
- [ ] `sudo insmod kernel/urp.ko && sudo rmmod urp` succeeds on host
- [ ] MicroVM kernel boots with `CONFIG_KUNIT=y`

---

## Phase 1: k0 -- Proof of Concept

**Goal**: Validate that eliminating UDS copies delivers measurable latency improvement. Build the minimum viable kernel module: one endpoint, one QP, one connection, basic send/recv pump. Decision gate at the end.

**Scope**: ~1,500 lines C (ref: [Section 21.12 k0](design/21-kernel-module.md))

### 1.1 Kernel Source Files

| File | Lines (est.) | Purpose |
|------|-------------|---------|
| `kernel/urp_main.c` | ~100 | `module_init`/`module_exit`, `module_param` for socket path + peer address |
| `kernel/urp_socket.c` | ~250 | Virtual UDS endpoint: `sock_create_kern()`, `kernel_bind()`, `kernel_listen()`, accept loop kthread, custom `proto_ops` |
| `kernel/urp_rdma.c` | ~400 | RDMA CM connection: `rdma_create_id()`, `rdma_resolve_addr()`, `rdma_connect()`, QP creation (`ib_create_qp`), buffer allocation (`alloc_pages` + `ib_dma_map_page`), simple free list (`list_head` + `spinlock_t`) |
| `kernel/urp_pump.c` | ~300 | TX kthread: `kernel_recvmsg()` -> frame encode -> `ib_post_send()`. RX: CQ completion -> frame decode -> `kernel_sendmsg()`. No reorder, no credits -- just raw send/recv |
| `kernel/urp_proc.c` | ~100 | `/proc/urp/stats`: tx_bytes, rx_bytes, tx_frames, rx_frames, connections |
| `kernel/include/uapi/linux/urp.h` | ~50 | Minimal UAPI (module_param only at this stage; full GENL UAPI comes in Phase 2) |

### 1.2 Data Path (k0, simplified)

```
App write(fd, buf, len)
  -> urp_sendmsg(sock, msg, len)
    -> copy_from_iter(dma_buf + 20, len, &msg->msg_iter)     [copy 1]
    -> urp_frame_encode(dma_buf, 0, seq++, 0, 0, len)        [shared crate FFI]
    -> ib_post_send(qp, &wr)
    -> NIC DMA                                                [0 copies]
    -> ib_poll_cq(cq) on remote
    -> urp_frame_decode(dma_buf)                              [shared crate FFI]
    -> copy_to_iter(dma_buf + 20, payload_len, &msg->msg_iter) [copy 2]
  -> App read(fd, buf, len) on remote
```

### 1.3 Module Configuration (k0 only)

```bash
# Load with hardcoded single endpoint
sudo insmod urp.ko \
    listen_path="/var/run/urp/test.sock" \
    peer_address="10.0.99.2" \
    peer_port=4791

# Or for the acceptor side:
sudo insmod urp.ko \
    connect_path="/tmp/target.sock" \
    bind_port=4791
```

### 1.4 Local Testing (k0)

All tests run in network namespaces with `rdma_rxe` on the host:

```bash
# Terminal 1: Load acceptor
setup-rxe
sudo ip netns exec ns_rdma_b insmod kernel/urp.ko \
    connect_path="/tmp/echo.sock" bind_port=4791

# Terminal 2: Start echo server on UDS (the "application")
sudo ip netns exec ns_rdma_b socat UNIX-LISTEN:/tmp/echo.sock,fork EXEC:cat

# Terminal 3: Load initiator
sudo ip netns exec ns_rdma_a insmod kernel/urp.ko \
    listen_path="/tmp/proxy.sock" peer_address="10.0.99.2" peer_port=4791

# Terminal 4: Test
echo "hello RDMA kernel" | sudo ip netns exec ns_rdma_a \
    socat - UNIX-CONNECT:/tmp/proxy.sock
```

**Automated test script** (`scripts/test-kmod-k0.sh`):
- Basic echo: send N bytes, verify exact match
- Throughput: send 100MB, measure time, compute MB/s
- Latency: 1000x 64-byte echo roundtrips, compute p50/p99
- Comparison: run same tests with userspace proxy, compute ratio

**KUnit tests** (`kernel/urp_test.c`):
- Frame encode/decode roundtrip (call shared crate FFI)
- Buffer pool alloc/free cycle (verify no leaks via counter)
- DMA mapping lifecycle (alloc -> map -> use -> unmap -> free)

**Kernel sanitizers** (run in MicroVM with debug kernel):
- `CONFIG_KASAN=y` -- heap buffer overflow, use-after-free
- `CONFIG_KMEMLEAK=y` -- memory leak detection
- `CONFIG_LOCKDEP=y` -- lock ordering validation

### 1.5 Latency Measurement

The primary k0 deliverable is a latency comparison:

| Test | Metric | Method |
|------|--------|--------|
| Kernel module RTT | p50, p99 (ns) | 64B echo via UDS through kernel module over rdma_rxe |
| Userspace proxy RTT | p50, p99 (ns) | Same test through userspace proxy |
| Raw RDMA RTT | p50, p99 (ns) | `ib_send_lat -d rxe_a` baseline |

### Definition of Done -- Phase 1 (k0)

- [ ] Module loads, creates UDS socket, accepts connections, pumps data over RDMA
- [ ] Echo test: 1000 roundtrips, all data matches
- [ ] Throughput test: sustained 100MB transfer, no hangs or crashes
- [ ] `/proc/urp/stats` shows correct byte/frame counters
- [ ] KUnit tests pass: frame codec, buffer lifecycle
- [ ] KASAN clean: no memory errors during test suite
- [ ] KMEMLEAK clean: no leaks after `rmmod`
- [ ] Latency comparison measured and documented
- [ ] **Decision gate**: If latency improvement < 15% vs userspace v2, revisit whether kernel module path is justified

---

## Phase 2: urp CLI + GENL Interface

**Goal**: Replace `module_param` with proper GENL-based configuration. Build the `urp` CLI tool so we can manage multiple endpoints dynamically. This is a prerequisite for Phase 3 (k1) where we need multi-endpoint support.

### 2.1 UAPI Header

**File**: `kernel/include/uapi/linux/urp.h` (ref: [Section 22.5](design/22-genl-interface.md))

Full UAPI with all enums:
- Commands: `URP_CMD_NEW_ENDPOINT` (1), `DEL` (2), `SET` (3), `GET` (4)
- Attribute sets: `urp_attr`, `urp_endpoint_attr`, `urp_qp_attr`, `urp_stream_attr`, `urp_stats_attr`
- State enums: `urp_endpoint_state`, `urp_qp_state`, `urp_stream_state`
- Constants: `URP_GENL_NAME "urp"`, `URP_GENL_VERSION 1`

### 2.2 Kernel GENL Handlers

**File**: `kernel/urp_netlink.c` (~400-600 lines, ref: [Section 22.8](design/22-genl-interface.md))

- `genl_family` registration with `genl_split_ops` (5 entries: NEW doit, DEL doit, SET doit, GET doit, GET dumpit)
- `nla_policy` arrays for each attribute level (endpoint, qp, stream, stats)
- `urp_new_endpoint()` -- parse nested attrs, validate, allocate `struct urp_endpoint`, start RDMA CM + UDS socket, insert into rhashtable
- `urp_del_endpoint()` -- lookup by name, drain, tear down, remove from rhashtable
- `urp_set_endpoint()` -- modify mutable attributes (num_qps, buffer_count, password)
- `urp_get_endpoint()` -- serialize config + state + stats into reply
- `urp_dump_endpoints()` -- iterate rhashtable, emit one NLMSG per endpoint
- `urp_send_event()` -- multicast notification on state changes
- `urp_fill_endpoint()` -- shared serialization for GET/dump/events

**File**: `kernel/urp_endpoint.c` (~300 lines)

- `struct urp_endpoint` -- name, paths, RDMA config, state, QP array, connection table, stats counters
- `urp_endpoint_create()` -- allocate and initialize from GENL attributes
- `urp_endpoint_activate()` -- async: start RDMA CM, create UDS socket, transition CREATING -> ACTIVE
- `urp_endpoint_drain()` -- FIN all streams, transition ACTIVE -> DRAINING
- `urp_endpoint_destroy()` -- free all resources, remove UDS socket file
- Global endpoint table: `rhashtable` keyed by name

**Update**: `kernel/urp_main.c` -- remove `module_param`, add `genl_register_family()` / `genl_unregister_family()`

### 2.3 urp CLI Tool (Rust)

**Crate**: `crates/urp-cli/` (ref: [Section 23](design/23-cli-tool.md))

**Files**:
```
crates/urp-cli/
  Cargo.toml          # clap, neli, neli-proc-macros, serde, serde_json
  src/
    main.rs           # clap derive with 7 subcommands
    netlink.rs        # UrpSocket: connect, resolve family, send/recv, check_ack
    commands/
      mod.rs
      add.rs          # Build NEW_ENDPOINT GENL message from CLI args
      remove.rs       # Build DEL_ENDPOINT
      set.rs          # Build SET_ENDPOINT
      show.rs         # Parse GET_ENDPOINT reply, format output
      stats.rs        # Parse GET_ENDPOINT (stats subset)
      monitor.rs      # Subscribe to multicast "events" group, print loop
      drain.rs        # Build SET_ENDPOINT with state=DRAINING
    format.rs         # Human-readable, JSON, oneline output formatters
    uapi.rs           # neli_enum derives mirroring UAPI constants
```

**Dependencies**: `neli 0.7`, `neli-proc-macros 0.2`, `clap 4` (derive), `serde 1`, `serde_json 1`

### 2.4 Testing (Phase 2)

**urp CLI unit tests** (`cargo test -p urp-cli`):
- UAPI constant values match C header (parse the header, compare)
- Attribute encoding: build a NEW_ENDPOINT message, decode it, verify fields
- Argument validation: missing required args -> error, out-of-range num_qps -> error
- Output formatting: known endpoint struct -> human/JSON/oneline strings match expected

**Integration tests** (local, rdma_rxe):
```bash
# Load module (no module_param now -- starts empty)
sudo insmod kernel/urp.ko

# Create endpoint via urp CLI
urp add test --listen-path /tmp/test.sock --peer-address 10.0.99.2:4791

# Verify
urp show test        # state should be "creating" then "active"
urp show --json      # verify JSON structure

# Basic data path test (socat echo)
echo "hello" | socat - UNIX-CONNECT:/tmp/test.sock

# Stats
urp stats test       # verify counters increment

# Drain and remove
urp drain test
urp show test        # state should be "draining" -> "stopped"
urp remove test
urp show             # empty list

# Error cases
urp add test --num-qps 99   # should fail (max 32)
urp remove nonexistent       # should fail (ENOENT)
urp add dup --listen-path /tmp/a.sock --peer-address 10.0.99.2:4791
urp add dup --listen-path /tmp/b.sock --peer-address 10.0.99.2:4791  # should fail (EEXIST)
```

**Monitor test**:
```bash
urp monitor &
MONITOR_PID=$!
urp add test --listen-path /tmp/test.sock --peer-address 10.0.99.2:4791
# Verify monitor prints "test: state creating -> active"
urp drain test
# Verify monitor prints "test: state active -> draining"
kill $MONITOR_PID
```

### Definition of Done -- Phase 2

- [ ] `urp add/remove/show/stats/drain/set/monitor` all work end-to-end
- [ ] `urp show --json` produces valid, parseable JSON with all fields
- [ ] Multiple endpoints can be created and managed simultaneously
- [ ] Multicast events fire on state transitions (verified via `urp monitor`)
- [ ] Error cases return meaningful messages (EEXIST, ENOENT, EINVAL)
- [ ] `urp add` with no module loaded -> "urp kernel module not loaded" error
- [ ] Module unload with active endpoints -> all endpoints drained and cleaned up
- [ ] `cargo test -p urp-cli` passes (unit tests for encoding, formatting, validation)
- [ ] KASAN/KMEMLEAK clean through full CLI exercise cycle

---

## Phase 3: k1 -- Functional

**Goal**: Feature parity with the userspace v2 proxy design. The kernel module becomes a real, usable product after this phase.

### 3.1 Shared Protocol Integration

Link the Rust `uds-rdma-protocol` crate as a static `no_std` library into the kernel module via C FFI (ref: [Section 21.6](design/21-kernel-module.md)).

**C FFI boundary** (in `kernel/urp_protocol.h`):
```c
/* Functions exported by Rust static lib */
extern int urp_frame_encode(uint8_t *buf, uint32_t stream_id, uint64_t seq,
                            uint8_t frame_type, uint8_t flags,
                            uint16_t credits, uint32_t payload_len);
extern int urp_frame_decode(const uint8_t *buf, struct urp_frame_header *out);
extern void *urp_credit_state_new(uint16_t initial_credits);
extern bool urp_credit_can_send(void *state);
extern int  urp_credit_consume(void *state);
extern void urp_credit_grant(void *state, uint16_t n);
extern void urp_credit_free(void *state);
extern void *urp_reorder_new(uint64_t expected_seq, size_t max_buffered);
extern int  urp_reorder_insert(void *buf, uint64_t seq, const uint8_t *data, size_t len);
extern size_t urp_reorder_drain(void *buf, uint8_t **out_data, size_t *out_len);
extern void urp_reorder_free(void *buf);
```

**Build**: Compile Rust crate as `staticlib` (`crate-type = ["staticlib"]`), link `liburp_protocol.a` into the kernel module via Kbuild.

### 3.2 Multi-QP with Reorder Buffer

Ref: [Section 8](design/08-multi-qp-ecmp.md)

- Configurable 1-32 QPs per endpoint (via `urp add --num-qps N` or `urp set --num-qps N`)
- QP selection: round-robin (k1 initial), adaptive weighted (k1 later)
- Per-direction global sequence numbers (per-stream in future)
- Reorder buffer: use shared crate's `ReorderBuffer` (BTreeMap) via FFI, or kernel `rbtree`
- Gap timeout: 100ms (RC guarantees delivery, gap = serious error)
- Frames delivered to UDS socket strictly in sequence order

### 3.3 Credit-Based Flow Control

Ref: [Section 5](design/05-rdma-transport.md)

- Per-QP credit tracking via shared crate's `CreditState`
- Sender: decrement on post_send, wait (`wait_event_interruptible`) if zero
- Receiver: grant when `pending >= threshold` (N/4), piggyback on DATA frames or send standalone CREDIT CONTROL frame
- Reserve 1 buffer for credit-only messages (deadlock avoidance)
- Credit stall counter in endpoint stats (`URP_STATS_A_CREDIT_STALLS`)

### 3.4 Stream Multiplexing

Ref: [Section 9](design/09-connection-multiplexing.md)

- `stream_id` allocation: RDMA initiator = odd (1,3,5...), acceptor = even (2,4,6...)
- `stream_id = 0` reserved for control channel
- Stream lifecycle: SYN -> ESTABLISHED -> FIN/RST -> CLOSED (via DATA frame flags)
- Per-stream `urp_connection` entries in endpoint's rhashtable
- Each accepted UDS connection gets a unique stream_id
- SYN carries first payload (no extra RTT)
- Half-close: FIN in one direction, other direction continues

### 3.5 Shared Receive Queue (SRQ)

Ref: [Section 5.3](design/05-rdma-transport.md)

- One SRQ per endpoint, shared across all QPs
- Prevents per-QP receive queue starvation
- Pre-post buffers from the endpoint's buffer pool
- Watermark refill: when posted < low_watermark, refill to high_watermark

### 3.6 QP Health Probes

Ref: [Section 8a](design/08a-qp-health-probes.md)

- PROBE frames (type 0x02) with PING/PONG payloads (32B/48B) via shared crate
- Per-QP probe state: `probe_seq`, `consecutive_misses`, RTT EWMA (alpha=0.2)
- Probe timer: `hrtimer_start()` at 250ms operational interval (50ms during qualifying)
- QP health state machine: Qualifying -> Active -> Draining -> Removed
- Qualifying criteria: >=3 consecutive probes, all RTT < 1000us, no misses
- QP_DISABLE control frame notifies peer of state transition
- Health state exposed in `urp show` per-QP output

### 3.7 Tier 0.5 PSK Authentication

Ref: [Section 17](design/17-security.md)

- `auth_method` (1B) + `auth_hash` (32B SHA-256) in rdma_cm `private_data` (49B total, << 196B limit)
- RDMA initiator: compute SHA-256(password), include in `rdma_connect()` private_data
- RDMA acceptor: compare hash against own SHA-256(password), `rdma_reject()` on mismatch
- Bidirectional verification: acceptor also sends its hash in `rdma_accept()` private_data
- Password stored as SHA-256 hash in `struct urp_endpoint` (never plaintext after `urp add`)
- Auth failure counter in endpoint stats (`URP_STATS_A_AUTH_FAILURES`)
- Auth failure events via GENL multicast

### 3.8 Testing (Phase 3)

**KUnit tests** (`kernel/urp_test.c`, expanded):

| Test | Category | Description |
|------|----------|-------------|
| `test_frame_roundtrip_all_types` | Frame codec | Encode/decode DATA, CONTROL, PROBE with all flag combinations |
| `test_frame_max_payload` | Frame codec | payload_length at PMTU boundary |
| `test_frame_zero_payload` | Frame codec | Control/probe frames with no payload |
| `test_credit_initial` | Flow control | Initial credit count correct after setup |
| `test_credit_exhaust_and_grant` | Flow control | Consume all -> can_send() false -> grant -> can_send() true |
| `test_credit_threshold_batching` | Flow control | Grants only fire at N/4 threshold |
| `test_reorder_in_order` | Reorder | Sequential insert -> immediate drain |
| `test_reorder_out_of_order` | Reorder | Insert seq 3,1,2 -> drain delivers 1,2,3 |
| `test_reorder_duplicate` | Reorder | Duplicate seq rejected |
| `test_reorder_gap_timeout` | Reorder | Missing seq -> timeout -> skip |
| `test_reorder_max_buffered` | Reorder | Buffer full -> reject new inserts |
| `test_buffer_pool_alloc_free` | Buffers | Alloc all slots, free all, alloc again |
| `test_buffer_pool_exhaustion` | Buffers | Pool full -> alloc returns NULL |
| `test_endpoint_state_machine` | Lifecycle | CREATING -> ACTIVE -> DRAINING -> STOPPED transitions |
| `test_stream_id_allocation` | Streams | Initiator gets odd, acceptor gets even |
| `test_psk_hash_match` | Security | Matching passwords -> SHA-256 hashes match |
| `test_psk_hash_mismatch` | Security | Different passwords -> hashes differ |

**Integration tests** (local, rdma_rxe):

| Test | Description | Validates |
|------|-------------|-----------|
| `test_basic_echo` | 1000x 64B echo roundtrips | Basic data path |
| `test_large_transfer` | 1GB unidirectional transfer | Sustained throughput, no leaks |
| `test_bidirectional` | Simultaneous 100MB both directions | Full-duplex pump |
| `test_multi_connection` | 100 concurrent UDS connections | Stream multiplexing, connection table |
| `test_half_close` | Writer closes, reader continues | FIN propagation, half-close |
| `test_abrupt_close` | Kill sender mid-transfer | RST, cleanup, no leaks |
| `test_multi_qp_reorder` | 8 QPs, verify in-order delivery | Reorder buffer correctness |
| `test_credit_backpressure` | Fast sender, slow receiver | Credit exhaustion, graceful stall |
| `test_buffer_exhaustion` | More sends than buffer slots | Backpressure, no crash |
| `test_psk_auth_success` | Matching passwords both sides | Auth flow works |
| `test_psk_auth_failure` | Mismatched passwords | Connection rejected, auth_failures counter increments |
| `test_psk_no_auth` | No password configured | Connection succeeds without auth |
| `test_endpoint_drain` | `urp drain` during active transfer | Graceful shutdown, no data loss |
| `test_module_unload` | `rmmod` with active endpoints | Clean teardown, no leaks |
| `test_qp_health_probes` | Verify PING/PONG exchange | RTT measurement, QP qualifying |

**Stress tests** (local, extended runs):

| Test | Duration | Description |
|------|----------|-------------|
| `soak_16conn_4k` | 1 hour | 16 connections, 4KB messages, 50% throughput |
| `connection_churn` | 30 min | 100 open/close per second |
| `oom_pressure` | 15 min | Minimal buffer pool, 64 connections at full rate |

**Kernel sanitizers** (MicroVM with debug kernel):
- KASAN: heap/stack overflow, use-after-free, slab-out-of-bounds
- KMEMLEAK: scan for leaks after each test, after `rmmod`
- KCSAN: data race detection (RCU, spinlock, atomic correctness)
- LOCKDEP: lock ordering, potential deadlocks (credit grant while holding connection lock, etc.)

### Definition of Done -- Phase 3 (k1)

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

---

## Phase 4: k2 -- Optimized

**Goal**: Exploit kernel-only optimizations that userspace cannot achieve. Performance should approach or exceed hardware NIC driver patterns.

### 4.1 page_pool Buffer Management

Ref: [Section 21.9](design/21-kernel-module.md)

Replace the Phase 1/3 `list_head + spinlock_t` free list with the kernel's `page_pool` API:
- `page_pool_create()` with `PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV`
- DMA mappings persist across recycles (no per-cycle `ib_dma_map_page`)
- Per-CPU caching (lock-free allocation on hot path)
- `page_pool_dev_alloc_pages()` / `page_pool_put_page()` lifecycle
- Validate `page_pool` works with RDMA's `ib_dma_map_page` (primary k2 engineering risk)

### 4.2 Zero-Copy Send

Ref: [Section 21.5](design/21-kernel-module.md)

- `get_user_pages_fast()` to pin application's pages
- `ib_dma_map_page()` directly on user page (no `copy_from_iter`)
- Track pinned pages, unpin in CQ send completion handler
- Constraint: page must remain pinned until CQE arrives

### 4.3 Adaptive CQ Polling

Ref: [Section 21.9 NAPI](design/21-kernel-module.md)

NAPI-inspired adaptive polling:
- Low load: event-driven (`ib_req_notify_cq` + `comp_handler`)
- High load: switch to busy-poll when > 1000 CQE/sec
- Budget per poll cycle (default 64, matching NAPI)
- Re-arm CQ notification when poll comes up empty

### 4.4 NUMA-Aware Allocation

- `page_pool` created with `nid` affinity matching RDMA device's NUMA node
- `kthread_bind()` TX/RX threads to same NUMA node
- Validate with NUMA negative affinity test (buffers on node 0, NIC on node 1)

### 4.5 Testing (Phase 4)

**Performance benchmarks** (local rdma_rxe, then hardware RDMA):

| Benchmark | Metric | Method |
|-----------|--------|--------|
| Latency (64B echo) | p50, p99 ns | 1 connection, 10K roundtrips |
| Throughput (4KB bulk) | GB/s | 16 connections, sustained 60s |
| Small message storm | messages/sec | 256 connections, 64B, 60s |
| page_pool vs free list | alloc/free ns | Micro-benchmark, 1M cycles |
| Zero-copy vs copy | latency delta | 4KB page-aligned writes, compare with/without zero-copy |
| NUMA cross-node penalty | latency increase | Same test, buffers cross-NUMA |

**Regression tests**: all Phase 3 tests must still pass after optimization changes.

**Hardware RDMA testing** (if ConnectX-6 or equivalent available):
- Same test suite on real hardware
- Compare rdma_rxe vs hardware latency/throughput
- Verify no software assumptions break on hardware (inline data, signaled completions, etc.)

### Definition of Done -- Phase 4 (k2)

- [ ] page_pool integrated: allocation uses `page_pool_dev_alloc_pages()`, recycle uses `page_pool_put_page()`
- [ ] Zero-copy send: page-aligned 4KB writes bypass `copy_from_iter` (verified via stats counter)
- [ ] Adaptive CQ polling: transitions between event-driven and busy-poll (verified via tracepoints)
- [ ] NUMA: buffer allocation and kthreads on same NUMA node as RDMA device
- [ ] All Phase 3 tests still pass (regression)
- [ ] Performance benchmarks documented: latency p50/p99, throughput, message rate
- [ ] page_pool recycle rate > 95% under sustained load (verify via page_pool stats)
- [ ] Hardware RDMA tests pass (if hardware available)

---

## Phase 5: MicroVM Integration

**Goal**: Graduate from local namespace testing to full kernel-isolated MicroVM pair testing. Validate cross-architecture support and build the CI pipeline.

### 5.1 MicroVM Infrastructure Updates

**Existing** (from design docs, needs implementation):
- `nix/microvms/mkVm.nix` -- builds minimal NixOS VM with proxy + rdma_rxe
- `nix/microvms/mkVmPair.nix` -- orchestrates listener + connector pair
- `nix/microvms/constants.nix` -- per-arch config, timeouts

**New for kernel module**:
- Add `urp.ko` kernel module to VM image
- Add `urp` CLI binary to VM image
- Update `systemd.services.rdma-setup` to also `insmod urp.ko`
- Add `systemd.services.urp-endpoints` to configure endpoints via `urp add` after RDMA setup
- Create kernel module-specific test scripts for expect-based orchestration

### 5.2 MicroVM Test Suites

**VM pair test** (`nix run .#microvms.test-kmod-x86_64`):

```
Phase 1: Boot acceptor VM, configure rdma_rxe, load urp.ko
Phase 2: Boot initiator VM, configure rdma_rxe, load urp.ko
Phase 3: On acceptor: urp add server --connect-path /tmp/app.sock --bind-address 0.0.0.0:4791
         On initiator: urp add client --listen-path /tmp/proxy.sock --peer-address 10.0.99.2:4791
Phase 4: Verify RDMA connection established (urp show -> state: active)
Phase 5: Run test suite:
         - Echo test (socat)
         - Bulk transfer (uds-rdma-bench producer/consumer)
         - Multi-connection (64 concurrent)
         - PSK auth test (matching + mismatching passwords)
Phase 6: urp drain/remove on both sides
Phase 7: rmmod urp on both VMs
Phase 8: Verify KMEMLEAK clean, collect /proc/urp/stats
Phase 9: Shutdown VMs, collect results
```

**Cross-architecture matrix**:

| Architecture | Emulation | Expected Duration | Priority |
|-------------|-----------|-------------------|----------|
| x86_64 | KVM (native) | ~3 min | P0 -- every commit |
| aarch64 | QEMU TCG | ~8 min | P1 -- nightly |
| riscv64 | QEMU TCG | ~15 min | P2 -- weekly |

### 5.3 Redpanda Cluster Compatibility

Ref: [Section 12](design/12-testing.md)

> **Spec correction (Phase 5, Track C).** The original sketch below was
> wrong in three concrete ways, found while scoping the harness:
> 1. **Redpanda has no UDS listener.** Its Seastar RPC / Kafka API bind
>    `{address, port}` TCP endpoints only; you cannot point `--connect-path`
>    at a Redpanda RPC socket. Every Redpanda hop must go
>    `redpanda(TCP) -> socat TCP↔UDS -> urp UDS↔RDMA -> ... -> socat -> redpanda(TCP)`.
> 2. **`urp` endpoints are unidirectional.** `urp add` takes *either*
>    `--listen-path` (initiator) *or* `--connect-path` (acceptor) --
>    `crates/urp-cli/src/commands/add.rs` marks them `conflicts_with`. The
>    single-`urp add`-with-both example is invalid; bidirectional inter-broker
>    RPC needs **two** urp endpoints per node pair (one per direction), each
>    on its own RDMA bind port.
> 3. **The broker is not in nixpkgs.** Only `redpanda-client` (= `rpk`,
>    unfree) is packaged; the C++/Seastar broker must be vendored (Docker
>    image / DEB) -- the hermeticity gate for this deliverable.
>
> Corrected staged plan (produce/consume through the kernel-module proxy):
>
> - **Stage 0** -- vendor the broker; prove `rpk redpanda start --mode
>   dev-container` reaches `rpk cluster health` OK in one microvm.
> - **Stage A (the concrete DoD item)** -- single broker on vm2, its Kafka
>   API bridged to vm1 over ONE urp tunnel via socat shims; `rpk topic
>   create/produce/consume` from vm1 round-trips with zero loss. Reuses the
>   existing 2-VM pair harness (swap the echo backend for the socat↔TCP
>   shim + broker).
> - **Stage B (follow-on, self-hosted/weekly)** -- true 3-node mesh: vm3,
>   a QEMU multicast socket-netdev hub, per-node virtual advertised-RPC IPs,
>   and 2 urp endpoints + 2 socats per direction per peer.

Original sketch (kept for reference; the `urp add` invocation is **invalid**
per correction #2 above):

```
Node A: urp add peer-b --listen-path /var/run/urp/to-B.sock \
                        --connect-path /var/run/redpanda/rpc.sock \
                        --peer-address 10.0.1.2:4791 --num-qps 8
```

Tests:
- `rpk topic create` / `rpk topic produce` / `rpk topic consume`
- `rpk cluster health`
- Node restart (rmmod + insmod + urp add) -> cluster recovers
- `rpk bench` throughput comparison: direct UDS vs kernel module proxy.
  **On soft-RoCE (rxe) treat bench as measured-and-reported, not a
  pass/fail gate** -- the "within 20% of native TCP" target is unrealistic
  without hardware RDMA.

### 5.4 CI Pipeline

| Trigger | Tests | Runner |
|---------|-------|--------|
| Every push | Shared crate unit tests, `cargo test -p urp-cli`, kernel module build | GitHub Actions ubuntu-latest |
| Every push | Local integration tests (rdma_rxe namespace) | Self-hosted Linux with rdma_rxe |
| Every push | MicroVM x86_64 pair test | Self-hosted with KVM |
| Nightly | MicroVM aarch64 + riscv64 | Self-hosted with QEMU |
| Nightly | 1-hour soak test (16 conn, 4KB) | Self-hosted |
| Weekly | Redpanda 3-node cluster test | Self-hosted with 3 VMs |
| Weekly | Kernel version matrix (LTS: 6.1, 6.6, 6.12; latest stable) | MicroVMs with pinned kernels |

### Definition of Done -- Phase 5

- [ ] MicroVM x86_64 pair test passes end-to-end (boot -> load -> test -> clean -> shutdown)
- [ ] MicroVM aarch64 pair test passes
- [ ] MicroVM riscv64 pair test passes
- [ ] KASAN/KMEMLEAK clean in all VM tests
- [ ] CI pipeline runs on every push (shared crate + CLI + namespace integration)
- [ ] Nightly CI runs MicroVM tests + soak test
- [ ] Kernel version matrix: module builds and tests pass on 6.1, 6.6, 6.12, latest
- [ ] Redpanda cluster test: produce/consume works through kernel module proxy

---

## Verification Matrix

Cross-reference of features vs testing coverage:

| Feature | KUnit | Local Integration | MicroVM | Soak | CI |
|---------|-------|-------------------|---------|------|-----|
| Frame encode/decode | x | | | | x |
| Credit flow control | x | x | x | x | x |
| Reorder buffer | x | x | x | x | x |
| Virtual UDS endpoint | | x | x | x | x |
| RDMA send/recv | | x | x | x | x |
| Multi-QP | | x | x | x | x |
| Stream multiplexing | x | x | x | x | x |
| SRQ | | x | x | x | |
| QP health probes | x | x | x | | |
| PSK authentication | x | x | x | | x |
| GENL interface | | x | x | | x |
| urp CLI | x | x | x | | x |
| Endpoint lifecycle | x | x | x | | x |
| Module load/unload | | x | x | | x |
| Buffer pool | x | x | x | x | x |
| page_pool (k2) | | x | x | x | |
| Zero-copy send (k2) | | x | x | | |
| Cross-architecture | | | x | | x |
| Kernel version compat | | | x | | x |


[Back to Design Overview](DESIGN.md)
