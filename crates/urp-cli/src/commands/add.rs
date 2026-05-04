use std::net::SocketAddr;

use clap::Args;

use crate::attr::AttrBuf;
use crate::error::UrpError;
use crate::format::encode_sockaddr_in6;
use crate::netlink::UrpSocket;
use crate::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr, URP_NUM_QPS_MAX, URP_NUM_QPS_MIN};

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
    #[arg(long, value_parser = clap::value_parser!(u32).range((URP_NUM_QPS_MIN as i64)..((URP_NUM_QPS_MAX + 1) as i64)))]
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
