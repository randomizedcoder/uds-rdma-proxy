//! Pure control-plane logic -- no tokio, no netlink, no tonic transport.
//!
//! Everything here is a plain function or small value type over plain data, so
//! it is exhaustively unit-testable with truth tables (house style R6). The
//! async plumbing in [`crate::serve`] / [`crate::connect`] is a thin shell that
//! calls into these; the interesting decisions all live here.
//!
//! The one non-`std` coupling is [`crate::pb`] (prost message types): those are
//! pure data (no runtime), so building/classifying pongs stays testable.

use subtle::ConstantTimeEq;

use crate::pb;
pub use urp_netlink::uapi::UrpEndpointState;
pub use urp_netlink::is_endpoint_ready;

/// Control-plane protocol version carried in `proto_version`. Bumped only on a
/// breaking handshake change; additive proto fields do NOT bump it.
pub const URP_CONTROL_PROTO_VERSION: u32 = 1;

/// Floor applied to any client-side backoff so a misconfigured server can't pin
/// the client in a hot reconnect loop.
pub const MIN_BACKOFF_MS: u32 = 100;

/// Backoff after an authentication failure. Long: a bad PSK is an operator
/// error, not a transient, so we retry rarely and loudly rather than hammering.
pub const HARDFAIL_BACKOFF_MS: u32 = 30_000;

// ---------------------------------------------------------------------------
// PSK auth (SHA-256 of the password file, constant-time compared).
// ---------------------------------------------------------------------------

/// Derive the 32-byte auth token from raw password-file bytes.
pub fn compute_token(password: &[u8]) -> [u8; 32] {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    h.update(password);
    h.finalize().into()
}

/// Constant-time compare a presented token against the expected one. A
/// wrong-length token is rejected without leaking where it diverged.
pub fn verify_token(presented: &[u8], expected: &[u8; 32]) -> bool {
    if presented.len() != expected.len() {
        return false;
    }
    presented.ct_eq(expected).into()
}

/// Terminal auth decision for a server handler.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum AuthOutcome {
    Ok,
    Unauthenticated,
}

pub fn authenticate(presented: &[u8], expected: &[u8; 32]) -> AuthOutcome {
    if verify_token(presented, expected) {
        AuthOutcome::Ok
    } else {
        AuthOutcome::Unauthenticated
    }
}

// ---------------------------------------------------------------------------
// Readiness gate (monotonic-open at boot).
// ---------------------------------------------------------------------------

/// The gate opens only when the peer reports ready AND our control channel is
/// up. Both conditions, full 2x2.
pub fn should_open_gate(ready: bool, channel_up: bool) -> bool {
    ready && channel_up
}

/// The app-readiness gate. Monotonic: once open it stays open across any later
/// transient not-ready/drop (the kernel Phase-1.5 layer owns RDMA reconnect, so
/// bouncing the app would be wrong). Closes ONLY on an explicit opt-in teardown.
#[derive(Debug, Default)]
pub struct Gate {
    open: bool,
}

impl Gate {
    pub fn new() -> Self {
        Gate { open: false }
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    /// Observe a (ready, channel_up) sample. Returns `true` only on the rising
    /// edge that first opens the gate -- the caller fires `sd_notify(READY=1)`
    /// exactly once on that edge.
    pub fn observe(&mut self, ready: bool, channel_up: bool) -> bool {
        if !self.open && should_open_gate(ready, channel_up) {
            self.open = true;
            return true;
        }
        false
    }

    /// Explicit, opt-in close (systemd `BindsTo` teardown). Off by default.
    pub fn teardown(&mut self) {
        self.open = false;
    }
}

// ---------------------------------------------------------------------------
// Backoff (capped exponential).
// ---------------------------------------------------------------------------

/// `base << attempt`, clamped to `ceil`. Overflow-safe (shift capped at 31),
/// and defensive against a misordered/zero config.
pub fn next_backoff(attempt: u32, base_ms: u32, ceil_ms: u32) -> u32 {
    if base_ms == 0 || ceil_ms == 0 {
        return 0;
    }
    if base_ms >= ceil_ms {
        return ceil_ms;
    }
    let shift = attempt.min(31);
    let scaled = (base_ms as u64).checked_shl(shift).unwrap_or(u64::MAX);
    scaled.min(ceil_ms as u64) as u32
}

/// Mutable backoff cursor: `next()` returns the delay for the current attempt
/// then advances; `reset()` returns to attempt 0 (call on a successful ready).
#[derive(Debug, Clone)]
pub struct Backoff {
    attempt: u32,
    base_ms: u32,
    ceil_ms: u32,
}

impl Backoff {
    pub fn new(base_ms: u32, ceil_ms: u32) -> Self {
        Backoff {
            attempt: 0,
            base_ms,
            ceil_ms,
        }
    }
    pub fn reset(&mut self) {
        self.attempt = 0;
    }
    pub fn attempt(&self) -> u32 {
        self.attempt
    }
    pub fn next(&mut self) -> u32 {
        let d = next_backoff(self.attempt, self.base_ms, self.ceil_ms);
        self.attempt = self.attempt.saturating_add(1);
        d
    }
}

// ---------------------------------------------------------------------------
// Heartbeat cadence (lazy ~60s with +/- jitter to desync many hosts).
// ---------------------------------------------------------------------------

/// Deterministic heartbeat delay given a uniform `draw` in [0, 1). `draw` maps
/// linearly to [-jitter, +jitter] around `base_ms`; the result never goes
/// negative. Split out from [`next_heartbeat_delay`] so the bounds are testable
/// without an rng.
pub fn heartbeat_delay_ms(base_ms: u64, jitter_frac: f64, draw: f64) -> u64 {
    let span = base_ms as f64 * jitter_frac;
    let offset = (draw * 2.0 - 1.0) * span; // [-span, +span]
    let ms = base_ms as f64 + offset;
    if ms <= 0.0 {
        0
    } else {
        ms.round() as u64
    }
}

/// Draw a jittered heartbeat delay from `rng`. For (60_000, 0.10) the result is
/// in [54_000, 66_000] ms.
pub fn next_heartbeat_delay<R: rand::Rng>(base_ms: u64, jitter_frac: f64, rng: &mut R) -> u64 {
    let draw: f64 = rng.gen_range(0.0..1.0);
    heartbeat_delay_ms(base_ms, jitter_frac, draw)
}

// ---------------------------------------------------------------------------
// Sequence numbers.
// ---------------------------------------------------------------------------

/// Next monotonic sequence number. Saturates at `u64::MAX` rather than wrapping
/// (a wrap would look like a massive backwards jump to the peer).
pub fn next_seq(prev: u64) -> u64 {
    prev.saturating_add(1)
}

/// Validate a pong's echoed seq against what we expect.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum SeqCheck {
    Ok,
    Stale, // echo < expected: a delayed/duplicate pong
    Gap,   // echo > expected: we missed a pong
}

pub fn check_seq_echo(expected: u64, got: u64) -> SeqCheck {
    use std::cmp::Ordering::*;
    match got.cmp(&expected) {
        Equal => SeqCheck::Ok,
        Less => SeqCheck::Stale,
        Greater => SeqCheck::Gap,
    }
}

// ---------------------------------------------------------------------------
// RDMA data-path down-edge (drives the immediate PROBE_RDMA_FAILURE ping).
// ---------------------------------------------------------------------------

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Edge {
    Down, // connected: yes -> no
    Up,   // connected: no -> yes
    None,
}

/// Classify the transition between two consecutive `connected` samples.
/// `prev == None` is the first sample -> no edge yet.
pub fn detect_edge(prev: Option<bool>, now: bool) -> Edge {
    match (prev, now) {
        (Some(true), false) => Edge::Down,
        (Some(false), true) => Edge::Up,
        _ => Edge::None,
    }
}

/// Only a down-edge triggers an out-of-band failure probe.
pub fn should_probe_now(edge: Edge) -> bool {
    matches!(edge, Edge::Down)
}

// ---------------------------------------------------------------------------
// Server overload ("I'm too busy -- go away").
// ---------------------------------------------------------------------------

/// Shed a *new* stream when the server is at/over its session cap. `cap == 0`
/// means "accept nothing" (fully closed).
pub fn should_shed(active: u32, cap: u32) -> bool {
    if cap == 0 {
        return true;
    }
    active >= cap
}

// ---------------------------------------------------------------------------
// Endpoint snapshot + server reply builders.
// ---------------------------------------------------------------------------

/// A point-in-time view of the acceptor's own endpoint, from netlink. Pure data
/// so the server handlers can be tested against a fake.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct EndpointSnapshot {
    pub present: bool,
    pub state: Option<UrpEndpointState>,
    pub num_qps: u32,
    pub buffer_size: u32,
    pub peer_id: String,
}

impl EndpointSnapshot {
    /// The absent endpoint: not present, therefore never ready.
    pub fn absent() -> Self {
        EndpointSnapshot::default()
    }
    fn ready(&self) -> bool {
        self.present && is_endpoint_ready(self.state, self.num_qps)
    }
}

pub fn build_rendezvous_reply(snap: &EndpointSnapshot) -> pb::RendezvousReply {
    pb::RendezvousReply {
        ready: snap.ready(),
        peer_id: snap.peer_id.clone(),
        num_qps: snap.num_qps,
        buffer_size: snap.buffer_size,
    }
}

/// Build a pong for an inbound ping. `shed` forces BUSY (mid-session overload);
/// a draining endpoint reports DRAINING; otherwise OK. `ready` is only ever true
/// under STATUS_OK.
pub fn build_pong(
    ping_seq: u64,
    snap: &EndpointSnapshot,
    shed: bool,
    busy_backoff_ms: u32,
) -> pb::HeartbeatPong {
    let draining = matches!(snap.state, Some(UrpEndpointState::Draining));
    let (status, backoff) = if draining {
        (pb::ServerStatus::Draining, busy_backoff_ms.max(MIN_BACKOFF_MS))
    } else if shed {
        (pb::ServerStatus::Busy, busy_backoff_ms.max(MIN_BACKOFF_MS))
    } else {
        (pb::ServerStatus::Ok, 0)
    };
    let ready = status == pb::ServerStatus::Ok && snap.ready();
    pb::HeartbeatPong {
        seq: ping_seq,
        ready,
        status: status as i32,
        suggested_backoff_ms: backoff,
        num_qps: snap.num_qps,
        buffer_size: snap.buffer_size,
        peer_id: snap.peer_id.clone(),
    }
}

// ---------------------------------------------------------------------------
// Client-side pong classification + reconnect planning.
// ---------------------------------------------------------------------------

/// What a pong means to the client. Status takes precedence over `ready` (a
/// DRAINING/BUSY server is going/too-busy regardless of the ready bit).
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum PongClass {
    Ready,
    NotReady,
    Busy(u32), // honor this backoff (>= MIN_BACKOFF_MS)
    Draining,
}

pub fn classify_pong(pong: &pb::HeartbeatPong) -> PongClass {
    match pb::ServerStatus::try_from(pong.status) {
        Ok(pb::ServerStatus::Draining) => PongClass::Draining,
        Ok(pb::ServerStatus::Busy) => PongClass::Busy(pong.suggested_backoff_ms.max(MIN_BACKOFF_MS)),
        Ok(pb::ServerStatus::Ok) => {
            if pong.ready {
                PongClass::Ready
            } else {
                PongClass::NotReady
            }
        }
        // Unknown/out-of-range status int: be defensive and back off rather
        // than trust a ready bit we can't interpret.
        Err(_) => PongClass::Busy(MIN_BACKOFF_MS),
    }
}

/// An event in the client's session lifecycle.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ConnEvent {
    ReadyPong,
    NotReadyPong,
    BusyPong(u32),
    DrainingPong,
    TransportError,
    Unauthenticated,
}

/// What the client should do next.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ConnAction {
    OpenGate,             // ready: open the gate, keep beating
    Wait,                 // not-ready: hold the stream, keep beating
    ReconnectAfter(u32),  // tear the stream, sleep, reconnect + re-rendezvous
    HardFailAfter(u32),   // auth failure: loud, long backoff, gate stays closed
}

/// Map an event to an action, advancing `backoff` for the exponential cases and
/// resetting it on a ready pong.
pub fn plan_conn(ev: ConnEvent, backoff: &mut Backoff) -> ConnAction {
    match ev {
        ConnEvent::ReadyPong => {
            backoff.reset();
            ConnAction::OpenGate
        }
        ConnEvent::NotReadyPong => ConnAction::Wait,
        // Honor the server's suggested backoff verbatim; do NOT grow the
        // exponential schedule (BUSY is advisory, not an error).
        ConnEvent::BusyPong(ms) => ConnAction::ReconnectAfter(ms.max(MIN_BACKOFF_MS)),
        ConnEvent::DrainingPong => ConnAction::ReconnectAfter(backoff.next().max(MIN_BACKOFF_MS)),
        ConnEvent::TransportError => ConnAction::ReconnectAfter(backoff.next().max(MIN_BACKOFF_MS)),
        ConnEvent::Unauthenticated => ConnAction::HardFailAfter(HARDFAIL_BACKOFF_MS),
    }
}

/// Build a ping message (client side).
pub fn build_ping(
    endpoint_name: &str,
    local_id: &str,
    token: &[u8; 32],
    seq: u64,
    reason: pb::ProbeReason,
    send_unix_ns: u64,
) -> pb::HeartbeatPing {
    pb::HeartbeatPing {
        endpoint_name: endpoint_name.to_string(),
        proto_version: URP_CONTROL_PROTO_VERSION,
        local_id: local_id.to_string(),
        auth_token: token.to_vec(),
        seq,
        reason: reason as i32,
        send_unix_ns,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use prost::Message;

    // -- SHA-256 known-answer vectors (NIST / RFC examples). --
    #[test]
    fn compute_token_kats() {
        // "" -> e3b0c442...  ; "abc" -> ba7816bf...
        let empty = compute_token(b"");
        assert_eq!(
            hex(&empty),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        let abc = compute_token(b"abc");
        assert_eq!(
            hex(&abc),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        // Deterministic: same input twice -> identical.
        assert_eq!(compute_token(b"hunter2"), compute_token(b"hunter2"));
        // Boundary: >64-byte (past the SHA block) and NUL/non-UTF8 bytes work.
        let big = compute_token(&[0xffu8; 100]);
        assert_eq!(big, compute_token(&[0xffu8; 100]));
        let nul = compute_token(&[0u8, 1, 2, 0, 255]);
        assert_eq!(nul.len(), 32);
    }

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    #[test]
    fn verify_token_truth_table() {
        let a = [7u8; 32];
        let mut b;
        // P: equal -> true
        assert!(verify_token(&a, &a));
        // N: differ in first / middle / last byte -> false
        b = a;
        b[0] ^= 1;
        assert!(!verify_token(&b, &a));
        b = a;
        b[16] ^= 1;
        assert!(!verify_token(&b, &a));
        b = a;
        b[31] ^= 1;
        assert!(!verify_token(&b, &a));
        // B: all-zero == all-zero -> true
        assert!(verify_token(&[0u8; 32], &[0u8; 32]));
        // C: wrong length (0 / 31 / 33) -> false, no panic
        assert!(!verify_token(&[], &a));
        assert!(!verify_token(&[7u8; 31], &a));
        assert!(!verify_token(&[7u8; 33], &a));
    }

    #[test]
    fn should_open_gate_2x2() {
        assert!(!should_open_gate(false, false));
        assert!(!should_open_gate(true, false));
        assert!(!should_open_gate(false, true));
        assert!(should_open_gate(true, true));
    }

    #[test]
    fn gate_monotonic_open() {
        // P: [notready, ready, notready, drop] opens at #2, stays open.
        let mut g = Gate::new();
        assert!(!g.observe(false, true)); // not ready
        assert!(!g.is_open());
        assert!(g.observe(true, true)); // rising edge
        assert!(g.is_open());
        assert!(!g.observe(false, true)); // stays open, no new edge
        assert!(g.is_open());
        assert!(!g.observe(true, false)); // channel drop: stays open
        assert!(g.is_open());
        // B: [drop, drop, ready] opens at #3.
        let mut g2 = Gate::new();
        assert!(!g2.observe(true, false));
        assert!(!g2.observe(false, false));
        assert!(g2.observe(true, true));
        assert!(g2.is_open());
        // C: closes ONLY on an explicit teardown.
        g2.teardown();
        assert!(!g2.is_open());
    }

    #[test]
    fn next_backoff_table() {
        // P: capped exponential
        assert_eq!(next_backoff(0, 100, 10_000), 100);
        assert_eq!(next_backoff(1, 100, 10_000), 200);
        assert_eq!(next_backoff(2, 100, 10_000), 400);
        // B: value hits ceil exactly, next clamps
        assert_eq!(next_backoff(3, 100, 800), 800); // 800 == ceil
        assert_eq!(next_backoff(4, 100, 800), 800); // clamped
        // C: overflow-safe + misordered/zero configs
        assert_eq!(next_backoff(u32::MAX, 100, 10_000), 10_000);
        assert_eq!(next_backoff(5, 900, 800), 800); // base > ceil -> ceil
        assert_eq!(next_backoff(0, 0, 800), 0); // base 0
        assert_eq!(next_backoff(0, 100, 0), 0); // ceil 0
        // Property: non-decreasing up to the clamp.
        let mut prev = 0;
        for a in 0..20 {
            let v = next_backoff(a, 100, 10_000);
            assert!(v >= prev);
            prev = v;
        }
    }

    #[test]
    fn backoff_cursor_grows_and_resets() {
        let mut b = Backoff::new(100, 10_000);
        assert_eq!(b.next(), 100);
        assert_eq!(b.next(), 200);
        assert_eq!(b.next(), 400);
        b.reset();
        assert_eq!(b.attempt(), 0);
        assert_eq!(b.next(), 100);
    }

    #[test]
    fn heartbeat_delay_bounds() {
        // P/B: 60s +/- 10% across min/max draws.
        assert_eq!(heartbeat_delay_ms(60_000, 0.10, 0.0), 54_000);
        assert_eq!(heartbeat_delay_ms(60_000, 0.10, 1.0), 66_000);
        // midpoint
        assert_eq!(heartbeat_delay_ms(60_000, 0.10, 0.5), 60_000);
        // C: frac 0 -> exactly base; base 0 -> 0; frac 1.0 -> [0, 2*base], never negative.
        assert_eq!(heartbeat_delay_ms(60_000, 0.0, 0.9), 60_000);
        assert_eq!(heartbeat_delay_ms(0, 0.10, 0.9), 0);
        assert_eq!(heartbeat_delay_ms(60_000, 1.0, 0.0), 0);
        assert_eq!(heartbeat_delay_ms(60_000, 1.0, 1.0), 120_000);
    }

    #[test]
    fn heartbeat_delay_within_window_for_all_draws() {
        for i in 0..=1000u32 {
            let draw = i as f64 / 1000.0;
            let d = heartbeat_delay_ms(60_000, 0.10, draw);
            assert!((54_000..=66_000).contains(&d), "draw {draw} -> {d}");
        }
    }

    #[test]
    fn next_seq_saturates() {
        assert_eq!(next_seq(0), 1);
        assert_eq!(next_seq(41), 42);
        assert_eq!(next_seq(u64::MAX), u64::MAX);
    }

    #[test]
    fn check_seq_echo_table() {
        assert_eq!(check_seq_echo(5, 5), SeqCheck::Ok);
        assert_eq!(check_seq_echo(5, 4), SeqCheck::Stale);
        assert_eq!(check_seq_echo(5, 6), SeqCheck::Gap);
        // wraparound boundary
        assert_eq!(check_seq_echo(u64::MAX, u64::MAX), SeqCheck::Ok);
        assert_eq!(check_seq_echo(u64::MAX, 0), SeqCheck::Stale);
    }

    #[test]
    fn detect_edge_table() {
        assert_eq!(detect_edge(Some(true), false), Edge::Down);
        assert!(should_probe_now(detect_edge(Some(true), false)));
        assert_eq!(detect_edge(Some(false), false), Edge::None);
        assert_eq!(detect_edge(Some(true), true), Edge::None);
        assert_eq!(detect_edge(Some(false), true), Edge::Up);
        assert!(!should_probe_now(detect_edge(Some(false), true)));
        // C: first sample -> no edge
        assert_eq!(detect_edge(None, true), Edge::None);
        assert_eq!(detect_edge(None, false), Edge::None);
    }

    #[test]
    fn should_shed_table() {
        assert!(!should_shed(0, 4));
        assert!(!should_shed(3, 4)); // cap-1
        assert!(should_shed(4, 4)); // cap
        assert!(should_shed(5, 4)); // cap+1
        assert!(should_shed(0, 0)); // cap 0 -> always shed
    }

    #[test]
    fn is_endpoint_ready_reexport() {
        assert!(is_endpoint_ready(Some(UrpEndpointState::Active), 1));
        assert!(!is_endpoint_ready(Some(UrpEndpointState::Active), 0));
        assert!(!is_endpoint_ready(Some(UrpEndpointState::Draining), 1));
        assert!(!is_endpoint_ready(None, 1));
    }

    fn snap(state: Option<UrpEndpointState>, qps: u32) -> EndpointSnapshot {
        EndpointSnapshot {
            present: true,
            state,
            num_qps: qps,
            buffer_size: 4096,
            peer_id: "peer".into(),
        }
    }

    #[test]
    fn build_pong_and_classify_roundtrip() {
        // Active + not shed -> OK/ready -> classify Ready.
        let p = build_pong(7, &snap(Some(UrpEndpointState::Active), 1), false, 0);
        assert_eq!(p.seq, 7);
        assert!(p.ready);
        assert_eq!(p.status, pb::ServerStatus::Ok as i32);
        assert_eq!(classify_pong(&p), PongClass::Ready);

        // Active but shed -> BUSY (ready forced false).
        let p = build_pong(8, &snap(Some(UrpEndpointState::Active), 1), true, 250);
        assert!(!p.ready);
        assert_eq!(p.status, pb::ServerStatus::Busy as i32);
        assert_eq!(classify_pong(&p), PongClass::Busy(250));

        // Draining endpoint -> DRAINING regardless of ready.
        let p = build_pong(9, &snap(Some(UrpEndpointState::Draining), 1), false, 0);
        assert!(!p.ready);
        assert_eq!(classify_pong(&p), PongClass::Draining);

        // Active-but-zero-QP -> OK/not-ready -> NotReady.
        let p = build_pong(10, &snap(Some(UrpEndpointState::Active), 0), false, 0);
        assert!(!p.ready);
        assert_eq!(classify_pong(&p), PongClass::NotReady);

        // Absent endpoint -> not ready.
        let p = build_pong(11, &EndpointSnapshot::absent(), false, 0);
        assert!(!p.ready);
        assert_eq!(classify_pong(&p), PongClass::NotReady);
    }

    #[test]
    fn classify_pong_corner_cases() {
        // BUSY with backoff 0 -> min clamp.
        let p = pb::HeartbeatPong {
            status: pb::ServerStatus::Busy as i32,
            suggested_backoff_ms: 0,
            ..Default::default()
        };
        assert_eq!(classify_pong(&p), PongClass::Busy(MIN_BACKOFF_MS));
        // Unknown status int -> defensive Busy(min).
        let p = pb::HeartbeatPong {
            status: 99,
            ready: true,
            ..Default::default()
        };
        assert_eq!(classify_pong(&p), PongClass::Busy(MIN_BACKOFF_MS));
    }

    #[test]
    fn plan_conn_fsm() {
        // [transport-err x2, ok]: backoff grows then resets.
        let mut b = Backoff::new(100, 10_000);
        assert_eq!(plan_conn(ConnEvent::TransportError, &mut b), ConnAction::ReconnectAfter(100));
        assert_eq!(plan_conn(ConnEvent::TransportError, &mut b), ConnAction::ReconnectAfter(200));
        assert_eq!(plan_conn(ConnEvent::ReadyPong, &mut b), ConnAction::OpenGate);
        assert_eq!(b.attempt(), 0);
        assert_eq!(plan_conn(ConnEvent::TransportError, &mut b), ConnAction::ReconnectAfter(100));

        // BUSY(500) honors 500, not the exp schedule.
        let mut b2 = Backoff::new(100, 10_000);
        b2.next();
        b2.next(); // advance so exp != 500
        assert_eq!(plan_conn(ConnEvent::BusyPong(500), &mut b2), ConnAction::ReconnectAfter(500));

        // NotReady -> Wait.
        let mut b3 = Backoff::new(100, 10_000);
        assert_eq!(plan_conn(ConnEvent::NotReadyPong, &mut b3), ConnAction::Wait);

        // UNAUTHENTICATED -> HardFail, then a ready pong resets backoff.
        let mut b4 = Backoff::new(100, 10_000);
        assert_eq!(
            plan_conn(ConnEvent::Unauthenticated, &mut b4),
            ConnAction::HardFailAfter(HARDFAIL_BACKOFF_MS)
        );
        assert_eq!(plan_conn(ConnEvent::ReadyPong, &mut b4), ConnAction::OpenGate);
        assert_eq!(b4.attempt(), 0);
    }

    // -- proto round-trip for all four messages (prost encode <-> decode). --

    #[test]
    fn proto_roundtrip_rendezvous() {
        let req = pb::RendezvousRequest {
            endpoint_name: "pair".into(),
            proto_version: 1,
            local_id: "hp3".into(),
            auth_token: vec![9u8; 32],
        };
        let mut buf = Vec::new();
        req.encode(&mut buf).unwrap();
        assert_eq!(pb::RendezvousRequest::decode(&buf[..]).unwrap(), req);

        let reply = pb::RendezvousReply {
            ready: true,
            peer_id: "hp1".into(),
            num_qps: 1,
            buffer_size: 4096,
        };
        let mut buf = Vec::new();
        reply.encode(&mut buf).unwrap();
        assert_eq!(pb::RendezvousReply::decode(&buf[..]).unwrap(), reply);

        // Default/empty round-trips.
        let mut buf = Vec::new();
        pb::RendezvousReply::default().encode(&mut buf).unwrap();
        assert_eq!(
            pb::RendezvousReply::decode(&buf[..]).unwrap(),
            pb::RendezvousReply::default()
        );
    }

    #[test]
    fn proto_roundtrip_heartbeat() {
        for tok in [vec![], vec![1u8; 32], vec![2u8; 64]] {
            let ping = pb::HeartbeatPing {
                endpoint_name: "pair".into(),
                proto_version: 1,
                local_id: "hp3".into(),
                auth_token: tok.clone(),
                seq: 12345,
                reason: pb::ProbeReason::RdmaFailure as i32,
                send_unix_ns: 999,
            };
            let mut buf = Vec::new();
            ping.encode(&mut buf).unwrap();
            let got = pb::HeartbeatPing::decode(&buf[..]).unwrap();
            assert_eq!(got, ping);
            assert_eq!(got.auth_token, tok);
        }

        let pong = pb::HeartbeatPong {
            seq: 12345,
            ready: true,
            status: pb::ServerStatus::Ok as i32,
            suggested_backoff_ms: 0,
            num_qps: 1,
            buffer_size: 4096,
            peer_id: "hp1".into(),
        };
        let mut buf = Vec::new();
        pong.encode(&mut buf).unwrap();
        assert_eq!(pb::HeartbeatPong::decode(&buf[..]).unwrap(), pong);
    }

    #[test]
    fn proto_forward_compat_unknown_field_and_enum() {
        // Unknown field number (8, in the reserved range) must be ignored on
        // decode, with known fields intact -- proves additive discipline.
        let ping = pb::HeartbeatPing {
            endpoint_name: "pair".into(),
            seq: 7,
            ..Default::default()
        };
        let mut buf = Vec::new();
        ping.encode(&mut buf).unwrap();
        // Append field 8, wire-type 0 (varint): tag = (8<<3)|0 = 64, value = 42.
        buf.push(64);
        buf.push(42);
        let got = pb::HeartbeatPing::decode(&buf[..]).unwrap();
        assert_eq!(got.endpoint_name, "pair");
        assert_eq!(got.seq, 7);

        // Unknown enum int (reason = 99) decodes without panic.
        let ping = pb::HeartbeatPing {
            reason: 99,
            ..Default::default()
        };
        let mut buf = Vec::new();
        ping.encode(&mut buf).unwrap();
        let got = pb::HeartbeatPing::decode(&buf[..]).unwrap();
        assert_eq!(got.reason, 99);
        assert!(pb::ProbeReason::try_from(got.reason).is_err());
    }
}
