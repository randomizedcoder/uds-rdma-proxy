/// Frame header size in bytes (20 bytes total).
pub const FRAME_HEADER_SIZE: usize = 20;

/// PING payload size in bytes.
pub const PING_PAYLOAD_SIZE: usize = 32;

/// PONG payload size in bytes.
pub const PONG_PAYLOAD_SIZE: usize = 48;

// -- Frame types --

/// UDS payload data.
pub const FRAME_TYPE_DATA: u8 = 0x00;

/// Flow control grants and QP management.
pub const FRAME_TYPE_CONTROL: u8 = 0x01;

/// Health probe (PING/PONG) with timestamps.
pub const FRAME_TYPE_PROBE: u8 = 0x02;

// -- Data frame flags (frame_type = 0x00) --

/// New stream (open connection).
pub const DATA_FLAG_SYN: u8 = 1 << 0;

/// Half-close (sender is done writing).
pub const DATA_FLAG_FIN: u8 = 1 << 1;

/// Abort (immediate teardown, error condition).
pub const DATA_FLAG_RST: u8 = 1 << 2;

/// Mask of all valid data flags.
pub const DATA_FLAGS_VALID: u8 = DATA_FLAG_SYN | DATA_FLAG_FIN | DATA_FLAG_RST;

// -- Control frame flags (frame_type = 0x01) --

/// Credit-only grant (payload_length = 0).
pub const CTRL_FLAG_CREDIT: u8 = 1 << 0;

/// Notify peer to remove QP (payload: qp_index u8).
pub const CTRL_FLAG_QP_DISABLE: u8 = 1 << 1;

/// Reserved for future QP re-addition.
pub const CTRL_FLAG_QP_ENABLE: u8 = 1 << 2;

/// Stream receive window update (payload: window_increment u32).
pub const CTRL_FLAG_STREAM_WINDOW: u8 = 1 << 3;

/// Authentication handshake frame (stream_id must be 0).
pub const CTRL_FLAG_AUTH: u8 = 1 << 4;

/// Mask of all valid control flags.
pub const CTRL_FLAGS_VALID: u8 = CTRL_FLAG_CREDIT
    | CTRL_FLAG_QP_DISABLE
    | CTRL_FLAG_QP_ENABLE
    | CTRL_FLAG_STREAM_WINDOW
    | CTRL_FLAG_AUTH;

// -- Probe frame flags (frame_type = 0x02) --

/// 0 = PING (request), 1 = PONG (response).
pub const PROBE_FLAG_PONG: u8 = 1 << 0;

/// Mask of all valid probe flags.
pub const PROBE_FLAGS_VALID: u8 = PROBE_FLAG_PONG;

// -- Clock flags for probe payloads --

/// Sender populated t_send_real with PTP-synced clock.
pub const CLOCK_HAS_REALTIME: u8 = 1 << 0;

/// Responder populated t_recv_real and t_pong_real with PTP-synced clock.
pub const CLOCK_HAS_RECV_REALTIME: u8 = 1 << 1;

// -- Stream ID conventions --

/// Reserved for connection-level control frames (credit grants, QP management, probes).
pub const STREAM_ID_CONTROL: u32 = 0;

// -- RoCEv2 overhead --

/// Total RoCEv2 header overhead subtracted from Ethernet MTU.
/// Ethernet(14) + IP(20) + UDP(8) + BTH(12) + ICRC(4) = 58 bytes.
/// However the design doc (Section 4.7) uses 44 bytes (Ethernet header excluded
/// from the calculation since it's below the IP MTU). We follow the design doc.
pub const ROCEV2_HEADER_OVERHEAD: usize = 44;

// -- Default parameters --

/// Default initial credits per QP.
pub const DEFAULT_INITIAL_CREDITS: u16 = 128;

/// Divisor for credit grant threshold (grant when pending >= initial / divisor).
pub const DEFAULT_CREDIT_THRESHOLD_DIVISOR: u16 = 4;

/// Default maximum buffered out-of-order frames per reorder buffer.
pub const DEFAULT_MAX_BUFFERED: usize = 64;

/// Default gap timeout in milliseconds before declaring a serious error.
pub const DEFAULT_GAP_TIMEOUT_MS: u64 = 100;

/// Default per-stream receive window in bytes.
pub const DEFAULT_STREAM_WINDOW_SIZE: u32 = 65536;
