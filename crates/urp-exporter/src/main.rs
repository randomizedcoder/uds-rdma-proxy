//! `urp-exporter` binary -- a thin shell over the `urp_exporter` library. Parse
//! argv into a `Config`, then run the blocking serve loop. All logic lives in
//! the library so it is unit-testable without a socket (design 39 §39.3).

use urp_exporter::config::Config;
use urp_exporter::exporter::Exporter;

fn main() {
    let cfg = Config::from_args();
    let mut exporter = Exporter::new(cfg);
    if let Err(e) = exporter.run() {
        eprintln!("urp-exporter: fatal: {e}");
        std::process::exit(1);
    }
}
