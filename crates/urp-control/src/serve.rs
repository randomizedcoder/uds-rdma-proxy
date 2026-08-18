//! The acceptor side: a tonic gRPC server exposing `Rendezvous` (unary) and
//! `Heartbeat` (bidi streaming). All decisions delegate to [`crate::logic`];
//! this file is the async shell + overload accounting.

use std::pin::Pin;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::time::Duration;

use tokio_stream::Stream;
use tonic::{Request, Response, Status, Streaming};

use crate::logic::{
    authenticate, build_pong, build_rendezvous_reply, should_shed, AuthOutcome, MIN_BACKOFF_MS,
};
use crate::pb::urp_control_server::UrpControl;
use crate::pb::{HeartbeatPing, HeartbeatPong, RendezvousReply, RendezvousRequest};
use crate::state::EndpointStateSource;

/// Max time to wait for a peer's first ping before authenticating it. Bounds a
/// slowloris open (stream opened, nothing ever sent) so it cannot pin a session
/// slot while unauthenticated.
const FIRST_PING_TIMEOUT: Duration = Duration::from_secs(5);

/// Shared server state. `session_cap == 0` disables the cap (unbounded).
pub struct ControlService<S: EndpointStateSource> {
    source: Arc<S>,
    expected_token: Arc<[u8; 32]>,
    active: Arc<AtomicU32>,
    session_cap: u32,
}

impl<S: EndpointStateSource> ControlService<S> {
    pub fn new(source: Arc<S>, expected_token: [u8; 32], session_cap: u32) -> Self {
        ControlService {
            source,
            expected_token: Arc::new(expected_token),
            active: Arc::new(AtomicU32::new(0)),
            session_cap,
        }
    }

    /// Live count of accepted Heartbeat streams (test/observability hook).
    pub fn active_sessions(&self) -> u32 {
        self.active.load(Ordering::SeqCst)
    }
}

/// Decrements the active-session counter when a stream ends (normal or error).
struct SessionGuard(Arc<AtomicU32>);
impl Drop for SessionGuard {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}

type PongStream = Pin<Box<dyn Stream<Item = Result<HeartbeatPong, Status>> + Send>>;

#[tonic::async_trait]
impl<S: EndpointStateSource + 'static> UrpControl for ControlService<S> {
    async fn rendezvous(
        &self,
        request: Request<RendezvousRequest>,
    ) -> Result<Response<RendezvousReply>, Status> {
        let req = request.into_inner();
        if let AuthOutcome::Unauthenticated = authenticate(&req.auth_token, &self.expected_token) {
            return Err(Status::unauthenticated("bad psk"));
        }
        let snap = self.source.snapshot(&req.endpoint_name).await;
        Ok(Response::new(build_rendezvous_reply(&snap)))
    }

    type HeartbeatStream = PongStream;

    async fn heartbeat(
        &self,
        request: Request<Streaming<HeartbeatPing>>,
    ) -> Result<Response<Self::HeartbeatStream>, Status> {
        let mut inbound = request.into_inner();

        // Authenticate the FIRST ping BEFORE committing a session slot. The old
        // order (reserve slot, then auth per-ping) let an unauthenticated peer
        // exhaust the cap: opening `session_cap` streams -- or one that sends
        // nothing at all -- pinned every slot with no PSK. The bounded wait also
        // evicts a slowloris open that never sends a ping.
        let first = match tokio::time::timeout(FIRST_PING_TIMEOUT, inbound.message()).await {
            Ok(Ok(Some(ping))) => ping,
            Ok(Ok(None)) => return Err(Status::unauthenticated("stream closed before any ping")),
            Ok(Err(status)) => return Err(status),
            Err(_) => return Err(Status::deadline_exceeded("no ping within first-ping timeout")),
        };
        if let AuthOutcome::Unauthenticated = authenticate(&first.auth_token, &self.expected_token) {
            return Err(Status::unauthenticated("bad psk"));
        }

        // Authenticated -- only now admit against the session cap ("I'm too busy
        // -- go away" => RESOURCE_EXHAUSTED). Shedding only authenticated peers
        // means a flood of anonymous opens can no longer deny legitimate ones.
        let current = self.active.load(Ordering::SeqCst);
        if should_shed(current, self.session_cap) {
            return Err(Status::resource_exhausted("server at session capacity"));
        }
        self.active.fetch_add(1, Ordering::SeqCst);
        let guard = SessionGuard(self.active.clone());

        let source = self.source.clone();
        let expected = self.expected_token.clone();

        let out = async_stream::try_stream! {
            let _guard = guard; // dropped when the stream ends
            // Answer the first ping (already authenticated above).
            let snap = source.snapshot(&first.endpoint_name).await;
            yield build_pong(first.seq, &snap, false, MIN_BACKOFF_MS);
            // Subsequent pings re-auth per message (stateless server auth).
            while let Some(ping) = inbound.message().await? {
                if let AuthOutcome::Unauthenticated = authenticate(&ping.auth_token, &expected) {
                    Err(Status::unauthenticated("bad psk"))?;
                }
                let snap = source.snapshot(&ping.endpoint_name).await;
                // Accepted sessions aren't mid-stream shed here; the cap is on
                // admission. A draining endpoint still reports DRAINING via
                // build_pong so the client re-rendezvous.
                yield build_pong(ping.seq, &snap, false, MIN_BACKOFF_MS);
            }
        };

        Ok(Response::new(Box::pin(out)))
    }
}
