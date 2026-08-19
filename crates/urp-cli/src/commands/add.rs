use std::net::SocketAddr;

use clap::Args;

use urp_netlink::attr::AttrBuf;
use crate::commands::num_qps_parser;
use urp_netlink::error::UrpError;
use urp_netlink::format::encode_sockaddr_in6;
use urp_netlink::netlink::UrpSocket;
use urp_netlink::uapi::{
    UrpAttr, UrpCmd, UrpEndpointAttr, URP_EP_KIND_FAST, URP_EP_KIND_UDS, URP_EP_MODE_K0,
    URP_EP_MODE_MULTISTREAM,
};

/// Endpoint operating mode (mirrors kernel `enum urp_ep_mode`).
#[derive(Copy, Clone, Debug, Eq, PartialEq, Default, clap::ValueEnum)]
pub enum EpMode {
    /// Per-stream backend connect (default; correct for real multi-stream traffic).
    #[default]
    Multistream,
    /// Legacy single-connection mode (stream_id 0); acceptor eager-connects the backend.
    K0,
}

impl EpMode {
    fn as_u8(self) -> u8 {
        match self {
            EpMode::Multistream => URP_EP_MODE_MULTISTREAM,
            EpMode::K0 => URP_EP_MODE_K0,
        }
    }
}

/// Endpoint data path (mirrors kernel `enum urp_ep_kind`).
#[derive(Copy, Clone, Debug, Eq, PartialEq, Default, clap::ValueEnum)]
pub enum EpKind {
    /// Copy path: the kernel runs an AF_UNIX pump (default; unmodified apps).
    #[default]
    Uds,
    /// Zero-copy path: the app drives the QP over /dev/urp (design 31; the module
    /// must be built with CONFIG_URP_FAST, else the kernel rejects with EOPNOTSUPP).
    Fast,
}

impl EpKind {
    fn as_u8(self) -> u8 {
        match self {
            EpKind::Uds => URP_EP_KIND_UDS,
            EpKind::Fast => URP_EP_KIND_FAST,
        }
    }
}

#[derive(Args, Debug)]
pub struct AddArgs {
    /// Endpoint name (max 15 bytes).
    pub name: String,

    /// UDS path the endpoint listens on (initiator side).
    #[arg(long, conflicts_with = "connect_path")]
    pub listen_path: Option<String>,

    /// UDS path the endpoint connects to (acceptor side).
    #[arg(long)]
    pub connect_path: Option<String>,

    /// Peer address (initiator target), `ip:port`.
    #[arg(long)]
    pub peer: Option<SocketAddr>,

    /// Bind address (acceptor side), `ip:port`.
    #[arg(long)]
    pub bind: Option<SocketAddr>,

    /// Number of QPs (1..32).
    #[arg(long, value_parser = num_qps_parser())]
    pub num_qps: Option<u32>,

    /// Buffer count (>=16).
    #[arg(long)]
    pub buffer_count: Option<u32>,

    /// Buffer size (20..65536).
    #[arg(long)]
    pub buffer_size: Option<u32>,

    /// PSK (write-only, never returned).
    #[arg(long)]
    pub password: Option<String>,

    /// Specific RDMA device (e.g. mlx5_0). Optional, kernel auto-picks.
    #[arg(long)]
    pub rdma_device: Option<String>,

    /// Endpoint mode: `multistream` (default) or `k0` (legacy single connection).
    #[arg(long, value_enum, default_value_t = EpMode::Multistream)]
    pub mode: EpMode,

    /// Endpoint kind: `uds` (default, copy path) or `fast` (zero-copy, design 31).
    #[arg(long, value_enum, default_value_t = EpKind::Uds)]
    pub kind: EpKind,
}

pub fn build_payload(args: &AddArgs) -> Vec<u8> {
    let mut top = AttrBuf::new();
    top.nest(UrpAttr::Endpoint as u16, |ep| {
        ep.put_string(UrpEndpointAttr::Name as u16, &args.name);
        if let Some(p) = &args.listen_path {
            ep.put_string(UrpEndpointAttr::ListenPath as u16, p);
        }
        if let Some(p) = &args.connect_path {
            ep.put_string(UrpEndpointAttr::ConnectPath as u16, p);
        }
        if let Some(d) = &args.rdma_device {
            ep.put_string(UrpEndpointAttr::RdmaDevice as u16, d);
        }
        if let Some(sa) = args.peer {
            ep.put_bytes(UrpEndpointAttr::PeerAddr as u16, &encode_sockaddr_in6(sa));
        }
        if let Some(sa) = args.bind {
            ep.put_bytes(UrpEndpointAttr::BindAddr as u16, &encode_sockaddr_in6(sa));
        }
        if let Some(v) = args.num_qps {
            ep.put_u32(UrpEndpointAttr::NumQps as u16, v);
        }
        if let Some(v) = args.buffer_count {
            ep.put_u32(UrpEndpointAttr::BufferCount as u16, v);
        }
        if let Some(v) = args.buffer_size {
            ep.put_u32(UrpEndpointAttr::BufferSize as u16, v);
        }
        if let Some(p) = &args.password {
            ep.put_string(UrpEndpointAttr::Password as u16, p);
        }
        /* Only send the mode attr when non-default (k0), so a newer CLI still
         * works against an older module for the common multistream case (the
         * kernel defaults an absent attr to multistream). */
        if args.mode != EpMode::Multistream {
            ep.put_u8(UrpEndpointAttr::Mode as u16, args.mode.as_u8());
        }
        /* Same forward-compat rule for kind: only send the attr for the
         * non-default (fast) path so a newer CLI keeps working against an older
         * module for the common uds case (absent attr defaults to uds). */
        if args.kind != EpKind::Uds {
            ep.put_u8(UrpEndpointAttr::Kind as u16, args.kind.as_u8());
        }
    });
    top.into_bytes()
}

pub fn run(args: AddArgs) -> Result<(), UrpError> {
    let payload = build_payload(&args);
    let mut sock = UrpSocket::connect()?;
    sock.send_request(UrpCmd::NewEndpoint as u8, &payload)?;
    println!("ok: endpoint {} created", args.name);
    Ok(())
}
