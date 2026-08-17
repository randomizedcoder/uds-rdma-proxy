# Design 33 Implementation Tracker — Initiator Connection Bring-up & Recovery

Progress tracker for [33. Initiator Connection Bring-up & Recovery](33-initiator-connection-bringup.md).

**Last updated**: 2026-08-17 (Phase 0 bug fixes implemented + build-verified across
the 6.1/6.6/6.12/7.1 kernel matrix; Phases 1–3 not started.)

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [Compounding recovery bugs](#phase-0-compounding-recovery-bugs) | In progress — code done, hw verify pending | 3/4 |
| 1 | [Bounded kernel connect-retry + backoff](#phase-1-bounded-kernel-connect-retry--backoff) | Not started | 0/4 |
| 2 | [Lazy connect on first UDS accept](#phase-2-lazy-connect-on-first-uds-accept) | Not started | 0/4 |
| 3 | [Userland gRPC control plane](#phase-3-userland-grpc-control-plane) | Not started | 0/5 |

Legend: **Not started** / **In progress** / **Done** / **Blocked**.

Layering (design [§33.8](33-initiator-connection-bringup.md)): fix the two
compounding bugs first (they gate any auto-recovery), then the bounded kernel
retry (correctness floor), then lazy-connect (idiomatic "connect on first use"),
then the userland gRPC rendezvous. PR structure: Phase 0 ships as its own fast PR;
Phases 1–3 follow.

---

## Phase 0: Compounding recovery bugs

**Status**: In progress — both fixes implemented and build-verified across the
kernel matrix; the hardware reboot-recovery verification on hp1/hp3 is pending.

The design-32 simultaneous-reboot race left the session permanently down until a
*coordinated* manual restart, because two secondary bugs block automatic
recovery: the acceptor leaks its QP slot on a half-open child, and the
initiator's UDS listen socket is never unlinked. Both are prerequisites for any
retry/rendezvous scheme (design [§33.7](33-initiator-connection-bringup.md)).

### Definition of done

- [x] **Bug 1 — acceptor QP-slot leak.** Release `ep->qps_accepted` on ANY CM
      teardown of an accepted child, not only the `established` branch. New per-QP
      `accept_slot_held` flag (`kernel/urp.h`), set true on successful
      `rdma_accept` (`kernel/urp_rdma.c`), released via the pure predicate
      `urp_acceptor_should_release_slot(is_initiator, slot_held)`
      (`kernel/urp_conn_plan.h`), cleared to prevent a double-decrement.
- [x] **Bug 2 — stale UDS socket.** Unlink the initiator's pathname listen socket
      before bind (`urp_listen_uds`) and on cleanup (`urp_socket_cleanup`), gated
      by the pure predicate `urp_should_unlink_listen_path(is_initiator,
      listen_path_set)`. Action `urp_unlink_stale_socket()` uses only
      matrix-portable exported symbols (`kern_path` + `dget_parent` +
      `vfs_unlink`, idmap arg version-gated), refuses to remove a non-socket.
- [x] **TDD.** KUnit truth-table tests `test_acceptor_should_release_slot` +
      `test_should_unlink_listen_path` in `kernel/urp_test.c`; standalone
      RED→GREEN predicate harness green against the shipped `urp_conn_plan.h`.
- [ ] **Hardware verify** on hp1/hp3: the design-32 reboot sequence self-heals —
      initiator re-`urp add` succeeds without `rm /run/urp.sock`; the acceptor
      accepts a fresh session after a dropped one without a restart.

### Verification

- [x] `urp.ko` builds clean across **6.1.180 / 6.6.148 / 6.12.101 / 7.1.6**
      (`nix build .#checks…urp-ko-{6_1,6_6,6_12} .#checks…kernel-module-build`).
- [x] `nix run .#ci-local` = GREEN (9/9 builds + 4/4 fuzz-smoke).
- [ ] hp1/hp3 reboot-recovery (see the pending DoD item above).

### Notes

- The `vfs_unlink()` idmap argument switched `struct user_namespace *` →
  `struct mnt_idmap *` in v6.3; gated on `LINUX_VERSION_CODE` like the existing
  `>=6.8` fast path. `kern_path_locked()` (not module-exported on 6.1) and
  `lookup_one_len()` (removed by 7.x) were both avoided in favour of
  `kern_path` + `dget_parent`, which are stable and exported across the matrix.
- The unlink decision is pure/unit-tested; the `vfs_unlink` action is filesystem
  I/O and is covered by the hardware drain+re-add integration test, not KUnit.

---

## Phase 1: Bounded kernel connect-retry + backoff

**Status**: Not started.

On a failed resolve/connect the initiator re-arms with capped exponential
backoff instead of giving up — the correctness floor that makes bring-up
independent of orchestration (design R1/R2).

### Definition of done

- [ ] Per-QP retry state (`connect_attempts`, `struct delayed_work
      connect_retry_work`) on `struct urp_qp`; init in `urp_qps_init`, cancel in
      `urp_qps_destroy`.
- [ ] `urp_connect_retry_work_fn` re-arms a fresh cm_id (mirror `urp_make_cm_id`)
      + `rdma_resolve_addr` from a work context (CM `qp_mutex` constraint), reusing
      the guarded shared PD/CQ/pool.
- [ ] CM error handler schedules the retry (initiator only) with
      `urp_connect_backoff_ms(attempt)` and does **not** `complete(&ep->cm_done)`
      until retries are exhausted; `connect_attempts` reset on ESTABLISHED.
- [ ] Pure predicates `urp_connect_should_retry` + `urp_connect_backoff_ms` in
      `kernel/urp_retry_plan.h`, KUnit table in `urp_test.c`.

### Verification

- [ ] KUnit + standalone harness green; `nix run .#ci-local` GREEN.
- [ ] hp1/hp3 boot race self-heals — initiator retries until the acceptor is
      listening → `all 1 QPs established`, no coordinated manual restart.

### Notes

- Mirror the existing async idioms: `connect_work` (`work_struct`, deferred off
  the CM handler) for the mutex constraint, `probe_work` (`delayed_work`) for the
  backoff timing.

---

## Phase 2: Lazy connect on first UDS accept

**Status**: Not started.

Dial RDMA on the first UDS accept, not at `urp add` — an idle endpoint holds no
RDMA resources, and bring-up mirrors `connect()`-on-first-use (the TCP-socket
analogy, design R4).

### Definition of done

- [ ] Split `urp_rdma_init` so the initiator connect loop does not run at
      `urp_endpoint_activate`; acceptor listen + shared setup unchanged.
- [ ] Trigger the connect from `urp_accept_thread_fn` on first accept, guarded by
      a one-shot `atomic_t connect_started` and the pure predicate
      `urp_should_start_lazy_connect(is_initiator, already_started)`.
- [ ] Teardown hazard handled: the accept thread parked in
      `wait_for_completion_interruptible(&ep->cm_done)` is woken on drain/remove
      (verify `kthread_stop` returns the wait; else add a `cm_abort` completion).
- [ ] Status doc + code note the deferred shared-resource (PD/CQ) allocation to
      first accept.

### Verification

- [ ] Idle initiator shows no QP/CM/PD until first UDS client (`urp show`); first
      client triggers connect → `established` → BENCH_OK.
- [ ] Clean `drain`+`remove` of a never-connected endpoint leaves no stuck kthread.

### Notes

- Composes with Phase 1: the first accept can still race the peer's listen, so the
  bounded retry remains the safety-net.

---

## Phase 3: Userland gRPC control plane

**Status**: Not started.

A small userland rendezvous over IP (RoCEv2 already implies L3 reachability) that,
on success, triggers the initiator's UDS connect → (via Phase 2) the kernel dials
RDMA. Committed to protobuf + gRPC as an extensible control plane
(design [§33.6](33-initiator-connection-bringup.md)).

### Definition of done

- [ ] New Rust crate `urp-control/` (tonic/prost, matching `urp-cli`), wired into
      the workspace `Cargo.toml` + nix `packages` (mindful of the
      `microvms.packages`/`redpandaUdsTest` follows caveat).
- [ ] `proto/urp_control/v1/control.proto`: `UrpControl.Rendezvous`
      (RendezvousRequest/Reply) with reserved numbers for
      `NegotiateParams`/`Heartbeat`/`DrainStream`/`GetStats`; additive-only
      versioning discipline documented.
- [ ] Asymmetric topology: acceptor serves the RPC once `rdma_listen`-ing;
      initiator calls it, and on `peer_ready` connects its local UDS → triggers
      Phase-2 lazy connect. Control address from the endpoint's peer IP:port.
- [ ] NixOS: optional `services.urp.control = { enable; port; }` in
      `nix/nixos-module.nix`; acceptor server unit ordered before `urp add`;
      initiator `urp add` gated behind a client-side `Rendezvous`.
- [ ] Tests: Rust unit tests (proto round-trip, peer-ready gating,
      retry-until-ready); a VM/hw integration proving cold simultaneous boot →
      rendezvous → lazy connect → `established` → BENCH_OK, zero manual steps.

### Verification

- [ ] `cargo test` for `urp-control` green; `nix run .#ci-local` GREEN.
- [ ] Cold simultaneous boot on hp1/hp3 reaches BENCH_OK with no manual steps.

### Notes

- One-shot for v1 (bring-up only), structured so a persistent
  heartbeat/liveness/reconnect mode is an additive follow-up. gRPC-OK is a hint,
  not a RoCEv2 guarantee (the control channel shares L2/L3/ARP but not L4/QoS/PFC),
  so it is always paired with the Phase-1 retry safety-net.

### Results

_(populated once the control plane is integrated and measured)_

| Scenario | Result | Notes |
|----------|--------|-------|
| cold simultaneous boot → BENCH_OK | _pending_ | |
