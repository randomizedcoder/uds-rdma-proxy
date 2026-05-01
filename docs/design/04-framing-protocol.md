# Framing Protocol

## 4.1 Why Framing Is Needed

UDS uses `SOCK_STREAM` (byte stream semantics) while RDMA SEND/RECV is message-oriented. The proxy must delimit messages so the receiver knows where one UDS write ends and the next begins. Framing is also required for:

- **Stream multiplexing** (v3+): `stream_id` identifies which UDS connection a frame belongs to
- **Flow control**: Credit grant messages need to be distinguishable from data
- **Connection lifecycle**: SYN/FIN/RST signaling
- **Multi-QP reordering**: Sequence numbers for in-order delivery

> **See also**: [Section 3a — Stream-to-Message Adaptation](03a-stream-message-adaptation.md) provides the theoretical foundation for framing: the stream vs. message semantic gap, a survey of framing techniques (length-prefix, delimiter, TLV, MPA markers), prior art from iWARP (RFC 5044), RPC-over-RDMA (RFC 8166), and iSER (RFC 7145), message sizing strategies (immediate vs. adaptive dispatch), and the rationale for our length-prefix design.

## 4.2 Wire Format

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                          stream_id                            |  4 bytes
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                       sequence_number                         |  8 bytes
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  frame_type   |     flags     |       credits_granted         |  4 bytes
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                       payload_length                          |  4 bytes
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                       payload (0..N bytes)                    |
 |                                                               |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 Total header size: 20 bytes
```

## 4.3 Field Definitions

| Field | Type | Description |
|-------|------|-------------|
| `stream_id` | `u32` | Identifies which UDS connection (stream) this frame belongs to. `0` is reserved for connection-level control frames (QP management, health probes). Non-zero values identify multiplexed streams. See [Section 9](09-connection-multiplexing.md). |
| `sequence_number` | `u64` | Monotonic counter per-direction, assigned by sender before QP selection. Used by receiver's reorder buffer for in-order delivery. **v2**: global scope (one counter per connection). **v3**: per-stream scope (one counter per `stream_id`), enabling independent per-stream reorder buffers without cross-stream HOL blocking. See [Section 9.6](09-connection-multiplexing.md#96-per-stream-sequence-numbers-and-reorder-buffers). Probe frames use a separate per-QP `probe_seq` in the payload and do not consume sequence numbers. |
| `frame_type` | `u8` | Discriminates the frame category. Determines how `flags` and `payload` are interpreted. See [Section 4.4](#44-frame-types). |
| `flags` | `u8` | Type-specific bitfield. Interpretation depends on `frame_type`. See [Section 4.5](#45-per-type-flag-definitions). |
| `credits_granted` | `u16` | Number of receive buffer credits being granted to the peer. Present in ALL frame types — data frames piggyback credits to avoid separate credit messages. |
| `payload_length` | `u32` | Length of payload in bytes. `0` is valid (control-only frames). Max value determined by buffer slot size. |
| `payload` | `[u8]` | Contents depend on `frame_type`: raw UDS bytes for Data, type-specific for Control and Probe. |

## 4.4 Frame Types

The `frame_type` field discriminates between fundamentally different frame categories, each with its own payload format and flag semantics. This is the same pattern used by SRT (Secure Reliable Transport), which separates data packets from control packets (ACK, NAK, keepalive) to enable clean payload dispatch.

| `frame_type` | Name | Description | Payload format |
|-------------|------|-------------|----------------|
| `0x00` | `DATA` | UDS payload data. The common case (~99% of frames). | Raw bytes from UDS read. No encoding. |
| `0x01` | `CONTROL` | Flow control grants and QP management. | Type-specific; see flags. |
| `0x02` | `PROBE` | Health probe (PING/PONG) with timestamps. | `PingPayload` (32B) or `PongPayload` (48B). See [Section 8a](08a-qp-health-probes.md). |
| `0x03-0xFF` | Reserved | Must not be sent. Receiver should log and discard. | — |

## 4.5 Per-Type Flag Definitions

The `flags` field (8 bits) is interpreted according to `frame_type`:

#### Data Flags (`frame_type = 0x00`)

```
 Bit 0: SYN  - New stream (open connection)
 Bit 1: FIN  - Half-close (sender is done writing)
 Bit 2: RST  - Abort (immediate teardown, error condition)
 Bits 3-7: Reserved (must be zero)
```

#### Control Flags (`frame_type = 0x01`)

```
 Bit 0: CREDIT         - Credit-only grant (payload_length = 0)
 Bit 1: QP_DISABLE     - Notify peer to remove a QP from active set (payload: qp_index u8)
 Bit 2: QP_ENABLE      - Reserved for future QP re-addition
 Bit 3: STREAM_WINDOW  - Stream receive window update (stream_id > 0, payload: window_increment u32)
 Bit 4: AUTH            - Authentication handshake frame (stream_id must be 0). Used for
                          Tier 1 certificate exchange post-QP-establishment. Payload:
                          { step: u8, data: [u8] } where step=1 (cert_chain), step=2
                          (challenge_response), step=3 (auth_complete). See Section 17.7.
 Bits 5-7: Reserved (must be zero)
```

#### Probe Flags (`frame_type = 0x02`)

```
 Bit 0: PONG  - 0 = PING (health probe request), 1 = PONG (health probe response)
 Bits 1-7: Reserved (must be zero)
```

See [Section 8a — QP Health Probes](08a-qp-health-probes.md) for the PING/PONG payload wire format (32-byte PING, 48-byte PONG), the per-QP probe state machine, RTT and one-way latency measurement, and qualifying criteria.

## 4.6 Design Decisions

- **Frame type field**: The previous design used a flat `u16` flags field where SYN/FIN/RST, CREDIT, PING/PONG, and QP management all shared one namespace. This required checking multiple flag combinations to determine the payload format. Splitting into `u8 frame_type` + `u8 flags` makes dispatch explicit — the parser reads one byte to know the frame category, then interprets flags and payload accordingly. The split occupies the same 2 bytes at the same offset, so the header remains 20 bytes. With `u8` for frame_type, there are 256 possible types (3 used, 253 reserved for future evolution). Each type gets 8 flag bits, which is sufficient — Data uses 3 (SYN/FIN/RST), Control uses 3 (CREDIT/QP_DISABLE/QP_ENABLE), Probe uses 1 (PONG).
- **Byte order**: Little-endian. Matches x86/ARM native order, avoiding conversion overhead on the dominant architectures. This is a private protocol between two proxy instances; interoperability with big-endian systems is not a concern.
- **Maximum payload size**: Configurable, default 4,076 bytes (chosen to fit a 4,096-byte PMTU with the 20-byte header). See [Section 4.7](#47-mtu-and-payload-sizing) for the relationship between Ethernet MTU, IB PMTU, and payload sizing.
- **Zero-copy encoding**: The frame header and payload are written contiguously into a pre-registered buffer slot. The encoder writes the 20-byte header, then the UDS `read()` fills the payload region directly. No separate allocation or copy.
- **Credit piggybacking**: The `credits_granted` field is present in ALL frame types, allowing the receiver to piggyback credit grants on data frames and avoiding the overhead of separate credit-only messages in the common case. When the receiver has no data to send but needs to grant credits, it sends a Control frame with the `CREDIT` flag and zero-length payload.

## 4.7 MTU and Payload Sizing

The frame's maximum payload size should be chosen so that the total frame (20-byte header + payload) fits within a single RDMA packet on the wire. Frames that exceed the IB Path MTU (PMTU) still work — the NIC transparently segments them into multiple packets — but at a cost.

### RoCEv2 Per-Packet Header Overhead

Every RoCEv2 packet on the wire carries these headers before the IB payload:

```
 | Ethernet (14B) | IP (20B) | UDP (8B) | BTH (12B) | IB Payload | ICRC (4B) |
                   |<-------------- Ethernet MTU ---------------------->|
```

| Header | Bytes | Notes |
|--------|-------|-------|
| Ethernet | 14 | 18 with 802.1Q VLAN tag (but VLAN tag is outside the MTU) |
| IPv4 | 20 | |
| UDP | 8 | Destination port 4791 (RoCEv2) |
| IB BTH | 12 | Base Transport Header (RC SEND — no additional IB headers) |
| ICRC | 4 | Invariant CRC covering IB headers + payload |
| **Total overhead** | **44** | Subtracted from Ethernet MTU to get max IB payload per packet |

### Ethernet MTU → IB PMTU → Max Frame Payload

RDMA uses discrete Path MTU values (256, 512, 1024, 2048, 4096). The QP's PMTU is set to the largest standard value whose packet fits within the Ethernet MTU after headers:

| Ethernet MTU | Scenario | Available (MTU − 44) | IB PMTU | Max frame payload (PMTU − 20) |
|-------------|----------|----------------------|---------|-------------------------------|
| 1496 | VLAN-tagged, conservative | 1452 | 1024 | **1004** |
| 1500 | Default Ethernet | 1456 | 1024 | **1004** |
| 9000 | Jumbo frames | 8956 | 4096 | **4076** |
| 9216 | Common jumbo ceiling | 9172 | 4096 | **4076** |

### Impact of Oversized Frames

When a frame exceeds the PMTU, the RDMA layer segments it into multiple IB packets and the receiver reassembles before delivering a single completion. This is transparent but has costs:

| Impact | Explanation |
|--------|-------------|
| **Higher latency** | Message is not complete until the last segment arrives. A 4096-byte frame at PMTU 1024 requires 4 packets. |
| **More header overhead** | Each additional packet carries 44 bytes of RoCEv2 headers. 4 packets × 44B = 176B overhead vs 44B for a single packet. |
| **Larger retransmission blast radius** | RC uses Go-Back-N retransmission. If segment 2 of 4 is dropped, segments 2–4 are all retransmitted. More segments per message means more wasted bandwidth on any single loss. |
| **Higher NIC resource usage** | More WQEs consumed internally per application-level message. |

### Recommended Configuration

| Network | `max_payload_size` | `buffer_size` | Rationale |
|---------|-------------------|---------------|-----------|
| MTU 1500 (default / misconfigured) | 1004 | 1024 | Single-packet frames. No segmentation overhead. |
| MTU 9000–9216 (properly configured) | 4076 | 4096 | Single-packet frames at PMTU 4096. Good balance of throughput and latency. |
| MTU 9000+ (bulk throughput) | 8192–65536 | 8192–65536 | Accepts 2–16× segmentation for higher per-frame throughput. Useful when batching many small UDS writes into one large frame. |

The proxy should auto-detect the active PMTU from the QP attributes after connection establishment (`ibv_query_qp`) and warn if `max_payload_size + 20 > PMTU`, since this means every data frame will be multi-packet on the wire.

## 4.8 Rust Representation

```rust
#[repr(C, packed)]
struct FrameHeader {
    stream_id: u32,
    sequence_number: u64,
    frame_type: u8,
    flags: u8,
    credits_granted: u16,
    payload_length: u32,
}

const FRAME_HEADER_SIZE: usize = 20;

// Frame types
const FRAME_TYPE_DATA: u8    = 0x00;
const FRAME_TYPE_CONTROL: u8 = 0x01;
const FRAME_TYPE_PROBE: u8   = 0x02;

// Data flags (frame_type = DATA)
const DATA_FLAG_SYN: u8 = 1 << 0;
const DATA_FLAG_FIN: u8 = 1 << 1;
const DATA_FLAG_RST: u8 = 1 << 2;

// Control flags (frame_type = CONTROL)
const CTRL_FLAG_CREDIT: u8        = 1 << 0;
const CTRL_FLAG_QP_DISABLE: u8    = 1 << 1;
const CTRL_FLAG_QP_ENABLE: u8     = 1 << 2;
const CTRL_FLAG_STREAM_WINDOW: u8 = 1 << 3;
const CTRL_FLAG_AUTH: u8          = 1 << 4;  // Tier 1 cert exchange (Section 17.7)

// Probe flags (frame_type = PROBE)
const PROBE_FLAG_PONG: u8 = 1 << 0;  // 0 = PING, 1 = PONG
```


[Back to Design Overview](../DESIGN.md)
