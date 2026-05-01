use core::fmt;

/// Protocol errors for the UDS-RDMA proxy wire format.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProtocolError {
    /// Buffer is too short for the operation.
    BufferTooShort { need: usize, have: usize },

    /// Unrecognized frame type.
    InvalidFrameType(u8),

    /// Reserved flag bits are set.
    InvalidFlags { frame_type: u8, flags: u8 },

    /// Duplicate sequence number in reorder buffer.
    ReorderDuplicate { seq: u64 },

    /// Reorder buffer is at capacity.
    ReorderFull { max_buffered: usize },

    /// No send credits remaining.
    CreditExhausted,
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BufferTooShort { need, have } => {
                write!(f, "buffer too short: need {need} bytes, have {have}")
            }
            Self::InvalidFrameType(t) => write!(f, "invalid frame type: 0x{t:02x}"),
            Self::InvalidFlags { frame_type, flags } => {
                write!(
                    f,
                    "invalid flags 0x{flags:02x} for frame type 0x{frame_type:02x}"
                )
            }
            Self::ReorderDuplicate { seq } => write!(f, "duplicate sequence number: {seq}"),
            Self::ReorderFull { max_buffered } => {
                write!(f, "reorder buffer full (max {max_buffered})")
            }
            Self::CreditExhausted => write!(f, "no send credits remaining"),
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for ProtocolError {}
