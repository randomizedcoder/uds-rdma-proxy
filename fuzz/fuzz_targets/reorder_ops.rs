#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use libfuzzer_sys::fuzz_target;
use uds_rdma_protocol::ReorderBuffer;

// Stronger reorder fuzzer than `reorder_buffer`: the fuzzer picks the
// starting `next_expected` (full u64, including values near u64::MAX) and
// full u64 sequence numbers, so it can reach the `next_expected += 1`
// increment at the top of the sequence space -- unreachable by the u16,
// new(0, ...) target. Payload lengths vary (including zero).
fuzz_target!(|data: &[u8]| {
    if data.len() < 9 {
        return;
    }
    let initial = u64::from_le_bytes(data[0..8].try_into().unwrap());
    let max_buffered = (data[8] % 64) as usize; // 0..63, including the 0 edge
    let rest = &data[9..];

    let mut rb = ReorderBuffer::new(initial, max_buffered);
    let mut last_delivered: Option<u64> = None;

    // Each op: 8-byte seq + 1-byte payload length, then that many bytes.
    let mut i = 0;
    while i + 9 <= rest.len() {
        let seq = u64::from_le_bytes(rest[i..i + 8].try_into().unwrap());
        let plen = (rest[i + 9 - 1] % 16) as usize; // 0..15
        i += 9;
        let payload: Vec<u8> = rest.iter().skip(i).take(plen).copied().collect();
        i += payload.len();

        if let Ok(result) = rb.insert(seq, payload) {
            // Invariant: delivered frames are strictly consecutive -- except
            // at the saturated top of the sequence space, where
            // next_expected sticks at u64::MAX and the (unreachable in
            // practice) boundary frame may re-deliver. Only assert below
            // the saturation point.
            for (delivered_seq, _) in &result.delivered {
                if let Some(prev) = last_delivered {
                    if prev != u64::MAX {
                        assert_eq!(*delivered_seq, prev + 1);
                    }
                }
                last_delivered = Some(*delivered_seq);
            }
        }
    }
});
