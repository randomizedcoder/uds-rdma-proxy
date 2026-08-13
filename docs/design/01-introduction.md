# Introduction

> **Note (2026-08-11):** Written during the original userspace-proxy design
> era (2026-05). The protocol and architecture content below still describes
> the implemented wire behavior, but implementation specifics are
> userspace-flavored (Rust proxy process, userspace ibverbs) — the shipped
> implementation is the `urp` **kernel module** (see [DESIGN.md](../DESIGN.md)),
> and v0–v4 phase references follow the abandoned userspace roadmap (see the
> phase-numbering note in [KERNEL-MODULE-PLAN.md](../KERNEL-MODULE-PLAN.md)).

## 1.1 Problem Statement

Many high-performance applications use Unix Domain Sockets (UDS) for local inter-process communication. Databases (PostgreSQL, Redis, Redpanda/Kafka), container runtimes, gRPC services, and monitoring agents all support UDS for same-host IPC. UDS avoids the overhead of the kernel TCP/IP stack -- no routing, no congestion control, no checksum computation -- resulting in lower latency and higher throughput for local communication.

However, UDS is fundamentally local. When these applications need to communicate across machines, they fall back to TCP, which introduces:

- **Kernel network stack overhead**: syscalls for send/recv, protocol processing, buffer copies
- **Context switches**: each I/O operation requires a user-kernel transition
- **Memory copies**: data is copied from userspace to kernel socket buffers, through the network stack, and back to userspace on the other end
- **CPU consumption**: TCP checksum computation, congestion control algorithms, and timer management consume CPU cycles that could be used by the application

## 1.2 Proposed Solution

**uds-rdma-proxy** is a transparent proxy that accepts UDS connections on one machine and tunnels them over RDMA (specifically RoCEv2) to a peer proxy on another machine, which exposes another UDS endpoint. Applications see only local Unix sockets; the network transport is completely invisible.

```
Application A  <--UDS-->  uds-rdma-proxy  <==RDMA/RoCEv2==>  uds-rdma-proxy  <--UDS-->  Application B
  (Machine A)                (Machine A)                        (Machine B)                (Machine B)
```

## 1.3 Key Benefits

- **Near-wire-speed latency**: RDMA bypasses the kernel network stack on the data path. Single-digit microsecond latencies are typical on 25-100Gbps hardware, compared to 10-50us for optimized TCP.
- **Kernel bypass**: After connection setup, data transfers do not involve syscalls. The NIC reads/writes directly from/to userspace memory via DMA.
- **CPU offload**: The NIC handles reliable transport (retransmission, ordering, flow control for RC QPs), freeing CPU cycles for application work.
- **Universal compatibility**: Any application that supports UDS can be tunneled without modification. No application code changes, no library relinking, no protocol awareness.
- **Multi-path scalability**: Multiple Queue Pairs (QPs) can exploit ECMP (Equal-Cost Multi-Path) network topologies to aggregate bandwidth beyond a single path, with software reordering to maintain stream semantics.

## 1.4 Use Cases

| Use Case | Description |
|----------|-------------|
| **Database replication** | PostgreSQL streaming replication, Redis cluster replication, Redpanda/Kafka inter-broker traffic over UDS tunneled across hosts |
| **Container-to-container** | Sidecar proxies or service mesh data planes where pods on different nodes communicate via UDS |
| **gRPC over UDS** | Microservices that use gRPC with UDS transport for local comms, extended transparently across hosts |
| **GPU coordination** | Control plane traffic between GPU nodes where UDS is the native IPC mechanism |
| **Monitoring pipelines** | High-volume telemetry collection from UDS-based exporters across a fleet |

## 1.5 Non-Goals (v1)

- Not a general-purpose RDMA library or framework
- Not a replacement for rsockets or libfabric
- No support for `SOCK_DGRAM` UDS (stream sockets only)
- No passthrough of `SCM_RIGHTS` (file descriptor passing) or `SCM_CREDENTIALS`
- No built-in encryption (RDMA fabrics are assumed trusted)


[Back to Design Overview](../DESIGN.md)
