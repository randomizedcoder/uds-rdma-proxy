use clap::Args;

use crate::commands::fetch_endpoints;
use crate::error::UrpError;
use crate::format::Endpoint;
use crate::netlink::UrpSocket;

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
    let endpoints: Vec<Endpoint> = fetch_endpoints(&mut sock, args.name.as_deref())?;

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
