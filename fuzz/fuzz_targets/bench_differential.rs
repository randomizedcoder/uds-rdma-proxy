#![no_main]

// C-vs-Rust differential for the urp-bench 24-byte header codec
// (design 30 §30.12, the F0 pattern from design 27): both decoders see
// the same bytes and must agree on accept/reject, the error code, and
// every decoded field. build.rs links the REAL tools/urp-bench-core.c.

use libfuzzer_sys::fuzz_target;
use urp_bench::frame::Hdr;
use urp_bench::Error;

#[repr(C)]
#[derive(Default)]
struct CBenchHdr {
    magic: u32,
    version: u8,
    flags: u8,
    origin_id: u16,
    payload_len: u32,
    seq: u32,
    t_send_ns: u64,
}

extern "C" {
    fn bench_hdr_decode(
        buf: *const u8,
        len: usize,
        max_payload: u32,
        out: *mut CBenchHdr,
    ) -> i32;
}

/// Rust Error -> the C core's positive BENCH_E* code (same order).
fn err_code(e: Error) -> i32 {
    e as i32 + 1
}

fuzz_target!(|input: (u32, &[u8])| {
    let (max_payload, data) = input;
    let mut c_out = CBenchHdr::default();
    // SAFETY: data/len describe a valid slice; c_out is a valid struct.
    let c_ret = unsafe { bench_hdr_decode(data.as_ptr(), data.len(), max_payload, &mut c_out) };

    match Hdr::decode(data, max_payload) {
        Ok(h) => {
            assert_eq!(c_ret, 0, "C rejected ({c_ret}) what Rust accepted");
            assert_eq!(c_out.magic, h.magic);
            assert_eq!(c_out.version, h.version);
            assert_eq!(c_out.flags, h.flags);
            assert_eq!(c_out.origin_id, h.origin_id);
            assert_eq!(c_out.payload_len, h.payload_len);
            assert_eq!(c_out.seq, h.seq);
            assert_eq!(c_out.t_send_ns, h.t_send_ns);
        }
        Err(e) => {
            assert_eq!(c_ret, -err_code(e), "error-code divergence");
        }
    }
});
