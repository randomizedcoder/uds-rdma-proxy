//! `urp-exporter` -- a lightweight Prometheus exporter for the `urp` kernel
//! module (design 39). The binary (`src/main.rs`) is a thin shell over this
//! library so every piece -- arg parsing, the netlink scrape, the exposition
//! renderer, and the hand-rolled HTTP responder -- is unit-testable in-process
//! without a socket or the module loaded.
//!
//! Design constraint (design 39 §39.3): the mesh is per-node-CPU / copy-path
//! bound, so the exporter must steal negligible CPU. That is enforced here by
//! construction -- single thread, blocking, no async runtime, reused render
//! buffer (zero steady-state alloc on the hot path), and a min-interval scrape
//! cache -- and tested against (`render` micro-bench + the table tests).

pub mod config;
pub mod exporter;
pub mod http;
pub mod render;
pub mod scrape;

pub use config::Config;
pub use exporter::Exporter;
