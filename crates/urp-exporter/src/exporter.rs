//! The running exporter: owns the reused render buffer, the min-interval scrape
//! cache, the netlink socket, and the blocking accept loop (design 39 §39.3).
//!
//! Steady-state allocation discipline: the render buffer is allocated once and
//! `clear()`ed per render, never reallocated (design 39 rule 4). The scrape
//! itself still allocates -- `urp-netlink` returns owned `Vec`s today -- which is
//! the bounded, measured per-scrape cost noted as a follow-up (design 39 §39.11);
//! the cache keeps it off the hot path for bursts of scrapes.

use std::net::TcpListener;
use std::time::{Duration, Instant};

use urp_netlink::UrpSocket;

use crate::config::Config;
use crate::http;
use crate::render::{self, SelfStats};
use crate::scrape;

/// Decide whether a request at `now` can be served from a render taken at
/// `last`, given `ttl`. `None` last render (never scraped) always refreshes.
/// Pure, so the cache gate is table-testable (design 39 §39.7 `cache_ttl_gate`).
pub fn cache_fresh(last: Option<Duration>, ttl: Duration) -> bool {
    match last {
        Some(age) => age < ttl,
        None => false,
    }
}

pub struct Exporter {
    cfg: Config,
    sock: Option<UrpSocket>,
    /// The last rendered `/metrics` document, reused across scrapes.
    buf: String,
    /// Reused cardinality-cap scratch so a warm render allocates nothing
    /// (design 39 §39.8a); paired with `buf` in `render_into_scratch`.
    counts: Vec<usize>,
    /// When the buffer was last (re)rendered.
    last_render: Option<Instant>,
    stats: SelfStats,
    /// Test/stress double: when set (only reachable under the `mock` feature),
    /// scrapes serve this synthetic fleet instead of touching netlink, so the
    /// render + HTTP hot path is drivable with no kernel module (design 39 PR3).
    #[cfg(feature = "mock")]
    mock_fleet: Option<Vec<urp_netlink::Endpoint>>,
}

impl Exporter {
    pub fn new(cfg: Config) -> Exporter {
        Exporter {
            cfg,
            sock: None,
            buf: String::with_capacity(64 * 1024),
            counts: Vec::with_capacity(64),
            last_render: None,
            stats: SelfStats::default(),
            #[cfg(feature = "mock")]
            mock_fleet: None,
        }
    }

    /// Construct an exporter that serves a fixed synthetic fleet (no netlink).
    /// Only available under the `mock` feature -- used by the stress runner and
    /// dashboard demos (design 39 PR3).
    #[cfg(feature = "mock")]
    pub fn new_mock(cfg: Config, fleet: Vec<urp_netlink::Endpoint>) -> Exporter {
        let mut e = Exporter::new(cfg);
        e.mock_fleet = Some(fleet);
        e
    }

    /// Bind and serve forever. One connection handled at a time (Prometheus
    /// scrapes serially); errors on a single connection are logged and skipped.
    pub fn run(&mut self) -> std::io::Result<()> {
        let listener = TcpListener::bind(self.cfg.listen)?;
        eprintln!(
            "urp-exporter: serving /metrics on http://{}",
            self.cfg.listen
        );
        for conn in listener.incoming() {
            let mut stream = match conn {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("urp-exporter: accept error: {e}");
                    continue;
                }
            };
            self.refresh_if_stale();
            if let Err(e) = http::serve(&mut stream, self.cfg.scrape_timeout, &self.buf) {
                eprintln!("urp-exporter: serve error: {e}");
            }
        }
        Ok(())
    }

    /// Re-scrape + re-render unless the cached buffer is still within
    /// `--cache-ttl`. Updates the cache-hit / render bookkeeping.
    fn refresh_if_stale(&mut self) {
        let age = self.last_render.map(|t| t.elapsed());
        if cache_fresh(age, self.cfg.cache_ttl) {
            self.stats.cache_hits += 1;
            return;
        }
        self.scrape_and_render();
        self.last_render = Some(Instant::now());
    }

    /// One real scrape: (re)connect if needed, fetch, render into `self.buf`.
    /// A netlink failure renders a minimal `urp_up 0` document rather than
    /// erroring the connection.
    fn scrape_and_render(&mut self) {
        // Mock source (feature `mock` only): render the synthetic fleet with no
        // socket. Fields are borrowed disjointly, so no aliasing conflict.
        #[cfg(feature = "mock")]
        if let Some(fleet) = self.mock_fleet.as_ref() {
            let start = Instant::now();
            self.stats.up = true;
            self.stats.netlink_requests = 0;
            self.stats.endpoints = fleet.len();
            let capped = render::render_into_scratch(
                &mut self.buf,
                &mut self.counts,
                fleet,
                &self.cfg,
                &self.stats,
            );
            self.stats.series_capped += capped;
            self.stats.scrape_duration_seconds = start.elapsed().as_secs_f64();
            return;
        }

        // Lazily (re)connect; drop a dead socket so the next scrape retries.
        if self.sock.is_none() {
            match UrpSocket::connect() {
                Ok(s) => self.sock = Some(s),
                Err(e) => {
                    self.stats.up = false;
                    self.stats.scrape_errors += 1;
                    eprintln!("urp-exporter: netlink connect failed: {e}");
                    self.render_down();
                    return;
                }
            }
        }
        let start = Instant::now();
        let result = {
            let sock = self.sock.as_mut().expect("connected above");
            scrape::scrape(sock)
        };
        match result {
            Ok(r) => {
                self.stats.up = true;
                self.stats.scrape_duration_seconds = start.elapsed().as_secs_f64();
                self.stats.netlink_requests = r.netlink_requests;
                self.stats.endpoints = r.endpoints.len();
                let capped = render::render_into_scratch(
                    &mut self.buf,
                    &mut self.counts,
                    &r.endpoints,
                    &self.cfg,
                    &self.stats,
                );
                self.stats.series_capped += capped;
            }
            Err(e) => {
                self.stats.up = false;
                self.stats.scrape_errors += 1;
                self.sock = None; // force reconnect next time
                eprintln!("urp-exporter: scrape failed: {e}");
                self.render_down();
            }
        }
    }

    /// Render the minimal "module unreachable" document (`urp_up 0` + self
    /// metrics), so `up`/alerting still works when the module is not loaded.
    fn render_down(&mut self) {
        self.stats.up = false;
        self.stats.endpoints = 0;
        render::render_into_scratch(&mut self.buf, &mut self.counts, &[], &self.cfg, &self.stats);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // design 39 §39.7: (elapsed, ttl) -> served-from-cache vs refresh.
    #[test]
    fn cache_ttl_gate_truth_table() {
        let ttl = Duration::from_millis(250);
        // (last-render age, expected fresh?)
        let cases = [
            // positive: well within the window -> fresh (serve cache)
            (Some(Duration::from_millis(10)), true),
            // boundary: exactly at ttl -> NOT fresh (refresh)
            (Some(Duration::from_millis(250)), false),
            // boundary: one ms under -> fresh
            (Some(Duration::from_millis(249)), true),
            // negative: well past -> refresh
            (Some(Duration::from_millis(1000)), false),
            // corner: never rendered -> refresh
            (None, false),
        ];
        for (i, (age, want)) in cases.into_iter().enumerate() {
            assert_eq!(cache_fresh(age, ttl), want, "case {i}: age={age:?}");
        }
    }
}
