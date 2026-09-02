#![no_main]

//! Fuzz the netlink-reply decoder (design 39 §39.6): arbitrary bytes fed to
//! `Endpoint::parse_top` / `parse_nested` must never panic or UB. The exporter
//! trusts the kernel for these bytes, but a decode that panics on a malformed
//! (or truncated, or hostile) reply would crash the scraper -- so it is fuzzed.

use libfuzzer_sys::fuzz_target;
use urp_netlink::format::Endpoint;

fuzz_target!(|data: &[u8]| {
    // Top-level GENL/attr walk over arbitrary bytes.
    let _ = Endpoint::parse_top(data);
    // Nested single-endpoint decode over arbitrary bytes.
    let _ = Endpoint::parse_nested(data);
});
