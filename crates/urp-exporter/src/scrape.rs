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

    // io_uring fan-out (design 39 §39.4): one submit/reap for all N verbose GETs
    // instead of N serial round-trips. Falls back to the blocking loop on any
    // ring/socket error, so it is never worse than the default path.
    #[cfg(feature = "io-uring")]
    {
        let names: Vec<&str> = scalars.iter().map(|e| e.name.as_str()).collect();
        let n_names = names.len();
        match urp_netlink::get_endpoints_batch_uring(sock, &names) {
            Ok(verbose) => {
                drop(names); // release the borrow of `scalars` before consuming it
                let mut endpoints = Vec::with_capacity(scalars.len());
                for (scalar, full) in scalars.into_iter().zip(verbose) {
                    // Raced away / GET errored -> keep the scalar-only record.
                    endpoints.push(full.unwrap_or(scalar));
                }
                return Ok(ScrapeResult {
                    endpoints,
                    netlink_requests: 1 + n_names as u64,
                });
            }
            Err(e) => {
                eprintln!("urp-exporter: io_uring batch failed ({e}); using blocking GETs");
                // fall through to the blocking path below
            }
        }
    }

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
