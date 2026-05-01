use crate::constants::*;
use crate::error::ProtocolError;

/// Decoded frame header. All fields are native Rust types.
///
/// Wire format is 20 bytes little-endian:
/// ```text
/// Offset  Field              Type   Size
/// 0-3     stream_id          u32    4B
/// 4-11    sequence_number    u64    8B
/// 12      frame_type         u8     1B
/// 13      flags              u8     1B
/// 14-15   credits_granted    u16    2B
/// 16-19   payload_length     u32    4B
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameHeader {
    pub stream_id: u32,
    pub sequence_number: u64,
    pub frame_type: u8,
    pub flags: u8,
    pub credits_granted: u16,
    pub payload_length: u32,
}

impl FrameHeader {
    /// Encode this header into `buf` as 20 bytes, little-endian.
    pub fn encode(&self, buf: &mut [u8]) -> Result<(), ProtocolError> {
        if buf.len() < FRAME_HEADER_SIZE {
            return Err(ProtocolError::BufferTooShort {
                need: FRAME_HEADER_SIZE,
                have: buf.len(),
            });
        }
        buf[0..4].copy_from_slice(&self.stream_id.to_le_bytes());
        buf[4..12].copy_from_slice(&self.sequence_number.to_le_bytes());
        buf[12] = self.frame_type;
        buf[13] = self.flags;
        buf[14..16].copy_from_slice(&self.credits_granted.to_le_bytes());
        buf[16..20].copy_from_slice(&self.payload_length.to_le_bytes());
        Ok(())
    }

    /// Decode a header from the first 20 bytes of `buf`, little-endian.
    pub fn decode(buf: &[u8]) -> Result<Self, ProtocolError> {
        if buf.len() < FRAME_HEADER_SIZE {
            return Err(ProtocolError::BufferTooShort {
                need: FRAME_HEADER_SIZE,
                have: buf.len(),
            });
        }
        let frame_type = buf[12];
        if frame_type > FRAME_TYPE_PROBE {
            return Err(ProtocolError::InvalidFrameType(frame_type));
        }
        Ok(Self {
            stream_id: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
            sequence_number: u64::from_le_bytes([
                buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
            ]),
            frame_type,
            flags: buf[13],
            credits_granted: u16::from_le_bytes([buf[14], buf[15]]),
            payload_length: u32::from_le_bytes([buf[16], buf[17], buf[18], buf[19]]),
        })
    }

    /// Validate that flags are consistent with frame_type.
    /// Returns `Ok(())` if valid, `Err(InvalidFlags)` if reserved bits are set.
    pub fn validate_flags(&self) -> Result<(), ProtocolError> {
        let valid_mask = match self.frame_type {
            FRAME_TYPE_DATA => DATA_FLAGS_VALID,
            FRAME_TYPE_CONTROL => CTRL_FLAGS_VALID,
            FRAME_TYPE_PROBE => PROBE_FLAGS_VALID,
            _ => return Err(ProtocolError::InvalidFrameType(self.frame_type)),
        };
        if self.flags & !valid_mask != 0 {
            return Err(ProtocolError::InvalidFlags {
                frame_type: self.frame_type,
                flags: self.flags,
            });
        }
        Ok(())
    }

    #[inline]
    pub fn is_data(&self) -> bool {
        self.frame_type == FRAME_TYPE_DATA
    }

    #[inline]
    pub fn is_control(&self) -> bool {
        self.frame_type == FRAME_TYPE_CONTROL
    }

    #[inline]
    pub fn is_probe(&self) -> bool {
        self.frame_type == FRAME_TYPE_PROBE
    }

    #[inline]
    pub fn has_syn(&self) -> bool {
        self.flags & DATA_FLAG_SYN != 0
    }

    #[inline]
    pub fn has_fin(&self) -> bool {
        self.flags & DATA_FLAG_FIN != 0
    }

    #[inline]
    pub fn has_rst(&self) -> bool {
        self.flags & DATA_FLAG_RST != 0
    }

    #[inline]
    pub fn is_pong(&self) -> bool {
        self.flags & PROBE_FLAG_PONG != 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(header: &FrameHeader) {
        let mut buf = [0u8; FRAME_HEADER_SIZE];
        header.encode(&mut buf).unwrap();
        let decoded = FrameHeader::decode(&buf).unwrap();
        assert_eq!(*header, decoded);
    }

    // -- Roundtrip encode/decode --

    #[test]
    fn roundtrip_data_frame() {
        roundtrip(&FrameHeader {
            stream_id: 42,
            sequence_number: 1000,
            frame_type: FRAME_TYPE_DATA,
            flags: 0,
            credits_granted: 16,
            payload_length: 4096,
        });
    }

    #[test]
    fn roundtrip_control_credit() {
        roundtrip(&FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_CONTROL,
            flags: CTRL_FLAG_CREDIT,
            credits_granted: 64,
            payload_length: 0,
        });
    }

    #[test]
    fn roundtrip_probe_ping() {
        roundtrip(&FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_PROBE,
            flags: 0,
            credits_granted: 0,
            payload_length: PING_PAYLOAD_SIZE as u32,
        });
    }

    #[test]
    fn roundtrip_probe_pong() {
        roundtrip(&FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_PROBE,
            flags: PROBE_FLAG_PONG,
            credits_granted: 0,
            payload_length: PONG_PAYLOAD_SIZE as u32,
        });
    }

    // -- All flag combinations --

    #[test]
    fn data_flag_syn() {
        let h = FrameHeader {
            stream_id: 1,
            sequence_number: 0,
            frame_type: FRAME_TYPE_DATA,
            flags: DATA_FLAG_SYN,
            credits_granted: 0,
            payload_length: 100,
        };
        assert!(h.has_syn());
        assert!(!h.has_fin());
        assert!(!h.has_rst());
        roundtrip(&h);
    }

    #[test]
    fn data_flag_fin() {
        let h = FrameHeader {
            stream_id: 1,
            sequence_number: 5,
            frame_type: FRAME_TYPE_DATA,
            flags: DATA_FLAG_FIN,
            credits_granted: 0,
            payload_length: 0,
        };
        assert!(h.has_fin());
        roundtrip(&h);
    }

    #[test]
    fn data_flag_rst() {
        let h = FrameHeader {
            stream_id: 3,
            sequence_number: 10,
            frame_type: FRAME_TYPE_DATA,
            flags: DATA_FLAG_RST,
            credits_granted: 0,
            payload_length: 0,
        };
        assert!(h.has_rst());
        roundtrip(&h);
    }

    #[test]
    fn data_flag_syn_fin() {
        let h = FrameHeader {
            stream_id: 1,
            sequence_number: 0,
            frame_type: FRAME_TYPE_DATA,
            flags: DATA_FLAG_SYN | DATA_FLAG_FIN,
            credits_granted: 0,
            payload_length: 50,
        };
        assert!(h.has_syn());
        assert!(h.has_fin());
        roundtrip(&h);
    }

    #[test]
    fn control_all_flags() {
        for flag in [
            CTRL_FLAG_CREDIT,
            CTRL_FLAG_QP_DISABLE,
            CTRL_FLAG_QP_ENABLE,
            CTRL_FLAG_STREAM_WINDOW,
            CTRL_FLAG_AUTH,
        ] {
            let h = FrameHeader {
                stream_id: 0,
                sequence_number: 0,
                frame_type: FRAME_TYPE_CONTROL,
                flags: flag,
                credits_granted: 0,
                payload_length: 0,
            };
            assert!(h.is_control());
            h.validate_flags().unwrap();
            roundtrip(&h);
        }
    }

    #[test]
    fn probe_ping_pong() {
        let ping = FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_PROBE,
            flags: 0,
            credits_granted: 0,
            payload_length: PING_PAYLOAD_SIZE as u32,
        };
        assert!(!ping.is_pong());

        let pong = FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_PROBE,
            flags: PROBE_FLAG_PONG,
            credits_granted: 0,
            payload_length: PONG_PAYLOAD_SIZE as u32,
        };
        assert!(pong.is_pong());
    }

    // -- Boundary: zero and max payload --

    #[test]
    fn zero_payload_control() {
        roundtrip(&FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_CONTROL,
            flags: CTRL_FLAG_CREDIT,
            credits_granted: 128,
            payload_length: 0,
        });
    }

    #[test]
    fn max_payload_length() {
        roundtrip(&FrameHeader {
            stream_id: 1,
            sequence_number: u64::MAX,
            frame_type: FRAME_TYPE_DATA,
            flags: 0,
            credits_granted: u16::MAX,
            payload_length: u32::MAX,
        });
    }

    #[test]
    fn max_values_all_fields() {
        roundtrip(&FrameHeader {
            stream_id: u32::MAX,
            sequence_number: u64::MAX,
            frame_type: FRAME_TYPE_PROBE,
            flags: PROBE_FLAG_PONG,
            credits_granted: u16::MAX,
            payload_length: u32::MAX,
        });
    }

    // -- Negative: buffer too short --

    #[test]
    fn encode_buffer_too_short() {
        let h = FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_DATA,
            flags: 0,
            credits_granted: 0,
            payload_length: 0,
        };
        let mut buf = [0u8; 19];
        assert_eq!(
            h.encode(&mut buf),
            Err(ProtocolError::BufferTooShort { need: 20, have: 19 })
        );
    }

    #[test]
    fn decode_empty_buffer() {
        assert_eq!(
            FrameHeader::decode(&[]),
            Err(ProtocolError::BufferTooShort { need: 20, have: 0 })
        );
    }

    #[test]
    fn decode_one_byte() {
        assert_eq!(
            FrameHeader::decode(&[0]),
            Err(ProtocolError::BufferTooShort { need: 20, have: 1 })
        );
    }

    #[test]
    fn decode_nineteen_bytes() {
        assert_eq!(
            FrameHeader::decode(&[0u8; 19]),
            Err(ProtocolError::BufferTooShort { need: 20, have: 19 })
        );
    }

    // -- Negative: invalid frame type --

    #[test]
    fn decode_invalid_frame_type_0x03() {
        let mut buf = [0u8; 20];
        buf[12] = 0x03;
        assert_eq!(
            FrameHeader::decode(&buf),
            Err(ProtocolError::InvalidFrameType(0x03))
        );
    }

    #[test]
    fn decode_invalid_frame_type_0xff() {
        let mut buf = [0u8; 20];
        buf[12] = 0xFF;
        assert_eq!(
            FrameHeader::decode(&buf),
            Err(ProtocolError::InvalidFrameType(0xFF))
        );
    }

    // -- Negative: reserved flag bits --

    #[test]
    fn validate_data_reserved_flags() {
        let h = FrameHeader {
            stream_id: 1,
            sequence_number: 0,
            frame_type: FRAME_TYPE_DATA,
            flags: 0x80, // bit 7 set
            credits_granted: 0,
            payload_length: 0,
        };
        assert_eq!(
            h.validate_flags(),
            Err(ProtocolError::InvalidFlags {
                frame_type: FRAME_TYPE_DATA,
                flags: 0x80
            })
        );
    }

    #[test]
    fn validate_control_reserved_flags() {
        let h = FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_CONTROL,
            flags: 0x80,
            credits_granted: 0,
            payload_length: 0,
        };
        assert!(h.validate_flags().is_err());
    }

    #[test]
    fn validate_probe_reserved_flags() {
        let h = FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_PROBE,
            flags: 0x02, // bit 1 is reserved for probe
            credits_granted: 0,
            payload_length: 0,
        };
        assert!(h.validate_flags().is_err());
    }

    // -- Encoding correctness (verify exact byte layout) --

    #[test]
    fn encode_byte_layout() {
        let h = FrameHeader {
            stream_id: 0x04030201,
            sequence_number: 0x0807060504030201,
            frame_type: FRAME_TYPE_DATA,
            flags: DATA_FLAG_SYN,
            credits_granted: 0x0201,
            payload_length: 0x04030201,
        };
        let mut buf = [0u8; 20];
        h.encode(&mut buf).unwrap();
        // stream_id LE
        assert_eq!(&buf[0..4], &[0x01, 0x02, 0x03, 0x04]);
        // sequence_number LE
        assert_eq!(
            &buf[4..12],
            &[0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]
        );
        // frame_type
        assert_eq!(buf[12], 0x00);
        // flags
        assert_eq!(buf[13], 0x01);
        // credits_granted LE
        assert_eq!(&buf[14..16], &[0x01, 0x02]);
        // payload_length LE
        assert_eq!(&buf[16..20], &[0x01, 0x02, 0x03, 0x04]);
    }

    // -- Type helpers --

    #[test]
    fn type_helpers() {
        let data = FrameHeader {
            stream_id: 0,
            sequence_number: 0,
            frame_type: FRAME_TYPE_DATA,
            flags: 0,
            credits_granted: 0,
            payload_length: 0,
        };
        assert!(data.is_data());
        assert!(!data.is_control());
        assert!(!data.is_probe());

        let ctrl = FrameHeader {
            frame_type: FRAME_TYPE_CONTROL,
            ..data
        };
        assert!(ctrl.is_control());

        let probe = FrameHeader {
            frame_type: FRAME_TYPE_PROBE,
            ..data
        };
        assert!(probe.is_probe());
    }
}
