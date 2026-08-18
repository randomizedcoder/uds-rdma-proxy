//! `urp-control` daemon entrypoint. Two subcommands:
//!   serve    -- acceptor: serve Rendezvous/Heartbeat over gRPC
//!   connect  -- initiator: gate the app on the acceptor reporting ready
//!
//! PSK is read from a file path (systemd `LoadCredential`), never argv/env; it
//! is hashed to a token in-process and the raw bytes are dropped immediately.

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use tokio::sync::mpsc;

use urp_control::connect::{self, ConnectConfig, ReadyNotifier};
use urp_control::logic::{compute_token, detect_edge, should_probe_now};
use urp_control::pb::urp_control_server::UrpControlServer;
use urp_control::serve::ControlService;
use urp_control::state::LiveSource;

#[derive(Parser, Debug)]
#[command(name = "urp-control", version, about = "urp control-plane gRPC daemon")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Acceptor: serve the control plane for a local endpoint.
    Serve(ServeArgs),
    /// Initiator: connect to a peer acceptor and gate the app on ready.
    Connect(ConnectArgs),
}

#[derive(Parser, Debug)]
struct ServeArgs {
    /// Local endpoint name whose state answers `ready`.
    #[arg(long)]
    endpoint: String,
    /// Address to bind (host:port).
    #[arg(long, default_value = "0.0.0.0:50051")]
    listen: SocketAddr,
    /// Path to the PSK file (hashed in-process; never logged).
    #[arg(long)]
    password_file: PathBuf,
    /// Max concurrent Heartbeat streams (0 = unbounded).
    #[arg(long, default_value_t = 256)]
    session_cap: u32,
    /// Identity reported as peer_id (observability).
    #[arg(long, default_value = "")]
    local_id: String,
}

#[derive(Parser, Debug)]
struct ConnectArgs {
    /// Peer endpoint name to reach.
    #[arg(long)]
    endpoint: String,
    /// Peer control-plane target, e.g. http://host:50051.
    #[arg(long)]
    target: String,
    /// Path to the PSK file (hashed in-process; never logged).
    #[arg(long)]
    password_file: PathBuf,
    /// This host's identity (observability).
    #[arg(long, default_value = "")]
    local_id: String,
    /// Heartbeat base cadence in ms (jittered +/- jitter-frac).
    #[arg(long, default_value_t = 60_000)]
    heartbeat_ms: u64,
    /// Heartbeat jitter fraction (0.10 = +/-10%).
    #[arg(long, default_value_t = 0.10)]
    jitter_frac: f64,
    /// Base reconnect backoff in ms.
    #[arg(long, default_value_t = 100)]
    backoff_base_ms: u32,
    /// Ceiling reconnect backoff in ms.
    #[arg(long, default_value_t = 2_000)]
    backoff_ceil_ms: u32,
    /// Poll interval for the kernel-state RDMA-failure watcher, in ms.
    #[arg(long, default_value_t = 1_000)]
    poll_ms: u64,
}

/// Production readiness notifier: sd_notify(READY=1).
struct SdNotifier;
impl ReadyNotifier for SdNotifier {
    fn notify_ready(&self) {
        if let Err(e) = sd_notify::notify(false, &[sd_notify::NotifyState::Ready]) {
            tracing::warn!(error = %e, "sd_notify(READY=1) failed");
        }
    }
}

fn read_token(path: &PathBuf) -> Result<[u8; 32]> {
    let bytes = std::fs::read(path)
        .with_context(|| format!("reading PSK file {}", path.display()))?;
    Ok(compute_token(&bytes))
}

fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .init();

    let cli = Cli::parse();
    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?;
    rt.block_on(async {
        match cli.cmd {
            Cmd::Serve(a) => run_serve(a).await,
            Cmd::Connect(a) => run_connect(a).await,
        }
    })
}

async fn run_serve(a: ServeArgs) -> Result<()> {
    let token = read_token(&a.password_file)?;
    let source = Arc::new(LiveSource {
        local_id: a.local_id.clone(),
    });
    let svc = ControlService::new(source, token, a.session_cap);
    tracing::info!(endpoint = %a.endpoint, listen = %a.listen, "urp-control serve");

    tonic::transport::Server::builder()
        .http2_keepalive_interval(Some(Duration::from_secs(20)))
        .add_service(UrpControlServer::new(svc))
        .serve_with_shutdown(a.listen, async {
            let _ = tokio::signal::ctrl_c().await;
            tracing::info!("shutting down (GOAWAY)");
        })
        .await
        .context("gRPC server")?;
    Ok(())
}

async fn run_connect(a: ConnectArgs) -> Result<()> {
    let token = read_token(&a.password_file)?;
    let cfg = ConnectConfig {
        target: a.target.clone(),
        endpoint_name: a.endpoint.clone(),
        local_id: a.local_id.clone(),
        token,
        base_ms: a.backoff_base_ms,
        ceil_ms: a.backoff_ceil_ms,
        heartbeat_ms: a.heartbeat_ms,
        jitter_frac: a.jitter_frac,
        do_rendezvous: true,
        connect_timeout_ms: 3_000,
        keepalive_ms: 10_000,
    };

    // Kernel-state watcher: poll the local endpoint's `connected` and signal on
    // the down-edge (yes -> no), driving an immediate PROBE_RDMA_FAILURE.
    let (down_tx, down_rx) = mpsc::channel::<()>(4);
    let watch_name = a.endpoint.clone();
    let poll = Duration::from_millis(a.poll_ms);
    tokio::spawn(async move {
        let mut prev: Option<bool> = None;
        loop {
            let name = watch_name.clone();
            let connected = tokio::task::spawn_blocking(move || endpoint_connected(&name))
                .await
                .unwrap_or(None);
            if let Some(now) = connected {
                if should_probe_now(detect_edge(prev, now)) {
                    let _ = down_tx.try_send(());
                }
                prev = Some(now);
            }
            tokio::time::sleep(poll).await;
        }
    });

    tracing::info!(endpoint = %a.endpoint, target = %a.target, "urp-control connect");
    connect::run(cfg, SdNotifier, down_rx).await
}

/// Blocking netlink read of the local endpoint's connected state. `None` if the
/// endpoint is absent or the query fails (treated as "no edge").
fn endpoint_connected(name: &str) -> Option<bool> {
    let mut sock = urp_netlink::UrpSocket::connect().ok()?;
    let ep = urp_netlink::get_endpoint(&mut sock, name).ok()??;
    // "connected" == the endpoint is Active with >=1 QP up.
    Some(urp_netlink::is_endpoint_ready(
        urp_netlink::uapi::UrpEndpointState::from_str(&ep.state),
        ep.num_qps,
    ))
}
