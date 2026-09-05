//! Endpoint-summary parsing + formatting (human / oneline / JSON).

use std::net::{Ipv6Addr, SocketAddr, SocketAddrV4, SocketAddrV6};

use serde::Serialize;
use serde_json::{json, Value};

use crate::attr::{payload_str, payload_u16, payload_u32, payload_u64, payload_u8, AttrIter};
use crate::uapi::{
    UrpEndpointAttr, UrpEndpointState, UrpHistAttr, UrpQpAttr, UrpQpState, UrpStatsAttr,
    UrpStreamAttr, UrpStreamState, URP_HIST_NBUCKETS,
};

/// le upper-bound edges (nanoseconds) of the histogram buckets, mirroring
/// `urp_hist_edge_ns()` in `kernel/urp_hist.h`. The trailing +Inf bucket has no
/// finite edge, so this has `URP_HIST_NBUCKETS - 1` entries. The exporter turns
/// each into an `le="<seconds>"` label; a parity test pins it to the kernel
/// header. **Keep in lock-step with the kernel edge table.**
pub const URP_HIST_EDGES_NS: [u64; URP_HIST_NBUCKETS - 1] = [
    250, 500, 1_000, 2_000, 5_000, 10_000, 25_000, 50_000, 100_000, 250_000, 500_000, 1_000_000,
    5_000_000, 25_000_000,
];

/// One classic histogram (design 40 §40.1) decoded from a `URP_HIST_A_*` sub-nest.
/// `buckets` holds the `URP_HIST_NBUCKETS` **per-bucket** (non-cumulative) counts;
/// the exporter accumulates them into the Prometheus cumulative `_bucket` contract.
#[derive(Debug, Default, Clone, Serialize)]
pub struct Histogram {
    pub stride: u32,
    pub buckets: Vec<u64>,
    pub sum_ns: u64,
    pub count: u64,
}

#[derive(Debug, Default, Clone, Serialize)]
pub struct Qp {
    pub index: u32,
    pub state: String,
    pub rtt_ns: u64,
    pub tx_bytes: u64,
    pub rx_bytes: u64,
    pub tx_frames: u64,
    pub rx_frames: u64,
}

#[derive(Debug, Default, Clone, Serialize)]
pub struct Stream {
    pub id: u32,
    pub state: String,
    pub tx_bytes: u64,
    pub rx_bytes: u64,
    pub reorder_depth: u32,
    pub credits_local: u16,
    pub credits_remote: u16,
}

#[derive(Debug, Default, Clone, Serialize)]
pub struct Stats {
    pub active_streams: u32,
    pub tx_bytes: u64,
    pub rx_bytes: u64,
    pub tx_frames: u64,
    pub rx_frames: u64,
    pub credit_stalls: u64,
    pub reorder_insertions: u64,
    pub reorder_drops: u64,
    pub buffer_alloc_fails: u64,
    pub auth_failures: u64,
}

#[derive(Debug, Default, Clone, Serialize)]
pub struct Endpoint {
    pub name: String,
    pub listen_path: Option<String>,
    pub connect_path: Option<String>,
    pub rdma_device: Option<String>,
    pub peer_addr: Option<String>,
    pub bind_addr: Option<String>,
    pub num_qps: u32,
    pub buffer_count: u32,
    pub buffer_size: u32,
    pub state: String,
    pub qps: Vec<Qp>,
    pub streams: Vec<Stream>,
    pub stats: Option<Stats>,
    /// design 40 §40.1: RX inter-arrival histograms, one per sampling stride
    /// (1/10/100). Empty when the module predates the feature or the sysctl is
    /// off (the exporter then emits no inter-arrival family).
    pub interarrival: Vec<Histogram>,
}

/// Decode a 28-byte sockaddr_in6 blob to a Rust SocketAddr (collapsing
/// IPv4-mapped addresses back to v4 for display).
pub fn decode_sockaddr_in6(b: &[u8]) -> Option<SocketAddr> {
    if b.len() != 28 {
        return None;
    }
    let family = u16::from_ne_bytes([b[0], b[1]]);
    if family != libc::AF_INET6 as u16 {
        return None;
    }
    let port = u16::from_be_bytes([b[2], b[3]]);
    let mut addr16 = [0u8; 16];
    addr16.copy_from_slice(&b[8..24]);
    let scope = u32::from_be_bytes([b[24], b[25], b[26], b[27]]);
    let v6 = Ipv6Addr::from(addr16);
    if let Some(v4) = v6.to_ipv4_mapped() {
        Some(SocketAddr::V4(SocketAddrV4::new(v4, port)))
    } else {
        Some(SocketAddr::V6(SocketAddrV6::new(v6, port, 0, scope)))
    }
}

/// Encode a SocketAddr into the kernel's 28-byte sockaddr_in6 form
/// (IPv4 → IPv4-mapped IPv6).
pub fn encode_sockaddr_in6(sa: SocketAddr) -> [u8; 28] {
    let mut out = [0u8; 28];
    out[0..2].copy_from_slice(&(libc::AF_INET6 as u16).to_ne_bytes());
    let (port, addr16, scope) = match sa {
        SocketAddr::V4(v4) => {
            let mapped = v4.ip().to_ipv6_mapped();
            (v4.port(), mapped.octets(), 0u32)
        }
        SocketAddr::V6(v6) => (v6.port(), v6.ip().octets(), v6.scope_id()),
    };
    out[2..4].copy_from_slice(&port.to_be_bytes());
    // flowinfo (4 bytes) left zero
    out[8..24].copy_from_slice(&addr16);
    out[24..28].copy_from_slice(&scope.to_be_bytes());
    out
}

impl Endpoint {
    /// Parse the URP_A_ENDPOINT nested payload.
    pub fn parse_nested(buf: &[u8]) -> Option<Self> {
        let mut ep = Endpoint::default();
        for (t, p) in AttrIter::new(buf) {
            match t {
                x if x == UrpEndpointAttr::Name as u16 => {
                    ep.name = payload_str(p).unwrap_or("").to_string();
                }
                x if x == UrpEndpointAttr::ListenPath as u16 => {
                    ep.listen_path = payload_str(p).map(String::from);
                }
                x if x == UrpEndpointAttr::ConnectPath as u16 => {
                    ep.connect_path = payload_str(p).map(String::from);
                }
                x if x == UrpEndpointAttr::RdmaDevice as u16 => {
                    ep.rdma_device = payload_str(p).map(String::from);
                }
                x if x == UrpEndpointAttr::PeerAddr as u16 => {
                    ep.peer_addr = decode_sockaddr_in6(p).map(|s| s.to_string());
                }
                x if x == UrpEndpointAttr::BindAddr as u16 => {
                    ep.bind_addr = decode_sockaddr_in6(p).map(|s| s.to_string());
                }
                x if x == UrpEndpointAttr::NumQps as u16 => {
                    ep.num_qps = payload_u32(p).unwrap_or(0);
                }
                x if x == UrpEndpointAttr::BufferCount as u16 => {
                    ep.buffer_count = payload_u32(p).unwrap_or(0);
                }
                x if x == UrpEndpointAttr::BufferSize as u16 => {
                    ep.buffer_size = payload_u32(p).unwrap_or(0);
                }
                x if x == UrpEndpointAttr::State as u16 => {
                    let v = payload_u8(p).unwrap_or(0);
                    ep.state = UrpEndpointState::from_u8(v)
                        .map(|s| s.as_str().to_string())
                        .unwrap_or_else(|| format!("unknown({v})"));
                }
                x if x == UrpEndpointAttr::Qps as u16 => {
                    ep.qps = parse_qps(p);
                }
                x if x == UrpEndpointAttr::Streams as u16 => {
                    ep.streams = parse_streams(p);
                }
                x if x == UrpEndpointAttr::Stats as u16 => {
                    ep.stats = Some(parse_stats(p));
                }
                x if x == UrpEndpointAttr::Interarrival as u16 => {
                    ep.interarrival = parse_histograms(p);
                }
                _ => {}
            }
        }
        if ep.name.is_empty() {
            return None;
        }
        Some(ep)
    }

    /// Top-level parse: payload begins with URP_A_ENDPOINT nested attr.
    pub fn parse_top(buf: &[u8]) -> Option<Self> {
        for (t, p) in AttrIter::new(buf) {
            if t == crate::uapi::UrpAttr::Endpoint as u16 {
                return Self::parse_nested(p);
            }
        }
        None
    }

    pub fn format_oneline(&self) -> String {
        let path = self
            .listen_path
            .as_deref()
            .or(self.connect_path.as_deref())
            .unwrap_or("-");
        let addr = self
            .peer_addr
            .as_deref()
            .or(self.bind_addr.as_deref())
            .unwrap_or("-");
        format!(
            "{:<16} {:<8} qps={:<2} buf={}x{:<6} {} {}",
            self.name, self.state, self.num_qps, self.buffer_count, self.buffer_size, path, addr
        )
    }

    pub fn format_human(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!("endpoint: {}\n", self.name));
        out.push_str(&format!("  state:        {}\n", self.state));
        if let Some(p) = &self.listen_path {
            out.push_str(&format!("  listen-path:  {p}\n"));
        }
        if let Some(p) = &self.connect_path {
            out.push_str(&format!("  connect-path: {p}\n"));
        }
        if let Some(p) = &self.peer_addr {
            out.push_str(&format!("  peer-addr:    {p}\n"));
        }
        if let Some(p) = &self.bind_addr {
            out.push_str(&format!("  bind-addr:    {p}\n"));
        }
        if let Some(d) = &self.rdma_device {
            out.push_str(&format!("  rdma-device:  {d}\n"));
        }
        out.push_str(&format!("  num-qps:      {}\n", self.num_qps));
        out.push_str(&format!("  buffer-count: {}\n", self.buffer_count));
        out.push_str(&format!("  buffer-size:  {}\n", self.buffer_size));
        if !self.qps.is_empty() {
            out.push_str("  qps:\n");
            for q in &self.qps {
                out.push_str(&format!(
                    "    [{}] state={} rtt_ns={} tx={}/{} rx={}/{}\n",
                    q.index, q.state, q.rtt_ns, q.tx_frames, q.tx_bytes, q.rx_frames, q.rx_bytes
                ));
            }
        }
        if !self.streams.is_empty() {
            out.push_str("  streams:\n");
            for s in &self.streams {
                out.push_str(&format!(
                    "    [{}] state={} tx={} rx={} reorder={} credits=l{}/r{}\n",
                    s.id,
                    s.state,
                    s.tx_bytes,
                    s.rx_bytes,
                    s.reorder_depth,
                    s.credits_local,
                    s.credits_remote
                ));
            }
        }
        if let Some(st) = &self.stats {
            out.push_str("  stats:\n");
            out.push_str(&format!("    active-streams:    {}\n", st.active_streams));
            out.push_str(&format!("    tx-bytes:          {}\n", st.tx_bytes));
            out.push_str(&format!("    rx-bytes:          {}\n", st.rx_bytes));
            out.push_str(&format!("    tx-frames:         {}\n", st.tx_frames));
            out.push_str(&format!("    rx-frames:         {}\n", st.rx_frames));
            out.push_str(&format!("    credit-stalls:     {}\n", st.credit_stalls));
            out.push_str(&format!(
                "    reorder-insertions:{}\n",
                st.reorder_insertions
            ));
            out.push_str(&format!("    reorder-drops:     {}\n", st.reorder_drops));
            out.push_str(&format!(
                "    buffer-alloc-fails:{}\n",
                st.buffer_alloc_fails
            ));
            out.push_str(&format!("    auth-failures:     {}\n", st.auth_failures));
        }
        if !self.interarrival.is_empty() {
            out.push_str("  interarrival (RX):\n");
            for h in &self.interarrival {
                let mean_us = if h.count > 0 {
                    h.sum_ns as f64 / h.count as f64 / 1000.0
                } else {
                    0.0
                };
                out.push_str(&format!(
                    "    stride={:<3} count={} mean={:.1}us\n",
                    h.stride, h.count, mean_us
                ));
            }
        }
        out
    }

    pub fn format_json(&self) -> Value {
        json!({
            "name": self.name,
            "listen_path": self.listen_path,
            "connect_path": self.connect_path,
            "rdma_device": self.rdma_device,
            "peer_addr": self.peer_addr,
            "bind_addr": self.bind_addr,
            "num_qps": self.num_qps,
            "buffer_count": self.buffer_count,
            "buffer_size": self.buffer_size,
            "state": self.state,
            "qps": self.qps,
            "streams": self.streams,
            "stats": self.stats,
            "interarrival": self.interarrival,
        })
    }
}

fn parse_qps(buf: &[u8]) -> Vec<Qp> {
    let mut out = Vec::new();
    for (_idx, p) in AttrIter::new(buf) {
        let mut qp = Qp::default();
        for (t, val) in AttrIter::new(p) {
            match t {
                x if x == UrpQpAttr::Index as u16 => qp.index = payload_u32(val).unwrap_or(0),
                x if x == UrpQpAttr::State as u16 => {
                    let v = payload_u8(val).unwrap_or(0);
                    qp.state = UrpQpState::from_u8(v)
                        .map(|s| s.as_str().to_string())
                        .unwrap_or_else(|| format!("unknown({v})"));
                }
                x if x == UrpQpAttr::RttNs as u16 => qp.rtt_ns = payload_u64(val).unwrap_or(0),
                x if x == UrpQpAttr::TxBytes as u16 => qp.tx_bytes = payload_u64(val).unwrap_or(0),
                x if x == UrpQpAttr::RxBytes as u16 => qp.rx_bytes = payload_u64(val).unwrap_or(0),
                x if x == UrpQpAttr::TxFrames as u16 => {
                    qp.tx_frames = payload_u64(val).unwrap_or(0)
                }
                x if x == UrpQpAttr::RxFrames as u16 => {
                    qp.rx_frames = payload_u64(val).unwrap_or(0)
                }
                _ => {}
            }
        }
        out.push(qp);
    }
    out
}

fn parse_streams(buf: &[u8]) -> Vec<Stream> {
    let mut out = Vec::new();
    for (_idx, p) in AttrIter::new(buf) {
        let mut s = Stream::default();
        for (t, val) in AttrIter::new(p) {
            match t {
                x if x == UrpStreamAttr::Id as u16 => s.id = payload_u32(val).unwrap_or(0),
                x if x == UrpStreamAttr::State as u16 => {
                    let v = payload_u8(val).unwrap_or(0);
                    s.state = UrpStreamState::from_u8(v)
                        .map(|q| q.as_str().to_string())
                        .unwrap_or_else(|| format!("unknown({v})"));
                }
                x if x == UrpStreamAttr::TxBytes as u16 => {
                    s.tx_bytes = payload_u64(val).unwrap_or(0)
                }
                x if x == UrpStreamAttr::RxBytes as u16 => {
                    s.rx_bytes = payload_u64(val).unwrap_or(0)
                }
                x if x == UrpStreamAttr::ReorderDepth as u16 => {
                    s.reorder_depth = payload_u32(val).unwrap_or(0)
                }
                x if x == UrpStreamAttr::CreditsLocal as u16 => {
                    s.credits_local = payload_u16(val).unwrap_or(0)
                }
                x if x == UrpStreamAttr::CreditsRemote as u16 => {
                    s.credits_remote = payload_u16(val).unwrap_or(0)
                }
                _ => {}
            }
        }
        out.push(s);
    }
    out
}

fn parse_stats(buf: &[u8]) -> Stats {
    let mut s = Stats::default();
    for (t, val) in AttrIter::new(buf) {
        match t {
            x if x == UrpStatsAttr::ActiveStreams as u16 => {
                s.active_streams = payload_u32(val).unwrap_or(0)
            }
            x if x == UrpStatsAttr::TxBytes as u16 => s.tx_bytes = payload_u64(val).unwrap_or(0),
            x if x == UrpStatsAttr::RxBytes as u16 => s.rx_bytes = payload_u64(val).unwrap_or(0),
            x if x == UrpStatsAttr::TxFrames as u16 => s.tx_frames = payload_u64(val).unwrap_or(0),
            x if x == UrpStatsAttr::RxFrames as u16 => s.rx_frames = payload_u64(val).unwrap_or(0),
            x if x == UrpStatsAttr::CreditStalls as u16 => {
                s.credit_stalls = payload_u64(val).unwrap_or(0)
            }
            x if x == UrpStatsAttr::ReorderInsertions as u16 => {
                s.reorder_insertions = payload_u64(val).unwrap_or(0)
            }
            x if x == UrpStatsAttr::ReorderDrops as u16 => {
                s.reorder_drops = payload_u64(val).unwrap_or(0)
            }
            x if x == UrpStatsAttr::BufferAllocFails as u16 => {
                s.buffer_alloc_fails = payload_u64(val).unwrap_or(0)
            }
            x if x == UrpStatsAttr::AuthFailures as u16 => {
                s.auth_failures = payload_u64(val).unwrap_or(0)
            }
            _ => {}
        }
    }
    s
}

/// Decode the URP_ENDPOINT_A_INTERARRIVAL nest: an array of per-stride
/// sub-nests, each a `urp_hist_attr` set (design 40 §40.1). A bucket blob whose
/// length is not exactly `URP_HIST_NBUCKETS` u64s is treated as absent (the
/// histogram is dropped) rather than partially decoded.
fn parse_histograms(buf: &[u8]) -> Vec<Histogram> {
    let mut out = Vec::new();
    for (_idx, p) in AttrIter::new(buf) {
        let mut h = Histogram::default();
        let mut buckets_ok = false;
        for (t, val) in AttrIter::new(p) {
            match t {
                x if x == UrpHistAttr::Stride as u16 => h.stride = payload_u32(val).unwrap_or(0),
                x if x == UrpHistAttr::Buckets as u16 => {
                    if val.len() == URP_HIST_NBUCKETS * 8 {
                        h.buckets = (0..URP_HIST_NBUCKETS)
                            .map(|i| {
                                let o = i * 8;
                                u64::from_ne_bytes(val[o..o + 8].try_into().unwrap())
                            })
                            .collect();
                        buckets_ok = true;
                    }
                }
                x if x == UrpHistAttr::SumNs as u16 => h.sum_ns = payload_u64(val).unwrap_or(0),
                x if x == UrpHistAttr::Count as u16 => h.count = payload_u64(val).unwrap_or(0),
                _ => {}
            }
        }
        if buckets_ok {
            out.push(h);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_endpoint() -> Endpoint {
        Endpoint {
            name: "ep0".into(),
            listen_path: Some("/run/urp/ep0.sock".into()),
            connect_path: None,
            rdma_device: Some("mlx5_0".into()),
            peer_addr: Some("10.0.0.1:4791".into()),
            bind_addr: None,
            num_qps: 4,
            buffer_count: 1024,
            buffer_size: 4076,
            state: "active".into(),
            qps: vec![Qp {
                index: 0,
                state: "active".into(),
                rtt_ns: 1234,
                tx_bytes: 1,
                rx_bytes: 2,
                tx_frames: 3,
                rx_frames: 4,
            }],
            streams: vec![],
            stats: Some(Stats {
                active_streams: 1,
                tx_bytes: 10,
                rx_bytes: 20,
                tx_frames: 30,
                rx_frames: 40,
                ..Stats::default()
            }),
            interarrival: vec![],
        }
    }

    #[test]
    fn sockaddr_in6_v4_mapped() {
        let sa: SocketAddr = "10.0.0.1:4791".parse().unwrap();
        let bytes = encode_sockaddr_in6(sa);
        assert_eq!(bytes.len(), 28);
        // family AF_INET6, native byte order
        let fam = u16::from_ne_bytes([bytes[0], bytes[1]]);
        assert_eq!(fam, libc::AF_INET6 as u16);
        // port: BE
        assert_eq!(u16::from_be_bytes([bytes[2], bytes[3]]), 4791);
        // IPv4-mapped: ::ffff:10.0.0.1 → bytes 18/19 == 0xff,0xff and 20..24 == 10.0.0.1
        assert_eq!(&bytes[8..18], &[0u8; 10]);
        assert_eq!(&bytes[18..20], &[0xff, 0xff]);
        assert_eq!(&bytes[20..24], &[10, 0, 0, 1]);
        // round-trip
        let decoded = decode_sockaddr_in6(&bytes).unwrap();
        assert_eq!(decoded, sa);
    }

    #[test]
    fn format_human_smoke() {
        let ep = sample_endpoint();
        let h = ep.format_human();
        assert!(h.contains("endpoint: ep0"));
        assert!(h.contains("state:"));
        assert!(h.contains("num-qps:"));
        assert!(h.contains("mlx5_0"));
    }

    #[test]
    fn format_json_smoke() {
        let ep = sample_endpoint();
        let v = ep.format_json();
        let s = serde_json::to_string(&v).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&s).unwrap();
        for k in [
            "name",
            "listen_path",
            "connect_path",
            "rdma_device",
            "peer_addr",
            "bind_addr",
            "num_qps",
            "buffer_count",
            "buffer_size",
            "state",
            "qps",
            "streams",
            "stats",
            "interarrival",
        ] {
            assert!(parsed.get(k).is_some(), "missing key {k} in json");
        }
    }

    // design 40 §40.1: the URP_HIST_EDGES_NS le table must stay in lock-step
    // with the kernel header (urp_hist_edge_ns in kernel/urp_hist.h). Re-parse
    // the header's edge list and assert equality; skipped when the header is
    // absent (published tarball) so the test still runs there.
    #[test]
    fn hist_edges_match_kernel_header() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../kernel/urp_hist.h");
        let src = match std::fs::read_to_string(path) {
            Ok(s) => s,
            Err(_) => return, // header not shipped; nothing to check
        };
        // Grab the numbers inside the `edges[] = { ... };` initializer.
        let start = src.find("edges[URP_HIST_NEDGES] = {").expect("edges table");
        let end = src[start..].find("};").expect("edges end") + start;
        let body = &src[start..end];
        let nums: Vec<u64> = body
            .split(|c: char| !c.is_ascii_digit())
            .filter(|t| !t.is_empty())
            .filter_map(|t| t.parse::<u64>().ok())
            .collect();
        assert_eq!(
            nums, URP_HIST_EDGES_NS,
            "Rust le table drifted from kernel/urp_hist.h edges"
        );
    }

    // design 40 §40.1: decode a synthetic interarrival nest and confirm the
    // per-stride histograms round-trip (buckets len, sum, count, stride).
    #[test]
    fn parse_interarrival_round_trip() {
        use crate::attr::AttrBuf;
        let mut nest = AttrBuf::new();
        for stride in [1u32, 10, 100] {
            nest.nest(stride as u16, |h| {
                h.put_u32(UrpHistAttr::Stride as u16, stride);
                let mut blob = Vec::new();
                for b in 0..URP_HIST_NBUCKETS as u64 {
                    blob.extend_from_slice(&(b + stride as u64).to_ne_bytes());
                }
                h.put_bytes(UrpHistAttr::Buckets as u16, &blob);
                h.put_u64(UrpHistAttr::SumNs as u16, 123 * stride as u64);
                h.put_u64(UrpHistAttr::Count as u16, 7 * stride as u64);
            });
        }
        let hs = parse_histograms(nest.as_bytes());
        assert_eq!(hs.len(), 3, "three strides");
        for (i, stride) in [1u32, 10, 100].into_iter().enumerate() {
            assert_eq!(hs[i].stride, stride);
            assert_eq!(hs[i].buckets.len(), URP_HIST_NBUCKETS);
            assert_eq!(hs[i].buckets[0], stride as u64); // b==0 -> 0+stride
            assert_eq!(hs[i].sum_ns, 123 * stride as u64);
            assert_eq!(hs[i].count, 7 * stride as u64);
        }
        // negative: a sub-nest with a wrong-length bucket blob is dropped.
        let mut bad = AttrBuf::new();
        bad.nest(1, |h| {
            h.put_u32(UrpHistAttr::Stride as u16, 1);
            h.put_bytes(UrpHistAttr::Buckets as u16, &[0u8; 8]); // too short
            h.put_u64(UrpHistAttr::Count as u16, 1);
        });
        assert!(
            parse_histograms(bad.as_bytes()).is_empty(),
            "short blob dropped"
        );
    }
}
