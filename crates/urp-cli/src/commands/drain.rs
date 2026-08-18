use clap::Args;

use urp_netlink::attr::AttrBuf;
use urp_netlink::error::UrpError;
use urp_netlink::netlink::UrpSocket;
use urp_netlink::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr, UrpEndpointState};

#[derive(Args, Debug)]
pub struct DrainArgs {
    /// Endpoint name to drain.
    pub name: String,
}

pub fn run(args: DrainArgs) -> Result<(), UrpError> {
    let mut top = AttrBuf::new();
    top.nest(UrpAttr::Endpoint as u16, |ep| {
        ep.put_string(UrpEndpointAttr::Name as u16, &args.name);
        ep.put_u8(
            UrpEndpointAttr::State as u16,
            UrpEndpointState::Draining as u8,
        );
    });

    let mut sock = UrpSocket::connect()?;
    sock.send_request(UrpCmd::SetEndpoint as u8, &top.into_bytes())?;
    println!("ok: endpoint {} draining", args.name);
    Ok(())
}
