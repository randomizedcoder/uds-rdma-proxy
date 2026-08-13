//! The machine-parseable `BENCH_OK` result line (§30.8). The output must
//! be byte-identical to the C core's `bench_format_result` for the same
//! inputs — the harness parses both without knowing the language.

use crate::config::Config;
use crate::stats::StatsResult;

pub struct Report<'a> {
    /// `"c"` / `"rust"`.
    pub lang: &'a str,
    pub cfg: &'a Config,
    pub rtt: StatsResult,
    /// Own originals completed.
    pub msgs: u64,
    /// Own original wire bytes echoed back.
    pub bytes: u64,
    pub elapsed_ns: u64,
    pub syscalls: u64,
    pub cpu_ns: u64,
    /// Messages delivered via the assembly buffer.
    pub reassembled: u64,
    /// All messages received (originals + echoes).
    pub msgs_rx_total: u64,
}

impl Report<'_> {
    /// Single-line result, no trailing newline — same field order and
    /// formatting as the C `bench_format_result`.
    pub fn format(&self) -> String {
        let secs = self.elapsed_ns as f64 / 1e9;
        let (mut mbps, mut msgs_per_s) = (0.0f64, 0.0f64);
        if secs > 0.0 {
            mbps = self.bytes as f64 / 1e6 / secs;
            msgs_per_s = self.msgs as f64 / secs;
        }
        let (mut syscalls_per_msg, mut cpu_us_per_msg) = (0.0f64, 0.0f64);
        let denom = self.msgs + self.msgs_rx_total;
        if denom > 0 {
            syscalls_per_msg = self.syscalls as f64 / denom as f64;
            cpu_us_per_msg = self.cpu_ns as f64 / 1e3 / denom as f64;
        }
        let reassembled_pct = if self.msgs_rx_total > 0 {
            100.0 * self.reassembled as f64 / self.msgs_rx_total as f64
        } else {
            0.0
        };

        format!(
            "BENCH_OK lang={} mode={} msg_size={} batch={} msgs={} \
             mbps={:.1} msgs_per_s={:.0} p50_us={:.1} p99_us={:.1} \
             min_us={:.1} max_us={:.1} syscalls_per_msg={:.2} \
             cpu_us_per_msg={:.2} reassembled_pct={:.1} verify={}",
            self.lang,
            self.cfg.mode.as_str(),
            self.cfg.msg_size,
            self.cfg.batch,
            self.msgs,
            mbps,
            msgs_per_s,
            self.rtt.p50_ns as f64 / 1e3,
            self.rtt.p99_ns as f64 / 1e3,
            self.rtt.min_ns as f64 / 1e3,
            self.rtt.max_ns as f64 / 1e3,
            syscalls_per_msg,
            cpu_us_per_msg,
            reassembled_pct,
            self.cfg.verify.as_str(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Mode, Role, Verify};

    #[test]
    fn golden_line() {
        // Same inputs as C test_format_result; the assertions check the
        // same substrings, and the full line is pinned as a golden string.
        let cfg = Config {
            role: Some(Role::Connect),
            id: 1,
            mode: Mode::UringFixed,
            verify: Verify::Header,
            msg_size: 4076,
            batch: 32,
            count: 100_000,
            duration_s: 0,
            defer_taskrun: false,
        };
        let r = Report {
            lang: "c",
            cfg: &cfg,
            rtt: StatsResult {
                min_ns: 7900,
                max_ns: 310_000,
                p50_ns: 9800,
                p99_ns: 22_100,
                count: 100_000,
            },
            msgs: 100_000,
            bytes: 407_600_000,
            elapsed_ns: 500_000_000,
            syscalls: 26_000,
            cpu_ns: 380_000_000,
            reassembled: 800,
            msgs_rx_total: 200_000,
        };
        let line = r.format();
        assert_eq!(
            line,
            "BENCH_OK lang=c mode=uring-fixed msg_size=4076 batch=32 \
             msgs=100000 mbps=815.2 msgs_per_s=200000 p50_us=9.8 \
             p99_us=22.1 min_us=7.9 max_us=310.0 syscalls_per_msg=0.09 \
             cpu_us_per_msg=1.27 reassembled_pct=0.4 verify=header"
        );
    }
}
