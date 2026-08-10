use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use crate::error::ProtocolError;

/// Result of inserting a frame into the reorder buffer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DrainResult {
    /// Frames delivered in-order, as `(sequence_number, payload)` pairs.
    pub delivered: Vec<(u64, Vec<u8>)>,
    /// Number of out-of-order frames still buffered.
    pub gap_count: usize,
}

/// BTreeMap-based reorder buffer for in-order frame delivery across multiple QPs.
///
/// When frames arrive out of order (due to multi-QP ECMP), they are buffered
/// until all preceding frames have been delivered. Frames are drained in
/// sequence-number order starting from `next_expected`.
///
/// See design doc Section 8.4 for the full specification.
#[derive(Debug)]
pub struct ReorderBuffer {
    pending: BTreeMap<u64, Vec<u8>>,
    next_expected: u64,
    max_buffered: usize,
}

impl ReorderBuffer {
    /// Create a new reorder buffer.
    ///
    /// - `initial_expected`: the first sequence number expected (usually 0).
    /// - `max_buffered`: maximum number of out-of-order frames to hold.
    pub fn new(initial_expected: u64, max_buffered: usize) -> Self {
        Self {
            pending: BTreeMap::new(),
            next_expected: initial_expected,
            max_buffered,
        }
    }

    /// Insert a frame with the given sequence number and payload.
    ///
    /// Returns a `DrainResult` containing all frames that can now be delivered
    /// in order, plus the count of frames still buffered.
    ///
    /// Errors:
    /// - `ReorderDuplicate` if `seq < next_expected` (already delivered) or
    ///   `seq` is already in the buffer.
    /// - `ReorderFull` if the buffer is at capacity and `seq != next_expected`.
    pub fn insert(&mut self, seq: u64, data: Vec<u8>) -> Result<DrainResult, ProtocolError> {
        // Already delivered
        if seq < self.next_expected {
            return Err(ProtocolError::ReorderDuplicate { seq });
        }

        // Duplicate in buffer
        if self.pending.contains_key(&seq) {
            return Err(ProtocolError::ReorderDuplicate { seq });
        }

        // Buffer full (but allow if it's the next expected — it'll drain immediately)
        if self.pending.len() >= self.max_buffered && seq != self.next_expected {
            return Err(ProtocolError::ReorderFull {
                max_buffered: self.max_buffered,
            });
        }

        self.pending.insert(seq, data);

        // Drain consecutive frames starting from next_expected.
        // saturating_add so the top of the u64 sequence space cannot
        // overflow-panic (found by fuzz/reorder_ops): a peer would need
        // 2^64 in-order deliveries to reach it, but a panic here aborts
        // the kernel via the FFI backend, so bound it. The C twin
        // (kernel/urp_reorder.c) matches.
        let mut delivered = Vec::new();
        while let Some(payload) = self.pending.remove(&self.next_expected) {
            delivered.push((self.next_expected, payload));
            self.next_expected = self.next_expected.saturating_add(1);
        }

        Ok(DrainResult {
            delivered,
            gap_count: self.pending.len(),
        })
    }

    /// The next sequence number expected for in-order delivery.
    #[inline]
    pub fn next_expected(&self) -> u64 {
        self.next_expected
    }

    /// Number of out-of-order frames currently buffered.
    #[inline]
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    /// Alias for `pending_count`.
    #[inline]
    pub fn gap_count(&self) -> usize {
        self.pending.len()
    }

    /// Whether the buffer has no pending out-of-order frames.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn in_order_delivery() {
        let mut rb = ReorderBuffer::new(0, 64);
        for seq in 0..3 {
            let result = rb.insert(seq, vec![seq as u8]).unwrap();
            assert_eq!(result.delivered.len(), 1);
            assert_eq!(result.delivered[0], (seq, vec![seq as u8]));
            assert_eq!(result.gap_count, 0);
        }
        assert_eq!(rb.next_expected(), 3);
    }

    #[test]
    fn out_of_order() {
        let mut rb = ReorderBuffer::new(0, 64);

        // Insert seq 2 (out of order)
        let r = rb.insert(2, vec![2]).unwrap();
        assert!(r.delivered.is_empty());
        assert_eq!(r.gap_count, 1);

        // Insert seq 0 (delivers immediately)
        let r = rb.insert(0, vec![0]).unwrap();
        assert_eq!(r.delivered, vec![(0, vec![0])]);
        assert_eq!(r.gap_count, 1); // seq 2 still buffered

        // Insert seq 1 (fills gap, drains 1 and 2)
        let r = rb.insert(1, vec![1]).unwrap();
        assert_eq!(r.delivered, vec![(1, vec![1]), (2, vec![2])]);
        assert_eq!(r.gap_count, 0);
        assert_eq!(rb.next_expected(), 3);
    }

    #[test]
    fn gap_then_fill() {
        let mut rb = ReorderBuffer::new(0, 64);

        // Insert 0 (delivers)
        let r = rb.insert(0, vec![0]).unwrap();
        assert_eq!(r.delivered.len(), 1);

        // Insert 2, 3 (buffered, gap at 1)
        let r = rb.insert(2, vec![2]).unwrap();
        assert!(r.delivered.is_empty());
        let r = rb.insert(3, vec![3]).unwrap();
        assert!(r.delivered.is_empty());
        assert_eq!(rb.pending_count(), 2);

        // Insert 1 (fills gap, drains 1, 2, 3)
        let r = rb.insert(1, vec![1]).unwrap();
        assert_eq!(r.delivered, vec![(1, vec![1]), (2, vec![2]), (3, vec![3])]);
        assert_eq!(rb.next_expected(), 4);
        assert!(rb.is_empty());
    }

    #[test]
    fn duplicate_in_buffer() {
        let mut rb = ReorderBuffer::new(0, 64);
        rb.insert(2, vec![2]).unwrap();
        assert_eq!(
            rb.insert(2, vec![2]),
            Err(ProtocolError::ReorderDuplicate { seq: 2 })
        );
    }

    #[test]
    fn duplicate_already_delivered() {
        let mut rb = ReorderBuffer::new(0, 64);
        rb.insert(0, vec![0]).unwrap();
        assert_eq!(rb.next_expected(), 1);
        assert_eq!(
            rb.insert(0, vec![0]),
            Err(ProtocolError::ReorderDuplicate { seq: 0 })
        );
    }

    #[test]
    fn buffer_full() {
        let mut rb = ReorderBuffer::new(0, 2);
        rb.insert(2, vec![2]).unwrap();
        rb.insert(3, vec![3]).unwrap();
        assert_eq!(
            rb.insert(4, vec![4]),
            Err(ProtocolError::ReorderFull { max_buffered: 2 })
        );
        // But inserting next_expected still works (drains immediately)
        let r = rb.insert(0, vec![0]).unwrap();
        assert_eq!(r.delivered, vec![(0, vec![0])]);
    }

    #[test]
    fn empty_initial() {
        let rb = ReorderBuffer::new(0, 64);
        assert!(rb.is_empty());
        assert_eq!(rb.pending_count(), 0);
        assert_eq!(rb.gap_count(), 0);
        assert_eq!(rb.next_expected(), 0);
    }

    #[test]
    fn large_gap() {
        let mut rb = ReorderBuffer::new(0, 64);
        // Insert seq 0 (delivers)
        let r = rb.insert(0, vec![0]).unwrap();
        assert_eq!(r.delivered.len(), 1);

        // Insert seq 100 (buffered)
        let r = rb.insert(100, vec![100]).unwrap();
        assert!(r.delivered.is_empty());
        assert_eq!(rb.pending_count(), 1);
        assert_eq!(rb.next_expected(), 1);
    }

    // Regression: fuzz/reorder_ops found `next_expected += 1` overflow-
    // panicking at the top of the u64 sequence space. Delivering the frame
    // at u64::MAX must saturate, not panic.
    #[test]
    fn insert_at_u64_max_saturates() {
        let mut rb = ReorderBuffer::new(u64::MAX, 16);
        let r = rb.insert(u64::MAX, vec![0xAB]).unwrap();
        assert_eq!(r.delivered.len(), 1);
        assert_eq!(r.delivered[0].0, u64::MAX);
        assert_eq!(rb.next_expected(), u64::MAX); // saturated, no overflow
    }
}
