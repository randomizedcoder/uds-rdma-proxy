use clap::Args;

use crate::attr::AttrBuf;
use crate::error::UrpError;
use crate::format::Endpoint;
use crate::netlink::UrpSocket;
use crate::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr};

#[derive(Args, Debug)]
pub struct StatsArgs {
    /// Show stats for one endpoint. Omit to aggregate over all.
    pub name: Option<String>,

    /// Emit JSON.
    #[arg(long)]
    pub json: bool,
}

pub fn run(args: StatsArgs) -> Result<(), UrpError> {
    let mut sock = UrpSocket::connect()?;
    let endpoints: Vec<Endpoint> = if let Some(name) = &args.name {
        let mut top = AttrBuf::new();
        top.nest(UrpAttr::Endpoint as u16, |ep| {
            ep.put_string(UrpEndpointAttr::Name as u16, name);
        });
        let reply = sock.send_request(UrpCmd::GetEndpoint as u8, &top.into_bytes())?;
        Endpoint::parse_top(&reply).into_iter().collect()
    } else {
        let payload = AttrBuf::new().into_bytes();
        sock.dump(UrpCmd::GetEndpoint as u8, &payload)?
            .iter()
            .filter_map(|r| Endpoint::parse_top(r))
            .collect()
    };

    if args.json {
        let arr: Vec<_> = endpoints
            .iter()
            .map(|e| {
                serde_json::json!({
                    "name": e.name,
                    "stats": e.stats,
                })
            })
            .collect();
        println!("{}", serde_json::to_string_pretty(&arr).unwrap());
        return Ok(());
    }

    for e in &endpoints {
        println!("{}:", e.name);
        if let Some(s) = &e.stats {
            println!("  active-streams:     {}", s.active_streams);
            println!("  tx-bytes:           {}", s.tx_bytes);
            println!("  rx-bytes:           {}", s.rx_bytes);
            println!("  tx-frames:          {}", s.tx_frames);
            println!("  rx-frames:          {}", s.rx_frames);
            println!("  credit-stalls:      {}", s.credit_stalls);
            println!("  reorder-insertions: {}", s.reorder_insertions);
            println!("  reorder-drops:      {}", s.reorder_drops);
            println!("  buffer-alloc-fails: {}", s.buffer_alloc_fails);
            println!("  auth-failures:      {}", s.auth_failures);
        } else {
            println!("  (no stats reported)");
        }
    }
    Ok(())
}
