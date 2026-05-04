use clap::Args;

use crate::attr::AttrBuf;
use crate::error::UrpError;
use crate::format::Endpoint;
use crate::netlink::UrpSocket;
use crate::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr};

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

    let endpoints: Vec<Endpoint> = if let Some(name) = &args.name {
        let mut top = AttrBuf::new();
        top.nest(UrpAttr::Endpoint as u16, |ep| {
            ep.put_string(UrpEndpointAttr::Name as u16, name);
        });
        let reply = sock.send_request(UrpCmd::GetEndpoint as u8, &top.into_bytes())?;
        Endpoint::parse_top(&reply).into_iter().collect()
    } else {
        let payload = AttrBuf::new().into_bytes();
        let replies = sock.dump(UrpCmd::GetEndpoint as u8, &payload)?;
        replies
            .iter()
            .filter_map(|r| Endpoint::parse_top(r))
            .collect()
    };

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
