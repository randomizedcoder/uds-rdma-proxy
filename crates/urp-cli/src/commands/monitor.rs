use clap::Args;

use crate::error::UrpError;
use crate::format::Endpoint;
use crate::netlink::UrpSocket;

#[derive(Args, Debug)]
pub struct MonitorArgs {}

pub fn run(_args: MonitorArgs) -> Result<(), UrpError> {
    let mut sock = UrpSocket::connect()?;
    sock.subscribe_events()?;
    eprintln!("monitoring urp events (Ctrl-C to stop)");
    loop {
        match sock.recv_event() {
            Ok(buf) => {
                if let Some(ep) = Endpoint::parse_top(&buf) {
                    println!("{}: {}", ep.name, ep.state);
                }
            }
            Err(UrpError::Io(e)) if e.kind() == std::io::ErrorKind::Interrupted => {
                return Ok(());
            }
            Err(e) => return Err(e),
        }
    }
}
