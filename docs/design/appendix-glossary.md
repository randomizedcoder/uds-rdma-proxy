# Glossary

| Term | Definition |
|------|-----------|
| **BFD** | Bidirectional Forwarding Detection -- protocol for active path liveness detection with configurable probe intervals (RFC 5880) |
| **CQ** | Completion Queue -- where RDMA operation completion notifications are delivered |
| **CQE** | Completion Queue Entry -- a single completion notification |
| **DCQCN** | Data Center Quantized Congestion Notification -- RoCEv2 congestion control protocol |
| **DDP** | Direct Data Placement -- iWARP protocol layer that segments messages and places data directly into receive buffers (RFC 5041) |
| **DRR** | Deficit Round-Robin -- O(1) fair queueing algorithm that assigns each flow a quantum of bytes per round, carrying deficit forward. Used in Linux `sch_drr`, NIC hardware schedulers, and our stream scheduler |
| **DSCP** | Differentiated Services Code Point -- IP header field for traffic classification |
| **ECMP** | Equal-Cost Multi-Path -- routing technique that spreads traffic across multiple paths |
| **GENL** | Generic Netlink -- kernel framework for registering custom netlink families with type-safe attributes, used for module configuration (WireGuard, devlink, nl80211, urp) |
| **ECN** | Explicit Congestion Notification -- mechanism for signaling network congestion without dropping packets |
| **EWMA** | Exponentially Weighted Moving Average -- smoothing technique where `ewma = alpha * sample + (1 - alpha) * ewma`. Used for latency tracking (alpha=0.2 matches TCP SRTT convention) |
| **HOL blocking** | Head-of-Line blocking -- when a delayed frame prevents delivery of later frames. In multiplexed protocols, cross-stream HOL blocking occurs when streams share a single ordering domain. Per-stream sequence numbers (QUIC, our v3) eliminate it |
| **iWARP** | Internet Wide Area RDMA Protocol -- RDMA protocol stack running over TCP/IP via MPA + DDP + RDMAP (RFC 5040/5041/5044) |
| **MPA** | Marker PDU Aligned Framing -- iWARP framing layer that creates message boundaries over TCP byte streams (RFC 5044) |
| **NLA** | Netlink Attribute -- type-length-value (TLV) encoding used in netlink messages. Supports nesting (`NLA_NESTED`), typed values (`NLA_U32`, `NLA_STRING`), and kernel-side policy validation |
| **MR** | Memory Region -- a contiguous block of memory registered with an RDMA NIC for DMA access |
| **PFC** | Priority Flow Control -- Ethernet mechanism that pauses specific traffic classes to prevent drops |
| **PMTU** | Path Maximum Transmission Unit -- the largest IB payload size that fits in a single wire packet (discrete values: 256, 512, 1024, 2048, 4096) |
| **PTP** | Precision Time Protocol -- IEEE 1588 protocol for sub-microsecond clock synchronization across a network. Enables one-way latency measurement in health probes |
| **QP** | Queue Pair -- RDMA endpoint consisting of a Send Queue and Receive Queue |
| **RC** | Reliable Connected -- RDMA transport type with guaranteed, in-order delivery |
| **RDMAP** | Remote Direct Memory Access Protocol -- iWARP protocol layer providing RDMA Send/Read/Write operations over DDP (RFC 5040) |
| **RoCEv2** | RDMA over Converged Ethernet v2 -- RDMA protocol running over UDP/IP/Ethernet |
| **RTT** | Round-Trip Time -- time for a probe to travel from sender to receiver and back. Primary health metric for QP path quality |
| **SQE** | Submission Queue Entry -- an io_uring operation submission |
| **SRQ** | Shared Receive Queue -- allows multiple QPs to share a pool of receive buffers |
| **SRT** | Secure Reliable Transport -- video streaming protocol that embeds application-layer latency measurement in its control packets. Prior art for our health probe design |
| **UAPI** | User API -- kernel header files in `include/uapi/` that define the stable user-kernel ABI (system call arguments, ioctl numbers, netlink attributes) |
| **UDS** | Unix Domain Socket -- IPC mechanism for same-host communication |
| **WCMP** | Weighted-Cost Multi-Path -- ECMP variant that distributes traffic proportionally to path quality/capacity rather than equally |
| **WQE** | Work Queue Element -- a single operation on an RDMA Send or Receive Queue |
| **WC** | Work Completion -- result of a completed WQE, polled from the CQ |

### Proxy Modes and Roles

| Term | Definition |
|------|-----------|
| **UDS listen mode** | Proxy creates and binds a UDS socket (`listen_path`), accepts connections from local applications. Used for outbound tunnel direction. |
| **UDS connect mode** | Proxy connects to an existing UDS socket (`connect_path`) where a local application is listening. Used for inbound tunnel delivery. |
| **Bidirectional mode** | Both `listen_path` and `connect_path` are configured. The proxy handles both outbound and inbound directions simultaneously. Required for peer-to-peer cluster deployments (Redpanda, ClickHouse). |
| **RDMA initiator** | The side that calls `rdma_connect()` to establish the RDMA connection. Configured via `peer_address`. Uses odd stream IDs. |
| **RDMA acceptor** | The side that calls `rdma_listen()` / `rdma_bind_addr()` to accept incoming RDMA connections. Configured via `bind_address`. Uses even stream IDs. |

### Security Terms

| Term | Definition |
|------|-----------|
| **IPsec inline** | Hardware-accelerated IPsec encryption/decryption at the NIC, transparent to the application. Available on ConnectX-6 Dx+ |
| **MACsec** | IEEE 802.1AE link-layer encryption on Ethernet. Point-to-point, requires switch support |
| **mTLS** | Mutual TLS -- both sides present certificates and verify each other's identity during the TLS handshake |
| **PSK** | Pre-Shared Key -- a shared secret known to both sides. In this project, used for VRRP-style accidental-mismatch protection (Tier 0.5) |
| **SA** | Security Association -- an IPsec construct that defines the security parameters (keys, algorithms, lifetime) for a communication channel |


[Back to Design Overview](../DESIGN.md)
