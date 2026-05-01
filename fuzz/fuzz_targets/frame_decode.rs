#![no_main]

use libfuzzer_sys::fuzz_target;
use uds_rdma_protocol::FrameHeader;

fuzz_target!(|data: &[u8]| {
    // Decode must never panic — it should return Ok or Err.
    let _ = FrameHeader::decode(data);
});
