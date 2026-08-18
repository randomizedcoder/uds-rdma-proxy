use clap::Args;

use urp_netlink::fetch_endpoints;
use urp_netlink::error::UrpError;
use urp_netlink::format::Endpoint;
use urp_netlink::netlink::UrpSocket;

#[derive(Args, Debug)]
pub struct ShowArgs {
    /// Specific endpoint to show. Omit to dump all.
    pub name: Option<String>,

    /// Emit JSON.
    #[arg(long)]
    pub json: bool,

    /// One-line per endpoint (only meaningful without --json).
    #[arg(long, conflicts_with = "json")]
    pub oneline: bool,
}

pub fn run(args: ShowArgs) -> Result<(), UrpError> {
    let mut sock = UrpSocket::connect()?;

    let endpoints: Vec<Endpoint> = fetch_endpoints(&mut sock, args.name.as_deref())?;

    if args.json {
        let arr: Vec<_> = endpoints.iter().map(|e| e.format_json()).collect();
        if args.name.is_some() && arr.len() == 1 {
            println!("{}", serde_json::to_string_pretty(&arr[0]).unwrap());
        } else {
            println!("{}", serde_json::to_string_pretty(&arr).unwrap());
        }
    } else if args.oneline {
        for e in &endpoints {
            println!("{}", e.format_oneline());
        }
    } else {
        for (i, e) in endpoints.iter().enumerate() {
            if i > 0 {
                println!();
            }
            print!("{}", e.format_human());
        }
        if endpoints.is_empty() {
            eprintln!("(no endpoints)");
        }
    }

    Ok(())
}
