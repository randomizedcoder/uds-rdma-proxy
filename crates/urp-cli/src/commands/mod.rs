pub mod add;
pub mod drain;
pub mod monitor;
pub mod remove;
pub mod set;
pub mod show;
pub mod stats;

use crate::attr::AttrBuf;
use crate::error::UrpError;
use crate::format::Endpoint;
use crate::netlink::UrpSocket;
use crate::uapi::{UrpAttr, UrpCmd, UrpEndpointAttr, URP_NUM_QPS_MAX, URP_NUM_QPS_MIN};

/// Fetch one endpoint by name (verbose GET doit) or dump all (scalar
/// dumpit). Shared by `show` and `stats`.
pub(crate) fn fetch_endpoints(
    sock: &mut UrpSocket,
    name: Option<&str>,
) -> Result<Vec<Endpoint>, UrpError> {
    Ok(if let Some(name) = name {
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
    })
}

/// clap value parser enforcing the kernel's num_qps policy range.
/// Shared by `add --num-qps` and `set --num-qps`.
pub(crate) fn num_qps_parser() -> clap::builder::RangedU64ValueParser<u32> {
    clap::builder::RangedU64ValueParser::new()
        .range((URP_NUM_QPS_MIN as u64)..=(URP_NUM_QPS_MAX as u64))
}
