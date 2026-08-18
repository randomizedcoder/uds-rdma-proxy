//! The initiator side: one persistent Heartbeat session behind an outer
//! reconnect loop. Pure decisions live in [`crate::logic`]; this is the async
//! shell. Both the readiness signal ([`ReadyNotifier`]) and the RDMA-failure
//! signal (an mpsc channel) are injected so the loopback test can drive them
//! without a real `NOTIFY_SOCKET` or a real kernel.

use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rand::SeedableRng;
use tokio::sync::mpsc;
use tokio_stream::wrappers::ReceiverStream;
use tonic::transport::{Channel, Endpoint};

use crate::logic::{
    build_ping, classify_pong, next_heartbeat_delay, next_seq, plan_conn, Backoff, ConnAction,
    ConnEvent, Gate, PongClass,
};
use crate::pb::urp_control_client::UrpControlClient;
use crate::pb::{ProbeReason, RendezvousRequest};

/// Fires once, on the rising edge that first opens the readiness gate. The
/// production impl calls `sd_notify(READY=1)`; the test impl records the edge.
pub trait ReadyNotifier: Send {
    fn notify_ready(&self);
}

#[derive(Clone, Debug)]
pub struct ConnectConfig {
    pub target: String, // e.g. "http://127.0.0.1:50051"
    pub endpoint_name: String,
    pub local_id: String,
    pub token: [u8; 32],
    pub base_ms: u32,
    pub ceil_ms: u32,
    pub heartbeat_ms: u64,
    pub jitter_frac: f64,
    pub do_rendezvous: bool,
    pub connect_timeout_ms: u64,
    pub keepalive_ms: u64,
}

impl Default for ConnectConfig {
    fn default() -> Self {
        ConnectConfig {
            target: "http://127.0.0.1:50051".into(),
            endpoint_name: String::new(),
            local_id: String::new(),
            token: [0u8; 32],
            base_ms: 100,
            ceil_ms: 2_000,
            heartbeat_ms: 60_000,
            jitter_frac: 0.10,
            do_rendezvous: true,
            connect_timeout_ms: 3_000,
            keepalive_ms: 10_000,
        }
    }
}

fn now_unix_ns() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0)
}

fn build_channel(cfg: &ConnectConfig) -> anyhow::Result<Channel> {
    let ep = Endpoint::from_shared(cfg.target.clone())?
        .connect_timeout(Duration::from_millis(cfg.connect_timeout_ms))
        .http2_keep_alive_interval(Duration::from_millis(cfg.keepalive_ms))
        .keep_alive_timeout(Duration::from_millis(cfg.keepalive_ms * 2))
        .tcp_keepalive(Some(Duration::from_millis(cfg.keepalive_ms)));
    // Lazy: the first RPC drives the actual connect, so a not-yet-up acceptor
    // just yields a transport error we back off on (rather than failing here).
    Ok(ep.connect_lazy())
}

/// Run the initiator forever: (re)connect, run one Heartbeat session, and on a
/// terminal session event back off and reconnect. `rdma_down` delivers a signal
/// on each RDMA data-path down-edge (from the kernel-state watcher, or a test).
pub async fn run<N: ReadyNotifier>(
    cfg: ConnectConfig,
    notifier: N,
    mut rdma_down: mpsc::Receiver<()>,
) -> anyhow::Result<()> {
    let mut gate = Gate::new();
    let mut backoff = Backoff::new(cfg.base_ms, cfg.ceil_ms);

    loop {
        let ev = session(&cfg, &notifier, &mut gate, &mut rdma_down).await;
        let action = plan_conn(ev, &mut backoff);
        let delay = match action {
            ConnAction::OpenGate | ConnAction::Wait => {
                // These are mid-session states; session() only returns terminal
                // events, so we shouldn't get here. Treat as a short retry.
                crate::logic::MIN_BACKOFF_MS
            }
            ConnAction::ReconnectAfter(ms) => {
                tracing::info!(backoff_ms = ms, "session ended; reconnecting after backoff");
                ms
            }
            ConnAction::HardFailAfter(ms) => {
                tracing::error!(
                    backoff_ms = ms,
                    "control-plane authentication FAILED (bad PSK); gate stays closed"
                );
                ms
            }
        };
        tokio::time::sleep(Duration::from_millis(delay as u64)).await;
    }
}

/// Run a single Heartbeat session; return the terminal [`ConnEvent`] that ends
/// it (TransportError / Unauthenticated / BusyPong / DrainingPong). Ready and
/// not-ready pongs are handled in-session (open the gate / keep waiting).
async fn session<N: ReadyNotifier>(
    cfg: &ConnectConfig,
    notifier: &N,
    gate: &mut Gate,
    rdma_down: &mut mpsc::Receiver<()>,
) -> ConnEvent {
    let channel = match build_channel(cfg) {
        Ok(c) => c,
        Err(e) => {
            tracing::warn!(error = %e, "bad target/channel config");
            return ConnEvent::TransportError;
        }
    };
    let mut client = UrpControlClient::new(channel);

    // Optional single-shot unary reachability/version check.
    if cfg.do_rendezvous {
        let req = RendezvousRequest {
            endpoint_name: cfg.endpoint_name.clone(),
            proto_version: crate::logic::URP_CONTROL_PROTO_VERSION,
            local_id: cfg.local_id.clone(),
            auth_token: cfg.token.to_vec(),
        };
        match client.rendezvous(req).await {
            Ok(_) => {}
            Err(status) if status.code() == tonic::Code::Unauthenticated => {
                return ConnEvent::Unauthenticated;
            }
            Err(status) => {
                tracing::debug!(%status, "rendezvous failed; will retry via stream");
            }
        }
    }

    // Open the bidi stream: outbound pings via an mpsc, inbound pongs read below.
    let (tx, rx) = mpsc::channel::<crate::pb::HeartbeatPing>(16);
    let mut seq = 0u64;
    // Seed the initial probe before we hand the stream to tonic (buffered).
    let initial = build_ping(
        &cfg.endpoint_name,
        &cfg.local_id,
        &cfg.token,
        seq,
        ProbeReason::Initial,
        now_unix_ns(),
    );
    if tx.send(initial).await.is_err() {
        return ConnEvent::TransportError;
    }

    let mut inbound = match client.heartbeat(ReceiverStream::new(rx)).await {
        Ok(resp) => resp.into_inner(),
        Err(status) if status.code() == tonic::Code::Unauthenticated => {
            return ConnEvent::Unauthenticated;
        }
        Err(status) if status.code() == tonic::Code::ResourceExhausted => {
            tracing::info!("server too busy (RESOURCE_EXHAUSTED); backing off");
            return ConnEvent::BusyPong(cfg.base_ms);
        }
        Err(_) => return ConnEvent::TransportError,
    };

    // StdRng (not ThreadRng) so the session future stays Send/spawnable.
    let mut rng = rand::rngs::StdRng::from_entropy();
    let next_delay = |rng: &mut rand::rngs::StdRng| {
        Duration::from_millis(next_heartbeat_delay(cfg.heartbeat_ms, cfg.jitter_frac, rng))
    };
    let timer = tokio::time::sleep(next_delay(&mut rng));
    tokio::pin!(timer);

    loop {
        tokio::select! {
            // (a) jittered periodic heartbeat
            _ = &mut timer => {
                seq = next_seq(seq);
                let ping = build_ping(&cfg.endpoint_name, &cfg.local_id, &cfg.token,
                    seq, ProbeReason::Periodic, now_unix_ns());
                if tx.send(ping).await.is_err() {
                    return ConnEvent::TransportError;
                }
                timer.as_mut().reset(tokio::time::Instant::now() + next_delay(&mut rng));
            }
            // (b) out-of-band probe on an RDMA data-path down-edge
            Some(()) = rdma_down.recv() => {
                seq = next_seq(seq);
                tracing::info!(seq, "RDMA data path down-edge; sending immediate PROBE_RDMA_FAILURE");
                let ping = build_ping(&cfg.endpoint_name, &cfg.local_id, &cfg.token,
                    seq, ProbeReason::RdmaFailure, now_unix_ns());
                if tx.send(ping).await.is_err() {
                    return ConnEvent::TransportError;
                }
            }
            // (c) inbound pong
            msg = inbound.message() => {
                match msg {
                    Ok(Some(pong)) => match classify_pong(&pong) {
                        PongClass::Ready => {
                            if gate.observe(true, true) {
                                tracing::info!("peer ready; opening readiness gate");
                                notifier.notify_ready();
                            }
                        }
                        PongClass::NotReady => {
                            tracing::debug!(seq = pong.seq, "peer not ready yet");
                        }
                        PongClass::Busy(ms) => return ConnEvent::BusyPong(ms),
                        PongClass::Draining => return ConnEvent::DrainingPong,
                    },
                    Ok(None) => return ConnEvent::TransportError, // server closed
                    Err(status) if status.code() == tonic::Code::Unauthenticated => {
                        return ConnEvent::Unauthenticated;
                    }
                    Err(_) => return ConnEvent::TransportError,
                }
            }
        }
    }
}
