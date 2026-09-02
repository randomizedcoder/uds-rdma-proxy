//! Synthetic `Endpoint` fleet for tests, benches, and the stress runner
//! (design 39 PR3). Building a *real* generic-netlink mock is impossible in
//! userspace -- the `urp` genl family only exists when the kernel module is
//! loaded -- so instead we inject a deterministic synthetic fleet at the data
//! boundary (`Vec<Endpoint>`). This drives the *real* render + HTTP hot path
//! with no module/hardware, which is exactly what the CPU/RSS/alloc budget
//! (design 39 §39.3 rule 6, §39.8a) needs to be measured against.
//!
//! Only compiled under `#[cfg(test)]` or the `mock` feature, so it never lands
//! in the production binary.

use urp_netlink::format::{Endpoint, Qp, Stats, Stream};

/// Build a deterministic fleet of `endpoints` endpoints, each with `qps` QPs and
/// `streams` streams. Counters vary by index so the rendered exposition looks
/// like a real fleet (distinct series, non-trivial values) while staying a pure
/// function of the inputs -- reproducible across runs (no clock, no RNG).
pub fn synthetic_fleet(endpoints: usize, qps: usize, streams: usize) -> Vec<Endpoint> {
    (0..endpoints)
        .map(|e| {
            let base = (e as u64 + 1) * 1_000_000;
            Endpoint {
                name: format!("mock{e}"),
                listen_path: Some(format!("/run/urp-mock-{e}.sock")),
                connect_path: None,
                rdma_device: Some("mlx5_0".to_string()),
                peer_addr: Some(format!("10.10.0.{}:4791", (e % 250) + 1)),
                bind_addr: None,
                num_qps: qps as u32,
                buffer_count: 4096,
                buffer_size: 65516,
                state: "active".to_string(),
                qps: (0..qps)
                    .map(|q| Qp {
                        index: q as u32,
                        state: "active".to_string(),
                        rtt_ns: 1_000 + (q as u64) * 37,
                        tx_bytes: base + q as u64 * 111,
                        rx_bytes: base + q as u64 * 113,
                        tx_frames: base / 1_000 + q as u64,
                        rx_frames: base / 1_000 + q as u64,
                    })
                    .collect(),
                streams: (0..streams)
                    .map(|s| Stream {
                        id: s as u32,
                        state: "established".to_string(),
                        tx_bytes: base + s as u64,
                        rx_bytes: base + s as u64,
                        reorder_depth: (s % 8) as u32,
                        credits_local: 64,
                        credits_remote: 64,
                    })
                    .collect(),
                stats: Some(Stats {
                    active_streams: streams as u32,
                    tx_bytes: base,
                    rx_bytes: base,
                    tx_frames: base / 1_000,
                    rx_frames: base / 1_000,
                    credit_stalls: e as u64,
                    reorder_insertions: e as u64 * 2,
                    reorder_drops: 0,
                    buffer_alloc_fails: 0,
                    auth_failures: 0,
                }),
            }
        })
        .collect()
}

/// Parse a `"<endpoints>,<qps>,<streams>"` spec (the `URP_EXPORTER_MOCK` env
/// value) into counts, defaulting missing fields. Returns `None` on a
/// non-numeric field so the caller can fall back to a real scrape.
pub fn parse_spec(spec: &str) -> Option<(usize, usize, usize)> {
    let mut it = spec.split(',');
    let n = it.next()?.trim().parse().ok()?;
    let q = it.next().map_or(Ok(4), |s| s.trim().parse()).ok()?;
    let s = it.next().map_or(Ok(0), |s| s.trim().parse()).ok()?;
    Some((n, q, s))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn synthetic_fleet_shape() {
        // Each case: (endpoints, qps, streams) -> structural expectations.
        let cases = [
            // positive: typical small fleet
            (3usize, 4usize, 2usize),
            // boundary: single endpoint, no streams
            (1, 1, 0),
            // corner: zero endpoints -> empty fleet
            (0, 8, 8),
            // positive: larger fan-out
            (5, 8, 4),
        ];
        for (i, (n, q, s)) in cases.into_iter().enumerate() {
            let fleet = synthetic_fleet(n, q, s);
            assert_eq!(fleet.len(), n, "case {i}: endpoint count");
            for ep in &fleet {
                assert_eq!(ep.qps.len(), q, "case {i}: qp count");
                assert_eq!(ep.streams.len(), s, "case {i}: stream count");
                assert!(ep.stats.is_some(), "case {i}: stats present");
                // names are unique + non-empty (distinct series)
                assert!(!ep.name.is_empty(), "case {i}: named");
            }
        }
    }

    #[test]
    fn parse_spec_truth_table() {
        // Each case: (input, expected).
        let cases: [(&str, Option<(usize, usize, usize)>); 6] = [
            // positive: full spec
            ("10,8,4", Some((10, 8, 4))),
            // positive: endpoints only -> qps/streams defaults
            ("5", Some((5, 4, 0))),
            // positive: endpoints+qps
            ("5,2", Some((5, 2, 0))),
            // boundary: zero endpoints is valid
            ("0", Some((0, 4, 0))),
            // negative: non-numeric -> None
            ("abc", None),
            // negative: bad second field -> None
            ("5,x", None),
        ];
        for (i, (input, want)) in cases.into_iter().enumerate() {
            assert_eq!(parse_spec(input), want, "case {i}: {input:?}");
        }
    }
}
