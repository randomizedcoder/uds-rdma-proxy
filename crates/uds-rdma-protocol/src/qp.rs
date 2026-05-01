/// Select a QP using round-robin based on a sequence number.
///
/// Simple and fair. Used as the default QP selection strategy.
#[inline]
pub fn qp_select_round_robin(seq: u64, num_qps: usize) -> usize {
    debug_assert!(num_qps > 0, "num_qps must be > 0");
    (seq as usize) % num_qps
}

/// Select a QP by hashing the stream_id.
///
/// Ensures all frames for a given stream use the same QP, which eliminates
/// cross-QP reordering for that stream (useful when per-stream reorder
/// buffers are not yet implemented).
#[inline]
pub fn qp_select_hash(stream_id: u32, num_qps: usize) -> usize {
    debug_assert!(num_qps > 0, "num_qps must be > 0");
    (stream_id as usize) % num_qps
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_robin_single_qp() {
        for seq in 0..10 {
            assert_eq!(qp_select_round_robin(seq, 1), 0);
        }
    }

    #[test]
    fn round_robin_distributes() {
        let num_qps = 4;
        for seq in 0u64..8 {
            assert_eq!(qp_select_round_robin(seq, num_qps), (seq as usize) % num_qps);
        }
    }

    #[test]
    fn hash_same_stream_same_qp() {
        let stream_id = 42;
        let qp = qp_select_hash(stream_id, 8);
        // Same stream always gets same QP
        for _ in 0..100 {
            assert_eq!(qp_select_hash(stream_id, 8), qp);
        }
    }

    #[test]
    fn hash_different_streams() {
        // With enough streams, we should hit multiple QPs
        let num_qps = 4;
        let mut hit = [false; 4];
        for stream_id in 0..100u32 {
            let qp = qp_select_hash(stream_id, num_qps);
            hit[qp] = true;
        }
        assert!(hit.iter().all(|&h| h), "all QPs should be hit");
    }
}
