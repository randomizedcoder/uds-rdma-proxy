//! `urp-netlink` -- the generic-netlink client for the urp (UDS-RDMA Proxy)
//! kernel module, shared by the `urp` CLI and the `urp-control` daemon.
//!
//! This crate is the kernel-facing layer: the raw `AF_NETLINK`/`NETLINK_GENERIC`
//! socket client ([`netlink`]), the attribute TLV codec ([`attr`]), the UAPI
//! constant mirror ([`uapi`], kept in lock-step with
//! `kernel/include/uapi/linux/urp.h`), the reply decoder ([`format`]), and the
//! error type ([`error`]). It was factored out of `urp-cli` so a second consumer
//! (the control plane) can query endpoint state without shelling out.

pub mod attr;
pub mod error;
pub mod format;
pub mod netlink;
pub mod uapi;

pub use error::UrpError;
pub use format::Endpoint;
pub use netlink::UrpSocket;

use attr::AttrBuf;
use uapi::{UrpAttr, UrpCmd, UrpEndpointAttr, UrpEndpointState};

/// Fetch one endpoint by name (verbose GET doit) or dump all (scalar dumpit).
/// Shared by the CLI's `show`/`stats` and the control plane.
pub fn fetch_endpoints(
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

/// Fetch a single endpoint by name, or `None` if it does not exist. Thin wrapper
/// over [`fetch_endpoints`] for the common "does this one endpoint exist and what
/// is its state?" query the control plane's acceptor asks to answer `ready`.
pub fn get_endpoint(
    sock: &mut UrpSocket,
    name: &str,
) -> Result<Option<Endpoint>, UrpError> {
    Ok(fetch_endpoints(sock, Some(name))?.into_iter().next())
}

/// Is an endpoint ready to accept an RC connection -- i.e. active and holding at
/// least one QP? The control plane's acceptor answers `Rendezvous.ready` with
/// this (design 33 Phase 3). A `Draining`/unknown state or zero QPs is not ready.
pub fn is_endpoint_ready(state: Option<UrpEndpointState>, num_qps: u32) -> bool {
    matches!(state, Some(UrpEndpointState::Active)) && num_qps >= 1
}

#[cfg(test)]
mod ready_tests {
    use super::*;
    use uapi::UrpEndpointState;

    // design 33 Phase 3: acceptor readiness truth table
    // (state x num_qps) -> ready.
    #[test]
    fn is_endpoint_ready_truth_table() {
        let cases = [
            // positive: active with >=1 QP
            (Some(UrpEndpointState::Active), 1u32, true),
            (Some(UrpEndpointState::Active), 32, true),
            // boundary: active with the minimum vs zero QPs
            (Some(UrpEndpointState::Active), 0, false),
            // negative: wrong state
            (Some(UrpEndpointState::Draining), 1, false),
            (Some(UrpEndpointState::Draining), 0, false),
            // corner: unknown/absent state never ready
            (None, 1, false),
            (None, 0, false),
        ];
        for (i, (state, qps, expect)) in cases.into_iter().enumerate() {
            assert_eq!(
                is_endpoint_ready(state, qps),
                expect,
                "case {i}: state={state:?} num_qps={qps}"
            );
        }
    }
}
