#![no_main]

//! Fuzz the exporter's hand-rolled HTTP head parser (design 39 §39.6): arbitrary
//! bytes fed to `route` + `status_for` must never panic or UB. This is the
//! exporter's only network-facing attack surface, so it is the priority target.

use libfuzzer_sys::fuzz_target;
use urp_exporter::http::{route, status_for};

fuzz_target!(|data: &[u8]| {
    // The request-line/head classifier must total over arbitrary bytes.
    let _ = route(data);

    // status_for over both boolean flags (timed_out, oversized) x arbitrary head.
    for &(timed_out, oversized) in &[(false, false), (true, false), (false, true), (true, true)] {
        let _ = status_for(data, timed_out, oversized);
    }
});
