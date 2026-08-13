#![no_main]

use libfuzzer_sys::fuzz_target;
use urp_bench::frame::{Hdr, HDR_SIZE};

fuzz_target!(|data: &[u8]| {
    // Decode must never panic; an accepted header must re-encode to the
    // exact input bytes (roundtrip identity — every field captured).
    if let Ok(h) = Hdr::decode(data, 0) {
        let mut out = [0u8; HDR_SIZE];
        h.encode(&mut out);
        assert_eq!(&out[..], &data[..HDR_SIZE], "decode->encode not identity");
    }
});
