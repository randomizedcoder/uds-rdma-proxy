# UDS-RDMA-Proxy: Tunneling Unix Domain Sockets over RoCEv2

A high-performance proxy that transparently tunnels Unix Domain Socket (UDS) connections over RDMA (Remote Direct Memory Access) using RoCEv2 (RDMA over Converged Ethernet v2).

```
Application A  <--UDS-->  uds-rdma-proxy  <==RDMA/RoCEv2==>  uds-rdma-proxy  <--UDS-->  Application B
  (Machine A)                (Machine A)                        (Machine B)                (Machine B)
```

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
| A | [Glossary](design/appendix-glossary.md) | CQ, CQE, DCQCN, DSCP, ECMP, ECN, MR, PFC, QP, RC, RoCEv2, SQE, SRQ, UDS, WQE, WC, mTLS, PSK, IPsec inline |
| B | [RoCEv2 Security Practices](design/appendix-rocev2-security.md) | Industry survey (cloud, storage, HPC), common practices (VLANs, isolation), NIC crypto capabilities (ConnectX-6 Dx+), justification for tiered security |
| | **Implementation** | |
| | [Kernel Module Plan](KERNEL-MODULE-PLAN.md) | 6-phase implementation plan: prerequisites, k0 PoC, urp CLI + GENL, k1 functional, k2 optimized, MicroVM integration |
| | [Implementation Tracker](KERNEL-MODULE-IMPLEMENTATION.md) | Progress tracking with per-phase status, definition-of-done checklists, test result tables |

---

## Architecture at a Glance

```
 Machine A                                                    Machine B
 +-------------+     +--------------------+   RoCEv2 RC QPs   +--------------------+     +-------------+
 |             |     |                    |  (1..N QPs over    |                    |     |             |
 |  App A      | UDS |  uds-rdma-proxy    |   ECMP paths)     |  uds-rdma-proxy    | UDS |  App B      |
 |  (client)   |<--->|  listen_path       |<=================>|  connect_path      |<--->|  (server)   |
 |             |     |                    |  RDMA initiator    |                    |     |             |
 +-------------+     +--------------------+                    +--------------------+     +-------------+
```

Both proxy instances run the **same binary**. Behavior is config-driven:
- **`listen_path`**: proxy creates a UDS socket, accepts local app connections (outbound tunnel)
- **`connect_path`**: proxy connects to an existing UDS socket (inbound tunnel delivery)
- **Both set**: bidirectional mode (needed for peer-to-peer cluster deployments like Redpanda/ClickHouse)

## Data Path

```
 App write() --[copy 1]--> UDS kernel buf --[copy 2]--> proxy TX buffer (registered MR)
                                                              |
                                                     NIC DMA (0 copies)
                                                              |
 proxy RX buffer (registered MR) --[copy 3]--> UDS kernel buf --[copy 4]--> App read()
```

Minimum **4 copies** end-to-end. RDMA segment is zero-copy (NIC DMA). With experimental `splice()` support, potentially reducible to 2-3 copies. See [Performance Optimization](design/13-performance.md).

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| RDMA verb type | Two-sided SEND/RECV (v2), one-sided WRITE (v4) | Two-sided is simpler, maps naturally to byte stream proxy |
| QP topology | Configurable 1-32 QPs per node pair | Exploit ECMP paths; software B-tree reorder buffer; Falcon hardware offload later |
| Receive queue | SRQ from v2 | Prevents per-QP starvation, reduces NIC cache pressure at scale |
| UDS I/O | io_uring with dual-registered buffers | Same buffers registered with both io_uring and ibverbs; eliminates intermediate copies |
| CQ polling | Adaptive (event-driven + busy-poll) | Low latency under load, low CPU when idle |
| Batching | Adaptive Nagle-like | Dynamic flush timeout scales with throughput |
| Flow control | Two-layer: per-QP credits + per-stream windows | QP credits prevent RNR errors; stream windows prevent HOL blocking from slow consumers |
| Stream multiplexing | Per-peer shared QP sets, per-stream sequence numbers | Reduces QP count 10-50× at cluster scale; per-stream reorder eliminates cross-stream HOL blocking (QUIC-like) |
| Ancillary data | Strict mode (default) / Graceful mode | Strict rejects SCM_RIGHTS; Graceful strips + warns |
| Security | Tiered: network isolation → shared password → certificate mTLS → IPsec HW offload | Matches industry norms; PSK initial implementation; upgrade path to full crypto |
| Proxy modes | Config-driven: listen_path / connect_path / both | Supports unidirectional and bidirectional (cluster peer-to-peer) deployments |
| Async runtime | tokio + dedicated CQ poll thread + raw io-uring | Control plane on tokio; data path avoids runtime overhead |

## Phased Roadmap

```
 v0 (TCP)  -->  v1 (rsockets)  -->  v2 (native ibverbs)  -->  v3 (multi-QP/ECMP)  -->  v4 (one-sided)
   |                                       |                          |
   validates architecture                  validates RDMA mechanics    validates ECMP scaling
   framing, metrics, pump                  flow control, SRQ           reorder, batching
                                           buffer pool, CQ polling
```

See [Phased Implementation Roadmap](design/03-roadmap.md) for details on each phase.

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

All testing uses Linux kernel software RDMA (`rdma_rxe` module). No RDMA hardware required for development or CI. Three tiers of testing:

- **Unit tests**: Table-driven tests (positive/negative/boundary/adversarial) for frame codec, buffer pool, credit state machine, reorder buffer, buffer poisoning
- **Namespace integration tests**: Network namespaces + veth pairs on the host kernel — fast, seconds per run
- **MicroVM pair tests**: Lightweight VMs (microvm.nix) with independent kernels and RDMA subsystems — full kernel isolation, cross-architecture (x86_64, aarch64, riscv64)
- Lossy network simulation (tc-netem packet loss/jitter/reorder on veth pair)
- NUMA negative affinity validation
- Redpanda 3-node cluster compatibility suite
- Load generator (`uds-rdma-bench`) with producer/consumer/echo/bidirectional modes

See [Testing Strategy](design/12-testing.md) for full details.

## Project Layout

```
uds-rdma-proxy/
├── Cargo.toml                    # Workspace root
├── crates/
│   ├── uds-rdma-proxy/           # Main proxy binary
│   │   └── src/
│   │       ├── main.rs, config.rs, proxy.rs, uds.rs, pump.rs, metrics.rs
│   │       ├── transport/        # tcp.rs, rsocket.rs, rdma.rs, frame.rs
│   │       └── rdma/             # connection.rs, qp.rs, buffer_pool.rs,
│   │                             # flow_control.rs, cq.rs, reorder.rs
│   └── uds-rdma-bench/           # Load generator binary
├── tests/integration/            # End-to-end tests (require rdma_rxe)
├── benches/                      # Criterion microbenchmarks
├── fuzz/fuzz_targets/            # cargo-fuzz targets (frame decode, reorder, config, protocol)
├── deploy/                       # systemd, Dockerfile, k8s/
├── docs/
│   ├── DESIGN.md                 # This file (overview + links)
│   └── design/                   # Detailed design documents
├── flake.nix                     # Delegates to ./nix/, includes microvm.nix input
└── nix/                          # Modular Nix configuration
    ├── packages.nix, env-vars.nix, devshell.nix, derivation.nix
    ├── cross-compilation.nix     # aarch64 + riscv64 cross-compiled builds
    ├── shell-functions/          # rdma-setup, validation, build, clean
    ├── tests/                    # Nix test derivations
    ├── bench/                    # Experiment factory (mkBenchExperiment)
    └── microvms/                 # MicroVM pair testing (x86_64, aarch64, riscv64)
```

See [Project Structure](design/19-project-structure.md) for full tree and Nix architecture.
