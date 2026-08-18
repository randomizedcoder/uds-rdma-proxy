# Design 33 Implementation Tracker — Initiator Connection Bring-up & Recovery

Progress tracker for [33. Initiator Connection Bring-up & Recovery](33-initiator-connection-bringup.md).

**Last updated**: 2026-08-17 (Phase 0 done + hardware-verified. **Phase 1 DONE +
hardware-verified** — bounded initiator connect-retry with capped exponential
backoff, reconnect-on-drop, runtime `/proc/sys/urp/` tunables, **plus Phase 1.5
probe→retry for silent drops** (hard peer reboot with no CM event); predicates
unit-tested, CI green, hp1/hp3 all four scenarios verified incl. the SysRq-b hard
drop self-heal. Phases 2–3 not started.)

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [Compounding recovery bugs](#phase-0-compounding-recovery-bugs) | Done — code + kernel matrix + hp1/hp3 hw verified | 4/4 |
| 1 | [Bounded kernel connect-retry + backoff](#phase-1-bounded-kernel-connect-retry--backoff) | Done — code + CI + hp1/hp3 hw verified (incl. Phase 1.5 probe→retry for silent drops) | 6/6 |
| 2 | [Lazy connect on first UDS accept](#phase-2-lazy-connect-on-first-uds-accept) | Done — code + CI + hp1/hp3 hw verified (128/128 matrix + 5 lazy scenarios) | 5/5 |
| 3 | [Userland gRPC control plane](#phase-3-userland-grpc-control-plane) | Not started | 0/5 |

Legend: **Not started** / **In progress** / **Done** / **Blocked**.

Layering (design [§33.8](33-initiator-connection-bringup.md)): fix the two
compounding bugs first (they gate any auto-recovery), then the bounded kernel
retry (correctness floor), then lazy-connect (idiomatic "connect on first use"),
then the userland gRPC rendezvous. PR structure: Phase 0 ships as its own fast PR;
Phases 1–3 follow.

---

## Phase 0: Compounding recovery bugs

**Status**: Done — both fixes implemented, build-verified across the kernel
matrix, and hardware-verified on hp1/hp3 (recovery self-heals with a plain
initiator restart; no manual `rm /run/urp.sock`, no acceptor restart).

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
- [x] **Hardware verify** on hp1/hp3: the design-32 reboot sequence self-heals —
      initiator re-`urp add`/restart succeeds without `rm /run/urp.sock`; the
      acceptor accepts a fresh session after a dropped one without a restart.

### Verification

- [x] `urp.ko` builds clean across **6.1.180 / 6.6.148 / 6.12.101 / 7.1.6**
      (`nix build .#checks…urp-ko-{6_1,6_6,6_12} .#checks…kernel-module-build`).
- [x] `nix run .#ci-local` = GREEN (9/9 builds + 4/4 fuzz-smoke).
- [x] hp1/hp3 reboot-recovery (deployed `5bfcd37`, both boxes on 7.1.8). A
      simultaneous reboot reproduced the boot race (hp3 initiator connects at
      ~22.7 s → `rejected` before hp1 listens at ~23.1 s). Recovery is then
      unassisted: a single `systemctl restart urp-endpoint-pair_initiator` on hp3
      logs `removed stale socket /run/urp.sock` (Bug 2 — was `-98 EADDRINUSE`),
      re-binds, and reaches `all 1 QPs established`; hp1 shows `CM event:
      established` with **no** `rejecting extra CONNECT_REQUEST` (Bug 1). Held
      across 3 establish→disconnect→re-establish cycles with no acceptor slot
      leak. (The boot race itself still needs a restart; Phase 1 retry removes
      that.)

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

**Status**: In progress — code complete, CI + hp1/hp3 hardware verification
pending.

On a failed resolve/connect the initiator re-arms with capped exponential
backoff instead of giving up — the correctness floor that makes bring-up
independent of orchestration (design R1/R2). Scope (locked with the user)
**also covers reconnect-on-drop**: a mid-session DISCONNECT of an established
session (e.g. the acceptor reboots) re-dials on the same bounded budget, not
just the first bring-up. The retry window and aggressiveness are **runtime
tunable** via `/proc/sys/urp/` so an operator can pick their stance without a
reload.

### Definition of done

- [x] Per-QP retry state (`connect_attempts`, `struct delayed_work
      connect_retry_work`) on `struct urp_qp`; `INIT_DELAYED_WORK` +
      `connect_attempts = 0` in `urp_qps_init`, `cancel_delayed_work_sync` in
      `urp_qps_destroy`.
- [x] `urp_connect_retry_work_fn` (`kernel/urp_rdma.c`) tears down the failed
      QP/cm_id via the shared `urp_qp_hard_teardown` helper and re-dials a fresh
      `urp_make_cm_id` + `rdma_resolve_addr` from a work context (CM `qp_mutex`
      constraint), reusing the guarded shared PD/CQ/pool and rebuilding the target
      from `ep->peer_addr` via `urp_endpoint_extract_v4`.
- [x] CM error handler (initiator only) captures `was_established`, re-arms the
      accept-thread waiter (`ep->connected = false` + `reinit_completion`) on a
      live drop, schedules the retry with `urp_connect_backoff_ms(...)`, and does
      **not** `complete(&ep->cm_done)` until the budget is exhausted;
      `connect_attempts` reset on ESTABLISHED.
- [x] Pure predicates `urp_should_retry_connect` + `urp_connect_backoff_ms` in
      `kernel/urp_retry_plan.h`; KUnit truth-tables (`test_should_retry_connect`,
      `test_connect_backoff_ms`) in `urp_test.c` + standalone harness (green).
- [x] Runtime tunables (`connect_max_attempts` / `connect_backoff_base_ms` /
      `connect_backoff_ceil_ms`) under `/proc/sys/urp/` via `kernel/urp_sysctl.c`
      (`register_sysctl`, `proc_douintvec_minmax` bounds); `#define` defaults in
      `urp.h`, live globals read by the CM path. `max_attempts=0` disables retry.
- [x] Teardown-race safety: `cancel_delayed_work_sync` at the top of
      `urp_rdma_cleanup` (before cm_id destruction) so a retry cannot race drain;
      exercised on hardware by repeated endpoint restarts during active retries.

### Verification

- [x] KUnit + standalone predicate harness green.
- [x] `nix run .#ci-local` GREEN (9/9 builds incl. 6.1→7.x + fuzz-smoke 4/4);
      sysctl compiles cleanly across the matrix (ctl_table sentinel version-gated
      at 6.11).
- [x] hp1/hp3 **boot race** self-heals — hp3 (initiator) hit the race and logged
      `initiator retry 14/15/16 … in 2000 ms` (`rejected` while hp1 wasn't
      listening) then `established` on retry 16, **no manual restart**. (Phase 0
      needed a restart here.)
- [x] hp1/hp3 **reconnect-on-drop, graceful** — a CM-delivered disconnect
      (acceptor endpoint restart / drain) fires `CM down (disconnected);
      initiator retry 1/300 in 100 ms` → re-dial → `established`, automatic. The
      `retry 1/300` (not 16) confirms `connect_attempts` resets on ESTABLISHED.
- [x] hp1/hp3 **reconnect-on-drop, hard reboot** — a hard acceptor reboot is a
      **silent drop** (no CM event delivered to the initiator). Closed by the
      Phase 1.5 probe→retry wiring below and **hardware-verified** (SysRq-b hard
      drop → hp3 self-heals via missed-probe detection; see the Phase 1.5 section).
- [x] hp1/hp3 **runtime tuning** — `sysctl -w urp.connect_backoff_base_ms=500`
      honoured on the next retry (`… in 500 ms`); `connect_max_attempts=0` takes
      the terminal path with **no** retry (`QP 0 CM down: disconnected`), then
      restored.

### Phase 1.5: probe-triggered retry for silent drops (code-complete)

A hard peer reboot/crash drops the RC connection without an RDMA-CM event, so the
CM-driven retry above never fires. The probe machinery (design 08a, missed-PONG
demotion) is the detector; on a demoted QP the initiator now schedules the same
`connect_retry_work` reconnect path, closing the silent-drop gap.

**Root cause of the hp1-reboot miss (now fixed):** probes were gated on
`num_qps > 1`, so the single-QP real-hardware config **never probed at all** —
there were no missed PONGs to demote the stale QP. The gate is now
`urp_should_emit_probes(is_initiator, num_qps)` = `num_qps > 1 || is_initiator`,
so a single-QP **initiator** emits liveness PINGs (a single-QP acceptor stays
quiet — it never retries, and still answers PINGs with PONGs in the recv path,
keeping the data-path baseline unchanged).

- [x] `urp_should_emit_probes` + `urp_silent_drop_should_reconnect` pure
      predicates in `kernel/urp_retry_plan.h`; KUnit truth-tables
      (`test_should_emit_probes`, `test_silent_drop_should_reconnect`) +
      standalone harness (green).
- [x] `urp_probe_work_fn` gate switched to `urp_should_emit_probes(...)` so the
      single-QP initiator probes for liveness (`kernel/urp_pump.c`).
- [x] `urp_emit_ping_on`: on `>= URP_QP_MISS_THRESHOLD` misses, an initiator QP
      that was established calls `urp_connect_retry_on_silent_drop` and stops
      pinging (the QP is being torn down).
- [x] `urp_connect_retry_on_silent_drop` (`kernel/urp_rdma.c`) mirrors the CM
      error handler's `was_established` bookkeeping (demote, re-arm the
      accept-thread waiter) then schedules `connect_retry_work` with the same
      capped-exponential backoff / budget — or takes the terminal `-ETIMEDOUT`
      path when the budget is spent. Runs in probe-work context; the actual
      cm_id teardown + re-dial stays in `connect_retry_work`.
- [x] ESTABLISHED resets the probe liveness counters
      (`last_ping_ns`/`consecutive_misses`/`consecutive_pongs`) so a freshly
      re-established QP can't re-trip the silent-drop path on its next tick.
- [x] `nix run .#ci-local` GREEN (9/9 builds + fuzz-smoke).
- [x] hp1/hp3 hardware re-test **PASS** (both 7.1.8, module `urp-ko` carrying
      `urp_connect_retry_on_silent_drop`): with a live established session, hp1
      (acceptor) was hard-dropped via SysRq-b (no urp drain, no `rdma_disconnect`,
      so **no CM event** reaches hp3 — a true silent drop). hp3 (initiator)
      self-healed with **no** manual restart:
      `QP 0 demoted to DRAINING after 3 misses` →
      `QP 0 silent drop (3 missed probes); initiator retry 1/300 in 100 ms` →
      fresh cm_id re-dials, now gets CM `rejected` while hp1 boots (retries 1→7,
      backoff climbing to the 2000 ms ceil — the probe path bridged silent →
      CM-visible, then the Phase 1 CM retry loop took over) → on hp1's return
      `all 1 QPs established`, both sides `connected: yes`. `retry 1/300`
      confirms `connect_attempts` reset on the prior ESTABLISHED.

**Convergence note:** the probe path only kickstarts the *first* teardown +
re-dial on a silent drop. If the acceptor is still down, that re-dial's fresh
cm_id *does* produce CM error events (CONNECT_ERROR / UNREACHABLE), so the normal
Phase 1 CM-driven retry loop takes over from there — the probe path is the
one-shot bridge from "silent" to "CM-visible".

### Notes

- Mirrors the existing async idioms: `connect_work` (`work_struct`, deferred off
  the CM handler) for the `qp_mutex` constraint, `probe_work` (`delayed_work`) for
  the backoff timing.
- The retry re-dial destroys and rebuilds the QP's cm_id, which is illegal from
  inside the cm_id's own event handler — hence the deferred work item, and the
  `urp_qp_hard_teardown` helper is shared with the acceptor's slot-reuse path.
- sysctl portability: `register_sysctl("urp", …)` has a stable signature 6.1→7.x;
  only the `ctl_table` terminating sentinel is version-gated (required <6.11,
  removed 6.11+).

---

## Phase 2: Lazy connect on first UDS accept

**Status**: Done — code complete + `nix run .#ci-local` GREEN + **hp1/hp3
hardware-verified** (all 6 scenarios below, real RoCEv2, both kernel 7.1.8).

Dial RDMA on the first UDS accept, not at `urp add` — an idle endpoint holds no
RDMA resources, and bring-up mirrors `connect()`-on-first-use (the TCP-socket
analogy, design R4).

### Definition of done

- [x] Split `urp_rdma_init` so the initiator connect loop does not run at
      `urp_endpoint_activate`; acceptor listen + shared setup unchanged. The
      initiator branch is now a `pr_info` + `return 0` (`urp_rdma.c`); the dial
      core was factored into `urp_qp_resolve_addr` (shared with the Phase-1 retry
      work) and driven by the new `urp_lazy_connect_start`.
- [x] Trigger the connect from `urp_accept_thread_fn` on first accept, guarded by
      a one-shot `atomic_t connect_started` (flipped via `atomic_cmpxchg`), a
      `state == URP_STATE_ACTIVE` gate, and the pure predicate
      `urp_should_start_lazy_connect(is_initiator, already_started)`
      (`kernel/urp_lazy_plan.h`, KUnit `test_should_start_lazy_connect`).
- [x] Teardown hazard handled: `urp_socket_cleanup` now `complete(&ep->cm_done)`
      **before** `kthread_stop`, and the accept thread breaks its loop on
      `state != URP_STATE_ACTIVE` after the wait (releasing the held `new_sock`
      first). This also closes a latent pre-existing hang in the eager path.
- [x] Fail-fast after retry exhaustion (chosen semantics: "stay dead until
      re-add"): a `bool connect_failed` flag, set on all three initiator terminal
      paths (CM-error exhaustion, retry-work rearm-else, silent-drop exhaustion),
      makes a late client reject fast instead of parking on a consumed `cm_done`.
- [x] Status doc + code note the deferred shared-resource (PD/CQ) allocation to
      first accept — inherited for free: `urp_endpoint_setup_shared` is already
      one-shot and only runs from the CM handler, so deferring the dial defers all
      RDMA-object allocation.

### Verification

hp1 (acceptor) ↔ hp3 (initiator), ConnectX-4 Lx 25GbE RoCEv2, both kernel 7.1.8,
module `urp_lazy_connect_start` symbol confirmed in the loaded ko before test.

- [x] **Idle = no RDMA** — hp3 boots to `initiator deferring RDMA dial to first
      UDS accept` + `listening on UDS`, `connected: no`, no resolve/route/
      established for 60 s until a client connects; hp1 listening.
- [x] **First client triggers connect** — the matrix's first bench client at
      t≈84.6 s fires `first client connect -> dialing RDMA` → resolved → route →
      `all 1 QPs established`; **128/128 BENCH_OK verify=full** (full C×C×Rust
      matrix, matching the design-32 baseline through the lazy path).
- [x] **Clean drain of a never-connected endpoint** — fresh initiator (deferring,
      1 parked `urp-accept` kthread), `systemctl stop` returns immediately (no 45 s
      hang), 0 kthreads after, `removed stale socket`, no leaked cm_id.
- [x] **Boot-race safety-net** — client triggers the one-shot dial while the
      acceptor is down → Phase-1 retry cycles (6→12, backoff to 2000 ms ceil, client
      already gone) → acceptor up mid-retry → auto-`established` at retry 12 with
      **no hp3 restart**.
- [x] **Fail-fast after exhaustion** — with `connect_max_attempts=3` and the
      acceptor down, client1 exhausts (retry 1→3, terminal `CM down: rejected`) and
      is released; client2 is **rejected in 4 ms** (`RDMA connect terminally
      failed (8); rejecting client`), no re-dial; stays dead even after the acceptor
      returns; recovers only after a service restart (= `urp remove`/`add`).

### Notes

- Composes with Phase 1: the first accept can still race the peer's listen, so the
  bounded retry remains the safety-net (`urp_lazy_connect_start` hands a QP whose
  inline dial fails straight to `connect_retry_work`).

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
