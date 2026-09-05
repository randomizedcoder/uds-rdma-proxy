//! Exporter configuration + the hand-rolled arg/env parser (design 39 §39.3 --
//! "no `clap`"). Mirrors the `urp-bench` idiom: `std::env::args()`, a
//! `while i < args.len()` match loop, and a `usage() -> !` that exits 2.

use std::net::SocketAddr;
use std::time::Duration;

/// Everything the running exporter needs. Cheap to `Clone`; passed by reference
/// into the render hot path.
#[derive(Debug, Clone)]
pub struct Config {
    /// Address to bind the HTTP listener. Localhost by default -- front with a
    /// reverse proxy for remote scrape / TLS (design 39 §39.3 rule 7).
    pub listen: SocketAddr,
    /// Serve the cached render if the last scrape is younger than this
    /// (design 39 §39.3 rule 5 -- bounds netlink load under over-eager scrapers).
    pub cache_ttl: Duration,
    /// Emit per-QP series (`urp_qp_*`). On by default -- QP granularity is the
    /// view that visualises the mesh fairness latch (design 38 §38.5).
    pub per_qp: bool,
    /// Emit per-stream series (`urp_stream_*`). OFF by default -- streams are
    /// ephemeral and churn label cardinality (design 39 §39.2).
    pub per_stream: bool,
    /// Hard cap on emitted sample lines; past it the renderer stops and bumps
    /// `urp_exporter_series_capped_total` rather than serve an unbounded payload.
    pub max_series: usize,
    /// Per-scrape netlink deadline; also the HTTP header read timeout.
    pub scrape_timeout: Duration,
    /// Emit the RX inter-arrival histogram family (`urp_endpoint_interarrival_
    /// seconds_*`). On by default -- surfaces only when the module reports the
    /// nest (design 40 §40.1), so an old module simply emits nothing.
    pub interarrival: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            listen: SocketAddr::from(([127, 0, 0, 1], 9975)),
            cache_ttl: Duration::from_millis(250),
            per_qp: true,
            per_stream: false,
            max_series: 100_000,
            scrape_timeout: Duration::from_millis(2000),
            interarrival: true,
        }
    }
}

/// Print usage to stderr and exit(2), matching the `urp-bench` convention.
pub fn usage() -> ! {
    eprintln!(
        "usage: urp-exporter [options]\n\
         \x20 --listen ADDR:PORT     (default 127.0.0.1:9975)\n\
         \x20 --cache-ttl-ms MS      (default 250)\n\
         \x20 --scrape-timeout-ms MS (default 2000)\n\
         \x20 --max-series N         (default 100000)\n\
         \x20 --per-qp / --no-per-qp (default on)\n\
         \x20 --per-stream           (default off)\n\
         \x20 --no-interarrival      (default on; RX inter-arrival histogram)\n\
         serves Prometheus /metrics scraped from the urp kernel module."
    );
    std::process::exit(2);
}

impl Config {
    /// Parse argv (skipping argv0). Unknown flags or bad values -> `usage()`.
    pub fn from_args() -> Config {
        let args: Vec<String> = std::env::args().collect();
        Config::parse(&args[1..])
    }

    /// Pure parser over a token slice, so it is table-testable without touching
    /// the process's real argv.
    pub fn parse(args: &[String]) -> Config {
        let mut cfg = Config::default();
        let mut i = 0;
        while i < args.len() {
            let need = |i: usize| args.get(i + 1).cloned().unwrap_or_else(|| usage());
            match args[i].as_str() {
                "--listen" => {
                    cfg.listen = need(i).parse().unwrap_or_else(|_| usage());
                    i += 1;
                }
                "--cache-ttl-ms" => {
                    cfg.cache_ttl =
                        Duration::from_millis(need(i).parse().unwrap_or_else(|_| usage()));
                    i += 1;
                }
                "--scrape-timeout-ms" => {
                    cfg.scrape_timeout =
                        Duration::from_millis(need(i).parse().unwrap_or_else(|_| usage()));
                    i += 1;
                }
                "--max-series" => {
                    cfg.max_series = need(i).parse().unwrap_or_else(|_| usage());
                    i += 1;
                }
                "--per-qp" => cfg.per_qp = true,
                "--no-per-qp" => cfg.per_qp = false,
                "--per-stream" => cfg.per_stream = true,
                "--interarrival" => cfg.interarrival = true,
                "--no-interarrival" => cfg.interarrival = false,
                "--help" | "-h" => usage(),
                _ => usage(),
            }
            i += 1;
        }
        cfg
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // design 39 §39.3: the hand-rolled parser maps flags -> Config fields.
    #[test]
    fn parse_flags_truth_table() {
        let s = |v: &str| v.to_string();
        // Each case: (argv tokens, predicate on the resulting Config).
        let cases: [(Vec<String>, fn(&Config) -> bool); 7] = [
            // positive: an explicit listen addr round-trips
            (vec![s("--listen"), s("0.0.0.0:9100")], |c| {
                c.listen.port() == 9100 && c.listen.ip().is_unspecified()
            }),
            // positive: ttl + max-series parse into the right units
            (
                vec![s("--cache-ttl-ms"), s("500"), s("--max-series"), s("42")],
                |c| c.cache_ttl.as_millis() == 500 && c.max_series == 42,
            ),
            // boundary: empty argv -> all defaults
            (vec![], |c| {
                c.listen.port() == 9975
                    && c.per_qp
                    && !c.per_stream
                    && c.cache_ttl.as_millis() == 250
            }),
            // positive: --per-stream flips the opt-in on
            (vec![s("--per-stream")], |c| c.per_stream),
            // positive: --no-per-qp flips the default off
            (vec![s("--no-per-qp")], |c| !c.per_qp),
            // corner: scrape timeout parses independently of cache ttl
            (vec![s("--scrape-timeout-ms"), s("1234")], |c| {
                c.scrape_timeout.as_millis() == 1234 && c.cache_ttl.as_millis() == 250
            }),
            // design 40: interarrival is on by default, --no-interarrival flips it
            (vec![s("--no-interarrival")], |c| !c.interarrival),
        ];
        for (i, (argv, pred)) in cases.into_iter().enumerate() {
            let cfg = Config::parse(&argv);
            assert!(pred(&cfg), "case {i}: argv={argv:?} produced {cfg:?}");
        }
    }
}
