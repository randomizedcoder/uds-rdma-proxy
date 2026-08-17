# Design 32 Implementation Tracker — Real-Hardware RoCEv2 Integration

Progress tracker for [32. Real-Hardware RoCEv2 Integration Testing](32-real-hardware-integration-testing.md).

**Last updated**: 2026-08-16 (docs written; awaiting review before any nix/kernel
code — Phases 0–3 all not-started.)

---

## Overview

| # | Phase | Status | Completion |
|---|-------|--------|------------|
| 0 | [urp.ko builds on the hp net-next kernel](#phase-0-urpko-builds-on-the-hp-kernel) | Done (no PR K) | 3/3 |
| 1 | [Docs (this pass)](#phase-1-docs) | Done | 3/3 |
| 1b | [nixosModule + hw-matrix runner](#phase-1b-nixosmodule--runner) | Done | 2/2 |
| 2 | [Pin, rebuild both boxes, first light](#phase-2-pin-rebuild-first-light) | Done — QP established, data path stalls (bug) | 5/5 |
| 3 | [Run the matrix, capture results](#phase-3-run-the-matrix) | Matrix done — 128/128 BENCH_OK; extra probes pending | 1/3 |

Legend: **Not started** / **In progress** / **Done** / **Blocked**.

---

## Phase 0: urp.ko builds on the hp kernel

**Status**: Done — hp1 + hp3 both GREEN, no source changes (**no PR K**).

hp1/hp3 run a custom net-next kernel newer than the flake's "latest" pin. Nothing
can load a module that will not build.

### Definition of done

- [x] `buildUrpKo` against **hp1** net-next 7.2-rc1 (series4-rfc-tail-v2)
      produces `urp.ko` — built on `l` via an impure expr against
      `hp1/netnext-kernel.nix`. `vermagic: 7.2.0-rc1 SMP preempt mod_unload`,
      205 KB, **zero warnings/errors** in the urp compile. No source change.
- [x] Same against **hp3** net-next 7.2-rc1 (series5-a) — `urp.ko` byte-size
      identical (205480 B), `vermagic: 7.2.0-rc1`, zero warnings. Confirms the
      series4/series5 branch difference (flow_dissector only) is irrelevant to
      urp.
- [x] No net-next API breakage → **no PR K needed**; the `LINUX_VERSION_CODE`
      gates already cover 7.2-rc1 (≥ 6.8 fast path incl. the `urp_sqe_cmd` shim).
      `nix run .#ci-local` = **`LOCAL_CI_RESULT=GREEN`** (9/9 builds + 4/4
      fuzz-smoke) — PR A regresses nothing.

### Notes

- Building urp.ko on `l` required a full `linux-7.2-rc1` source build first (the
  net-next tree is fetched from the local `file:///home/das/Downloads/net-next`
  checkout; not cached on `l`). On the hp boxes the kernel is already built, so
  the module build during `nixos-rebuild` is fast.
- The clean hp1 build is strong evidence the module will load on the boxes; the
  final proof is `insmod`/session bring-up in Phase 2 first-light.

---

## Phase 1: Docs

**Status**: In progress.

### Definition of done

- [x] `docs/design/32-real-hardware-integration-testing.md` written (design/spec).
- [x] `docs/design/32-implementation-status.md` written (this tracker).
- [x] Row added to `docs/DESIGN.md`; **user review checkpoint** passed (docs
      reviewed + PTP one-way latency folded in), then committed (`5ec051a`).

### Notes

Design doc covers: motivation (off emulated rxe), topology/addressing, RoCEv2
bring-up, the `nixosModules.urp` contract, the deployment workflow, the client
matrix (C/Rust real; Seastar future), the runner, oracles, risks, references, and
an empty Results section. Scope decisions locked with the user: reusable
nixosModule in the flake; defer Seastar; ssh-driven runner in the repo.

---

## Phase 1b: nixosModule + runner

**Status**: Done (PR A, commit pending).

### Definition of done

- [x] `nix/nixos-module.nix` → `nixosModules.urp` (+ `.default`), merged OUTSIDE
      `flake-utils.eachDefaultSystem` via `// { nixosModules.urp = …; }`,
      resolving `self.lib.<sys>.buildUrpKo config.boot.kernelPackages` +
      `self.packages.<sys>.urp-cli`. Options
      `services.urp.{enable,rdmaKernelModules,endpoints,extraPackages}`;
      `endpoints` is an `attrsOf submodule` ({role, connectPath/listenPath,
      bind/peer, numQps, bufferCount, bufferSize, passwordFile, rdmaDevice}) with
      acceptor/initiator assertions. Config sets `boot.extraModulePackages` +
      `boot.kernelModules`, adds `urp-cli` + `rdma-core`, and one systemd oneshot
      per endpoint (`ExecStart` = `urp add …` reading `passwordFile` inline for
      `--password`; `preStop` = `urp drain && urp remove`; `RemainAfterExit`).
- [x] `nix/urp-hw-matrix.nix` → package `urp-hw-matrix` (writeShellApplication,
      modeled on `urp-bench-matrix.nix`). Args `<acceptor> <initiator>
      <acceptor-ip>`; preflight (urp.ko loaded + endpoints present) then
      `nix copy` the bench closures to both hosts (both twins install a binary
      named `urp-bench`, so invoked by absolute store path); per-cell ssh
      listener (acceptor connectPath) + connector (initiator listenPath) +
      `BENCH_OK` assert (≤3 attempts, `BENCH_SKIP`→skip); 4-combo interop table
      (`c<->c/c<->rust/rust<->c/rust<->rust`, msgs_per_s) + `c<->c` RTT p50/p99.
      Reports the `pmc` PTP `offsetFromMaster` bounding the one-way estimate
      (RTT/2 ± offset). Wired into `flake.nix` `let` + `packages`. **Not** in
      `ci-local` (needs hardware — documented like the microVM tiers).

### Verification

- [x] `nix build .#urp-hw-matrix` succeeds (shellcheck clean).
- [x] Module evaluates in a throwaway `nixosSystem` (option types + submodules
      OK: acceptor/initiator, defaults `[ib_core rdma_cm mlx5_ib]`).
- [x] `nix run .#ci-local` = `LOCAL_CI_RESULT=GREEN` (9/9 builds + 4/4
      fuzz-smoke) — additive nix packaging regresses nothing.

### Notes

- **One-way latency**: `urp-bench` `BENCH_OK` only carries RTT (`p50_us`/
  `p99_us`, single-clock). A *direct* payload-timestamped one-way measurement
  needs a urp-bench change (stamp CLOCK_REALTIME in the payload, log on recv) —
  deferred. v1 ships RTT + the PTP offset that bounds the RTT/2 one-way estimate.
- **PSK on argv**: `urp add` has only `--password <STRING>` (no file flag), so
  `passwordFile` is `cat`'d at ExecStart → visible on the process argv. Fine for
  the trusted lab; revisit if the CLI grows a file-based flag.

---

## Phase 2: Pin, rebuild, first light

**Status**: Done — both boxes deployed, RoCEv2 **QP session established**. Data
path stalls at the stream handshake (real-HW bug, see Notes → blocks Phase 3).
Committed in `~/nixos` `efdc537`.

### Definition of done

- [x] Both `hp1/flake.nix` and `hp3/flake.nix`: add
      `inputs.uds-rdma-proxy.url = "github:randomizedcoder/uds-rdma-proxy/main"`
      with `inputs.nixpkgs.follows = "nixpkgs"`; add to the outputs
      argset + `modules` (`uds-rdma-proxy.nixosModules.urp`). (Do **not**
      `follows`-prune redpanda/microvm — the flake's `packages` guards aren't
      follows-safe; source-only fetch, never built.)
- [x] `services.urp` config declared: **hp1 acceptor** `pair_acceptor
      --connect-path /run/urp-echo.sock --bind 10.10.2.1:4791`; **hp3 initiator**
      `pair_initiator --listen-path /run/urp.sock --peer 10.10.2.1:4791`.
      Added `qperf linuxptp mstflint rdma-core` to `hp{1,3}/systemPackages.nix`
      (`perftest` not in this nixpkgs → `qperf` for raw-verbs baselines).
- [x] **PTP time sync** (lab infra, *not* in `nixosModules.urp`): `ptp.nix`
      imported by hp1/hp3 — `linuxptp`, `ptp4l -H` on link B (`enp1s0f1np1`,
      hp1 grandmaster / hp3 slave), `phc2sys`. HW timestamping confirmed
      (`ethtool -T enp1s0f1np1` → hardware-transmit/receive). See §32.8.
- [x] Deployed (see Notes — **not** on-box `make`; build-on-`l` + `nix copy`,
      then reboot both). hp1 and hp3 have separate locks.
- [x] **First light**: urp.ko loaded declaratively at boot on both; `urp show`
      both endpoints `active`; **RoCEv2 QP session established** hp1↔hp3
      (`all 1 QPs established`, both sides); acceptor splices to backend UDS
      (`connected to UDS` → `pump started`).

### Verification

- [x] System build + activation succeeds on **both** boxes (via `l` + `nix copy`
      + `switch-to-configuration`; on-box `nixos-rebuild` blocked by the boxes'
      `.git`-less net-next — see [[hp-urp-deploy-recipe]]).
- [x] `urp show` → QP `established` both sides; RoCEv2 addr+route resolution OK.
- [ ] `pmc … GET CURRENT_DATA_SET` on hp3 (`offsetFromMaster`) — PTP units up +
      HW timestamping confirmed; sub-µs lock reading not yet captured.

### Notes

**Deploy method (important — see [[hp-urp-deploy-recipe]]):** on-box
`nixos-rebuild` fails because the boxes' `/home/das/Downloads/net-next` has no
`.git`, so `netnext-kernel.nix`'s `fetchGit` faults. Built each system on `l`
(has net-next `.git` + both pinned revs), `nix copy --to ssh://root@hpN`, then
`switch-to-configuration`. hp1 kernel == running (`gwaliv1`, no kernel change);
hp3 config wanted a *new* kernel (`m9gjjis` ≠ running `3fnk7dy`) → full kernel
rebuild + reboot. Both rebooted; urp.ko now loads via `boot.kernelModules` and
endpoint + PTP services start on boot. (A new out-of-tree module does **not**
load on a `switch` — modprobe resolves against `/run/booted-system` — so the
reboot is required.)

**What works on real ConnectX-4 Lx RoCEv2:** urp.ko builds+loads on both
distinct net-next 7.2-rc1 trees (hp1 series4/`a208f86`, hp3 series5-a/`c9908e2`),
vermagic OK; RDMA-CM address+route resolution; QP connect/accept; acceptor→
backend UDS splice + pump; **QP session establishment** (`all 1 QPs
established`). This is the first proof off emulated `rdma_rxe`.

**Bugs found (first real-HW exposure — blocks Phase 3):**
1. **Stream flow-control handshake stalls (data-path blocker).** After the QP
   establishes, opening an app stream sticks: initiator `state=syn-sent
   tx=16384 rx=0 credits=l506/r0`, acceptor `state=syn-received tx=0 rx=0`.
   **Remote credits stay `r0`** on both — no credit grant is exchanged, so the
   SYN-ACK/data never flow (`rx=0`, `rtt_ns=0`, `buffer-alloc-fails=4`).
   `urp-bench` → `BENCH_FAIL reason=timeout` / backend `peer_closed_early`.
   The emulated-rxe path masked this. **Phase 3 (matrix) is blocked until this
   is fixed.**
2. **UDS bind doesn't unlink a stale socket** → `bind … failed: -98`
   (EADDRINUSE) on every re-activation; needs manual `rm` of the listen/connect
   path. (unlink-before-bind fix.)
3. **Acceptor leaks its QP slot after a dropped session** →
   `rejecting extra CONNECT_REQUEST (1 >= 1 QPs)` until the acceptor is
   restarted; a rejected/torn-down session doesn't free the slot.
4. **`WARNING … ib_free_cq`** on the failed-activate cleanup path
   (`urp_srq_destroy` ← `urp_rdma_cleanup` ← `urp_endpoint_activate`).
5. Establishment is order/timing-sensitive: the acceptor connects to its
   backend UDS *at CM connect-request time* and rejects if absent (contradicts
   the earlier "session establishes regardless of backend" assumption); and
   `urp-bench --listen` is one-shot (exits when its session drops).

---

## Phase 3: Run the matrix

**Status**: **Matrix done** (2026-08-17) — the Phase-2 credit-grant stall was
fixed (commit `bd133dd`) and the full sweep now passes **128/128 `BENCH_OK`,
`verify=full`, 0 skips**. Extra single-purpose probes (`urp-test-client`,
`urp-fast-poc`) not yet run.

### Definition of done

- [x] From `l`: `nix run .#urp-hw-matrix -- hp1 hp3 10.10.2.1` — all four C/Rust
      cells `BENCH_OK verify=full`, populated delta table (C↔Rust + Rust↔C are the
      live framing differential). **128/128 cells green** over `urp.ko` `5bfcd37`
      on kernel 7.1.8. Required fixing three latent `urp-hw-matrix` runner bugs
      (root ssh, self-killing `pkill`, listener/generator duration) + the table
      renderer.
- [ ] Extra phases: `urp-test-client 10.10.2.1 <port> {echo,throughput,latency,
      reorder,bigframe}` against a dedicated acceptor; `urp-fast-poc /dev/urp
      pair_acceptor` → `URP_FAST_POC_OK`.
- [ ] Fill the design-32 **Results** section with real mbps/p99/msgs_per_s;
      drop the "emulated — not perf" caveat; flip design 32 Status →
      *implemented*; refresh `status.md` (hardware-validation pass done). No
      `dmesg` splats.

### Results

Full results, setup, methodology and the 128-cell interop table live in
**[32-performance-results.md](32-performance-results.md)**. Summary (C↔C,
`verify=full`, RTT = round trip, single QP, PTP offset −36 ns):

| cell | mode | msg_size | payload MB/s | msgs/s | RTT p50 | RTT p99 |
|------|------|----------|------|--------|---------|---------|
| lowest latency | blocking | 24 | — | 21545 | **24.5 µs** | 29.4 µs |
| small pipelined | uring-rw | 1024 | ~103 | 99934 | 82.2 µs | 114.5 µs |
| peak msg rate | uring-bufring | 24 | — | **242325** | 38.7 µs | 64.7 µs |
| peak throughput | uring-rw | 4076 | **~161** | 39633 | 205.0 µs | 321.5 µs |
| large frame | blocking | 65516 | ~164 | 2500 | 3.39 ms | 4.22 ms |

**128/128 `BENCH_OK`, 0 fail, 0 skip** across c↔c / c↔rust / rust↔c / rust↔rust ×
{blocking, uring-rw, uring-fixed, uring-bufring} × {24, 1024, 4076, 65516} × {1, 16}.

### Notes

_(none yet)_
