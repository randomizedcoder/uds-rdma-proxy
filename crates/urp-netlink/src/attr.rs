//! Minimal netlink attribute (TLV) encoder + iterator.
//!
//! Wire format per attribute:
//!   u16 nla_len   (header + payload, host byte order, big enough that
//!                  the kernel uses it without swapping)
//!   u16 nla_type
//!   payload (nla_len - 4 bytes)
//!   pad to 4-byte alignment
//!
//! All multi-byte integers are encoded in *host* byte order, which matches
//! the kernel `nla_put_*` helpers. Network-order fields (sockaddr ports,
//! etc.) are the caller's responsibility.

use crate::uapi::NLA_F_NESTED;

const NLA_HDR_LEN: usize = 4;

/// Round up to the 4-byte netlink attribute alignment. Also used by the
/// message-level framing in netlink.rs (same NLMSG_ALIGNTO).
#[inline]
pub(crate) fn align4(n: usize) -> usize {
    (n + 3) & !3
}

#[derive(Debug, Default, Clone)]
pub struct AttrBuf {
    buf: Vec<u8>,
}

impl AttrBuf {
    pub fn new() -> Self {
        Self { buf: Vec::new() }
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.buf
    }

    #[allow(dead_code)]
    pub fn as_bytes(&self) -> &[u8] {
        &self.buf
    }

    fn put_header(&mut self, type_: u16, payload_len: usize) {
        let total = NLA_HDR_LEN + payload_len;
        self.buf.extend_from_slice(&(total as u16).to_ne_bytes());
        self.buf.extend_from_slice(&type_.to_ne_bytes());
    }

    fn pad(&mut self) {
        let pad = align4(self.buf.len()) - self.buf.len();
        for _ in 0..pad {
            self.buf.push(0);
        }
    }

    pub fn put_bytes(&mut self, type_: u16, data: &[u8]) {
        self.put_header(type_, data.len());
        self.buf.extend_from_slice(data);
        self.pad();
    }

    /// NUL-terminated string (kernel NLA_NUL_STRING expects this).
    pub fn put_string(&mut self, type_: u16, s: &str) {
        let len = s.len() + 1;
        self.put_header(type_, len);
        self.buf.extend_from_slice(s.as_bytes());
        self.buf.push(0);
        self.pad();
    }

    pub fn put_u8(&mut self, type_: u16, v: u8) {
        self.put_header(type_, 1);
        self.buf.push(v);
        self.pad();
    }

    #[allow(dead_code)]
    pub fn put_u16(&mut self, type_: u16, v: u16) {
        self.put_header(type_, 2);
        self.buf.extend_from_slice(&v.to_ne_bytes());
        self.pad();
    }

    pub fn put_u32(&mut self, type_: u16, v: u32) {
        self.put_header(type_, 4);
        self.buf.extend_from_slice(&v.to_ne_bytes());
        self.pad();
    }

    #[allow(dead_code)]
    pub fn put_u64(&mut self, type_: u16, v: u64) {
        self.put_header(type_, 8);
        self.buf.extend_from_slice(&v.to_ne_bytes());
        self.pad();
    }

    /// Build a nested attribute. The closure receives a fresh `AttrBuf`
    /// for the nested contents.
    pub fn nest<F: FnOnce(&mut AttrBuf)>(&mut self, type_: u16, f: F) {
        let mut inner = AttrBuf::new();
        f(&mut inner);
        let payload = inner.into_bytes();
        self.put_header(type_ | NLA_F_NESTED, payload.len());
        self.buf.extend_from_slice(&payload);
        self.pad();
    }
}

/// Iterate over a netlink attribute blob, yielding `(type_, &payload)`.
/// The `NLA_F_NESTED` bit is masked out of the returned type.
pub struct AttrIter<'a> {
    rest: &'a [u8],
}

impl<'a> AttrIter<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Self { rest: buf }
    }
}

impl<'a> Iterator for AttrIter<'a> {
    type Item = (u16, &'a [u8]);

    fn next(&mut self) -> Option<Self::Item> {
        if self.rest.len() < NLA_HDR_LEN {
            return None;
        }
        let nla_len = u16::from_ne_bytes([self.rest[0], self.rest[1]]) as usize;
        let nla_type = u16::from_ne_bytes([self.rest[2], self.rest[3]]);
        if nla_len < NLA_HDR_LEN || nla_len > self.rest.len() {
            return None;
        }
        let payload = &self.rest[NLA_HDR_LEN..nla_len];
        let aligned = align4(nla_len).min(self.rest.len());
        self.rest = &self.rest[aligned..];
        Some((nla_type & !NLA_F_NESTED, payload))
    }
}

/// Convenience: parse a u8 payload.
pub fn payload_u8(p: &[u8]) -> Option<u8> {
    p.first().copied()
}
pub fn payload_u16(p: &[u8]) -> Option<u16> {
    p.get(..2).map(|b| u16::from_ne_bytes([b[0], b[1]]))
}
pub fn payload_u32(p: &[u8]) -> Option<u32> {
    p.get(..4)
        .map(|b| u32::from_ne_bytes([b[0], b[1], b[2], b[3]]))
}
pub fn payload_u64(p: &[u8]) -> Option<u64> {
    p.get(..8)
        .map(|b| u64::from_ne_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
}
pub fn payload_str(p: &[u8]) -> Option<&str> {
    let end = p.iter().position(|&c| c == 0).unwrap_or(p.len());
    std::str::from_utf8(&p[..end]).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn attr_encode_roundtrip() {
        let mut b = AttrBuf::new();
        b.put_string(1, "hello");
        b.put_u32(2, 0xdeadbeef);
        b.nest(3, |inner| {
            inner.put_u8(11, 7);
            inner.put_u64(12, 0x0102030405060708);
        });

        let bytes = b.into_bytes();
        let mut found_str = None;
        let mut found_u32 = None;
        let mut found_u8 = None;
        let mut found_u64 = None;

        for (t, p) in AttrIter::new(&bytes) {
            match t {
                1 => found_str = payload_str(p).map(String::from),
                2 => found_u32 = payload_u32(p),
                3 => {
                    for (it, ip) in AttrIter::new(p) {
                        match it {
                            11 => found_u8 = payload_u8(ip),
                            12 => found_u64 = payload_u64(ip),
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
        }
        assert_eq!(found_str.as_deref(), Some("hello"));
        assert_eq!(found_u32, Some(0xdeadbeef));
        assert_eq!(found_u8, Some(7));
        assert_eq!(found_u64, Some(0x0102030405060708));
    }
}
