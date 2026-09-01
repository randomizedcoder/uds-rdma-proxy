//! The netlink scrape (design 39 §39.1). A full scrape is `1 dump + N verbose
//! GETs`: the cheap `dumpit` lists endpoint names + scalars (with
//! `stats: None`), then one verbose `doit` GET per endpoint fills its
//! `stats`/`qps`/`streams`. Blocking and serial -- `UrpSocket` holds a single fd
//! and a mutable seq counter, so scrapes must not share a socket across threads.
//!
//! PR4 (design 39 §39.4) will optionally batch the N GETs through io_uring; the
//! blocking dump stays as-is (it is multipart). PR1 is blocking throughout.

use urp_netlink::format::Endpoint;
use urp_netlink::{fetch_endpoints, get_endpoint, UrpError, UrpSocket};

/// The result of one scrape: the fully-filled endpoints plus how many netlink
/// round-trips it cost (for `urp_exporter_netlink_requests_total`).
pub struct ScrapeResult {
    pub endpoints: Vec<Endpoint>,
    pub netlink_requests: u64,
}

/// Perform a full scrape over `sock`. Errors from the initial dump propagate
/// (the whole scrape failed); a per-endpoint GET that fails or races to
/// nonexistence degrades to the scalar-only record from the dump rather than
/// failing the entire scrape.
pub fn scrape(sock: &mut UrpSocket) -> Result<ScrapeResult, UrpError> {
    let scalars = fetch_endpoints(sock, None)?; // 1 dump
    let mut requests = 1u64;
    let mut endpoints = Vec::with_capacity(scalars.len());
    for ep in scalars {
        requests += 1;
        match get_endpoint(sock, &ep.name) {
            Ok(Some(full)) => endpoints.push(full),
            // Raced away between dump and GET, or a transient GET error: keep the
            // scalar record so the endpoint still appears (with gauges only).
            Ok(None) | Err(_) => endpoints.push(ep),
        }
    }
    Ok(ScrapeResult {
        endpoints,
        netlink_requests: requests,
    })
}
