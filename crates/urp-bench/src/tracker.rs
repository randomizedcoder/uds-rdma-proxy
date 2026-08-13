//! Echo RTT tracker (§30.5): outstanding-window accounting keyed by
//! `seq % window`, u32 wraparound safe, dup/unknown detection.

use crate::Error;

pub struct Tracker {
    sent_ns: Vec<u64>,
    seqs: Vec<u32>,
    in_flight: Vec<bool>,
    window: u32,
    pub inflight_count: u32,
    pub dups: u64,
    pub unknowns: u64,
}

impl Tracker {
    /// `window` >= 1: the maximum number of outstanding originals.
    pub fn new(window: u32) -> Self {
        Tracker {
            sent_ns: vec![0; window as usize],
            seqs: vec![0; window as usize],
            in_flight: vec![false; window as usize],
            window,
            inflight_count: 0,
            dups: 0,
            unknowns: 0,
        }
    }

    /// Record an original as sent. `Err(Full)` if the window slot is busy.
    pub fn sent(&mut self, seq: u32, t_send_ns: u64) -> Result<(), Error> {
        let slot = (seq % self.window) as usize;
        if self.in_flight[slot] {
            return Err(Error::Full);
        }
        self.in_flight[slot] = true;
        self.seqs[slot] = seq;
        self.sent_ns[slot] = t_send_ns;
        self.inflight_count += 1;
        Ok(())
    }

    /// Record an echo; returns the RTT in ns (clamped at 0 if the clock
    /// stepped backwards).
    pub fn echo(&mut self, seq: u32, now_ns: u64) -> Result<u64, Error> {
        let slot = (seq % self.window) as usize;
        if !self.in_flight[slot] {
            // Slot free: either never sent, or already echoed (dup).
            if self.seqs[slot] == seq && self.sent_ns[slot] != 0 {
                self.dups += 1;
                return Err(Error::Dup);
            }
            self.unknowns += 1;
            return Err(Error::Unknown);
        }
        if self.seqs[slot] != seq {
            self.unknowns += 1;
            return Err(Error::Unknown);
        }
        self.in_flight[slot] = false;
        self.inflight_count -= 1;
        Ok(now_ns.saturating_sub(self.sent_ns[slot]))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tracker_table() {
        // Mirror of C test_tracker: same seqs, timestamps, expectations.
        let mut t = Tracker::new(4);

        // in-order
        t.sent(1, 100).unwrap();
        assert_eq!(t.echo(1, 150), Ok(50));

        // out-of-order
        t.sent(2, 200).unwrap();
        t.sent(3, 210).unwrap();
        assert_eq!(t.echo(3, 240), Ok(30));
        assert_eq!(t.echo(2, 260), Ok(60));

        // duplicate echo
        assert_eq!(t.echo(2, 270), Err(Error::Dup));
        assert_eq!(t.dups, 1);

        // unknown seq (never sent)
        assert_eq!(t.echo(77, 280), Err(Error::Unknown));

        // window full: slot collision at seq % 4
        t.sent(8, 300).unwrap();
        assert_eq!(t.sent(12, 310), Err(Error::Full));
        assert_eq!(t.echo(8, 350), Ok(50));

        // u32 wraparound
        t.sent(u32::MAX, 400).unwrap();
        t.sent(0, 410).unwrap();
        assert_eq!(t.echo(u32::MAX, 450), Ok(50));
        assert_eq!(t.echo(0, 460), Ok(50));
        assert_eq!(t.inflight_count, 0);

        // clock going backwards clamps to 0, never negative
        t.sent(20, 1000).unwrap();
        assert_eq!(t.echo(20, 999), Ok(0));
    }
}
