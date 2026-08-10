use crate::constants::*;
use crate::error::ProtocolError;

/// PING payload sent as a health probe request (32 bytes).
///
/// See design doc Section 8a.2 for the wire format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PingPayload {
    pub probe_seq: u32,
    pub qp_index: u16,
    pub clock_flags: u8,
    pub reserved: u8,
    pub t_send_mono: u64,
    pub t_send_real: u64,
    pub padding: u64,
}

/// PONG payload sent as a health probe response (48 bytes).
///
/// Echoes the PING fields and adds responder timestamps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PongPayload {
    pub probe_seq: u32,
    pub qp_index: u16,
    pub clock_flags: u8,
    pub reserved: u8,
    pub t_send_mono: u64,
    pub t_send_real: u64,
    pub t_recv_real: u64,
    pub t_pong_mono: u64,
    pub t_pong_real: u64,
}

impl PingPayload {
    /// Encode this PING payload into `buf` as 32 bytes, little-endian.
    pub fn encode(&self, buf: &mut [u8]) -> Result<(), ProtocolError> {
        if buf.len() < PING_PAYLOAD_SIZE {
            return Err(ProtocolError::BufferTooShort {
                need: PING_PAYLOAD_SIZE,
                have: buf.len(),
            });
        }
        buf[0..4].copy_from_slice(&self.probe_seq.to_le_bytes());
        buf[4..6].copy_from_slice(&self.qp_index.to_le_bytes());
        buf[6] = self.clock_flags;
        buf[7] = self.reserved;
        buf[8..16].copy_from_slice(&self.t_send_mono.to_le_bytes());
        buf[16..24].copy_from_slice(&self.t_send_real.to_le_bytes());
        buf[24..32].copy_from_slice(&self.padding.to_le_bytes());
        Ok(())
    }

    /// Decode a PING payload from the first 32 bytes of `buf`.
    pub fn decode(buf: &[u8]) -> Result<Self, ProtocolError> {
        if buf.len() < PING_PAYLOAD_SIZE {
            return Err(ProtocolError::BufferTooShort {
                need: PING_PAYLOAD_SIZE,
                have: buf.len(),
            });
        }
        Ok(Self {
            probe_seq: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
            qp_index: u16::from_le_bytes([buf[4], buf[5]]),
            clock_flags: buf[6],
            reserved: buf[7],
            t_send_mono: u64::from_le_bytes([
                buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
            ]),
            t_send_real: u64::from_le_bytes([
                buf[16], buf[17], buf[18], buf[19], buf[20], buf[21], buf[22], buf[23],
            ]),
            padding: u64::from_le_bytes([
                buf[24], buf[25], buf[26], buf[27], buf[28], buf[29], buf[30], buf[31],
            ]),
        })
    }
}

impl PongPayload {
    /// Encode this PONG payload into `buf` as 48 bytes, little-endian.
    pub fn encode(&self, buf: &mut [u8]) -> Result<(), ProtocolError> {
        if buf.len() < PONG_PAYLOAD_SIZE {
            return Err(ProtocolError::BufferTooShort {
                need: PONG_PAYLOAD_SIZE,
                have: buf.len(),
            });
        }
        buf[0..4].copy_from_slice(&self.probe_seq.to_le_bytes());
        buf[4..6].copy_from_slice(&self.qp_index.to_le_bytes());
        buf[6] = self.clock_flags;
        buf[7] = self.reserved;
        buf[8..16].copy_from_slice(&self.t_send_mono.to_le_bytes());
        buf[16..24].copy_from_slice(&self.t_send_real.to_le_bytes());
        buf[24..32].copy_from_slice(&self.t_recv_real.to_le_bytes());
        buf[32..40].copy_from_slice(&self.t_pong_mono.to_le_bytes());
        buf[40..48].copy_from_slice(&self.t_pong_real.to_le_bytes());
        Ok(())
    }

    /// Decode a PONG payload from the first 48 bytes of `buf`.
    pub fn decode(buf: &[u8]) -> Result<Self, ProtocolError> {
        if buf.len() < PONG_PAYLOAD_SIZE {
            return Err(ProtocolError::BufferTooShort {
                need: PONG_PAYLOAD_SIZE,
                have: buf.len(),
            });
        }
        Ok(Self {
            probe_seq: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
            qp_index: u16::from_le_bytes([buf[4], buf[5]]),
            clock_flags: buf[6],
            reserved: buf[7],
            t_send_mono: u64::from_le_bytes([
                buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
            ]),
            t_send_real: u64::from_le_bytes([
                buf[16], buf[17], buf[18], buf[19], buf[20], buf[21], buf[22], buf[23],
            ]),
            t_recv_real: u64::from_le_bytes([
                buf[24], buf[25], buf[26], buf[27], buf[28], buf[29], buf[30], buf[31],
            ]),
            t_pong_mono: u64::from_le_bytes([
                buf[32], buf[33], buf[34], buf[35], buf[36], buf[37], buf[38], buf[39],
            ]),
            t_pong_real: u64::from_le_bytes([
                buf[40], buf[41], buf[42], buf[43], buf[44], buf[45], buf[46], buf[47],
            ]),
        })
    }

    /// Construct a PONG by echoing PING fields and adding responder timestamps.
    pub fn from_ping(
        ping: &PingPayload,
        t_recv_real: u64,
        t_pong_mono: u64,
        t_pong_real: u64,
        has_recv_realtime: bool,
    ) -> Self {
        let mut clock_flags = ping.clock_flags;
        if has_recv_realtime {
            clock_flags |= CLOCK_HAS_RECV_REALTIME;
        }
        Self {
            probe_seq: ping.probe_seq,
            qp_index: ping.qp_index,
            clock_flags,
            reserved: ping.reserved,
            t_send_mono: ping.t_send_mono,
            t_send_real: ping.t_send_real,
            t_recv_real,
            t_pong_mono,
            t_pong_real,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_ping() -> PingPayload {
        PingPayload {
            probe_seq: 42,
            qp_index: 3,
            clock_flags: CLOCK_HAS_REALTIME,
            reserved: 0,
            t_send_mono: 1_000_000_000,
            t_send_real: 1_700_000_000_000_000_000,
            padding: 0,
        }
    }

    #[test]
    fn ping_roundtrip() {
        let ping = sample_ping();
        let mut buf = [0u8; PING_PAYLOAD_SIZE];
        ping.encode(&mut buf).unwrap();
        let decoded = PingPayload::decode(&buf).unwrap();
        assert_eq!(ping, decoded);
    }

    #[test]
    fn pong_roundtrip() {
        let pong = PongPayload {
            probe_seq: 42,
            qp_index: 3,
            clock_flags: CLOCK_HAS_REALTIME | CLOCK_HAS_RECV_REALTIME,
            reserved: 0,
            t_send_mono: 1_000_000_000,
            t_send_real: 1_700_000_000_000_000_000,
            t_recv_real: 1_700_000_000_000_001_000,
            t_pong_mono: 1_000_001_000,
            t_pong_real: 1_700_000_000_000_001_500,
        };
        let mut buf = [0u8; PONG_PAYLOAD_SIZE];
        pong.encode(&mut buf).unwrap();
        let decoded = PongPayload::decode(&buf).unwrap();
        assert_eq!(pong, decoded);
    }

    #[test]
    fn pong_from_ping() {
        let ping = sample_ping();
        let pong = PongPayload::from_ping(&ping, 500, 1000, 1500, true);
        // Echoed fields
        assert_eq!(pong.probe_seq, ping.probe_seq);
        assert_eq!(pong.qp_index, ping.qp_index);
        assert_eq!(pong.t_send_mono, ping.t_send_mono);
        assert_eq!(pong.t_send_real, ping.t_send_real);
        // Responder fields
        assert_eq!(pong.t_recv_real, 500);
        assert_eq!(pong.t_pong_mono, 1000);
        assert_eq!(pong.t_pong_real, 1500);
    }

    #[test]
    fn pong_from_ping_clock_flags() {
        let ping = PingPayload {
            clock_flags: CLOCK_HAS_REALTIME,
            ..sample_ping()
        };
        let pong = PongPayload::from_ping(&ping, 0, 0, 0, true);
        assert_eq!(
            pong.clock_flags,
            CLOCK_HAS_REALTIME | CLOCK_HAS_RECV_REALTIME
        );

        let pong_no_rt = PongPayload::from_ping(&ping, 0, 0, 0, false);
        assert_eq!(pong_no_rt.clock_flags, CLOCK_HAS_REALTIME);
    }

    #[test]
    fn ping_buffer_too_short() {
        assert_eq!(
            PingPayload::decode(&[0u8; 31]),
            Err(ProtocolError::BufferTooShort { need: 32, have: 31 })
        );
    }

    #[test]
    fn pong_buffer_too_short() {
        assert_eq!(
            PongPayload::decode(&[0u8; 47]),
            Err(ProtocolError::BufferTooShort { need: 48, have: 47 })
        );
    }
}
