#![no_main]

extern crate alloc;

use alloc::vec;
use libfuzzer_sys::fuzz_target;
use uds_rdma_protocol::ReorderBuffer;

fuzz_target!(|data: &[u8]| {
    // Interpret fuzzed bytes as a sequence of (seq: u16, data_byte: u8) operations.
    // Use a small max_buffered to stress the full/reject path.
    let mut rb = ReorderBuffer::new(0, 16);
    let mut last_delivered: Option<u64> = None;

    for chunk in data.chunks(3) {
        if chunk.len() < 3 {
            break;
        }
        let seq = u16::from_le_bytes([chunk[0], chunk[1]]) as u64;
        let payload = vec![chunk[2]];

        match rb.insert(seq, payload) {
            Ok(result) => {
                // Verify delivered frames are in strictly increasing order
                for (delivered_seq, _) in &result.delivered {
                    if let Some(prev) = last_delivered {
                        assert_eq!(*delivered_seq, prev + 1);
                    }
                    last_delivered = Some(*delivered_seq);
                }
            }
            Err(_) => {
                // Duplicate or full — both are fine
            }
        }
    }
});
