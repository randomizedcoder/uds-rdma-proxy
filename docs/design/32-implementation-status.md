# Design 32 Implementation Tracker — Real-Hardware RoCEv2 Integration

Progress tracker for [32. Real-Hardware RoCEv2 Integration Testing](32-real-hardware-integration-testing.md).

**Last updated**: 2026-08-16 (docs written; awaiting review before any nix/kernel
code — Phases 0–3 all not-started.)

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [urp.ko builds on the hp net-next kernel](#phase-0-urpko-builds-on-the-hp-kernel) | Not started | 0/2 |
| 1 | [Docs (this pass)](#phase-1-docs) | In progress | 2/3 |
| 1b | [nixosModule + hw-matrix runner](#phase-1b-nixosmodule--runner) | Not started | 0/2 |
| 2 | [Pin, rebuild both boxes, first light](#phase-2-pin-rebuild-first-light) | Not started | 0/4 |
| 3 | [Run the matrix, capture results](#phase-3-run-the-matrix) | Not started | 0/3 |

Legend: **Not started** / **In progress** / **Done** / **Blocked**.

---

## Phase 0: urp.ko builds on the hp kernel

**Status**: Not started — **blocking gate** for everything below.

hp1/hp3 run a custom net-next kernel newer than the flake's "latest" pin. Nothing
can load a module that will not build.

### Definition of done

- [ ] `buildUrpKo` against the hp net-next kernelPackages produces `urp.ko`
      (built locally on `l` via an impure expr if the net-next checkout is
      present, or on the hp box after the first `make sync`).
- [ ] Any net-next API breakage fixed behind the existing `LINUX_VERSION_CODE`
      gate pattern, with the 6.1 / 6.6 / 6.12 / latest matrix still green
      (`nix run .#ci-local` = `LOCAL_CI_RESULT=GREEN`). Land as a small
      uds-rdma-proxy PR **only if** changes are needed.

### Notes

_(none yet)_

---

## Phase 1: Docs

**Status**: In progress.

### Definition of done

- [x] `docs/design/32-real-hardware-integration-testing.md` written (design/spec).
- [x] `docs/design/32-implementation-status.md` written (this tracker).
- [ ] Row added to `docs/DESIGN.md`; **user review checkpoint** — pause for the
      user to read both docs and revise per feedback **before** any nix/kernel
      code.

### Notes

Design doc covers: motivation (off emulated rxe), topology/addressing, RoCEv2
bring-up, the `nixosModules.urp` contract, the deployment workflow, the client
matrix (C/Rust real; Seastar future), the runner, oracles, risks, references, and
an empty Results section. Scope decisions locked with the user: reusable
nixosModule in the flake; defer Seastar; ssh-driven runner in the repo.

---

## Phase 1b: nixosModule + runner

**Status**: Not started. (PR A1 — after the review checkpoint.)

### Definition of done

- [ ] `nix/nixos-module.nix` → top-level `nixosModules.urp` output. Options
      `services.urp.{enable,rdmaKernelModules,endpoints}`; config sets
      `boot.extraModulePackages`/`boot.kernelModules`, adds `urp-cli` +
      `rdma-core` (+ `perftest`, `mstflint`), and one systemd oneshot per
      endpoint (`ExecStart` = `urp add`, `ExecStop` = `urp drain && urp remove`,
      `RemainAfterExit`). Mirrors `nix/microvms/{mkVm.nix,lib.nix}`.
- [ ] `nix/urp-hw-matrix.nix` → package `urp-hw-matrix` (writeShellApplication,
      modeled on `urp-bench-matrix.nix`). Args `<acceptor> <initiator>
      <acceptor-ip> [cells]`; per-cell ssh listener/connector + `BENCH_OK`
      assert (≤3 attempts, `BENCH_SKIP`→skip); 4-way delta + interop table.
      Reports **RTT** p50/p99 always, and **one-way** latency gated on a healthy
      `pmc` PTP offset check (§32.8) — never a one-way number from an unsynced
      clock. Wired into `flake.nix` `let` + `packages`. **Not** added to
      `ci-local` (needs real hardware — documented like the microVM tiers).

### Verification

- [ ] `nix build .#urp-hw-matrix` succeeds.
- [ ] Module evaluates (throwaway `nixosConfigurations` or `nixos-rebuild build`
      on a box); `nix flake check` still green.

### Notes

_(none yet)_

---

## Phase 2: Pin, rebuild, first light

**Status**: Not started. (In the `~/nixos/hp` repo, not this one.)

### Definition of done

- [ ] Both `hp1/flake.nix` and `hp3/flake.nix`: add
      `inputs.uds-rdma-proxy.url = "github:randomizedcoder/uds-rdma-proxy/main"`
      (or a tag) with `inputs.nixpkgs.follows = "nixpkgs"`; add to the outputs
      argset + `modules` (`uds-rdma-proxy.nixosModules.urp`).
- [ ] `services.urp` config declared: **hp1 acceptor** `pair_acceptor
      --connect-path /run/urp-echo.sock --bind 10.10.2.1:4791`; **hp3 initiator**
      `pair_initiator --listen-path /run/urp.sock --peer 10.10.2.1:4791`.
      Add `rdma-core perftest mstflint` to `hp{1,3}/systemPackages.nix`.
- [ ] **PTP time sync** (lab infra, *not* in `nixosModules.urp`): a `ptp.nix`
      imported by hp1/hp3 — `linuxptp` package, a `ptp4l -H` unit on link B
      (`enp1s0f1np1`, hp1 grandmaster / hp3 slave), and a `phc2sys` unit steering
      the system clock from the PHC. Confirm the NIC does HW timestamping
      (`ethtool -T enp1s0f1np1`). See §32.8.
- [ ] `git add`; `make update` + `make sync` per host; ssh `hpN` → `make`
      (reboot via `bootstrap` if needed). hp1 and hp3 have separate locks.
- [ ] **First light**: `lsmod | grep urp`; `ibv_devices` + `show_gids mlx5_0`
      (RoCEv2 v2 GID on `10.10.2.x`); `urp show` (both endpoints, session
      established); optional `ib_send_bw 10.10.2.1` raw-verbs baseline.

### Verification

- [ ] `nixos-rebuild switch` succeeds on **both** boxes.
- [ ] `urp show` reports an established session; `show_gids` lists a RoCEv2 GID.
- [ ] `pmc -u -b 0 'GET CURRENT_DATA_SET'` on hp3 shows `offsetFromMaster` in the
      sub-µs / low-hundreds-of-ns range (PTP healthy → one-way latency valid).

### Notes

_(none yet)_

---

## Phase 3: Run the matrix

**Status**: Not started. (PR A2 — results back into this repo.)

### Definition of done

- [ ] From `l`: `nix run .#urp-hw-matrix -- hp1 hp3 10.10.2.1` — all four C/Rust
      cells `BENCH_OK verify=full`, populated delta table. C↔Rust + Rust↔C are
      the live framing differential.
- [ ] Extra phases: `urp-test-client 10.10.2.1 <port> {echo,throughput,latency,
      reorder,bigframe}` against a dedicated acceptor; `urp-fast-poc /dev/urp
      pair_acceptor` → `URP_FAST_POC_OK`.
- [ ] Fill the design-32 **Results** section with real mbps/p99/msgs_per_s;
      drop the "emulated — not perf" caveat; flip design 32 Status →
      *implemented*; refresh `status.md` (hardware-validation pass done). No
      `dmesg` splats.

### Results

_(pending — mirrors §32.12 of the design doc)_

| cell | mode | msg_size | mbps | msgs/s | RTT p50 | RTT p99 | 1-way p50 | verify | status |
|------|------|----------|------|--------|---------|---------|-----------|--------|--------|
| _(pending Phase 3)_ | | | | | | | | | |

### Notes

_(none yet)_
