#![no_main]

use libfuzzer_sys::fuzz_target;
use uds_rdma_protocol::CreditState;

fuzz_target!(|data: &[u8]| {
    if data.len() < 2 {
        return;
    }
    let initial = u16::from_le_bytes([data[0], data[1]]);
    // Limit initial credits to avoid very long loops
    let initial = initial % 1024;
    let mut cs = CreditState::new(initial);

    for &byte in &data[2..] {
        match byte % 4 {
            0 => {
                // Consume
                let _ = cs.consume();
            }
            1 => {
                // Grant
                let n = (byte >> 2) as u16;
                cs.grant(n);
            }
            2 => {
                // Record recv
                cs.record_recv();
            }
            3 => {
                // Take grants
                let grants = cs.take_grants();
                assert_eq!(cs.pending_grants(), 0);
                let _ = grants; // suppress unused
            }
            _ => unreachable!(),
        }

        // Invariant: can_send iff send_credits > 0
        assert_eq!(cs.can_send(), cs.send_credits() > 0);
    }
});
