use clap::Args;

use crate::attr::AttrBuf;
use crate::error::UrpError;
use crate::netlink::UrpSocket;
use crate::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr, URP_NUM_QPS_MAX, URP_NUM_QPS_MIN};

#[derive(Args, Debug)]
pub struct SetArgs {
    pub name: String,

    #[arg(long, value_parser = clap::value_parser!(u32).range((URP_NUM_QPS_MIN as i64)..((URP_NUM_QPS_MAX + 1) as i64)))]
    pub num_qps: Option<u32>,

    #[arg(long)]
    pub buffer_count: Option<u32>,

    #[arg(long)]
    pub password: Option<String>,
}

pub fn run(args: SetArgs) -> Result<(), UrpError> {
    let mut top = AttrBuf::new();
    top.nest(UrpAttr::Endpoint as u16, |ep| {
        ep.put_string(UrpEndpointAttr::Name as u16, &args.name);
        if let Some(v) = args.num_qps {
            ep.put_u32(UrpEndpointAttr::NumQps as u16, v);
        }
        if let Some(v) = args.buffer_count {
            ep.put_u32(UrpEndpointAttr::BufferCount as u16, v);
        }
        if let Some(p) = &args.password {
            ep.put_string(UrpEndpointAttr::Password as u16, p);
        }
    });

    let mut sock = UrpSocket::connect()?;
    sock.send_request(UrpCmd::SetEndpoint as u8, &top.into_bytes())?;
    println!("ok: endpoint {} updated", args.name);
    Ok(())
}
