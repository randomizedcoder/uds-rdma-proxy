# UDS-RDMA-Proxy (`urp`): Tunneling Unix Domain Sockets over RoCEv2

Transparently tunnel Unix Domain Socket (UDS) connections over RDMA (Remote
Direct Memory Access) using RoCEv2, with no application changes.

```
Application A  <--UDS-->  urp (kernel module)  <==RDMA/RoCEv2==>  urp (kernel module)  <--UDS-->  Application B
  (Machine A)                (Machine A)                             (Machine B)                    (Machine B)
```

`urp` is implemented as a **Linux kernel module** (`kernel/`, C) driven over
generic netlink by a Rust CLI (`crates/urp-cli`), with a shared `no_std` Rust
protocol crate (`crates/uds-rdma-protocol`) serving as the reference
implementation and differential-test oracle for the hand-written kernel C.

> **Historical note.** The project was originally designed as a *userspace*
> Rust proxy; design docs 01–20 below date from that era and are marked with
> status banners. The kernel-module design (docs 21–23) is what was built.
> The v0–v4 roadmap in doc 03 was superseded by the kernel module's
> Phase 0–5 / k0–k2 plan (`KERNEL-MODULE-PLAN.md`).

---

## Table of Contents

| # | Section | Summary |
|---|---------|---------|
| 1 | [Introduction](design/01-introduction.md) | Problem statement, proposed solution, key benefits, use cases, non-goals |
| 2 | [High-Level Architecture](design/02-architecture.md) | End-to-end system overview, internal component diagrams, data flow, copy analysis (4 copies min) |
| 3 | [Phased Implementation Roadmap](design/03-roadmap.md) | v0 TCP baseline, v1 rsockets, v2 native ibverbs, v3 multi-QP/ECMP, v4 one-sided RDMA |
| 3a | [Stream-to-Message Adaptation](design/03a-stream-message-adaptation.md) | Byte stream vs. message semantics, framing technique survey, iWARP/RPC-over-RDMA/iSER prior art, page alignment, immediate vs. adaptive dispatch modes, design rationale |
| 4 | [Framing Protocol](design/04-framing-protocol.md) | Wire format (20-byte header), frame types (Data/Control/Probe), per-type flags, credit piggybacking, MTU/PMTU payload sizing |
| 5 | [RDMA Transport Layer](design/05-rdma-transport.md) | Connection setup (rdma_cm), QP config, SRQ from v2, credit-based flow control, buffer pool, CQ polling modes |
| 6 | [UDS I/O Layer (io_uring)](design/06-uds-io-uring.md) | Why io_uring, dual-registered buffers (io_uring + ibverbs), buffer lifetime, crate selection |
| 7 | [Bidirectional Pump](design/07-bidirectional-pump.md) | Per-connection task model, half-close handling, error propagation, end-to-end backpressure |
| 8 | [Multi-QP & ECMP](design/08-multi-qp-ecmp.md) | ECMP path diversity, reordering problem, sequence numbers, B-tree reorder buffer, QP selection strategies (adaptive weighted), QP health state machine, dynamic working set management, Falcon reference |
| 8a | [QP Health Probes](design/08a-qp-health-probes.md) | PING/PONG probe protocol, per-QP RTT and one-way latency measurement (PTP), QP qualifying, asymmetry detection, SRT-like application-layer measurement |
| 9 | [Connection Multiplexing](design/09-connection-multiplexing.md) | Scale analysis (3.5k node QP budget), per-peer shared QP sets, stream lifecycle (SYN/FIN/RST), per-stream sequence numbers and reorder buffers, two-layer flow control (QP credits + stream windows), HOL blocking elimination (QUIC-like), DRR scheduling, NIC resource budget, protocol comparisons (HTTP/2, QUIC, SCTP) |
| 10 | [Configuration](design/10-configuration.md) | CLI interface, TOML config file (proxy, RDMA, buffers, flow control, reorder, CQ, batching, performance, metrics) |
| 11 | [Prometheus Metrics](design/11-metrics.md) | 50+ metrics across connections, throughput, latency, flow control, buffer pool, RDMA QP, reorder, io_uring, batching, ancillary data |
| 12 | [Testing Strategy](design/12-testing.md) | Table-driven unit tests, Criterion/macro benchmarks, fuzzing (cargo-fuzz, proptest), ASAN/MSAN/TSAN/Miri, adversarial protocol, stress/chaos, MicroVM pairs |
| 13 | [Performance Optimization](design/13-performance.md) | Copy analysis, io_uring SQPOLL, adaptive Nagle-like batching, signaled completions, inline sends, NUMA awareness, CPU pinning, splice() zero-copy |
| 14 | [Rust Crate Dependencies](design/14-dependencies.md) | rdma-sys, io-uring, tokio, metrics, clap, tracing, crossbeam, proptest, cargo-fuzz, cargo-geiger/audit/deny |
| 15 | [Network Configuration](design/15-network-config.md) | Software RDMA setup, RoCEv2 production requirements (PFC, ECN, DCQCN, DSCP), MTU considerations |
| 16 | [Limitations & Non-Goals](design/16-limitations.md) | SCM_RIGHTS handling (strict/graceful modes), SCM_CREDENTIALS, copy overhead, SOCK_DGRAM, ancillary data modes |
| 17 | [Security Considerations](design/17-security.md) | Tiered security (network isolation, shared password PSK, certificate mTLS, software TLS, hardware IPsec offload), UDS permissions, buffer zeroing, DoS prevention |
| 18 | [Deployment Model](design/18-deployment.md) | Systemd service (sd_notify), container (capabilities), Kubernetes DaemonSet with RDMA device plugin |
| 19 | [Project Structure](design/19-project-structure.md) | Cargo workspace, modular Nix flake, MicroVM pair testing (x86_64/aarch64/riscv64), cross-compilation, experiment factory |
| 20 | [Open Questions & Future Work](design/20-future-work.md) | AF_XDP kernel bypass, shared memory fast path, Falcon hardware offload, compression, security tiers 1/2/2-HW, mesh networking |
| 21 | [Kernel Module Alternative](design/21-kernel-module.md) | 4→2 copy elimination, virtual UDS endpoint, in-kernel RDMA verbs, NIC driver architecture parallels (NAPI, page_pool), Rust no_std code sharing, zero-copy page flipping, phased k0-k2 roadmap |
| 22 | [Generic Netlink Interface](design/22-genl-interface.md) | GENL family `"urp"`, 4-command model (NEW/DEL/SET/GET_ENDPOINT), nested attribute hierarchy, YAML spec, UAPI header, kernel handler design, multicast events |
| 23 | [`urp` CLI Tool](design/23-cli-tool.md) | Standalone Rust CLI (neli + clap), subcommands (add/remove/set/show/stats/monitor/drain), JSON/human/oneline output, systemd deployment, monitoring integration |
| 24 | [Network Namespaces & Multi-Tenancy](design/24-network-namespaces.md) | **Scope decision: no module change for trusted multi-cluster** (already supports multiple host-side UDS endpoints + shared-volume sockets; work is provisioning, not kernel). Retains a **deferred Phase 7** design for untrusted-tenant hardware isolation: `init_net` audit, exclusive-mode per-netns RDMA devices, two-plane split, nsfd control plane, connection-time token (no per-frame ns id), pernet lifecycle, `/proc/net/urp` |
| 25 | [Provisioning Layer (outline)](design/25-provisioning-layer.md) | Trusted multi-cluster deployment (no kernel change): `urp-agent` host daemon reconciling tunnels via the `urp` CLI, shared bind-mount socket volumes host↔pod, Redpanda `unix_path` wiring, k8s DaemonSet + CRD/ConfigMap, PSK/bind allocation; open items: inter-broker RPC, peer addressing |
| 26 | [Upstream Readiness](design/26-upstream-readiness.md) | Static-analysis tooling in Nix (`nix/analysis/`: sparse-master, smatch, checkpatch --strict, W=1/W=2, coccicheck, clippy, rustfmt — all hermetic, report-only), baseline → post-fix counts, real bugs found (NLA_POLICY_RANGE s16 truncation, 5.9KiB netlink stack frame, QP-slot leak on OOM), intentional checkpatch residual, prioritized maintainer follow-ups (kref lifetime, netns, lock scope, waitqueue pumps, kthread pinning) |
| 27 | [Comprehensive Fuzz-Testing Plan](design/27-fuzz-testing.md) | Threat model (S1 RDMA wire / S2 pre-auth PSK / S3 netlink / S4 lifecycle / S5 FFI); four tracks (F0 Rust parser+differential, F1 userspace C-harness extraction under libFuzzer+ASAN, F2 syzkaller-on-netlink + hostile-peer wire fuzzer in a KCOV/KASAN VM, F3 lifecycle churn + Nix targets + nightly CI); concrete entry points with file:line; **two seed security bugs found by the planning sweep** (RX stale-DMA info-leak via unchecked payload_len vs wc->byte_len; sleep-in-RCU on the peer-triggered RST path) |
| 28 | [Testability Review + Table-Driven-Test Plan](design/28-testability.md) | Why the §27.8 bugs lived in untestable code; coverage inventory (now 34 KUnit + ~70 Rust tests); extract-pure-function points — E1 frame classifier and E2 stream SYN/FIN/RST state machine both **done** (dual-compile units + Rust twin), E3 netlink config-build open; standardize a case-array idiom; phased plan; a `nix run .#kunit` target (still **planned**, not yet built) |
| 30 | [urp-bench: io_uring UDS Benchmark](design/30-urp-bench-io-uring.md) | Symmetric userland benchmark pair (C + liburing, Rust + `io-uring` crate) driving the UDS side with io_uring; effects-separation experiment (blocking control vs batched, fixed-buffer, provided-buffer-ring, SQPOLL, SEND_ZC probe) over a message-size × batch-size × mode matrix; honest hypothesis that AF_UNIX zero-copy does not exist and syscall batching is the measurable win; 24-byte app framing as the table-tested + fuzzed pure core; answers [design 20 §20.1](design/20-future-work.md) with data. (Design 29 was a code-review/refactor plan whose work landed but whose doc was never committed.) |
| 31 | [urp-fast: End-to-End Zero-Copy](design/31-urp-fast-zero-copy.md) | **Design / future work.** The opt-in fast path design 30's `AF_UNIX` ceiling motivates: an `IORING_OP_URING_CMD` interface into `urp.ko` (the ublk/nvme-passthrough pattern) where an app's own buffer pool is dual-registered — io_uring fixed buffers + RDMA MR — so the NIC DMAs directly into/out of the app's pages. Full SEND and RECEIVE flows with diagrams, the buffer-pool lifecycle (malloc-once → flat-pressure steady state), the submit/complete ownership model, and the payoff table (2 kernel + 2 app copies → **0 software copies on both hosts**). `urp-bench` becomes its measurement harness (new topology T3 / `uring-cmd` mode). |
| 31a | [urp-fast in C++/Seastar (Zero-Copy Demo)](design/31a-seastar-cpp-demo.md) | **Design / future work.** The C++/Seastar client of design 31, targeting the real application — **Redpanda is a Seastar program**. Shows how Seastar's architecture already embodies design 31's invariants (shard-per-core share-nothing = lock-free ownership; startup hugepage arena = malloc-once pool; `temporary_buffer` custom deleter = zero-copy handoff + recycle; io_uring reactor backend; futures = submit/complete), a per-shard `urp_fast_shard` service (pool + `uring_cmd` ring + one QP per shard) bridged into the reactor via a poller, SEND/RECV flows, a standalone demo app reusing the design-30 framing + `BENCH_OK` grammar for a three-way comparison, the path to a real Redpanda `iobuf`/RPC integration, a three-layer **table-driven unit-test plan** (pure-core + fake-ring async + safety/negative suites, incl. the RX length-overflow guard against the design-27 §27.8 seed bug), and a four-layer **nix integration environment** (sandboxed units → host smoke → microVM pair → matrix) with correctness / speed / zero-copy / memory-stability oracles — the last proving "malloc-once, then flat" via `seastar::memory::stats()` + RSS deltas. |
| A | [Glossary](design/appendix-glossary.md) | CQ, CQE, DCQCN, DSCP, ECMP, ECN, MR, PFC, QP, RC, RoCEv2, SQE, SRQ, UDS, WQE, WC, mTLS, PSK, IPsec inline |
| B | [RoCEv2 Security Practices](design/appendix-rocev2-security.md) | Industry survey (cloud, storage, HPC), common practices (VLANs, isolation), NIC crypto capabilities (ConnectX-6 Dx+), justification for tiered security |
| | **Implementation** | |
| | [Kernel Module Plan](KERNEL-MODULE-PLAN.md) | 6-phase implementation plan: prerequisites, k0 PoC, urp CLI + GENL, k1 functional, k2 optimized, MicroVM integration |
| | [Implementation Tracker](KERNEL-MODULE-IMPLEMENTATION.md) | Progress tracking with per-phase status, definition-of-done checklists, test result tables |

---

## Architecture at a Glance

```
 Machine A                                                      Machine B
 +-------------+     +---------------------+   RoCEv2 RC QPs   +---------------------+     +-------------+
 |             |     |  urp.ko             |  (1..N QPs, SRQ)  |  urp.ko             |     |             |
 |  App A      | UDS |  endpoint:          |<=================>|  endpoint:          | UDS |  App B      |
 |  (client)   |<--->|  listen_path        |  RDMA initiator   |  connect_path       |<--->|  (server)   |
 |             |     |  (accepts UDS conns)|                   |  (RDMA listener)    |     |             |
 +-------------+     +---------------------+                   +---------------------+     +-------------+
        control plane:  urp CLI --generic netlink ("urp" family)--> kernel module
        observability:  /proc/urp/<name>/stats, urp show/stats/monitor, genl multicast events
```

Both hosts load the **same module**; behavior per *endpoint* is config-driven:

- **`listen_path`** — the endpoint creates a UDS socket and accepts local app
  connections (the RDMA **initiator** side; one multiplexed *stream* per
  accepted UDS connection).
- **`connect_path`** — the endpoint listens for RDMA connections and delivers
  tunneled bytes into an existing local UDS socket (the **acceptor** side).

An endpoint is created/inspected/removed at runtime with
`urp add/show/stats/set/drain/remove/monitor` (design 22/23). Endpoints live
in an rhashtable keyed by name, with kref + RCU lifetime. Data moves through
per-connection kernel threads ("pumps"): TX pumps `kernel_recvmsg` from the
UDS socket, frame-encode into pre-registered MR buffers (page_pool backed),
and `ib_post_send` over a round-robin-selected QP; the RX completion handler
(`urp_recv_done`) classifies each frame (pure function `urp_classify_frame`),
runs the per-stream SYN/FIN/RST state machine (`urp_stream_next_state`), and
`kernel_sendmsg`s payload into the destination UDS socket. Flow control is
credit-based per QP, with credit grants piggybacked or sent as CONTROL
frames; QP health is measured with PING/PONG probes (RTT EWMA). Connections
authenticate with a PSK (SHA-256) carried in `rdma_cm` private_data.

## Data Path

```
 TX:  App write() -> UDS socket buf --kernel_recvmsg--> registered MR buffer (+20B header)
                                                            |
                                                   ib_post_send / NIC DMA
                                                            |
 RX:  CQE -> urp_recv_done -> classify -> stream lookup --kernel_sendmsg--> UDS socket buf -> App read()
```

Bytes never leave the kernel: there is no userspace proxy process and no
syscall boundary on the data path — the copies that remain are the
UDS-socket-to-MR-buffer moves on each side plus the zero-copy NIC DMA in the
middle. See [Kernel Module](design/21-kernel-module.md) for the copy
analysis versus the original userspace design, and
[Performance Optimization](design/13-performance.md) (historical) for the
userspace-era analysis.

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Implementation | **Linux kernel module** (C) + Rust control CLI | Eliminates the userspace proxy hop entirely; bytes move UDS⇄RDMA inside the kernel (design 21) |
| RDMA verb type | Two-sided SEND/RECV via in-kernel verbs (`rdma_cm` + `ib_post_send`) | Simple, maps naturally to a byte-stream tunnel; one-sided WRITE deferred |
| QP topology | Configurable N QPs per endpoint (`--num-qps`), round-robin selection | Exploit ECMP path diversity; per-QP health state machine |
| Receive queue | Shared Receive Queue (SRQ) across the endpoint's QPs | Prevents per-QP starvation, reduces NIC cache pressure at scale |
| UDS I/O | In-kernel `kernel_recvmsg`/`kernel_sendmsg` on kthread pumps | No syscalls, no userspace buffers on the data path |
| Buffers | Pre-registered MR pool, page_pool backed, NUMA-aware | DMA-ready buffers; no per-frame registration |
| Flow control | Two-layer: per-QP credits (+ per-stream windows planned) | QP credits prevent RNR errors; grants piggybacked on DATA or sent as CONTROL frames |
| Stream multiplexing | Per-endpoint shared QP set; one stream per UDS connection (SYN/FIN/RST lifecycle, per-stream seq) | Reduces QP count at cluster scale; QUIC-like independence between streams |
| Control plane | Generic netlink family `"urp"` (NEW/DEL/SET/GET_ENDPOINT) + Rust CLI | Standard kernel config-plane idiom; extack error strings; multicast events |
| Shared logic | `no_std` Rust crate mirrors frame/credit/reorder/stream-SM; C is differentially tested against it | One spec, two implementations, fuzzed against each other |
| Security | Tiered (design 17): network isolation → **PSK SHA-256 in `rdma_cm` private_data (implemented)** → mTLS/IPsec (future) | Matches industry norms; upgrade path to full crypto |
| Testing | Soft-RoCE (`rdma_rxe`) everywhere: host harness, microVM pairs, sanitizer VMs, fuzzers | No RDMA hardware needed for development or CI |

## Phased Roadmap

Implementation followed `KERNEL-MODULE-PLAN.md` (Phases 0–5, data-path
milestones k0→k2), **not** the original userspace v0–v4 roadmap in
[doc 03](design/03-roadmap.md) (historical):

```
 Phase 0        Phase 1 (k0)      Phase 2           Phase 3a/3b (k1)          Phase 4 (k2)        Phase 5
 skeleton  -->  RDMA echo    -->  GENL control  --> multi-QP + SRQ + credits  --> page_pool +   --> microVM pairs,
 + Nix          data path         plane + CLI       + streams + probes + PSK      NUMA + soak       kernel matrix,
                                                                                                    cross-arch, CI
```

Current state: Phases 0–4 complete, Phase 5 substantially complete — see
[status.md](../status.md). Follow-on work is tracked in design docs 24–28.

## Frame Wire Format

```
 +---------+------------------+------------+-------+-----------------+----------------+---------+
 | stream_ | sequence_number  | frame_type | flags | credits_granted | payload_length | payload |
 | id (4B) |     (8B)         |    (1B)    | (1B)  |     (2B)        |     (4B)       | (0..N)  |
 +---------+------------------+------------+-------+-----------------+----------------+---------+
 Total header: 20 bytes

 Frame types: DATA (0x00) — UDS payload
              CONTROL (0x01) — credit grants, QP management
              PROBE (0x02) — health probes (PING/PONG with timestamps)
```

See [Framing Protocol](design/04-framing-protocol.md) for field definitions, per-type flags, and encoding strategy.

## Testing Without Hardware

All testing uses Linux kernel software RDMA (`rdma_rxe`). No RDMA hardware is
required for development or CI. The tiers, all invocable via Nix:

- **Unit tests** — 34 KUnit cases in `kernel/urp_test.c` (frame codec, credit
  state machine, reorder backends, QP selection, probe codec, stream state
  machine, RX classifier) + ~70 Rust tests in the shared crate, many
  table-driven (design 28); the C and Rust implementations are differentially
  tested against the same contract.
- **Host integration** — `nix run .#test-kmod-k0` (root): 23 tests on
  single-host soft-RoCE loopback (module lifecycle, CLI/genl behavior, RDMA
  echo/throughput/latency, drain/remove).
- **MicroVM pair tests** — `nix run .#urp-microvm-pair-test` boots two
  microvm.nix VMs with independent kernels, links them, and runs the full
  URP-to-URP flow (12-concurrent-stream burst, teardown, dmesg oracle);
  `-debug` variant runs the same under a **KASAN + KMEMLEAK + lockdep + KCOV**
  kernel and additionally executes the live fuzz phases (netlink blind /
  coverage-guided / racer, hostile-peer wire fuzzer). Cross-arch: aarch64
  (QEMU TCG, full boot) and riscv64 (build gate).
- **Kernel-version matrix** — module builds against 6.1 / 6.6 / 6.12 LTS +
  latest mainline (`.#urp-ko-6_1` …), on every push.
- **Fuzzing** — hermetic libFuzzer harnesses compiling the real kernel C
  (`.#fuzz-classify`, `.#fuzz-rx-seq`, `.#fuzz-reorder`) + 5 cargo-fuzz
  targets; see [design 27](design/27-fuzz-testing.md).
- **Soak** — `nix run .#soak-1h`: 1-hour, 16-connection load with endpoint
  churn and a slab-leak budget.
- **Real workload** — Redpanda over the tunnel: `rpk` metadata and full
  produce/consume round-trips (`.#test-redpanda-uds`,
  `.#test-redpanda-produce-consume`).

See [Testing Strategy](design/12-testing.md) (§12.6 has the real CI layout)
and [design 27](design/27-fuzz-testing.md).

## Project Layout

```
uds-rdma-proxy/
├── Cargo.toml                     # Workspace root (protocol, ffi, cli crates)
├── kernel/                        # The urp kernel module (C)
│   ├── urp.h                      # Core structs + prototypes (see also urp_frame.h,
│   │                              #   urp_stream_sm.h, urp_credit.h, urp_reorder.h)
│   ├── urp_main.c                 # module init/exit
│   ├── urp_netlink.c              # genl "urp" family (NEW/DEL/SET/GET_ENDPOINT)
│   ├── urp_endpoint.c             # endpoint rhashtable + kref lifecycle
│   ├── urp_rdma.c                 # buffer pool, CM handshake + PSK, RX completion path
│   ├── urp_socket.c, urp_pump.c   # UDS accept/connect + TX pump kthreads
│   ├── urp_stream.c, urp_stream_sm.c  # stream table + pure SYN/FIN/RST state machine
│   ├── urp_frame.c, urp_credit.c  # pure RX classifier + credit state machine
│   ├── urp_qp.c, urp_srq.c, urp_proc.c
│   ├── urp_reorder.c, urp_reorder_rust.c  # reorder backends (C rbtree / Rust FFI)
│   ├── urp_test.c                 # KUnit suites (34 cases)
│   └── include/uapi/linux/urp.h   # UAPI: wire constants + genl attrs
├── crates/
│   ├── uds-rdma-protocol/         # no_std shared crate: frame, credit, reorder,
│   │                              #   stream SM, probe — reference + fuzz oracle
│   ├── uds-rdma-protocol-ffi/     # staticlib bridging ReorderBuffer into the module
│   └── urp-cli/                   # the `urp` CLI (clap + hand-rolled genl client)
├── tools/urp-test-client.c        # userspace RDMA peer for the host harness
├── fuzz/                          # cargo-fuzz targets + fuzz/regressions/ reproducers
├── docs/
│   ├── DESIGN.md                  # This file (overview + links)
│   └── design/                    # Detailed design documents (01–30)
├── flake.nix                      # Exposes packages/checks/apps; delegates to ./nix/
└── nix/
    ├── checks.nix                 # buildUrpKoWith + kernel matrix + protocol tests
    ├── analysis/                  # sparse/smatch/checkpatch/W=1/coccicheck/clippy/rustfmt
    ├── fuzz/                      # libFuzzer + live-VM fuzz harnesses (C)
    ├── microvms/                  # microVM pair harness (x86_64/aarch64/riscv64)
    ├── test-kmod-k0.nix, soak-1h.nix, test-redpanda-*.nix
    └── urp-cli.nix, urp-protocol-ffi.nix, devshell.nix, ...
```

The `nix/tests/`, `nix/bench/`, `crates/uds-rdma-proxy/`, `crates/uds-rdma-bench/`
trees described in [doc 19](design/19-project-structure.md) belong to the
historical userspace design and were never built.
