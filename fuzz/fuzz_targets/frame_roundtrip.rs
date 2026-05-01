#![no_main]

use libfuzzer_sys::fuzz_target;
use uds_rdma_protocol::{FrameHeader, FRAME_HEADER_SIZE};

fuzz_target!(|data: &[u8]| {
    if data.len() < FRAME_HEADER_SIZE {
        return;
    }
    // Try to decode from fuzzed bytes
    if let Ok(header) = FrameHeader::decode(data) {
        // Roundtrip: encode then decode must produce the same header
        let mut buf = [0u8; FRAME_HEADER_SIZE];
        header.encode(&mut buf).unwrap();
        let decoded = FrameHeader::decode(&buf).unwrap();
        assert_eq!(header, decoded);
    }
});
