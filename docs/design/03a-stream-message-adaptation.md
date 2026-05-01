# Stream-to-Message Adaptation

## 3a.1 The Fundamental Problem

The UDS-RDMA proxy's core operation is bridging two fundamentally different I/O models:

- **UDS `SOCK_STREAM`**: A bidirectional byte stream with no inherent message boundaries. A `read()` call may return anywhere from 1 byte to the buffer size, regardless of how many bytes the sender's `write()` submitted. Partial reads and writes are normal. The kernel may coalesce multiple small writes into a single read, or split a large write across multiple reads.

- **RDMA `SEND/RECV`**: A message-oriented transport. Each `ibv_post_send` submits a discrete message; the receiver's completion queue entry (CQE) delivers exactly that message — same byte count, same boundaries, atomically. There is no byte-stream abstraction.

The proxy must translate a continuous byte stream into discrete RDMA messages on the sending side, and reassemble those messages back into a continuous byte stream on the receiving side. This is a well-studied problem in networking — every protocol that runs a higher-level abstraction over a different transport model faces some variant of it.

**Key simplification**: A transparent byte-stream proxy does NOT need to preserve original `write()` boundaries. UDS `SOCK_STREAM` semantics already permit the kernel to split or merge writes arbitrarily. The proxy only needs to guarantee **byte ordering and completeness** — every byte written on one end must be delivered, in order, on the other end. This is a weaker (and simpler) requirement than message-preserving protocols like SCTP.

However, preserving write boundaries can be valuable for debugging and certain workloads — see [Section 3a.5](#3a5-message-sizing-strategies) for our dual dispatch mode design.


## 3a.2 Survey of Framing Techniques

Any stream-to-message adaptation requires a **framing layer** that imposes message boundaries on the byte stream. The standard techniques are:

### Length-Prefix (Our Choice)

Prepend each message with an N-byte integer specifying the payload length. The receiver reads the length, then reads exactly that many bytes.

- **Parse complexity**: O(1) — read fixed-size header, extract length field
- **Zero-copy friendly**: Yes — payload follows header contiguously, no scanning or transformation required
- **Used by**: MPA/iWARP [3], RPC-over-RDMA [4], gRPC (length-prefixed protobuf), Kafka wire protocol, most binary protocols
- **Relevance**: Ideal for our use case. The payload is raw UDS bytes — no encoding, no escaping. The 20-byte frame header (stream_id, sequence_number, flags, credits, payload_length) is a natural extension of simple length-prefix framing.

### Delimiter-Based

A special byte or byte sequence marks the end of each message. The receiver scans the byte stream for the delimiter.

- **Parse complexity**: O(n) — must scan every byte for the delimiter
- **Zero-copy friendly**: No — requires either escaping delimiter bytes in the payload or a scan-and-copy step
- **Used by**: HTTP/1.1 headers (CRLF), SMTP, Redis RESP (CRLF), newline-delimited JSON
- **Relevance**: Poor fit. UDS payloads are arbitrary binary data — any byte can appear in the stream, making delimiter escaping mandatory and destroying zero-copy properties.

### Fixed-Size

All messages are padded to a predetermined fixed size.

- **Parse complexity**: O(1) — no header parsing needed, just read N bytes
- **Zero-copy friendly**: Yes — but wastes bandwidth for variable-length payloads
- **Used by**: ATM cells (53 bytes), fixed-size database pages, some DMA ring buffers
- **Relevance**: Poor fit. UDS writes range from 1 byte to megabytes. Fixed-size framing would either waste bandwidth (large fixed size) or require fragmentation (small fixed size), adding complexity without benefit.

### Type-Length-Value (TLV)

Self-describing messages with a type tag, length, and value. Supports heterogeneous message types in a single stream.

- **Parse complexity**: O(1) per TLV, but type dispatch adds overhead
- **Zero-copy friendly**: Yes — similar to length-prefix but with an additional type field
- **Used by**: BER/DER (ASN.1), RADIUS, LLDP, TLS record protocol
- **Relevance**: Partial fit. Our frame header carries a `frame_type` field (Data/Control/Probe) plus per-type `flags` ([Section 4.4](04-framing-protocol.md#44-frame-types)), serving a similar role to the type tag. Our design is effectively TLV with a richer, typed header.

### MPA Markers (RFC 5044)

Periodic markers inserted at fixed intervals (every 512 bytes) in the TCP stream. Each marker contains a pointer back to the nearest FPDU (Framed PDU) header, enabling message boundary recovery after out-of-order TCP segment arrival.

- **Parse complexity**: O(1) per marker, but markers consume bandwidth and require careful alignment
- **Zero-copy friendly**: No — markers are interleaved with payload data, requiring removal
- **Used by**: MPA/iWARP [3] (optional)
- **Relevance**: Not needed. MPA markers solve a TCP-specific problem: recovering FPDU boundaries when TCP delivers segments out of order. Our RDMA transport delivers complete messages — there are no out-of-order segments to recover from.

### Comparison

| Technique | Parse Cost | Zero-Copy | Variable Payload | Out-of-Order Recovery | Used By |
|-----------|-----------|-----------|-----------------|----------------------|---------|
| **Length-prefix** | O(1) | Yes | Yes | No (not needed) | MPA, gRPC, Kafka |
| Delimiter | O(n) | No | Yes (with escaping) | No | HTTP, SMTP, Redis |
| Fixed-size | O(1) | Yes | No (padding waste) | No | ATM, fixed DMA rings |
| TLV | O(1) | Yes | Yes | No | ASN.1, RADIUS, TLS |
| MPA markers | O(1) | No | Yes | Yes | iWARP (TCP only) |


## 3a.3 Prior Art: RDMA Stream-Message Adaptation

### 3a.3.1 iWARP Protocol Stack (RFC 5040/5041/5044)

The iWARP protocol stack solves the **inverse** of our problem: providing RDMA message semantics over a TCP byte stream. It does so through a three-layer adaptation:

```
 TCP byte stream  →  MPA (framing)  →  DDP (data placement)  →  RDMAP (RDMA operations)
```

**MPA (Marker PDU Aligned, RFC 5044)** [3] is the framing layer that transforms TCP's unstructured bytes into discrete protocol data units. Each FPDU (Framed PDU) contains a 16-bit ULPDU length field — a length-prefix frame. MPA optionally inserts 4-byte markers at 512-byte intervals for out-of-order segment recovery, and appends CRC32c for integrity beyond TCP's weak checksum. MPA validates that length-prefix framing is the proven approach for stream-to-message adaptation in RDMA contexts.

**DDP (Direct Data Placement, RFC 5041)** [2] segments large ULP messages into DDP segments sized to fit MPA's MULPDU. It supports two buffer models: **tagged** (sender specifies remote buffer location via Steering Tag and offset — used for RDMA Write) and **untagged** (receiver pre-queues buffers, protocol assigns by Queue Number and Message Sequence Number — used for RDMA Send). DDP's key innovation is out-of-order data placement: segments can be placed into their final destination buffers immediately upon arrival, with delivery to the ULP occurring only after all segments of a message are present.

**RDMAP (RFC 5040)** [1] provides RDMA operations (Send, Read, Write) over DDP. Section 6 defines **Stream Management**: stream initialization after the LLP (Lower Layer Protocol) connection is established, transition to RDMA-enabled mode, and stream termination. This maps directly to our SYN/FIN/RST flag-based lifecycle management ([Section 4.4](04-framing-protocol.md#44-flag-definitions)).

### 3a.3.2 RPC-over-RDMA v1 (RFC 8166)

RPC-over-RDMA [4] transports ONC RPC (used by NFS) over RDMA and is widely deployed in the Linux kernel (`net/sunrpc/xprtrdma/`). Its key contribution to framing design is the **inline threshold** concept:

- Messages smaller than the inline threshold (~1024 bytes by default) are sent inline within an RDMA Send operation — the entire RPC call/reply fits in a single message
- Messages larger than the threshold use separate RDMA Read/Write operations with pre-registered buffers, allowing the NIC to DMA large payloads directly to/from the application's memory

This dual-mode approach optimizes for both small messages (low latency, no registration overhead) and large messages (zero-copy, high throughput). Our adaptive dispatch mode ([Section 3a.5.2](#3a52-adaptive-mode-dispatch_mode--adaptive-default)) follows a similar philosophy: converge to immediate dispatch for small/sparse writes (latency-optimal) and coalesce for high-throughput bursts (throughput-optimal).

RFC 8167 [4a] extends RPC-over-RDMA with bidirectional RPC on a single RDMA connection — analogous to our bidirectional pump ([Section 7](07-bidirectional-pump.md)).

### 3a.3.3 iSER (RFC 7145)

iSCSI Extensions for RDMA [5] adapt the iSCSI block storage protocol for RDMA transport. Control PDUs carry Steering Tags (STags) that the remote side uses for subsequent RDMA Read/Write data transfers. This separates the **control plane** (iSCSI protocol messages via RDMA Send) from the **data plane** (bulk block I/O via RDMA Read/Write with registered buffers).

This pattern — lightweight framed control messages plus bulk RDMA data operations — validates our approach of a compact frame header (20 bytes) carrying metadata alongside the data payload.

### 3a.3.4 Linux Kernel Implementations

The Linux kernel's `net/sunrpc/xprtrdma/` subsystem implements RFC 8166 framing and has been production-proven for over a decade in NFS-over-RDMA deployments. Key implementation patterns relevant to our design:

- **svc_rdma_recvfrom.c**: Server-side receive path — polls CQ, extracts RPC-over-RDMA headers, dispatches to RPC layer. Comparable to our `rdma_to_uds` pump task.
- **frwr_ops.c**: Fast Registration Work Requests for on-demand memory registration — relevant to our buffer pool design where pre-registration avoids this overhead.


## 3a.4 How Our Problem Differs

The critical distinction: iWARP puts **message semantics over a byte stream** (harder). We put a **byte stream over message semantics** (simpler).

```
 iWARP (message-over-stream):
 +--------+     +-------+     +-------+     +---------+
 | RDMA   | --> | RDMAP | --> | DDP   | --> | MPA     | --> TCP byte stream
 | ops    |     | msg   |     | segs  |     | framing |
 +--------+     +-------+     +-------+     +---------+

 UDS-RDMA Proxy (stream-over-message):
 +-----------+     +------------------+     +-------------+
 | UDS byte  | --> | Length-prefix    | --> | RDMA        | --> (wire)
 | stream    |     | frame (20B hdr) |     | SEND/RECV   |
 +-----------+     +------------------+     +-------------+
```

| Concern | iWARP | UDS-RDMA Proxy |
|---------|-------|----------------|
| **Message boundaries** | Must reconstruct from byte stream (MPA) | RDMA provides atomically (each CQE = one frame) |
| **Integrity** | CRC32c in MPA (TCP checksum too weak) | ICRC in RoCEv2 (sufficient) |
| **Out-of-order recovery** | MPA markers for TCP segment reordering | Not needed — RDMA delivers complete messages |
| **Segmentation** | DDP segments large messages for MPA | RDMA NIC segments transparently if frame > PMTU |
| **Framing complexity** | 3 layers (MPA + DDP + RDMAP) | 1 layer (our 20-byte header) |
| **Purpose of framing** | Reconstruct RDMA message boundaries | Chunk byte stream + carry metadata |

Our framing exists for two reasons only:
1. **Chunking**: divide the continuous UDS byte stream into RDMA-message-sized pieces
2. **Metadata**: carry stream_id, sequence number, credits, and lifecycle flags alongside the data

We do not need MPA-style markers (no out-of-order byte-stream recovery), DDP-style tagged/untagged buffer models (our buffer pool is simpler), or CRC (RoCEv2's ICRC covers integrity). This is why our single 20-byte header suffices where iWARP requires three protocol layers.


## 3a.5 Message Sizing Strategies

How the proxy chunks the byte stream into RDMA messages has significant performance implications. The proxy supports two configurable dispatch modes.

### 3a.5.1 Immediate Mode (`dispatch_mode = "immediate"`)

Each UDS `read()` result is framed and sent as one RDMA message immediately.

```
 App write(100B) → proxy read(100B) → frame(120B) → RDMA SEND → frame(120B) → proxy write(100B)
 App write(50B)  → proxy read(50B)  → frame(70B)  → RDMA SEND → frame(70B)  → proxy write(50B)
```

**Write-boundary preservation**: In immediate mode, the exact write sizes from the sending application are reproduced on the receiving application. If the sender writes 100 bytes then 50 bytes, the remote UDS delivers one 100-byte write followed by one 50-byte write. This is a stronger guarantee than `SOCK_STREAM` requires (TCP does not preserve write boundaries), but valuable for:
- **Debugging**: seeing the exact write pattern helps diagnose application protocol issues
- **Reproducibility**: deterministic message-to-frame mapping simplifies testing and analysis
- **Protocol-aware applications**: some applications may benefit from seeing writes at their original granularity

**Performance characteristics**:
- Minimum latency — no coalescing delay, data goes to wire as fast as the pump loop cycles
- Higher RDMA message rate — small writes produce many small messages with proportionally higher header overhead (a 100-byte write becomes a 120-byte RDMA message: 17% overhead)
- Higher CQ processing cost — more CQEs per byte transferred

**Best for**: Latency-sensitive workloads, debugging/protocol analysis, power users who size their writes appropriately (e.g., 4KB-aligned writes over jumbo frames — one write = one page-aligned RDMA message = one page-aligned write on the remote end).

### 3a.5.2 Adaptive Mode (`dispatch_mode = "adaptive"`, default)

Buffer arriving UDS bytes and flush to RDMA based on adaptive thresholds.

```
 App write(100B) → proxy read(100B) → buffer
 App write(50B)  → proxy read(50B)  → buffer (now 150B)
 App write(200B) → proxy read(200B) → buffer (now 350B)
 ...flush timer fires (25-100us)...
 → frame(370B) → RDMA SEND → frame(370B) → proxy write(350B)
```

**Adaptive flush timer** ([Section 13.3](13-performance.md#133-rdma-send-batching)): The flush timeout scales with observed throughput, from 0 up to a configurable ceiling (`max_flush_timeout_us`, default 100us):

| Message Rate | Flush Timeout (`max_flush_timeout_us = 100`) | Behavior |
|-------------|----------------------------------------------|----------|
| < 1,000/sec | 0 (immediate) | Converges to immediate mode under light load |
| 1,000–10,000/sec | 25us (25% of ceiling) | Light coalescing |
| 10,000–100,000/sec | 50us (50% of ceiling) | Moderate coalescing |
| > 100,000/sec | 100us (full ceiling) | Aggressive coalescing |

**Why microseconds**: RDMA hardware RTT is ~1-2us. Even the default 100us ceiling adds 50-100x the wire latency. Milliseconds would be far too coarse for an RDMA transport. Typical useful range: 50-200us. Users with latency-critical workloads who still want some coalescing can set `max_flush_timeout_us` as low as 10-25us.

**Additional flush triggers**:
- **Size threshold**: flush when accumulated payload reaches `size_threshold_pct`% of buffer slot capacity (default 75%)
- **Page alignment target**: on jumbo-frame networks (PMTU 4096), coalesce toward 4KB frames for DMA efficiency
- **Connection close**: flush immediately on FIN

**Trade-off**: Coalescing merges multiple UDS writes into a single RDMA message. Write boundaries are NOT preserved — the remote proxy delivers coalesced bytes in a single UDS write. This is semantically correct for `SOCK_STREAM` (same as TCP behavior) but differs from immediate mode.

**Best for**: Most workloads, especially small-write-heavy applications (databases, messaging systems, Redpanda). The adaptive timer converges to immediate behavior under low load, so latency-sensitive workloads with sparse writes see no penalty.

### 3a.5.3 Page-Aligned Batching

Within adaptive mode, the coalescing target should align with the kernel page size and RDMA PMTU:

- **Jumbo frames (PMTU 4096)**: Target 4076-byte payloads (4096 - 20 header). One frame = one RDMA packet = one 4KB page. Kalia et al. [6] demonstrate up to 12x speedup for page-aligned DMA operations.
- **Standard MTU (PMTU 1024)**: Target 1004-byte payloads (1024 - 20 header). One frame = one RDMA packet.

The 4KB page size on x86_64 and aarch64 aligning with the 4096-byte IB PMTU under jumbo frames is not coincidental — IB PMTU values were designed with DMA alignment in mind. The proxy exploits this alignment by sizing buffer pool slots at page boundaries (see [Section 5.4](05-rdma-transport.md#54-buffer-pool)).

### 3a.5.4 Inline Threshold (Future Consideration)

Inspired by RPC-over-RDMA's [4] dual-mode approach: below an inline threshold (~1KB), dispatch immediately even in adaptive mode; above the threshold, coalesce into page-aligned frames. This would combine latency benefits for small messages with throughput benefits for large ones. Could be a refinement of adaptive mode in v3+.

### 3a.5.5 Design Decision: Two Modes, Adaptive Default

| Aspect | Immediate | Adaptive (default) |
|--------|-----------|-------------------|
| Configuration | `dispatch_mode = "immediate"` | `dispatch_mode = "adaptive"` |
| Write boundaries | Preserved | Not preserved (coalesced) |
| Latency | Minimum | +0 to +100us (adaptive) |
| Throughput | Lower (more messages per byte) | Higher (fewer, larger messages) |
| CQ load | Higher | Lower |
| Header overhead | Higher for small writes | Amortized across coalesced bytes |
| Semantic model | Stronger than SOCK_STREAM | Same as SOCK_STREAM/TCP |

Both modes use the same frame format, pump loop, and RDMA transport — the only difference is when the pump flushes its buffer to RDMA. The configuration is a single TOML field and CLI flag ([Section 10.2](10-configuration.md#102-configuration-file-toml)).


## 3a.6 Page Alignment and Performance

Kalia, Kaminsky, and Andersen [6] provide systematic measurements of RDMA performance sensitivities in "Design Guidelines for High Performance RDMA Systems" (USENIX ATC 2016). Their key finding for buffer alignment:

- **Page-aligned buffers**: ~350 cycles per RDMA operation
- **8-byte-aligned buffers**: ~3,000 cycles per RDMA operation
- **Speedup**: approximately 12x for page-aligned access

**Why alignment matters**: NIC DMA engines and the IOMMU (I/O Memory Management Unit) operate in page granularity. A buffer that crosses a page boundary requires two TLB lookups, two scatter-gather entries, and potentially two DMA transactions. A page-aligned, page-sized buffer requires one of each.

**Alignment in our design**:
- Buffer pool slots ([Section 5.4](05-rdma-transport.md#54-buffer-pool)): sized at 4096 bytes (= page size = PMTU under jumbo frames), page-aligned within the huge-page-backed allocation
- Huge pages ([Section 13.5](13-performance.md)): 2MB huge pages reduce TLB pressure further — the entire buffer pool (256 × 4KB = 1MB) fits within a single 2MB huge page
- Frame payload sizing ([Section 4.6](04-framing-protocol.md#46-mtu-and-payload-sizing)): default 4076 bytes (4096 PMTU - 20 header), keeping the total frame within a single page

**Cross-architecture**: The 4KB default page size is shared by x86_64, aarch64, and riscv64 — all three target architectures for this project ([Section 19.4](19-project-structure.md)). The alignment strategy works uniformly across architectures.


## 3a.7 Stream Lifecycle Management

RFC 5040 Section 6 [1] defines **RDMAP Stream Management**: initialization after the LLP (Lower Layer Protocol) connection is established, transition to RDMA-enabled mode, and stream termination. Our proxy maps these concepts naturally to its flag-based lifecycle:

| RDMAP Concept | Our Proxy Equivalent | Mechanism |
|--------------|---------------------|-----------|
| Stream initialization | New stream setup | SYN flag ([Section 4.4](04-framing-protocol.md#44-flag-definitions)): allocate `stream_id`, establish QP set, exchange initial credits |
| RDMA-enabled mode | Data transfer | Normal frames (no lifecycle flags): payload data flowing through the pump |
| Graceful termination | Half-close | FIN flag: one side closes its write end, the other continues until it also sends FIN ([Section 7.2](07-bidirectional-pump.md#72-half-close-handling)) |
| Abortive termination | Error teardown | RST flag: immediate resource cleanup, no graceful shutdown |

**Key difference from RFC 5040**: RDMAP manages a single RDMA stream per TCP connection. Our proxy multiplexes **multiple streams** (via `stream_id`) over shared QPs (v3+), closer to SCTP's multi-stream model. Each stream has independent lifecycle management — one stream's FIN does not affect other streams sharing the same QPs.

The FIN-exchange protocol mirrors RDMAP's graceful stream teardown, with the addition of a configurable timeout (default 30s) to handle applications that never close their sockets ([Section 7.2](07-bidirectional-pump.md#72-half-close-handling)).


## 3a.8 Design Rationale

This section ties the preceding analysis back to the specific design choices in the framing protocol ([Section 4](04-framing-protocol.md)) and bidirectional pump ([Section 7](07-bidirectional-pump.md)).

**Why length-prefix framing**: Validated by MPA (RFC 5044) [3], RPC-over-RDMA (RFC 8166) [4], and decades of binary protocol design. O(1) parse cost — read 20-byte header, extract `payload_length`, consume that many bytes. Zero-copy friendly — payload follows header contiguously in a pre-registered buffer slot, no scanning or escaping. MPA's FPDU header serves an equivalent purpose; our 20-byte header is comparable in overhead.

**Why the specific 20-byte header**: Each field serves a purpose that cannot be removed:
- `stream_id` (4B): multiplexing multiple UDS connections over shared QPs (v3+)
- `sequence_number` (8B): in-order delivery across multiple QPs via the reorder buffer
- `flags` (2B): stream lifecycle (SYN/FIN/RST) and control (CREDIT/PING/PONG)
- `credits_granted` (2B): piggybacked flow control, avoiding separate credit-only messages
- `payload_length` (4B): the length-prefix that makes framing work

No field is speculative; all are used from v2 onward (`stream_id` becomes active in v3).

**Why two dispatch modes**: The throughput/latency trade-off is workload-dependent and cannot be resolved by a single strategy. Adaptive mode (default) converges to immediate behavior under low load and coalesces under high load — correct for most users. Immediate mode exists for power users who want minimum latency and/or write-boundary preservation. Both modes use the same frame format and transport — the only difference is flush timing.

**Why page-aligned buffer slots**: Kalia et al. [6] demonstrate the DMA performance impact. Our 4096-byte slots align with both the PMTU (jumbo frames) and the page size (x86_64/aarch64/riscv64), maximizing DMA and TLB efficiency. In immediate mode with 4KB writes, one write = one page-aligned RDMA message = one page-aligned delivery on the remote end — the optimal case.


## References

| # | Citation |
|---|---------|
| [1] | H. Shah et al., "A Remote Direct Memory Access Protocol Specification," RFC 5040, October 2007. https://www.rfc-editor.org/rfc/rfc5040 |
| [2] | H. Shah et al., "Direct Data Placement over Reliable Transports," RFC 5041, October 2007. https://www.rfc-editor.org/rfc/rfc5041 |
| [3] | P. Culley et al., "Marker PDU Aligned Framing for TCP Specification," RFC 5044, October 2007. https://www.rfc-editor.org/rfc/rfc5044 |
| [4] | C. Lever et al., "Remote Direct Memory Access Transport for RPC Version 1," RFC 8166, June 2017. https://www.rfc-editor.org/rfc/rfc8166 |
| [4a] | C. Lever, "Bidirectional Remote Procedure Call on RPC-over-RDMA Transports," RFC 8167, June 2017. https://www.rfc-editor.org/rfc/rfc8167 |
| [5] | M. Ko et al., "Internet Small Computer System Interface (iSCSI) Extensions for RDMA Specification," RFC 7145, April 2014. https://www.rfc-editor.org/rfc/rfc7145 |
| [6] | A. Kalia, M. Kaminsky, D. Andersen, "Design Guidelines for High Performance RDMA Systems," USENIX ATC '16, Denver, CO, June 2016. https://www.usenix.org/conference/atc16/technical-sessions/presentation/kalia |
| [7] | J. Nagle, "Congestion Control in IP/TCP Internetworks," RFC 896, January 1984. https://www.rfc-editor.org/rfc/rfc896 |


[Back to Design Overview](../DESIGN.md)
