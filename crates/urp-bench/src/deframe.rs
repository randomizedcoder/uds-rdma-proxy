//! Incremental deframer (§30.5): arbitrary chunk boundaries in, complete
//! messages out. Fast path delivers payloads in place (zero copy — the
//! buffer an echo goes back out of); messages spanning chunks are staged
//! in the assembly buffer and counted, never hidden.

use crate::frame::{Hdr, HDR_SIZE};
use crate::Error;

#[derive(Debug, PartialEq, Eq)]
enum State {
    WantHdr,
    WantPayload,
}

pub struct Deframer {
    state: State,
    hdr_buf: [u8; HDR_SIZE],
    hdr_have: usize,
    hdr: Option<Hdr>,
    asm_buf: Vec<u8>,
    asm_cap: usize,
    asm_have: usize,
    max_payload: u32,
    /// Messages delivered in total.
    pub msgs_total: u64,
    /// Messages delivered via the assembly buffer (spanned chunks).
    pub msgs_reassembled: u64,
}

impl Deframer {
    /// `asm_cap` must cover the largest payload the peer may send;
    /// `max_payload` = 0 means the absolute cap only.
    pub fn new(asm_cap: usize, max_payload: u32) -> Self {
        Deframer {
            state: State::WantHdr,
            hdr_buf: [0; HDR_SIZE],
            hdr_have: 0,
            hdr: None,
            asm_buf: vec![0; asm_cap],
            asm_cap,
            asm_have: 0,
            max_payload,
            msgs_total: 0,
            msgs_reassembled: 0,
        }
    }

    /// Feed one received chunk; `cb` runs once per completed message, in
    /// order. Errors (decode, cap, or the callback's) poison the stream —
    /// hard error, no resync.
    pub fn feed<F>(&mut self, chunk: &[u8], cb: &mut F) -> Result<(), Error>
    where
        F: FnMut(&Hdr, &[u8]) -> Result<(), Error>,
    {
        let mut off = 0usize;
        while off < chunk.len() {
            match self.state {
                State::WantHdr => {
                    let avail = chunk.len() - off;
                    // Fast path: complete header AND payload inside this
                    // chunk with nothing staged — deliver in place.
                    if self.hdr_have == 0 && avail >= HDR_SIZE {
                        match Hdr::decode(&chunk[off..], self.max_payload) {
                            Ok(h) => {
                                if avail - HDR_SIZE >= h.payload_len as usize {
                                    let start = off + HDR_SIZE;
                                    let end = start + h.payload_len as usize;
                                    off = end;
                                    self.msgs_total += 1;
                                    cb(&h, &chunk[start..end])?;
                                    continue;
                                }
                            }
                            Err(Error::Short) => unreachable!("avail >= HDR_SIZE"),
                            Err(e) => return Err(e),
                        }
                    }
                    // Slow path: stage header bytes.
                    let want = HDR_SIZE - self.hdr_have;
                    let take = want.min(avail);
                    self.hdr_buf[self.hdr_have..self.hdr_have + take]
                        .copy_from_slice(&chunk[off..off + take]);
                    self.hdr_have += take;
                    off += take;
                    if self.hdr_have < HDR_SIZE {
                        return Ok(()); // need more bytes
                    }
                    let h = Hdr::decode(&self.hdr_buf, self.max_payload)?;
                    if h.payload_len as usize > self.asm_cap {
                        return Err(Error::Cap);
                    }
                    self.asm_have = 0;
                    if h.payload_len == 0 {
                        self.msgs_total += 1;
                        self.msgs_reassembled += 1;
                        self.hdr_have = 0;
                        cb(&h, &[])?;
                    } else {
                        self.hdr = Some(h);
                        self.state = State::WantPayload;
                    }
                }
                State::WantPayload => {
                    let h = self.hdr.expect("hdr set in WantPayload");
                    let want = h.payload_len as usize - self.asm_have;
                    let avail = chunk.len() - off;
                    let take = want.min(avail);
                    self.asm_buf[self.asm_have..self.asm_have + take]
                        .copy_from_slice(&chunk[off..off + take]);
                    self.asm_have += take;
                    off += take;
                    if self.asm_have < h.payload_len as usize {
                        return Ok(()); // need more bytes
                    }
                    self.msgs_total += 1;
                    self.msgs_reassembled += 1;
                    self.state = State::WantHdr;
                    self.hdr_have = 0;
                    self.hdr = None;
                    cb(&h, &self.asm_buf[..h.payload_len as usize])?;
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame::{fill_payload, verify_payload, FLAG_FIN, MAGIC};

    /// Build one wire message — mirror of C `mk_msg`.
    fn mk_msg(origin: u16, seq: u32, payload_len: u32, flags: u8) -> Vec<u8> {
        let h = Hdr {
            magic: MAGIC,
            version: 1,
            flags,
            origin_id: origin,
            payload_len,
            seq,
            t_send_ns: 1000 + seq as u64,
        };
        let mut buf = vec![0u8; HDR_SIZE + payload_len as usize];
        let mut hdr_bytes = [0u8; HDR_SIZE];
        h.encode(&mut hdr_bytes);
        buf[..HDR_SIZE].copy_from_slice(&hdr_bytes);
        fill_payload(&mut buf[HDR_SIZE..], origin, seq);
        buf
    }

    #[test]
    fn chunking_every_split() {
        // Two back-to-back messages, fed at every possible split point —
        // mirror of C test_deframer_chunking.
        let mut wire = mk_msg(1, 10, 32, 0);
        let n1 = wire.len();
        wire.extend_from_slice(&mk_msg(1, 11, 32, 0));
        let total = wire.len();

        for split in 0..=total {
            let mut d = Deframer::new(64, 0);
            let mut msgs = Vec::new();
            let mut cb = |h: &Hdr, p: &[u8]| {
                msgs.push((*h, p.to_vec()));
                Ok(())
            };
            d.feed(&wire[..split], &mut cb)
                .unwrap_or_else(|e| panic!("split {split}: {e}"));
            d.feed(&wire[split..], &mut cb)
                .unwrap_or_else(|e| panic!("split {split}: {e}"));
            assert_eq!(msgs.len(), 2, "split {split}: msg count");
            assert_eq!(msgs[1].0.seq, 11, "split {split}: last seq");
            assert_eq!(
                verify_payload(&msgs[1].1, 1, 11),
                Ok(()),
                "split {split}: payload"
            );
            // Only splits inside a message force reassembly.
            if split == 0 || split == n1 || split == total {
                assert_eq!(
                    d.msgs_reassembled, 0,
                    "split {split}: clean split reassembled"
                );
            } else {
                assert!(
                    d.msgs_reassembled >= 1,
                    "split {split}: mid-msg split not counted"
                );
            }
        }
    }

    #[test]
    fn one_byte_drip() {
        let mut wire = mk_msg(2, 0, 0, FLAG_FIN);
        wire.extend_from_slice(&mk_msg(2, 1, 16, 0));
        let mut d = Deframer::new(32, 0);
        let mut msgs = 0;
        let mut cb = |_: &Hdr, _: &[u8]| {
            msgs += 1;
            Ok(())
        };
        for i in 0..wire.len() {
            d.feed(&wire[i..i + 1], &mut cb)
                .unwrap_or_else(|e| panic!("drip byte {i}: {e}"));
        }
        assert_eq!(msgs, 2);
        assert_eq!(d.msgs_total, 2);
        assert_eq!(d.msgs_reassembled, 2, "everything reassembled");
    }

    #[test]
    fn error_table() {
        let mut ok = |_: &Hdr, _: &[u8]| Ok(());

        // garbage first byte: hard error, no resync
        let mut wire = mk_msg(1, 0, 0, 0);
        wire[0] = 0xAA;
        let mut d = Deframer::new(64, 0);
        assert_eq!(d.feed(&wire, &mut ok), Err(Error::Magic));

        // config-cap violation detected at decode time (fast path)
        let wire = mk_msg(1, 0, 32, 0);
        let mut d = Deframer::new(64, 16);
        assert_eq!(d.feed(&wire, &mut ok), Err(Error::Cap));

        // payload larger than assembly buffer, forced onto the slow path
        let wire = mk_msg(1, 0, 32, 0);
        let mut d = Deframer::new(16, 0);
        assert_eq!(d.feed(&wire[..1], &mut ok), Ok(()));
        assert_eq!(d.feed(&wire[1..], &mut ok), Err(Error::Cap));

        // callback error propagates
        let wire = mk_msg(1, 5, 8, 0);
        let mut d = Deframer::new(64, 0);
        let mut fail = |_: &Hdr, _: &[u8]| Err(Error::Corrupt);
        assert_eq!(d.feed(&wire, &mut fail), Err(Error::Corrupt));

        // empty chunk is a no-op
        let mut d = Deframer::new(64, 0);
        assert_eq!(d.feed(&[], &mut ok), Ok(()));
    }
}
