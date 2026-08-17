# 33. Initiator Connection Bring-up & Recovery

Status: **Design.** Motivated by the design 32 real-hardware bring-up; recommends
a layered fix (lazy-connect + userland gRPC control plane + bounded kernel
retry). No implementation yet.

## 33.1 Motivation

Design 32 stood up the first real-hardware RoCEv2 session (hp1 acceptor ↔ hp3
initiator, ConnectX-4 Lx). It works — a fully declarative deploy reaches
`BENCH_OK verify=full`. But the *bring-up* is fragile: **rebooting both boxes
together left the RDMA session permanently down until a manual, coordinated
restart.**

The root cause is an ordering assumption baked into the initiator. On endpoint
activation (`urp add`), the initiator **eagerly** opens the RDMA-CM connection:

```
urp add (netlink)
  └─ urp_endpoint_activate            kernel/urp_endpoint.c:217
       ├─ urp_rdma_init (initiator)   kernel/urp_endpoint.c:273
       │    └─ rdma_resolve_addr ×N   kernel/urp_rdma.c:1089   ← dials the peer NOW
       │         → ADDR/ROUTE resolved → urp_connect_work_fn
       │              └─ rdma_connect  kernel/urp_rdma.c:277
       └─ urp_socket_init (listen)    kernel/urp_endpoint.c:277
```

This happens at *config* time, before any userland client exists and regardless
of whether the peer acceptor is listening yet. And there is **no retry**: if
`rdma_resolve_addr` fails, the initiator tears down every cm_id and returns the
error (`kernel/urp_rdma.c:1091-1095`, `:1130-1135`); if the CM later delivers
`REJECTED` / `ADDR_ERROR` / `ROUTE_ERROR` / `UNREACHABLE` / `CONNECT_ERROR`, the
handler marks the QP down but the **initiator never re-resolves** — only the
acceptor rearms its listener (`kernel/urp_rdma.c:985-1016`).

On a simultaneous boot the two boxes come up within a fraction of a second of
each other, and whichever ordering loses the race is fatal. Observed on hp1/hp3:

```
hp3 (initiator)  23.290  CM event: address resolved
hp3              23.290  CM event: route resolved
hp3              23.301  CM event: rejected (8)          ← acceptor not listening yet
hp3              23.301  QP 0 CM down: rejected
hp1 (acceptor)   23.806  RDMA listening on port 4791     ← ~0.5s too late
hp3              57.523  RDMA connection failed
```

The initiator connected **~0.5 s before** the acceptor started listening, got
rejected, and — with no retry — stayed down. Recovery then required restarting
*both* endpoints in a specific order, because two secondary bugs (§33.7) leave
stale state behind.

Beyond the race, eager connect is simply **the wrong time to dial**: it consumes
RDMA resources (QPs, CQ, SRQ, buffers) and holds a live RC connection even when
no client has ever connected and no traffic exists.

## 33.2 Framing: an endpoint pair is a TCP-like socket

The cleanest way to reason about the fix is by analogy. A urp endpoint pair is
essentially **a TCP-like reliable byte stream tunnelled over RDMA**:

| TCP world | urp world |
|---|---|
| App calls `connect()` | Userland connects to the initiator's UDS socket (`/run/urp.sock`) |
| Kernel builds the TCP connection *on demand* | urp should build the RDMA QP *on demand* |
| `listen()`/`accept()` are passive | The acceptor's `rdma_listen` is passive |
| Kernel retransmits SYN until the peer answers | urp should retry the CM connect until the peer is up |

The kernel does not open a TCP connection at socket-creation time; it opens it
when an application calls `connect()`. urp violates that contract by dialing at
`urp add` time. **The fix is to restore "connect on first use"**: the RDMA
transport should come up when a userland client actually connects — not at
config time — and should retry like TCP does, rather than failing permanently on
the first rejection.

## 33.3 Goals

- **R1 — Deterministic bring-up on simultaneous boot.** No manual step.
- **R2 — Recover from runtime transients.** Peer reboot, link flap, cable pull.
- **R3 — Minimal added complexity / attack surface / config.**
- **R4 — No RDMA resources while idle.** Don't hold a QP with no traffic.
- **R5 — Never signal a false "connected".**
- **R6 — Testable** via pure predicates, matching the house TDD style
  (`kernel/urp_conn_plan.h`, `kernel/urp_credit_plan.h` + their KUnit tables).

## 33.4 Current behavior (what we can reuse)

Two facts make the fix cheaper than it looks:

1. **The UDS accept path already gates on readiness.** The initiator's accept
   kthread `urp_accept_thread_fn` (`kernel/urp_socket.c:34`, accept at `:43`)
   already **waits on `ep->cm_done`** before turning a client connection into a
   stream (`kernel/urp_socket.c:54-56`) → `urp_stream_create` →
   `urp_stream_pump_start` → SYN on the first frame (`kernel/urp_pump.c:260-265`).
   That existing wait is the natural hook to *trigger* a lazy connect.
2. **"All QPs established" is already a signalled event.** The CM ESTABLISHED
   handler does `qps_connected == num_qps → ep->connected = true;
   complete(&ep->cm_done)` (`kernel/urp_rdma.c:975-981`); each QP carries an
   `established` flag (`kernel/urp.h:169`).

What is missing is only: (a) *when* to start the connect, and (b) *retrying* it.

Note the existing PING/PONG probe (`kernel/urp_pump.c:347`, `:405-410`) is a
**post-establishment** steady-state health check, gated on `num_qps > 1` — it is
not a bring-up or reconnect mechanism. There is **no TCP anywhere** in the module
today: peer networking is RDMA-CM, local I/O is `AF_UNIX`.

## 33.5 Options

| Option | Boot race (R1) | Runtime reconnect (R2) | Complexity (R3) | Idle-clean (R4) | Lives in |
|---|---|---|---|---|---|
| **A. Kernel connect-retry + backoff** | ✅ retries until acceptor up | ✅ re-resolve on CM-down | Low, localized | ❌ retries while idle | kernel CM handler |
| **B. Lazy connect on first UDS accept** | ⚠️ mostly (userland starts post-boot) | ⚠️ needs a re-trigger | Medium (accept-path refactor) | ✅ | kernel accept path |
| **C. Userland gRPC rendezvous** | ✅ explicit mutual-up gate | ✅ if channel persistent | Medium–High (new service + proto + port) | ✅ (pairs with B) | userland control plane |
| **D. systemd `ExecStartPre` probe** | ✅ at boot | ❌ boot-only | Low (no code) | n/a | deployment (NixOS) |

**A — Kernel connect-retry + backoff.** On `REJECTED`/`*_ERROR`, re-arm
`rdma_resolve_addr` after a capped exponential backoff (e.g. 100 ms → 2–5 s)
instead of giving up. This is the standard RDMA-CM pattern and the *only* option
that makes correctness independent of orchestration and timing — it handles the
boot race and every runtime transient (peer reboot, link flap). Downside: it
keeps trying while idle (contra R4), and needs sensible backoff parameters.
Testable as a pure `backoff(attempt)` schedule + a `should_retry(cm_event)`
predicate.

**B — Lazy connect on first UDS accept.** Defer `urp_rdma_init`'s resolve/connect
until the first client connects to the UDS socket; the accept thread already
blocks on `ep->cm_done`, so it becomes the trigger (start the connect, then wait
as today). This is the idiomatic model from §33.2, and it satisfies R4 (zero RDMA
work while idle). It mostly fixes R1 because userland clients start well after
boot, by which time the acceptor is listening — but a residual race remains if
both userland sides connect instantly and simultaneously, so B still wants a
small retry (A) underneath. Runtime reconnect needs a re-trigger (next client
connect, or A).

**C — Userland gRPC rendezvous.** See §33.6.

**D — systemd `ExecStartPre` probe.** Gate `urp add` on a deployment-level
reachability probe of the peer (e.g. TCP to a control port, or the RDMA port)
before activating the endpoint. Zero code change, effective at boot — but it only
helps at boot, does nothing for runtime transients, and couples correctness to
the orchestrator.

**These compose.** The interesting combinations are **A+B** (lazy connect with a
retry safety-net) and **C+B** (a userland rendezvous confirms both peers are up,
then the app connects the UDS socket, which lazily triggers the kernel connect).

## 33.6 The userland reachability control plane (protobuf / gRPC)

Because RoCEv2 runs over IP, the two peers already have mutual L3 reachability
before any RDMA is attempted. A small **userland** handshake over that IP path is
a cheap, explicit "both peers are up" rendezvous that removes the boot race
deterministically. Rather than a one-off raw TCP connect, we define it as a
**protobuf + gRPC** service, so the single message we need now becomes an
extensible control plane later.

### 33.6.1 v1 `.proto` (proposed)

```proto
syntax = "proto3";
package urp.control.v1;

// A per-box control service colocated with the urp endpoints. The initiator
// side calls Rendezvous on the acceptor side before (or instead of) letting the
// kernel dial RDMA. Additive-only evolution; reserve numbers for future RPCs.
service UrpControl {
  // Confirms the peer is up and its endpoint is ready to accept the RC
  // connection; optionally exchanges parameters for a future negotiation.
  rpc Rendezvous(RendezvousRequest) returns (RendezvousReply);

  // Reserved for later phases (do not implement in v1):
  // rpc NegotiateParams(NegotiateRequest) returns (NegotiateReply);
  // rpc Heartbeat(stream HeartbeatPing) returns (stream HeartbeatPong);
  // rpc DrainStream(DrainRequest) returns (DrainReply);
  // rpc GetStats(StatsRequest) returns (StatsReply);
}

message RendezvousRequest {
  string endpoint_name = 1;   // e.g. "pair_initiator"
  uint32 proto_version = 2;   // control-plane version
  string local_id      = 3;   // caller identity (host / instance)
  // reserved 4 to 15;         // future: auth token, requested params
}

message RendezvousReply {
  bool   ready         = 1;   // acceptor endpoint is active + rdma_listen-ing
  string peer_id       = 2;
  uint32 num_qps       = 3;   // advertised endpoint params (future negotiation)
  uint32 buffer_size   = 4;
  // reserved 5 to 15;
}
```

**Versioning discipline:** proto3 with additive fields only; never renumber;
`reserved` blocks stake out the extension surface (auth token, negotiated
params, heartbeat, drain, stats). This keeps old and new peers interoperable as
the control plane grows.

### 33.6.2 Placement, trigger, shape

- **Placement (userland):** a small per-box service/sidecar, or logic embedded in
  the real application. Rust (`tonic`/`prost`) is the natural fit alongside the
  existing `urp-cli`, likely as a new workspace crate. The endpoint already
  carries the peer IP + port before any RDMA resolve (`kernel/urp.h:310-313`;
  `urp_endpoint_extract_v4`, `kernel/urp_endpoint.c:108-122`), so the same
  address is reusable for the control channel (a **separate control port** — only
  4791 is stored today).
- **How it triggers the kernel:** on a successful `Rendezvous`, the initiator-side
  app connects its local UDS socket, which — via **lazy-connect (B)** — makes the
  kernel dial RDMA. Clean separation of concerns: *gRPC says "the peers agree
  they're both up"; the UDS connect says "an app has traffic to send".* (An
  alternative is a new "connect" netlink verb so the control plane drives the
  kernel directly, but that duplicates what a UDS connect already means.)
- **Asymmetric vs symmetric:** only the initiator actively connects RDMA, so an
  *asymmetric* gate matches the real asymmetry — the acceptor serves `Rendezvous`
  once its urp acceptor is active and `rdma_listen`-ing; the initiator calls it.
  A mutual handshake (both sides serve + call) is a cleaner "both up" signal at
  the cost of both sides running a server.
- **One-shot vs persistent:** a one-shot call covers only bring-up; a *persistent*
  gRPC channel additionally serves as heartbeat/liveness and a reconnect trigger
  (channel drop ⇒ re-rendezvous ⇒ re-connect), folding R2 into the control plane.

### 33.6.3 Trade-offs

- A successful gRPC call is a **stronger** readiness signal than a bare TCP
  connect — it proves the peer's control service is actually live, not just that
  a port is open — and it is language-neutral (C, Rust, and the future Seastar
  client of design 31a can all speak it).
- But it is a **heavier dependency**: a gRPC stack, a new long-lived service, and
  a control port to firewall and configure.
- **Crucially, control-plane reachability is not a RoCEv2 guarantee.** RoCEv2 is
  UDP/4791 on a *lossless* fabric (PFC/ECN/DCQCN/DSCP — see design 15). The
  control channel shares L2/L3 and ARP with the RDMA path but **not** the L4
  transport or the QoS/lossless configuration, so a green rendezvous can still be
  followed by a failed QP. This is exactly why the rendezvous must sit **on top
  of** the kernel retry safety-net (A), not replace it.

## 33.7 Compounding recovery bugs (prerequisites)

Any automatic recovery — retry *or* rendezvous — still wedges unless two existing
bugs are fixed first. Both were hit during the design-32 reboot and forced the
manual, ordered restart:

1. **Stale UDS socket (initiator).** `urp drain` + `urp remove` /
   `urp_socket_cleanup` (`kernel/urp_socket.c:292`) do not unlink the listen
   socket, so re-activating the endpoint fails at bind with `-98 EADDRINUSE`
   (`kernel/urp_socket.c:186-209`). *Fix direction:* unlink on remove, and/or
   unlink-before-bind on add.
2. **Acceptor QP-slot leak.** A half-open or rejected CM leaves the acceptor's
   `1 / num_qps` slot consumed, so the next connect is refused with
   `rejecting extra CONNECT_REQUEST (1 >= 1 QPs)` until the acceptor endpoint is
   restarted. *Fix direction:* release the slot in the acceptor's reject /
   disconnect CM handler.

**Repro:** the §33.1 simultaneous-reboot sequence. These are correctness
prerequisites for R1/R2 — without them, a retry loop or a rendezvous just retries
into a wedged endpoint.

## 33.8 Recommendation

Adopt the **layered** design the TCP analogy points to, in this order:

0. **Fix the two compounding bugs (§33.7)** — prerequisites; nothing auto-recovers
   without them.
1. **Bounded kernel retry + backoff (A)** — the correctness floor. With this
   alone, bring-up no longer depends on boot ordering, and runtime transients
   self-heal. This is the piece that must exist.
2. **Lazy connect on first UDS accept (B)** — restore the "connect on first use"
   contract (§33.2): dial RDMA when a client connects, not at `urp add`. Removes
   idle RDMA usage (R4) and shrinks the race window to near-zero.
3. **Userland gRPC control plane (C)** — the `UrpControl.Rendezvous` rendezvous
   makes bring-up *deterministic* (not just eventually-consistent via retry) and
   opens the door to negotiation, liveness, and telemetry later.

Why this ordering: **A** guarantees correctness by itself and is the cheapest,
most localized change; **B** makes the behavior idiomatic and idle-clean; **C**
is the largest lift (a new service + proto + deployment surface) and is best
landed on top of a system that is already correct without it.

**Suggested phasing:** the two bug fixes + **A** + **B** as the first
implementation PR(s) (all in-kernel, TDD with pure predicates per R6), then the
gRPC control plane **C** as its own follow-up with its own crate, proto, and
deployment wiring. This document does not commit that implementation.

## 33.9 Interactions & prior art

- The post-establishment PING/PONG probe (`kernel/urp_pump.c`) could later extend
  into the liveness/reconnect signal, but today it is steady-state only and gated
  on `num_qps > 1`.
- Adding any new kernel config knob (e.g. a control port, if the port is tracked
  in-module rather than purely in userland) follows the `URP_ENDPOINT_A_MODE`
  attribute pattern end-to-end: uapi enum + netlink policy + parse + CLI marshal
  + `struct urp_endpoint` field.
- Security tiers (design 17) and RoCEv2 network config (design 15) bound what the
  control plane can assume; mTLS on the gRPC channel is a natural fit for the
  tier-2 story.

## 33.10 Open questions

- gRPC v1 RPC scope: reachability-only, or stake out `NegotiateParams` /
  `Heartbeat` / `DrainStream` in v1 as reserved-but-unimplemented (current lean).
- New standalone daemon vs a library linked into the app.
- Control-channel auth: mTLS (design 17 tier 2) vs PSK vs none in a trusted lab.
- Kernel trigger: lazy UDS connect (recommended) vs a new netlink `connect` verb.
- Rendezvous lifetime: one-shot gate vs persistent channel (folds in R2).
- Asymmetric (initiator-only) vs symmetric handshake.
- Control port as an in-module endpoint attribute vs userland/deployment-only.
- Is runtime reconnect (R2) in scope for the first implementation, or retry-only?

## 33.11 References

- Design 32 — [Real-Hardware RoCEv2 Integration Testing](32-real-hardware-integration-testing.md)
  (the bring-up where this surfaced) and its
  [implementation status](32-implementation-status.md).
- Design 05 — [RDMA Transport Layer](05-rdma-transport.md) (rdma_cm setup, QP
  config).
- Design 08a — [QP Health Probes](08a-qp-health-probes.md) (the PING/PONG
  mechanism referenced in §33.9).
- Design 15 — [Network Configuration](15-network-config.md) (RoCEv2 lossless
  fabric — why control-plane reachability ≠ RoCEv2 reachability).
- Design 17 — [Security Considerations](17-security.md) (mTLS tier for the
  control channel).
- Design 31a — [urp-fast in C++/Seastar](31a-seastar-cpp-demo.md) (a future
  consumer of the control plane).
- Code anchors: `kernel/urp_endpoint.c:217,273,277`; `kernel/urp_rdma.c:975-1016,
  1063-1097`; `kernel/urp_socket.c:34,54-56,186-209,292`; `kernel/urp_pump.c:260-265`.
