//! Where the acceptor reads its own endpoint state to answer `ready`.
//!
//! Behind a trait so the gRPC handlers can run against a fake in tests and a
//! real generic-netlink query (`urp-netlink`) in production. The live query is
//! blocking (netlink syscalls), so it runs on a blocking thread; a fresh socket
//! per call keeps things Send-simple at the ~60s heartbeat cadence.

use crate::logic::{EndpointSnapshot, UrpEndpointState};

#[tonic::async_trait]
pub trait EndpointStateSource: Send + Sync {
    /// Snapshot the named endpoint. An absent endpoint -> `EndpointSnapshot`
    /// with `present = false` (never ready), not an error.
    async fn snapshot(&self, endpoint_name: &str) -> EndpointSnapshot;
}

/// Live source: opens a netlink socket per call and maps the reply.
pub struct LiveSource {
    /// Identity reported back as `peer_id` (observability only).
    pub local_id: String,
}

#[tonic::async_trait]
impl EndpointStateSource for LiveSource {
    async fn snapshot(&self, endpoint_name: &str) -> EndpointSnapshot {
        let name = endpoint_name.to_string();
        let peer_id = self.local_id.clone();
        tokio::task::spawn_blocking(move || snapshot_blocking(&name, &peer_id))
            .await
            .unwrap_or_else(|_| EndpointSnapshot::absent())
    }
}

fn snapshot_blocking(endpoint_name: &str, peer_id: &str) -> EndpointSnapshot {
    let mut sock = match urp_netlink::UrpSocket::connect() {
        Ok(s) => s,
        Err(_) => return EndpointSnapshot::absent(),
    };
    match urp_netlink::get_endpoint(&mut sock, endpoint_name) {
        Ok(Some(ep)) => EndpointSnapshot {
            present: true,
            state: UrpEndpointState::from_str(&ep.state),
            num_qps: ep.num_qps,
            buffer_size: ep.buffer_size,
            peer_id: peer_id.to_string(),
        },
        _ => EndpointSnapshot {
            peer_id: peer_id.to_string(),
            ..EndpointSnapshot::absent()
        },
    }
}

/// Test/fake source: a snapshot behind a lock the test can mutate to drive
/// ready/not-ready/draining transitions.
#[derive(Clone)]
pub struct FakeSource {
    inner: std::sync::Arc<std::sync::Mutex<EndpointSnapshot>>,
}

impl FakeSource {
    pub fn new(snap: EndpointSnapshot) -> Self {
        FakeSource {
            inner: std::sync::Arc::new(std::sync::Mutex::new(snap)),
        }
    }
    pub fn set(&self, snap: EndpointSnapshot) {
        *self.inner.lock().unwrap() = snap;
    }
}

#[tonic::async_trait]
impl EndpointStateSource for FakeSource {
    async fn snapshot(&self, _endpoint_name: &str) -> EndpointSnapshot {
        self.inner.lock().unwrap().clone()
    }
}
