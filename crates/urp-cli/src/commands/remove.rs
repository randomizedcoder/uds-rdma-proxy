use clap::Args;

use urp_netlink::attr::AttrBuf;
use urp_netlink::error::UrpError;
use urp_netlink::netlink::UrpSocket;
use urp_netlink::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr};

#[derive(Args, Debug)]
pub struct RemoveArgs {
    /// Endpoint name to remove.
    pub name: String,
}

pub fn run(args: RemoveArgs) -> Result<(), UrpError> {
    let mut top = AttrBuf::new();
    top.nest(UrpAttr::Endpoint as u16, |ep| {
        ep.put_string(UrpEndpointAttr::Name as u16, &args.name);
    });

    let mut sock = UrpSocket::connect()?;
    sock.send_request(UrpCmd::DelEndpoint as u8, &top.into_bytes())?;
    println!("ok: endpoint {} removed", args.name);
    Ok(())
}
