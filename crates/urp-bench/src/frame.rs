//! 24-byte little-endian wire header + deterministic payload (§30.5).

use crate::Error;

pub const HDR_SIZE: usize = 24;
/// `"URPB"` when LE-encoded.
pub const MAGIC: u32 = 0x4250_5255;
pub const VERSION: u8 = 1;
/// Absolute payload cap, 1 MiB.
pub const PAYLOAD_MAX: u32 = 1 << 20;
pub const MSG_MAX: u32 = HDR_SIZE as u32 + PAYLOAD_MAX;

pub const FLAG_ECHO: u8 = 1 << 0;
pub const FLAG_FIN: u8 = 1 << 1;
pub const FLAG_MASK: u8 = FLAG_ECHO | FLAG_FIN;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Hdr {
    pub magic: u32,
    pub version: u8,
    pub flags: u8,
    pub origin_id: u16,
    pub payload_len: u32,
    pub seq: u32,
    pub t_send_ns: u64,
}

impl Hdr {
    /// A valid header for the given fields (magic/version filled in).
    pub fn new(flags: u8, origin_id: u16, payload_len: u32, seq: u32, t_send_ns: u64) -> Self {
        Hdr {
            magic: MAGIC,
            version: VERSION,
            flags,
            origin_id,
            payload_len,
            seq,
            t_send_ns,
        }
    }

    pub fn encode(&self, out: &mut [u8; HDR_SIZE]) {
        out[0..4].copy_from_slice(&self.magic.to_le_bytes());
        out[4] = self.version;
        out[5] = self.flags;
        out[6..8].copy_from_slice(&self.origin_id.to_le_bytes());
        out[8..12].copy_from_slice(&self.payload_len.to_le_bytes());
        out[12..16].copy_from_slice(&self.seq.to_le_bytes());
        out[16..24].copy_from_slice(&self.t_send_ns.to_le_bytes());
    }

    /// Decode and validate. `max_payload` caps `payload_len` on top of
    /// [`PAYLOAD_MAX`]; pass 0 for the absolute cap only.
    pub fn decode(buf: &[u8], max_payload: u32) -> Result<Hdr, Error> {
        if buf.len() < HDR_SIZE {
            return Err(Error::Short);
        }
        let h = Hdr {
            magic: u32::from_le_bytes(buf[0..4].try_into().unwrap()),
            version: buf[4],
            flags: buf[5],
            origin_id: u16::from_le_bytes(buf[6..8].try_into().unwrap()),
            payload_len: u32::from_le_bytes(buf[8..12].try_into().unwrap()),
            seq: u32::from_le_bytes(buf[12..16].try_into().unwrap()),
            t_send_ns: u64::from_le_bytes(buf[16..24].try_into().unwrap()),
        };
        if h.magic != MAGIC {
            return Err(Error::Magic);
        }
        if h.version != VERSION {
            return Err(Error::Version);
        }
        if h.flags & !FLAG_MASK != 0 {
            return Err(Error::Flags);
        }
        let mut cap = PAYLOAD_MAX;
        if max_payload != 0 && max_payload < cap {
            cap = max_payload;
        }
        if h.payload_len > cap {
            return Err(Error::Cap);
        }
        Ok(h)
    }
}

fn seed(origin_id: u16, seq: u32) -> u32 {
    let s = ((origin_id as u32) << 16) ^ seq;
    if s != 0 {
        s
    } else {
        0x9e37_79b9 // xorshift32 state must be nonzero
    }
}

fn xorshift32(mut x: u32) -> u32 {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

/// Deterministic payload stream keyed by `(origin_id, seq)` — the receiver
/// can verify without the sender storing copies.
pub fn fill_payload(dst: &mut [u8], origin_id: u16, seq: u32) {
    let mut x = seed(origin_id, seq);
    for (i, b) in dst.iter_mut().enumerate() {
        if i & 3 == 0 {
            x = xorshift32(x);
        }
        *b = (x >> ((i & 3) * 8)) as u8;
    }
}

pub fn verify_payload(p: &[u8], origin_id: u16, seq: u32) -> Result<(), Error> {
    let mut x = seed(origin_id, seq);
    for (i, b) in p.iter().enumerate() {
        if i & 3 == 0 {
            x = xorshift32(x);
        }
        if *b != (x >> ((i & 3) * 8)) as u8 {
            return Err(Error::Corrupt);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Cross-language test vectors — duplicated VERBATIM from
    /// `tools/urp-bench-test.c` (`vectors[]`). If these tables ever
    /// disagree, one implementation is wrong by construction.
    const VECTORS: &[(&str, Hdr, [u8; HDR_SIZE])] = &[
        (
            "minimal",
            Hdr {
                magic: MAGIC,
                version: 1,
                flags: 0,
                origin_id: 0x0001,
                payload_len: 0,
                seq: 0,
                t_send_ns: 0,
            },
            [
                0x55, 0x52, 0x50, 0x42, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            ],
        ),
        (
            "echo-1k",
            Hdr {
                magic: MAGIC,
                version: 1,
                flags: FLAG_ECHO,
                origin_id: 0xbeef,
                payload_len: 1024,
                seq: 0x1234_5678,
                t_send_ns: 0x0102_0304_0506_0708,
            },
            [
                0x55, 0x52, 0x50, 0x42, 0x01, 0x01, 0xef, 0xbe, 0x00, 0x04, 0x00, 0x00, 0x78, 0x56,
                0x34, 0x12, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
            ],
        ),
        (
            "fin-extremes",
            Hdr {
                magic: MAGIC,
                version: 1,
                flags: FLAG_FIN,
                origin_id: 0xffff,
                payload_len: 0,
                seq: 0xffff_ffff,
                t_send_ns: 0xffff_ffff_ffff_ffff,
            },
            [
                0x55, 0x52, 0x50, 0x42, 0x01, 0x02, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            ],
        ),
        (
            "echo-fin-maxpayload",
            Hdr {
                magic: MAGIC,
                version: 1,
                flags: FLAG_ECHO | FLAG_FIN,
                origin_id: 0x0042,
                payload_len: PAYLOAD_MAX,
                seq: 7,
                t_send_ns: 1,
            },
            [
                0x55, 0x52, 0x50, 0x42, 0x01, 0x03, 0x42, 0x00, 0x00, 0x00, 0x10, 0x00, 0x07, 0x00,
                0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            ],
        ),
    ];

    #[test]
    fn hdr_vectors() {
        for (name, hdr, bytes) in VECTORS {
            let mut out = [0u8; HDR_SIZE];
            hdr.encode(&mut out);
            assert_eq!(&out, bytes, "vector {name}: encode mismatch");

            let dec = Hdr::decode(bytes, 0).unwrap_or_else(|e| panic!("vector {name}: decode {e}"));
            assert_eq!(dec, *hdr, "vector {name}: decode fields mismatch");
        }
    }

    #[test]
    fn hdr_decode_table() {
        // (name, fed_len, mutation (off, val), max_payload, payload_len override, want)
        // — same cases as C test_hdr_decode.
        let tmpl = Hdr {
            magic: MAGIC,
            version: 1,
            flags: 0,
            origin_id: 7,
            payload_len: 16,
            seq: 3,
            t_send_ns: 99,
        };
        #[allow(clippy::type_complexity)]
        let cases: &[(
            &str,
            usize,
            Option<(usize, u8)>,
            u32,
            Option<u32>,
            Result<(), Error>,
        )] = &[
            ("empty", 0, None, 0, None, Err(Error::Short)),
            ("one-byte", 1, None, 0, None, Err(Error::Short)),
            ("short-23", 23, None, 0, None, Err(Error::Short)),
            ("exact-24", 24, None, 0, None, Ok(())),
            ("bad-magic", 24, Some((0, 0xAA)), 0, None, Err(Error::Magic)),
            (
                "bad-version",
                24,
                Some((4, 2)),
                0,
                None,
                Err(Error::Version),
            ),
            (
                "reserved-bit2",
                24,
                Some((5, 0x04)),
                0,
                None,
                Err(Error::Flags),
            ),
            (
                "reserved-bit7",
                24,
                Some((5, 0x80)),
                0,
                None,
                Err(Error::Flags),
            ),
            ("flags-valid-both", 24, Some((5, 0x03)), 0, None, Ok(())),
            ("payload-at-abs-cap", 24, None, 0, Some(PAYLOAD_MAX), Ok(())),
            (
                "payload-over-abs-cap",
                24,
                None,
                0,
                Some(PAYLOAD_MAX + 1),
                Err(Error::Cap),
            ),
            ("payload-at-cfg-cap", 24, None, 16, Some(16), Ok(())),
            (
                "payload-over-cfg-cap",
                24,
                None,
                16,
                Some(17),
                Err(Error::Cap),
            ),
            (
                "payload-u32-max",
                24,
                None,
                0,
                Some(u32::MAX),
                Err(Error::Cap),
            ),
        ];
        for (name, len, mutation, max_payload, payload, want) in cases {
            let mut h = tmpl;
            if let Some(p) = payload {
                h.payload_len = *p;
            }
            let mut buf = [0u8; HDR_SIZE];
            h.encode(&mut buf);
            if let Some((off, val)) = mutation {
                buf[*off] = *val;
            }
            let got = Hdr::decode(&buf[..*len], *max_payload).map(|_| ());
            assert_eq!(got, *want, "case {name}");
        }
    }

    #[test]
    fn payload_fill_verify() {
        let mut a = vec![0u8; 257];
        let mut b = vec![0u8; 257];
        fill_payload(&mut a, 1, 42);
        fill_payload(&mut b, 1, 42);
        assert_eq!(a, b, "fill not deterministic");
        assert_eq!(verify_payload(&a, 1, 42), Ok(()));

        fill_payload(&mut b, 1, 43);
        assert_ne!(a, b, "seq not mixed into stream");
        fill_payload(&mut b, 2, 42);
        assert_ne!(a, b, "origin not mixed into stream");

        a[100] ^= 1;
        assert_eq!(verify_payload(&a, 1, 42), Err(Error::Corrupt));

        // zero-seed corner: origin 0, seq 0 must still produce a stream
        let mut z = [0u8; 8];
        fill_payload(&mut z, 0, 0);
        assert_eq!(verify_payload(&z, 0, 0), Ok(()));

        // zero-length is trivially valid
        assert_eq!(verify_payload(&[], 9, 9), Ok(()));
    }
}
