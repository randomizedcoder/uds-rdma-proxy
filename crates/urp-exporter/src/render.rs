//! Prometheus exposition renderer (design 39 §39.2). Hand-written text -- no
//! `serde_json`, no intermediate `Value` -- rendered into a caller-owned,
//! reused `String` so the steady-state path allocates nothing (design 39 rule 4).
//!
//! Families are emitted grouped (all samples of a metric contiguous, `# HELP` /
//! `# TYPE` exactly once) because the Prometheus text parser requires it; that
//! means an outer loop over families and an inner loop over endpoints/QPs. Both
//! bounds are tiny (a node runs a handful-to-low-tens of endpoints, <=8 QPs),
//! so the multi-pass cost is negligible and the grouping is trivially correct.

use std::fmt::Write;

use urp_netlink::format::{Endpoint, Qp, Stats, Stream, URP_HIST_EDGES_NS, URP_OWD_EDGES_NS};
use urp_netlink::uapi::{URP_HIST_NBUCKETS, URP_OWD_NBUCKETS};

use crate::config::Config;

/// Enumerated states we emit as a stateset (design 39 §39.2). Kept in lock-step
/// with `urp_netlink::uapi::{UrpEndpointState,UrpQpState,UrpStreamState}::as_str`.
const ENDPOINT_STATES: &[&str] = &["creating", "active", "draining", "stopped"];
const QP_STATES: &[&str] = &["qualifying", "active", "draining", "removed"];
const STREAM_STATES: &[&str] = &[
    "syn-sent",
    "syn-received",
    "established",
    "fin-wait",
    "close-wait",
    "closed",
];

/// Exporter self-observability snapshot, rendered as `urp_exporter_*` + `urp_up`.
#[derive(Debug, Default, Clone)]
pub struct SelfStats {
    /// Module loaded + netlink reachable this scrape.
    pub up: bool,
    /// Wall time of the last real (non-cached) scrape.
    pub scrape_duration_seconds: f64,
    /// Netlink round-trips in the last scrape (1 dump + N verbose GETs).
    pub netlink_requests: u64,
    /// Cumulative scrape errors since start.
    pub scrape_errors: u64,
    /// Cumulative cache hits (served within `--cache-ttl`).
    pub cache_hits: u64,
    /// Endpoints seen in the last scrape.
    pub endpoints: usize,
    /// Cumulative sample lines dropped by the `--max-series` cap.
    pub series_capped: u64,
}

/// Append `s` to `out` with Prometheus label-value escaping (`\` `"` newline).
/// Backslash MUST be escaped first so the escapes we add are not re-escaped.
pub fn escape_label_value(s: &str, out: &mut String) {
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            _ => out.push(c),
        }
    }
}

/// Number of sample lines a single endpoint contributes under `cfg`. Pure, so
/// the cardinality cap is table-testable (design 39 §39.7 `cardinality_cap`).
pub fn endpoint_series_count(ep: &Endpoint, cfg: &Config) -> usize {
    // endpoint counters (9, only when stats present) + gauges (active_streams
    // when stats present, num_qps always) + endpoint stateset + info line.
    let mut n = 0;
    if ep.stats.is_some() {
        n += 9; // the nine urp_endpoint_*_total counters
        n += 1; // urp_endpoint_active_streams
    }
    n += 1; // urp_endpoint_num_qps
    n += ENDPOINT_STATES.len() + 1; // stateset + the unknown series
    n += 1; // urp_endpoint_info
    if cfg.per_qp {
        // per QP: 4 counters + rtt gauge + stateset(+unknown)
        let per_qp = 4 + 1 + QP_STATES.len() + 1;
        n += ep.qps.len() * per_qp;
    }
    if cfg.per_stream {
        // per stream: 2 counters + 3 gauges + stateset(+unknown)
        let per_stream = 2 + 3 + STREAM_STATES.len() + 1;
        n += ep.streams.len() * per_stream;
    }
    if cfg.interarrival {
        // per stride: NBUCKETS _bucket lines + _sum + _count (design 40 §40.1).
        n += ep.interarrival.len() * (URP_HIST_NBUCKETS + 2);
    }
    if cfg.owd && ep.owd.is_some() {
        // OWD hist: NBUCKETS _bucket lines + _sum + _count, plus the clock-offset
        // gauge + anomalies counter (design 40 §40.2).
        n += URP_OWD_NBUCKETS + 2 + 2;
    }
    n
}

/// Greedily include whole endpoints while the running total stays within `max`.
/// Returns `(endpoints_included, capped_series)` -- the tail that did not fit is
/// counted, not rendered. Pure + table-tested (design 39 §39.7).
pub fn select_within_cap(counts: &[usize], max: usize) -> (usize, u64) {
    let mut running = 0usize;
    let mut included = 0usize;
    let mut capped = 0u64;
    for &c in counts {
        if running + c <= max {
            running += c;
            included += 1;
        } else {
            capped += c as u64;
        }
    }
    (included, capped)
}

/// Render the whole `/metrics` document into `buf` (cleared first). Returns the
/// number of sample lines the `--max-series` cap dropped this render, which the
/// caller folds into `SelfStats::series_capped`.
pub fn render_into(buf: &mut String, eps: &[Endpoint], cfg: &Config, s: &SelfStats) -> u64 {
    // Convenience wrapper for tests/one-shot callers: allocates a throwaway
    // scratch. The serve loop uses `render_into_scratch` with a reused scratch
    // so a steady-state render allocates nothing (design 39 §39.8a).
    let mut scratch = Vec::new();
    render_into_scratch(buf, &mut scratch, eps, cfg, s)
}

/// Same as [`render_into`] but takes a caller-owned `counts` scratch `Vec` so a
/// warm call (buffer + scratch already at capacity) performs **zero** heap
/// allocations. This is the hot path the `Exporter` drives; the alloc bench
/// asserts 0 allocs/render against it (design 39 §39.8a).
pub fn render_into_scratch(
    buf: &mut String,
    counts: &mut Vec<usize>,
    eps: &[Endpoint],
    cfg: &Config,
    s: &SelfStats,
) -> u64 {
    buf.clear();

    // Cardinality cap: pick the prefix of endpoints that fits under max_series.
    // Reuse the caller's scratch (cleared, capacity retained) so this is
    // alloc-free once warm.
    counts.clear();
    counts.extend(eps.iter().map(|e| endpoint_series_count(e, cfg)));
    let (included, capped_now) = select_within_cap(counts, cfg.max_series);
    let eps = &eps[..included];
    if capped_now > 0 {
        let _ = writeln!(
            buf,
            "# capped: {capped_now} series over --max-series={} dropped",
            cfg.max_series
        );
    }

    render_self(buf, s, capped_now);
    render_endpoints(buf, eps, cfg);
    render_qps(buf, eps, cfg);
    if cfg.per_stream {
        render_streams(buf, eps);
    }
    render_interarrival(buf, eps, cfg);
    render_owd(buf, eps, cfg);
    capped_now
}

fn render_self(buf: &mut String, s: &SelfStats, capped_now: u64) {
    let git = option_env!("URP_GIT").unwrap_or("unknown");
    let ver = env!("CARGO_PKG_VERSION");
    help(
        buf,
        "urp_exporter_build_info",
        "gauge",
        "Exporter build metadata.",
    );
    let _ = writeln!(
        buf,
        "urp_exporter_build_info{{version=\"{ver}\",git=\"{git}\"}} 1"
    );

    help(
        buf,
        "urp_up",
        "gauge",
        "1 if the urp module is loaded and netlink is reachable.",
    );
    let _ = writeln!(buf, "urp_up {}", u8::from(s.up));

    gauge(
        buf,
        "urp_exporter_scrape_duration_seconds",
        "Wall time of the last scrape.",
        s.scrape_duration_seconds,
    );
    gauge(
        buf,
        "urp_exporter_endpoints",
        "Endpoints observed in the last scrape.",
        s.endpoints as f64,
    );
    counter(
        buf,
        "urp_exporter_netlink_requests_total",
        "Netlink round-trips (dump + N GETs).",
        s.netlink_requests,
    );
    counter(
        buf,
        "urp_exporter_scrape_errors_total",
        "Scrapes that failed.",
        s.scrape_errors,
    );
    counter(
        buf,
        "urp_exporter_cache_hits_total",
        "Requests served from the min-interval cache.",
        s.cache_hits,
    );
    counter(
        buf,
        "urp_exporter_series_capped_total",
        "Sample lines dropped by --max-series.",
        s.series_capped + capped_now,
    );
}

fn render_endpoints(buf: &mut String, eps: &[Endpoint], _cfg: &Config) {
    // Counters -- emitted only for endpoints whose verbose GET carried stats.
    let counters: [(&str, &str, fn(&Stats) -> u64); 9] = [
        ("urp_endpoint_tx_bytes_total", "Bytes transmitted.", |s| {
            s.tx_bytes
        }),
        ("urp_endpoint_rx_bytes_total", "Bytes received.", |s| {
            s.rx_bytes
        }),
        ("urp_endpoint_tx_frames_total", "Frames transmitted.", |s| {
            s.tx_frames
        }),
        ("urp_endpoint_rx_frames_total", "Frames received.", |s| {
            s.rx_frames
        }),
        (
            "urp_endpoint_credit_stalls_total",
            "Times TX blocked on flow-control credit.",
            |s| s.credit_stalls,
        ),
        (
            "urp_endpoint_reorder_insertions_total",
            "Frames inserted into the reorder queue.",
            |s| s.reorder_insertions,
        ),
        (
            "urp_endpoint_reorder_drops_total",
            "Frames dropped by the reorder queue (loss).",
            |s| s.reorder_drops,
        ),
        (
            "urp_endpoint_buffer_alloc_fails_total",
            "Receive-buffer allocation failures.",
            |s| s.buffer_alloc_fails,
        ),
        (
            "urp_endpoint_auth_failures_total",
            "PSK/authentication failures.",
            |s| s.auth_failures,
        ),
    ];
    for (name, h, get) in counters {
        help(buf, name, "counter", h);
        for ep in eps {
            if let Some(st) = &ep.stats {
                let _ = write!(buf, "{name}");
                ep_labels(buf, ep);
                let _ = writeln!(buf, " {}", get(st));
            }
        }
    }

    // Gauges.
    help(
        buf,
        "urp_endpoint_active_streams",
        "gauge",
        "Currently active streams.",
    );
    for ep in eps {
        if let Some(st) = &ep.stats {
            let _ = write!(buf, "urp_endpoint_active_streams");
            ep_labels(buf, ep);
            let _ = writeln!(buf, " {}", st.active_streams);
        }
    }
    help(
        buf,
        "urp_endpoint_num_qps",
        "gauge",
        "Configured queue pairs.",
    );
    for ep in eps {
        let _ = write!(buf, "urp_endpoint_num_qps");
        ep_labels(buf, ep);
        let _ = writeln!(buf, " {}", ep.num_qps);
    }

    // Stateset.
    help(
        buf,
        "urp_endpoint_state",
        "gauge",
        "Endpoint state (1 for the current state).",
    );
    for ep in eps {
        emit_stateset(buf, "urp_endpoint_state", ENDPOINT_STATES, &ep.state, |b| {
            ep_labels_open(b, ep)
        });
    }

    // Info.
    help(
        buf,
        "urp_endpoint_info",
        "gauge",
        "Static endpoint configuration (always 1).",
    );
    for ep in eps {
        let _ = write!(buf, "urp_endpoint_info{{endpoint=\"");
        escape_label_value(&ep.name, buf);
        buf.push('"');
        opt_label(buf, "device", &ep.rdma_device);
        opt_label(buf, "listen_path", &ep.listen_path);
        opt_label(buf, "connect_path", &ep.connect_path);
        opt_label(buf, "peer", &ep.peer_addr);
        opt_label(buf, "bind", &ep.bind_addr);
        let _ = write!(buf, ",buffer_size=\"{}\"", ep.buffer_size);
        let _ = write!(buf, ",buffer_count=\"{}\"", ep.buffer_count);
        let _ = writeln!(buf, "}} 1");
    }
}

fn render_qps(buf: &mut String, eps: &[Endpoint], cfg: &Config) {
    if !cfg.per_qp {
        return;
    }
    let counters: [(&str, &str, fn(&Qp) -> u64); 4] = [
        (
            "urp_qp_tx_bytes_total",
            "Bytes transmitted on the QP.",
            |q| q.tx_bytes,
        ),
        ("urp_qp_rx_bytes_total", "Bytes received on the QP.", |q| {
            q.rx_bytes
        }),
        (
            "urp_qp_tx_frames_total",
            "Frames transmitted on the QP.",
            |q| q.tx_frames,
        ),
        (
            "urp_qp_rx_frames_total",
            "Frames received on the QP.",
            |q| q.rx_frames,
        ),
    ];
    for (name, h, get) in counters {
        help(buf, name, "counter", h);
        for ep in eps {
            for qp in &ep.qps {
                let _ = write!(buf, "{name}");
                qp_labels(buf, ep, qp);
                let _ = writeln!(buf, " {}", get(qp));
            }
        }
    }
    help(
        buf,
        "urp_qp_rtt_ns",
        "gauge",
        "EWMA round-trip time (ns; 0 until first PONG).",
    );
    for ep in eps {
        for qp in &ep.qps {
            let _ = write!(buf, "urp_qp_rtt_ns");
            qp_labels(buf, ep, qp);
            let _ = writeln!(buf, " {}", qp.rtt_ns);
        }
    }
    help(
        buf,
        "urp_qp_state",
        "gauge",
        "QP state (1 for the current state).",
    );
    for ep in eps {
        for qp in &ep.qps {
            emit_stateset(buf, "urp_qp_state", QP_STATES, &qp.state, |b| {
                qp_labels_open(b, ep, qp)
            });
        }
    }
}

fn render_streams(buf: &mut String, eps: &[Endpoint]) {
    let counters: [(&str, &str, fn(&Stream) -> u64); 2] = [
        (
            "urp_stream_tx_bytes_total",
            "Bytes transmitted on the stream.",
            |s| s.tx_bytes,
        ),
        (
            "urp_stream_rx_bytes_total",
            "Bytes received on the stream.",
            |s| s.rx_bytes,
        ),
    ];
    for (name, h, get) in counters {
        help(buf, name, "counter", h);
        for ep in eps {
            for st in &ep.streams {
                let _ = write!(buf, "{name}");
                stream_labels(buf, ep, st);
                let _ = writeln!(buf, " {}", get(st));
            }
        }
    }
    let gauges: [(&str, &str, fn(&Stream) -> u64); 3] = [
        (
            "urp_stream_reorder_depth",
            "Frames currently queued for reorder.",
            |s| s.reorder_depth as u64,
        ),
        (
            "urp_stream_credits_local",
            "Local flow-control credits.",
            |s| s.credits_local as u64,
        ),
        (
            "urp_stream_credits_remote",
            "Remote flow-control credits.",
            |s| s.credits_remote as u64,
        ),
    ];
    for (name, h, get) in gauges {
        help(buf, name, "gauge", h);
        for ep in eps {
            for st in &ep.streams {
                let _ = write!(buf, "{name}");
                stream_labels(buf, ep, st);
                let _ = writeln!(buf, " {}", get(st));
            }
        }
    }
}

/// RX inter-arrival histogram family (design 40 §40.1). The module stores
/// per-bucket counts; we accumulate them into the Prometheus cumulative
/// `_bucket` contract as we emit, so the hot path allocates nothing. Emitted
/// only when enabled AND at least one endpoint carries the nest (an old module
/// reports none, so the family silently vanishes).
fn render_interarrival(buf: &mut String, eps: &[Endpoint], cfg: &Config) {
    if !cfg.interarrival || eps.iter().all(|e| e.interarrival.is_empty()) {
        return;
    }
    help(
        buf,
        "urp_endpoint_interarrival_seconds",
        "histogram",
        "RX inter-arrival time between delivered DATA frames, per sampling stride (1/10/100).",
    );
    for ep in eps {
        for h in &ep.interarrival {
            let mut running = 0u64;
            for (i, &c) in h.buckets.iter().enumerate() {
                running += c;
                let _ = write!(buf, "urp_endpoint_interarrival_seconds_bucket");
                ia_labels_le(buf, ep, h.stride, i);
                let _ = writeln!(buf, " {running}");
            }
            let _ = write!(buf, "urp_endpoint_interarrival_seconds_sum");
            ia_labels(buf, ep, h.stride);
            let _ = writeln!(buf, " {}", h.sum_ns as f64 / 1e9);
            let _ = write!(buf, "urp_endpoint_interarrival_seconds_count");
            ia_labels(buf, ep, h.stride);
            let _ = writeln!(buf, " {}", h.count);
        }
    }
}

/// `{endpoint,device,stride}` -- the inter-arrival label set for `_sum`/`_count`.
fn ia_labels(buf: &mut String, ep: &Endpoint, stride: u32) {
    ep_labels_open(buf, ep);
    let _ = write!(buf, ",stride=\"{stride}\"}}");
}

/// Same, plus the `le` bucket bound. Bucket `i < NBUCKETS-1` uses the finite
/// edge (ns -> seconds); the last bucket is `+Inf`. Edges come from the const
/// table shared with the kernel header (design 40 §40.1).
fn ia_labels_le(buf: &mut String, ep: &Endpoint, stride: u32, bucket: usize) {
    ep_labels_open(buf, ep);
    let _ = write!(buf, ",stride=\"{stride}\",le=\"");
    if bucket + 1 < URP_HIST_NBUCKETS {
        let secs = URP_HIST_EDGES_NS[bucket] as f64 / 1e9;
        let _ = write!(buf, "{secs:e}");
    } else {
        let _ = write!(buf, "+Inf");
    }
    buf.push_str("\"}");
}

/// RX one-way delivery-latency family (design 40 §40.2): a single classic
/// histogram plus the PTP clock-offset gauge and the skew-anomaly counter. Like
/// the inter-arrival family it accumulates per-bucket counts into the cumulative
/// `_bucket` contract as it emits (zero steady-state alloc) and is emitted only
/// when enabled AND at least one endpoint carries the nest -- a module without
/// the feature, an unsampled endpoint, or an un-negotiated peer reports `None`,
/// so the whole family silently vanishes.
fn render_owd(buf: &mut String, eps: &[Endpoint], cfg: &Config) {
    if !cfg.owd || eps.iter().all(|e| e.owd.is_none()) {
        return;
    }
    help(
        buf,
        "urp_endpoint_owd_seconds",
        "histogram",
        "RX one-way delivery latency (sender->receiver) of sampled DATA frames; needs PTP-synced clocks.",
    );
    for ep in eps {
        let Some(o) = &ep.owd else { continue };
        let mut running = 0u64;
        for (i, &c) in o.hist.buckets.iter().enumerate() {
            running += c;
            let _ = write!(buf, "urp_endpoint_owd_seconds_bucket");
            owd_labels_le(buf, ep, i);
            let _ = writeln!(buf, " {running}");
        }
        let _ = write!(buf, "urp_endpoint_owd_seconds_sum");
        ep_labels(buf, ep);
        let _ = writeln!(buf, " {}", o.hist.sum_ns as f64 / 1e9);
        let _ = write!(buf, "urp_endpoint_owd_seconds_count");
        ep_labels(buf, ep);
        let _ = writeln!(buf, " {}", o.hist.count);
    }
    // PTP servo offset (last observed, nanoseconds -> seconds). 0 == unknown.
    help(
        buf,
        "urp_endpoint_owd_clock_offset_seconds",
        "gauge",
        "Last PTP servo clock offset between this receiver and its peers; 0 = unknown.",
    );
    for ep in eps {
        let Some(o) = &ep.owd else { continue };
        let _ = write!(buf, "urp_endpoint_owd_clock_offset_seconds");
        ep_labels(buf, ep);
        let _ = writeln!(buf, " {}", o.clock_offset_ns as f64 / 1e9);
    }
    // Samples rejected as clock-skewed (negative OWD), never bucketed.
    help(
        buf,
        "urp_endpoint_owd_anomalies_total",
        "counter",
        "Sampled frames whose computed OWD was negative (clock skew); rejected, not bucketed.",
    );
    for ep in eps {
        let Some(o) = &ep.owd else { continue };
        let _ = write!(buf, "urp_endpoint_owd_anomalies_total");
        ep_labels(buf, ep);
        let _ = writeln!(buf, " {}", o.anomalies);
    }
}

/// `{endpoint,device,le}` -- the OWD bucket label set. Bucket `i < NBUCKETS-1`
/// uses the finite edge (ns -> seconds); the last bucket is `+Inf`. Edges come
/// from the const table shared with the kernel header (design 40 §40.2).
fn owd_labels_le(buf: &mut String, ep: &Endpoint, bucket: usize) {
    ep_labels_open(buf, ep);
    buf.push_str(",le=\"");
    if bucket + 1 < URP_OWD_NBUCKETS {
        let secs = URP_OWD_EDGES_NS[bucket] as f64 / 1e9;
        let _ = write!(buf, "{secs:e}");
    } else {
        let _ = write!(buf, "+Inf");
    }
    buf.push_str("\"}");
}

/* ---- label + header helpers ------------------------------------------- */

fn help(buf: &mut String, name: &str, kind: &str, text: &str) {
    let _ = writeln!(buf, "# HELP {name} {text}");
    let _ = writeln!(buf, "# TYPE {name} {kind}");
}

fn gauge(buf: &mut String, name: &str, text: &str, v: f64) {
    help(buf, name, "gauge", text);
    let _ = writeln!(buf, "{name} {v}");
}

fn counter(buf: &mut String, name: &str, text: &str, v: u64) {
    help(buf, name, "counter", text);
    let _ = writeln!(buf, "{name} {v}");
}

/// `{endpoint="..",device=".."}` -- the common endpoint label set.
fn ep_labels(buf: &mut String, ep: &Endpoint) {
    ep_labels_open(buf, ep);
    buf.push('}');
}

/// Same as [`ep_labels`] but leaves the brace open so a stateset can append
/// `,state="..."`.
fn ep_labels_open(buf: &mut String, ep: &Endpoint) {
    buf.push_str("{endpoint=\"");
    escape_label_value(&ep.name, buf);
    buf.push('"');
    let _ = write!(buf, ",device=\"");
    escape_label_value(ep.rdma_device.as_deref().unwrap_or(""), buf);
    buf.push('"');
}

fn qp_labels(buf: &mut String, ep: &Endpoint, qp: &Qp) {
    qp_labels_open(buf, ep, qp);
    buf.push('}');
}

fn qp_labels_open(buf: &mut String, ep: &Endpoint, qp: &Qp) {
    ep_labels_open(buf, ep);
    let _ = write!(buf, ",qp=\"{}\"", qp.index);
}

fn stream_labels(buf: &mut String, ep: &Endpoint, st: &Stream) {
    buf.push_str("{endpoint=\"");
    escape_label_value(&ep.name, buf);
    let _ = write!(buf, "\",stream=\"{}\"}}", st.id);
}

/// Append `,name="value"` only when the option is present (design 39 §39.7
/// `render_endpoint_exposition` corner: absent config fields are omitted).
fn opt_label(buf: &mut String, name: &str, v: &Option<String>) {
    if let Some(v) = v {
        let _ = write!(buf, ",{name}=\"");
        escape_label_value(v, buf);
        buf.push('"');
    }
}

/// Emit one series per known state (value 0/1) plus an `unknown` series that is
/// 1 iff `actual` matched none of them -- so exactly one series is ever 1, and
/// an unknown/empty state never panics (design 39 §39.7 `state_to_stateset`).
fn emit_stateset(
    buf: &mut String,
    name: &str,
    states: &[&str],
    actual: &str,
    open_labels: impl Fn(&mut String),
) {
    let mut matched = false;
    for &st in states {
        let v = u8::from(st == actual);
        if v == 1 {
            matched = true;
        }
        let _ = write!(buf, "{name}");
        open_labels(buf);
        let _ = writeln!(buf, ",state=\"{st}\"}} {v}");
    }
    let _ = write!(buf, "{name}");
    open_labels(buf);
    let _ = writeln!(buf, ",state=\"unknown\"}} {}", u8::from(!matched));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ep_fixture() -> Endpoint {
        Endpoint {
            name: "pair_acceptor".into(),
            listen_path: Some("/run/urp-echo.sock".into()),
            connect_path: None,
            rdma_device: Some("mlx5_0".into()),
            peer_addr: None,
            bind_addr: Some("10.10.13.1:4791".into()),
            num_qps: 2,
            buffer_count: 256,
            buffer_size: 4096,
            state: "active".into(),
            qps: vec![
                Qp {
                    index: 0,
                    state: "active".into(),
                    rtt_ns: 1500,
                    tx_bytes: 10,
                    rx_bytes: 20,
                    tx_frames: 1,
                    rx_frames: 2,
                },
                Qp {
                    index: 1,
                    state: "qualifying".into(),
                    rtt_ns: 0,
                    tx_bytes: 0,
                    rx_bytes: 0,
                    tx_frames: 0,
                    rx_frames: 0,
                },
            ],
            streams: vec![],
            stats: Some(Stats {
                active_streams: 3,
                tx_bytes: 1000,
                rx_bytes: 2000,
                tx_frames: 10,
                rx_frames: 20,
                credit_stalls: 0,
                reorder_insertions: 5,
                reorder_drops: 0,
                buffer_alloc_fails: 0,
                auth_failures: 0,
            }),
            interarrival: vec![],
            owd: None,
        }
    }

    // design 39 §39.7: escaping of label values.
    #[test]
    fn escape_label_value_truth_table() {
        let cases = [
            // positive: plain text is untouched
            ("mlx5_0", "mlx5_0"),
            // corner: empty string
            ("", ""),
            // negative-ish: the three characters that MUST be escaped
            ("a\\b", "a\\\\b"),
            ("a\"b", "a\\\"b"),
            ("a\nb", "a\\nb"),
            // boundary: backslash escaped before quote (order matters)
            ("\\\"", "\\\\\\\""),
        ];
        for (i, (raw, want)) in cases.into_iter().enumerate() {
            let mut out = String::new();
            escape_label_value(raw, &mut out);
            assert_eq!(out, want, "case {i}: raw={raw:?}");
        }
    }

    // design 39 §39.7: state string -> which stateset series is 1.
    #[test]
    fn state_to_stateset_truth_table() {
        // (states, actual, expected series that is 1)
        let cases: [(&[&str], &str, &str); 5] = [
            // positive: a known endpoint state
            (ENDPOINT_STATES, "active", "active"),
            // boundary: the last known state
            (ENDPOINT_STATES, "stopped", "stopped"),
            // corner: unknown value -> the unknown series, never a panic
            (ENDPOINT_STATES, "unknown(9)", "unknown"),
            // corner: empty state -> unknown
            (QP_STATES, "", "unknown"),
            // positive: qp state
            (QP_STATES, "qualifying", "qualifying"),
        ];
        for (i, (states, actual, want_one)) in cases.into_iter().enumerate() {
            let mut buf = String::new();
            emit_stateset(&mut buf, "m", states, actual, |b| b.push_str("{"));
            // exactly one line ends in " 1"
            let ones: Vec<&str> = buf.lines().filter(|l| l.ends_with(" 1")).collect();
            assert_eq!(ones.len(), 1, "case {i}: {actual:?} -> {buf}");
            assert!(
                ones[0].contains(&format!("state=\"{want_one}\"")),
                "case {i}: expected {want_one} to be 1, got {}",
                ones[0]
            );
        }
    }

    // design 39 §39.7: series counting + the greedy cap.
    #[test]
    fn cardinality_cap_truth_table() {
        // (per-endpoint counts, max, expected included, expected capped)
        let cases: [(&[usize], usize, usize, u64); 5] = [
            // positive: everything fits
            (&[10, 10, 10], 100, 3, 0),
            // boundary: exactly at cap
            (&[10, 10], 20, 2, 0),
            // negative: nothing fits the first endpoint
            (&[50], 10, 0, 50),
            // corner: prefix fits, tail is capped and counted
            (&[10, 10, 10], 25, 2, 10),
            // boundary: zero budget
            (&[1], 0, 0, 1),
        ];
        for (i, (counts, max, want_incl, want_capped)) in cases.into_iter().enumerate() {
            let (incl, capped) = select_within_cap(counts, max);
            assert_eq!(
                (incl, capped),
                (want_incl, want_capped),
                "case {i}: {counts:?} max={max}"
            );
        }
    }

    // design 39 §39.7: a full endpoint renders the expected families, HELP/TYPE
    // once each, and (negative) a stats=None endpoint emits gauges but no
    // counter series.
    #[test]
    fn render_endpoint_exposition_truth_table() {
        let cfg = Config::default();
        let self_stats = SelfStats {
            up: true,
            ..Default::default()
        };

        // positive: full endpoint
        let mut buf = String::new();
        render_into(
            &mut buf,
            std::slice::from_ref(&ep_fixture()),
            &cfg,
            &self_stats,
        );
        // HELP/TYPE exactly once per family
        assert_eq!(
            buf.matches("# TYPE urp_endpoint_reorder_drops_total")
                .count(),
            1,
            "type once\n{buf}"
        );
        // the reorder_drops counter carries endpoint+device labels and value 0
        assert!(
            buf.contains(
                "urp_endpoint_reorder_drops_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 0"
            ),
            "drops line missing:\n{buf}"
        );
        // stateset: active is 1, draining is 0, unknown is 0
        assert!(buf.contains("urp_endpoint_state{endpoint=\"pair_acceptor\",device=\"mlx5_0\",state=\"active\"} 1"), "{buf}");
        assert!(buf.contains("state=\"draining\"} 0"), "{buf}");
        // per-QP present (default per_qp=on), qp label carried
        assert!(
            buf.contains(
                "urp_qp_rtt_ns{endpoint=\"pair_acceptor\",device=\"mlx5_0\",qp=\"0\"} 1500"
            ),
            "{buf}"
        );
        // info omits connect_path (None) but includes listen_path (Some)
        assert!(buf.contains("listen_path=\"/run/urp-echo.sock\""), "{buf}");
        assert!(
            !buf.contains("connect_path="),
            "connect_path should be omitted:\n{buf}"
        );

        // negative: stats=None -> gauges (num_qps, state, info) but NO counter series
        let mut dumped = ep_fixture();
        dumped.stats = None;
        dumped.qps.clear();
        let mut buf2 = String::new();
        render_into(&mut buf2, std::slice::from_ref(&dumped), &cfg, &self_stats);
        assert!(
            !buf2.contains("urp_endpoint_tx_bytes_total{endpoint"),
            "no counter samples w/o stats:\n{buf2}"
        );
        assert!(
            buf2.contains("urp_endpoint_num_qps{endpoint=\"pair_acceptor\""),
            "num_qps still emitted:\n{buf2}"
        );
    }

    // design 39 §39.7: field -> metric value mapping is stable (golden values).
    #[test]
    fn netlink_reply_to_metrics_truth_table() {
        let cfg = Config {
            per_qp: false,
            ..Config::default()
        };
        let mut ep = ep_fixture();
        if let Some(s) = ep.stats.as_mut() {
            s.reorder_drops = 42;
            s.credit_stalls = 7;
            s.auth_failures = 3;
        }
        let mut buf = String::new();
        render_into(
            &mut buf,
            std::slice::from_ref(&ep),
            &cfg,
            &SelfStats::default(),
        );
        // (substring that must appear -> proves the field->metric mapping)
        let cases = [
            "urp_endpoint_reorder_drops_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 42",
            "urp_endpoint_credit_stalls_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 7",
            "urp_endpoint_auth_failures_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 3",
            "urp_endpoint_rx_bytes_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 2000",
        ];
        for (i, want) in cases.into_iter().enumerate() {
            assert!(buf.contains(want), "case {i}: missing {want}\n{buf}");
        }
    }

    // design 39 §39.7: a counter that went backwards (endpoint recreated) is
    // rendered as-is; the renderer must not panic or clamp.
    #[test]
    fn counter_reset_tolerated() {
        let cfg = Config {
            per_qp: false,
            ..Config::default()
        };
        let mut ep = ep_fixture();
        // first scrape: large value
        ep.stats.as_mut().unwrap().tx_bytes = 1_000_000;
        let mut a = String::new();
        render_into(
            &mut a,
            std::slice::from_ref(&ep),
            &cfg,
            &SelfStats::default(),
        );
        assert!(a.contains(
            "urp_endpoint_tx_bytes_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 1000000"
        ));
        // second scrape: went backwards -- rendered verbatim, no panic/clamp
        ep.stats.as_mut().unwrap().tx_bytes = 5;
        let mut b = String::new();
        render_into(
            &mut b,
            std::slice::from_ref(&ep),
            &cfg,
            &SelfStats::default(),
        );
        assert!(
            b.contains(
                "urp_endpoint_tx_bytes_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 5"
            ),
            "{b}"
        );
    }

    // design 40 §40.1: the inter-arrival histogram renders as a valid
    // Prometheus histogram -- cumulative _bucket, bucket[+Inf] == _count, HELP/
    // TYPE once, stride label present, and the family vanishes when no endpoint
    // carries the nest (old module) or the flag is off.
    #[test]
    fn interarrival_histogram_truth_table() {
        use urp_netlink::format::Histogram;
        let cfg = Config {
            per_qp: false,
            ..Config::default()
        };
        // A histogram with known per-bucket counts summing to 45 (0+1+..+9 over
        // the first 10 buckets, then zeros), so cumulative[+Inf] must equal 45.
        let mut ep = ep_fixture();
        let buckets: Vec<u64> = (0..URP_HIST_NBUCKETS as u64)
            .map(|b| if b < 10 { b } else { 0 })
            .collect();
        let total: u64 = buckets.iter().sum();
        ep.interarrival = vec![Histogram {
            stride: 10,
            buckets,
            sum_ns: 12_000,
            count: total,
        }];
        let mut buf = String::new();
        render_into(
            &mut buf,
            std::slice::from_ref(&ep),
            &cfg,
            &SelfStats::default(),
        );

        // HELP/TYPE histogram exactly once.
        assert_eq!(
            buf.matches("# TYPE urp_endpoint_interarrival_seconds histogram")
                .count(),
            1,
            "type once\n{buf}"
        );
        // stride label carried, first finite le is the 250ns == 2.5e-7 s edge.
        assert!(
            buf.contains("urp_endpoint_interarrival_seconds_bucket{endpoint=\"pair_acceptor\",device=\"mlx5_0\",stride=\"10\",le=\"2.5e-7\"}"),
            "first bucket le label missing:\n{buf}"
        );
        // cumulative +Inf bucket == count == 45.
        assert!(
            buf.contains(&format!("stride=\"10\",le=\"+Inf\"}} {total}")),
            "+Inf cumulative should equal count {total}:\n{buf}"
        );
        assert!(
            buf.contains(&format!(
                "urp_endpoint_interarrival_seconds_count{{endpoint=\"pair_acceptor\",device=\"mlx5_0\",stride=\"10\"}} {total}"
            )),
            "_count missing:\n{buf}"
        );
        // _bucket lines are monotonic non-decreasing across le for this stride.
        let mut last = 0u64;
        for line in buf.lines().filter(|l| {
            l.starts_with("urp_endpoint_interarrival_seconds_bucket") && l.contains("stride=\"10\"")
        }) {
            let v: u64 = line.rsplit(' ').next().unwrap().parse().unwrap();
            assert!(v >= last, "cumulative went backwards: {line}");
            last = v;
        }
        assert_eq!(last, total, "final cumulative == count");

        // negative: no nest (old module) -> family absent even with flag on.
        let mut bare = ep_fixture();
        bare.interarrival.clear();
        let mut b2 = String::new();
        render_into(
            &mut b2,
            std::slice::from_ref(&bare),
            &cfg,
            &SelfStats::default(),
        );
        assert!(
            !b2.contains("urp_endpoint_interarrival_seconds"),
            "family must vanish without the nest:\n{b2}"
        );

        // negative: flag off -> family absent even with data present.
        let cfg_off = Config {
            interarrival: false,
            ..cfg.clone()
        };
        let mut b3 = String::new();
        render_into(
            &mut b3,
            std::slice::from_ref(&ep),
            &cfg_off,
            &SelfStats::default(),
        );
        assert!(
            !b3.contains("urp_endpoint_interarrival_seconds"),
            "family must vanish when --no-interarrival:\n{b3}"
        );
    }

    // design 40 §40.2: the OWD histogram renders as a valid Prometheus histogram
    // (cumulative _bucket, bucket[+Inf] == _count), the clock-offset gauge +
    // anomalies counter emit once, and the whole family vanishes when no endpoint
    // carries the nest (unsampled / old module) or --no-owd is set.
    #[test]
    fn owd_histogram_truth_table() {
        use urp_netlink::format::{Histogram, Owd};
        let cfg = Config {
            per_qp: false,
            ..Config::default()
        };
        // counts 0..9 over the first 10 buckets, then zeros -> cumulative[+Inf]==45.
        let mut ep = ep_fixture();
        let buckets: Vec<u64> = (0..URP_OWD_NBUCKETS as u64)
            .map(|b| if b < 10 { b } else { 0 })
            .collect();
        let total: u64 = buckets.iter().sum();
        ep.owd = Some(Owd {
            hist: Histogram {
                stride: 0,
                buckets,
                sum_ns: 9_000,
                count: total,
            },
            clock_offset_ns: 36,
            anomalies: 3,
        });
        let mut buf = String::new();
        render_into(
            &mut buf,
            std::slice::from_ref(&ep),
            &cfg,
            &SelfStats::default(),
        );

        // HELP/TYPE histogram exactly once; no stride label on OWD (single hist).
        assert_eq!(
            buf.matches("# TYPE urp_endpoint_owd_seconds histogram")
                .count(),
            1,
            "type once\n{buf}"
        );
        // first finite le is the 1000ns == 1e-6 s edge.
        assert!(
            buf.contains("urp_endpoint_owd_seconds_bucket{endpoint=\"pair_acceptor\",device=\"mlx5_0\",le=\"1e-6\"}"),
            "first owd bucket le label missing:\n{buf}"
        );
        // cumulative +Inf bucket == count == 45.
        assert!(
            buf.contains(&format!(
                "urp_endpoint_owd_seconds_bucket{{endpoint=\"pair_acceptor\",device=\"mlx5_0\",le=\"+Inf\"}} {total}"
            )),
            "+Inf cumulative should equal count {total}:\n{buf}"
        );
        assert!(
            buf.contains(&format!(
                "urp_endpoint_owd_seconds_count{{endpoint=\"pair_acceptor\",device=\"mlx5_0\"}} {total}"
            )),
            "_count missing:\n{buf}"
        );
        // clock offset gauge (36ns -> 3.6e-8 s) + anomalies counter, once each.
        assert_eq!(
            buf.matches("# TYPE urp_endpoint_owd_clock_offset_seconds gauge")
                .count(),
            1,
            "offset gauge type once\n{buf}"
        );
        assert!(
            buf.contains(
                "urp_endpoint_owd_clock_offset_seconds{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 0.000000036"
            ) || buf.contains(
                "urp_endpoint_owd_clock_offset_seconds{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 3.6e-8"
            ),
            "clock offset value missing:\n{buf}"
        );
        assert!(
            buf.contains(
                "urp_endpoint_owd_anomalies_total{endpoint=\"pair_acceptor\",device=\"mlx5_0\"} 3"
            ),
            "anomalies counter missing:\n{buf}"
        );
        // _bucket lines monotonic non-decreasing across le.
        let mut last = 0u64;
        for line in buf
            .lines()
            .filter(|l| l.starts_with("urp_endpoint_owd_seconds_bucket"))
        {
            let v: u64 = line.rsplit(' ').next().unwrap().parse().unwrap();
            assert!(v >= last, "cumulative went backwards: {line}");
            last = v;
        }
        assert_eq!(last, total, "final cumulative == count");

        // negative: no nest (unsampled / old module) -> family absent, flag on.
        let bare = ep_fixture(); // owd: None
        let mut b2 = String::new();
        render_into(
            &mut b2,
            std::slice::from_ref(&bare),
            &cfg,
            &SelfStats::default(),
        );
        assert!(
            !b2.contains("urp_endpoint_owd_seconds"),
            "family must vanish without the nest:\n{b2}"
        );

        // negative: --no-owd -> family absent even with data present.
        let cfg_off = Config {
            owd: false,
            ..cfg.clone()
        };
        let mut b3 = String::new();
        render_into(
            &mut b3,
            std::slice::from_ref(&ep),
            &cfg_off,
            &SelfStats::default(),
        );
        assert!(
            !b3.contains("urp_endpoint_owd_seconds"),
            "family must vanish when --no-owd:\n{b3}"
        );
    }

    // design 39 §39.8a: a WARM render (buffer + scratch already at capacity)
    // must perform ZERO heap allocations -- the CPU-cost budget depends on the
    // hot path not churning the allocator. Measured via the crate's test-only
    // counting global allocator, bracketing exactly one render_into_scratch.
    #[test]
    fn render_is_zero_alloc_when_warm() {
        // per_stream on so every metric family is exercised in the hot path.
        let cfg = Config {
            per_stream: true,
            ..Config::default()
        };
        let fleet = crate::mock::synthetic_fleet(8, 8, 4);
        let mut buf = String::with_capacity(64 * 1024);
        let mut scratch: Vec<usize> = Vec::with_capacity(64);
        let stats = SelfStats::default();

        // Warm up: grow buf + scratch to steady-state capacity.
        for _ in 0..4 {
            render_into_scratch(&mut buf, &mut scratch, &fleet, &cfg, &stats);
        }
        let len_warm = buf.len();

        // Measured region: exactly one render, bracketed by the alloc counter.
        let before = crate::alloc_count();
        let _ = render_into_scratch(&mut buf, &mut scratch, &fleet, &cfg, &stats);
        let after = crate::alloc_count();

        assert_eq!(
            after - before,
            0,
            "warm render_into_scratch allocated {} time(s); hot path must be zero-alloc",
            after - before
        );
        // Sanity: deterministic output, so the warm render reproduced the size.
        assert_eq!(buf.len(), len_warm, "render is non-deterministic in size");
    }

    // ns/render should scale ~linearly with fleet size; assert the cheap,
    // non-flaky proxy (output grows monotonically with N) and print timings for
    // the human reader (`cargo test -- --nocapture`).
    #[test]
    fn render_scales_with_fleet() {
        let cfg = Config {
            per_stream: true,
            ..Config::default()
        };
        let stats = SelfStats::default();
        let mut buf = String::with_capacity(256 * 1024);
        let mut scratch: Vec<usize> = Vec::with_capacity(256);
        let mut last_len = 0usize;
        for &n in &[1usize, 4, 16, 64] {
            let fleet = crate::mock::synthetic_fleet(n, 8, 4);
            // warm
            render_into_scratch(&mut buf, &mut scratch, &fleet, &cfg, &stats);
            let start = std::time::Instant::now();
            let iters = 200;
            for _ in 0..iters {
                render_into_scratch(&mut buf, &mut scratch, &fleet, &cfg, &stats);
            }
            let ns = start.elapsed().as_nanos() / iters;
            eprintln!("render N={n:>3} -> {} bytes, {ns} ns/render", buf.len());
            assert!(
                buf.len() > last_len,
                "output did not grow from N<{n} ({} <= {last_len})",
                buf.len()
            );
            last_len = buf.len();
        }
    }
}
