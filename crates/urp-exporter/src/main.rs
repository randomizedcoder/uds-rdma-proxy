//! `urp-exporter` binary -- a thin shell over the `urp_exporter` library. Parse
//! argv into a `Config`, then run the blocking serve loop. All logic lives in
//! the library so it is unit-testable without a socket (design 39 §39.3).

use urp_exporter::config::Config;
use urp_exporter::exporter::Exporter;

fn main() {
    // Bench mode (io-uring feature, hardware only): URP_EXPORTER_BENCH[=<name>]
    // runs the blocking-vs-io_uring N-sweep and exits (design 39 §39.4).
    #[cfg(feature = "io-uring")]
    if let Ok(v) = std::env::var("URP_EXPORTER_BENCH") {
        let target = if v.is_empty() { None } else { Some(v.as_str()) };
        urp_exporter::bench::run_and_exit(target);
    }

    let cfg = Config::from_args();
    let mut exporter = build_exporter(cfg);
    if let Err(e) = exporter.run() {
        eprintln!("urp-exporter: fatal: {e}");
        std::process::exit(1);
    }
}

/// Production build: always the real netlink-scraping exporter.
#[cfg(not(feature = "mock"))]
fn build_exporter(cfg: Config) -> Exporter {
    Exporter::new(cfg)
}

/// Mock build: if `URP_EXPORTER_MOCK=<endpoints>[,<qps>[,<streams>]]` is set,
/// serve that synthetic fleet with no netlink (design 39 PR3 stress runner);
/// otherwise behave exactly like production.
#[cfg(feature = "mock")]
fn build_exporter(cfg: Config) -> Exporter {
    match std::env::var("URP_EXPORTER_MOCK") {
        Ok(spec) => match urp_exporter::mock::parse_spec(&spec) {
            Some((n, q, s)) => {
                eprintln!("urp-exporter: MOCK fleet endpoints={n} qps={q} streams={s}");
                Exporter::new_mock(cfg, urp_exporter::mock::synthetic_fleet(n, q, s))
            }
            None => {
                eprintln!("urp-exporter: bad URP_EXPORTER_MOCK spec {spec:?}, scraping netlink");
                Exporter::new(cfg)
            }
        },
        Err(_) => Exporter::new(cfg),
    }
}
