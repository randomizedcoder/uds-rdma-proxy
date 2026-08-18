//! `urp-control` -- the design-33 Phase 3 userland gRPC control plane.
//!
//! Asymmetric: the **acceptor** ([`serve`]) serves `Rendezvous`/`Heartbeat`
//! once its urp endpoint is `rdma_listen`-ing; the **initiator** ([`connect`])
//! calls it and, on mutual-ready, opens a readiness gate the app starts behind.
//! The app's own UDS connect then fires the Phase-2 kernel dial. gRPC-OK is a
//! hint; the kernel Phase-1 retry stays the safety net (this plane never
//! duplicates it and never touches `/run/urp.sock`).
//!
//! All real decisions live in [`logic`] (pure, truth-table-tested); [`serve`]
//! and [`connect`] are thin async shells over it.

/// Generated from `proto/urp_control/v1/control.proto` by `build.rs` (tonic).
pub mod pb {
    tonic::include_proto!("urp.control.v1");
}

pub mod connect;
pub mod logic;
pub mod serve;
pub mod state;
