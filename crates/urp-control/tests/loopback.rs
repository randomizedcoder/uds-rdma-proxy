//! Two-process-style loopback integration: a real tonic server on 127.0.0.1
//! plus the real `connect::run` client, over TCP. Covers the behaviors the pure
//! unit tests can't: READY firing, ping flow with incrementing seq, the
//! RDMA-failure probe, BUSY-driven reconnect, wrong-PSK gate-closed, and the
//! server session cap.

use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::sync::mpsc;
use tokio_stream::wrappers::{ReceiverStream, TcpListenerStream};
use tokio_stream::Stream;
use tonic::transport::Server;
use tonic::{Request, Response, Status, Streaming};

use urp_control::connect::{self, ConnectConfig, ReadyNotifier};
use urp_control::logic::{
    build_pong, compute_token, EndpointSnapshot, UrpEndpointState, MIN_BACKOFF_MS,
};
use urp_control::pb::urp_control_client::UrpControlClient;
use urp_control::pb::urp_control_server::{UrpControl, UrpControlServer};
use urp_control::pb::{
    HeartbeatPing, HeartbeatPong, ProbeReason, RendezvousReply, RendezvousRequest, ServerStatus,
};
use urp_control::serve::ControlService;
use urp_control::state::FakeSource;

// --- a recording server: logs received (seq, reason), replies ready pongs. ---

#[derive(Clone)]
struct RecordServer {
    token: [u8; 32],
    ready: Arc<AtomicBool>,
    pings: Arc<Mutex<Vec<(u64, i32)>>>,
    streams_opened: Arc<AtomicU32>,
    busy_once: Arc<AtomicBool>, // reply BUSY on the first stream, then normal
}

impl RecordServer {
    fn new(token: [u8; 32]) -> Self {
        RecordServer {
            token,
            ready: Arc::new(AtomicBool::new(true)),
            pings: Arc::new(Mutex::new(Vec::new())),
            streams_opened: Arc::new(AtomicU32::new(0)),
            busy_once: Arc::new(AtomicBool::new(false)),
        }
    }
}

type PongStream = Pin<Box<dyn Stream<Item = Result<HeartbeatPong, Status>> + Send>>;

#[tonic::async_trait]
impl UrpControl for RecordServer {
    async fn rendezvous(
        &self,
        request: Request<RendezvousRequest>,
    ) -> Result<Response<RendezvousReply>, Status> {
        let req = request.into_inner();
        if req.auth_token != self.token {
            return Err(Status::unauthenticated("bad psk"));
        }
        Ok(Response::new(RendezvousReply {
            ready: self.ready.load(Ordering::SeqCst),
            peer_id: "rec".into(),
            num_qps: 1,
            buffer_size: 4096,
        }))
    }

    type HeartbeatStream = PongStream;

    async fn heartbeat(
        &self,
        request: Request<Streaming<HeartbeatPing>>,
    ) -> Result<Response<Self::HeartbeatStream>, Status> {
        let this_stream = self.streams_opened.fetch_add(1, Ordering::SeqCst);
        let token = self.token;
        let ready = self.ready.clone();
        let pings = self.pings.clone();
        let busy_once = self.busy_once.clone();
        let mut inbound = request.into_inner();

        let out = async_stream::try_stream! {
            while let Some(ping) = inbound.message().await? {
                if ping.auth_token != token {
                    Err(Status::unauthenticated("bad psk"))?;
                }
                pings.lock().unwrap().push((ping.seq, ping.reason));

                if busy_once.swap(false, Ordering::SeqCst) && this_stream == 0 {
                    // One BUSY pong, then the stream ends -> client reconnects.
                    yield HeartbeatPong {
                        seq: ping.seq,
                        ready: false,
                        status: ServerStatus::Busy as i32,
                        suggested_backoff_ms: MIN_BACKOFF_MS,
                        ..Default::default()
                    };
                    return;
                }

                yield HeartbeatPong {
                    seq: ping.seq,
                    ready: ready.load(Ordering::SeqCst),
                    status: ServerStatus::Ok as i32,
                    num_qps: 1,
                    buffer_size: 4096,
                    peer_id: "rec".into(),
                    ..Default::default()
                };
            }
        };
        Ok(Response::new(Box::pin(out)))
    }
}

#[derive(Clone)]
struct FlagNotifier(Arc<AtomicBool>);
impl ReadyNotifier for FlagNotifier {
    fn notify_ready(&self) {
        self.0.store(true, Ordering::SeqCst);
    }
}

async fn spawn_record_server(server: RecordServer) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let incoming = TcpListenerStream::new(listener);
    tokio::spawn(async move {
        Server::builder()
            .add_service(UrpControlServer::new(server))
            .serve_with_incoming(incoming)
            .await
            .unwrap();
    });
    format!("http://127.0.0.1:{}", addr.port())
}

async fn eventually<F: Fn() -> bool>(within: Duration, f: F) -> bool {
    let start = tokio::time::Instant::now();
    while start.elapsed() < within {
        if f() {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    f()
}

fn fast_cfg(target: String, token: [u8; 32]) -> ConnectConfig {
    ConnectConfig {
        target,
        endpoint_name: "pair".into(),
        local_id: "hp-test".into(),
        token,
        base_ms: 50,
        ceil_ms: 200,
        heartbeat_ms: 80, // fast so periodic pings flow in the test window
        jitter_frac: 0.10,
        do_rendezvous: true,
        connect_timeout_ms: 1_000,
        keepalive_ms: 1_000,
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ready_pings_and_rdma_failure_probe() {
    let token = compute_token(b"correct horse");
    let server = RecordServer::new(token);
    let pings = server.pings.clone();
    let target = spawn_record_server(server).await;

    let notified = Arc::new(AtomicBool::new(false));
    let (down_tx, down_rx) = mpsc::channel::<()>(4);
    let cfg = fast_cfg(target, token);
    let handle = tokio::spawn(connect::run(cfg, FlagNotifier(notified.clone()), down_rx));

    // (a) READY fires within a couple seconds.
    assert!(
        eventually(Duration::from_secs(3), || notified.load(Ordering::SeqCst)).await,
        "gate should open (READY) once the server reports ready"
    );

    // (b) pings flow with incrementing seq; the first is the INITIAL probe.
    assert!(
        eventually(Duration::from_secs(2), || pings.lock().unwrap().len() >= 3).await,
        "expected several pings to flow"
    );
    {
        let p = pings.lock().unwrap();
        assert_eq!(p[0], (0, ProbeReason::Initial as i32), "first ping is INITIAL seq0");
        let seqs: Vec<u64> = p.iter().map(|(s, _)| *s).collect();
        for w in seqs.windows(2) {
            assert!(w[1] > w[0], "seqs must strictly increase: {seqs:?}");
        }
    }

    // (c) an injected RDMA down-edge triggers an immediate PROBE_RDMA_FAILURE.
    down_tx.send(()).await.unwrap();
    assert!(
        eventually(Duration::from_secs(2), || {
            pings
                .lock()
                .unwrap()
                .iter()
                .any(|(_, r)| *r == ProbeReason::RdmaFailure as i32)
        })
        .await,
        "expected a PROBE_RDMA_FAILURE ping after the down-edge"
    );

    handle.abort();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn busy_pong_triggers_reconnect() {
    let token = compute_token(b"pw");
    let server = RecordServer::new(token);
    server.busy_once.store(true, Ordering::SeqCst);
    let streams = server.streams_opened.clone();
    let target = spawn_record_server(server).await;

    let notified = Arc::new(AtomicBool::new(false));
    let (_down_tx, down_rx) = mpsc::channel::<()>(4);
    let cfg = fast_cfg(target, token);
    let handle = tokio::spawn(connect::run(cfg, FlagNotifier(notified.clone()), down_rx));

    // First stream gets BUSY and ends; the client must reconnect (2nd stream).
    assert!(
        eventually(Duration::from_secs(3), || streams.load(Ordering::SeqCst) >= 2).await,
        "client should reconnect after a BUSY pong"
    );
    // After reconnect the server is normal -> the gate eventually opens.
    assert!(
        eventually(Duration::from_secs(3), || notified.load(Ordering::SeqCst)).await,
        "gate should open on the reconnected (non-busy) stream"
    );
    handle.abort();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn wrong_psk_keeps_gate_closed() {
    let token = compute_token(b"the real secret");
    let server = RecordServer::new(token);
    let target = spawn_record_server(server).await;

    let notified = Arc::new(AtomicBool::new(false));
    let (_down_tx, down_rx) = mpsc::channel::<()>(4);
    let wrong = compute_token(b"WRONG secret");
    let cfg = fast_cfg(target, wrong);
    let handle = tokio::spawn(connect::run(cfg, FlagNotifier(notified.clone()), down_rx));

    // Give it time to try + hit HARDFAIL backoff; the gate must never open.
    tokio::time::sleep(Duration::from_millis(800)).await;
    assert!(
        !notified.load(Ordering::SeqCst),
        "gate must stay closed under a wrong PSK"
    );
    handle.abort();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn server_sheds_new_stream_at_cap() {
    // Drive the REAL ControlService (serve.rs) with a fake source, cap = 1.
    let token = compute_token(b"cap-test");
    let source = Arc::new(FakeSource::new(EndpointSnapshot {
        present: true,
        state: Some(UrpEndpointState::Active),
        num_qps: 1,
        buffer_size: 4096,
        peer_id: "acc".into(),
    }));
    let svc = ControlService::new(source, token, 1);

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let incoming = TcpListenerStream::new(listener);
    tokio::spawn(async move {
        Server::builder()
            .add_service(UrpControlServer::new(svc))
            .serve_with_incoming(incoming)
            .await
            .unwrap();
    });
    let target = format!("http://127.0.0.1:{}", addr.port());

    let mut client = UrpControlClient::connect(target).await.unwrap();

    // First stream: keep it open by holding its ping sender.
    let (tx1, rx1) = mpsc::channel::<HeartbeatPing>(4);
    tx1.send(HeartbeatPing {
        endpoint_name: "pair".into(),
        auth_token: token.to_vec(),
        seq: 0,
        ..Default::default()
    })
    .await
    .unwrap();
    let mut stream1 = client
        .heartbeat(ReceiverStream::new(rx1))
        .await
        .expect("first stream admitted")
        .into_inner();
    // Pull one pong so we know the handler is running (active == 1).
    let _ = stream1.message().await.unwrap();

    // Second stream: sends a VALID first ping (shed now happens after auth), so
    // it authenticates and is then rejected at the cap -> RESOURCE_EXHAUSTED.
    let (tx2, rx2) = mpsc::channel::<HeartbeatPing>(4);
    tx2.send(HeartbeatPing {
        endpoint_name: "pair".into(),
        auth_token: token.to_vec(),
        seq: 0,
        ..Default::default()
    })
    .await
    .unwrap();
    let err = client
        .heartbeat(ReceiverStream::new(rx2))
        .await
        .expect_err("second stream should be shed at cap");
    assert_eq!(err.code(), tonic::Code::ResourceExhausted, "got: {err:?}");

    // Keep the senders alive until here so both streams stayed open.
    drop(tx1);
    drop(tx2);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn silent_open_does_not_consume_cap() {
    // Regression (security): a peer that opens a Heartbeat stream and never
    // sends a ping must NOT pin a session slot. Under the old admit-then-auth
    // order this unauthenticated slowloris open exhausted the cap; now the slot
    // is only reserved after the first ping authenticates.
    let token = compute_token(b"cap-test");
    let source = Arc::new(FakeSource::new(EndpointSnapshot {
        present: true,
        state: Some(UrpEndpointState::Active),
        num_qps: 1,
        buffer_size: 4096,
        peer_id: "acc".into(),
    }));
    let svc = ControlService::new(source, token, 1); // cap = 1

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let incoming = TcpListenerStream::new(listener);
    tokio::spawn(async move {
        Server::builder()
            .add_service(UrpControlServer::new(svc))
            .serve_with_incoming(incoming)
            .await
            .unwrap();
    });
    let target = format!("http://127.0.0.1:{}", addr.port());

    let mut client = UrpControlClient::connect(target).await.unwrap();

    // A silent open in flight: the server is blocked awaiting its first ping and
    // has reserved no slot. Hold `_silent_tx` so the request stream stays open.
    let mut c_silent = client.clone();
    let (_silent_tx, silent_rx) = mpsc::channel::<HeartbeatPing>(4);
    tokio::spawn(async move {
        let _ = c_silent.heartbeat(ReceiverStream::new(silent_rx)).await;
    });
    // Let the silent open reach the server (would have pinned the slot before).
    tokio::time::sleep(Duration::from_millis(150)).await;

    // A legitimate stream must still be admitted despite the silent open.
    let (tx, rx) = mpsc::channel::<HeartbeatPing>(4);
    tx.send(HeartbeatPing {
        endpoint_name: "pair".into(),
        auth_token: token.to_vec(),
        seq: 0,
        ..Default::default()
    })
    .await
    .unwrap();
    let mut good = client
        .heartbeat(ReceiverStream::new(rx))
        .await
        .expect("legit stream must be admitted; silent open must not consume the cap")
        .into_inner();
    let pong = good.message().await.unwrap().expect("a pong");
    assert!(pong.ready, "admitted stream should get a ready pong");
    drop(tx);
}

/// build_pong is exercised indirectly above; assert the shape once here too so a
/// serve.rs regression surfaces in the integration suite as well.
#[test]
fn build_pong_shape() {
    let snap = EndpointSnapshot {
        present: true,
        state: Some(UrpEndpointState::Active),
        num_qps: 2,
        buffer_size: 8192,
        peer_id: "x".into(),
    };
    let p = build_pong(5, &snap, false, 0);
    assert_eq!(p.seq, 5);
    assert!(p.ready);
    assert_eq!(p.num_qps, 2);
}
