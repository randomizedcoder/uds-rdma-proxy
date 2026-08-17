# 32. Real-Hardware RoCEv2 Integration Testing (hp1 ↔ hp3)

Status: **in progress** (design + tracking; implementation staged per
[`32-implementation-status.md`](32-implementation-status.md))

## 32.1 Motivation

Every data-path test to date has run over **soft-RoCE (`rdma_rxe`)** — either a
single host or the microVM pair (design 19 / Phase 5). `status.md` is explicit
that these are *correctness* gates, not performance: the tunneled numbers are
labelled "emulated rxe — not perf", and a real "hardware-validation pass" has
been deferred throughout.

We now have the hardware for that pass: two NixOS boxes, **hp1** and **hp3**,
each with a **Mellanox ConnectX-4 Lx 25 GbE** NIC (`mlx5_core` / `mlx5_ib`),
cabled **back-to-back** on two `/29` links, in a secure lab with direct SSH root
access from the workstation `l`.

This document specifies the first true end-to-end proof off emulation:

1. Make `urp.ko` consumable as a **pinned flake input** in the hp1/hp3 NixOS
   configs (a reusable `nixosModules.urp`), and rebuild both boxes to
   instantiate the module against their kernel.
2. Stand up a persistent `urp` endpoint pair over **real RoCEv2** and run the
   **client-combination matrix** (C↔C, C↔Rust, Rust↔C, Rust↔Rust) tunneled
   across the wire — the first real interop proof and the first real numbers.

This is the hardware complement to design 05 (RDMA transport), design 15 (RoCEv2
network config: PFC/ECN/DCQCN/DSCP), design 30 (`urp-bench` clients), and design
31 (`urp-fast` zero-copy, whose `/dev/urp` fast path we can finally exercise on a
kernel ≥ 6.8 with a real NIC).

## 32.2 Topology & addressing

```
   l (workstation)                 hp1 (acceptor)                     hp3 (initiator / DUT)
  ┌───────────────┐  make sync +  ┌───────────────────┐  RoCEv2 RC   ┌───────────────────┐
  │ edit nix cfg  │──ssh make────▶│ urp.ko + endpoint  │◀==========▶ │ urp.ko + endpoint  │
  │ nix run       │               │ pair_acceptor      │  mlx5_0     │ pair_initiator     │
  │  .#urp-hw-    │──ssh clients─▶│ UDS /run/urp-echo  │ 10.10.2.1   │ UDS /run/urp.sock  │
  │  matrix       │◀─BENCH_OK────│ (bench listener)   │  :4791      │ (bench connector)  │
  └───────────────┘               └───────────────────┘             └───────────────────┘
```

Back-to-back links (from the hp testbed config, `xdp2.testbed`):

| Host | NIC port      | IP            | Notes            |
|------|---------------|---------------|------------------|
| hp1  | `enp1s0f0np0` | `10.10.2.1/29`| link A (matrix)  |
| hp1  | `enp1s0f1np1` | `10.10.3.1/29`| link B (PTP sync)|
| hp3  | `enp1s0f0np0` | `10.10.2.3/29`| link A (matrix)  |
| hp3  | `enp1s0f1np1` | `10.10.3.3/29`| link B (PTP sync)|

Management is on `eno1` (separate). RoCEv2 uses UDP dst port **4791**. The matrix
rides **link A** (`10.10.2.0/29`); **link B** (`10.10.3.0/29`) carries the PTP
clock sync (§32.8) on a dedicated link so it never contends with the traffic
under measurement — and remains available for a second pair or future
multi-QP/ECMP work (design 08).

## 32.3 RoCEv2 bring-up sequence

On each box, after the module load (Phase 2):

1. **RDMA stack loaded** — `ib_core`, `rdma_cm`, `mlx5_ib` autoload with the NIC;
   the module declares them so they are present regardless (`services.urp.
   rdmaKernelModules`). Confirm with `lsmod | grep -E 'mlx5|ib_core|rdma_cm'`.
2. **Device present** — `ibv_devices` lists `mlx5_0` (and `mlx5_1` for the second
   port). `ibv_devinfo -d mlx5_0` shows the port `ACTIVE`.
3. **RoCEv2 GID** — `show_gids mlx5_0` lists a **v2** (RoCEv2) GID bound to the
   `10.10.2.x` address on the right netdev. This GID is what RDMA-CM resolves.
4. **Device selection is by IP, not by name** — `urp` selects the RDMA device via
   the bind/peer **IP** through RDMA-CM (`rdma_resolve_addr`), *not* the
   `--rdma-device` flag (which is recorded only). The `10.10.2.x` address must be
   live on the mlx5 netdev for CM to pick `mlx5_0`. This is why the IPs are
   assigned declaratively by the testbed config and asserted in first-light.
5. **No rxe here** — `kernel/urp_rdma.c` runtime-detects software devices
   (`PP_FLAG_DMA_MAP`, NUMA) and takes the hardware path for mlx5 automatically.
   The microVM's `rdma link add … type rxe` step has no analogue on hardware.

## 32.4 The `nixosModules.urp` contract

`urp.ko` is made consumable as a reusable NixOS module exported from this flake
(`nixosModules.urp`, impl `nix/nixos-module.nix`). hp1/hp3 pin the flake and set
options; they carry no urp build logic of their own.

The module builds the module against the **host's** kernel via the exported
`self.lib.${system}.buildUrpKo config.boot.kernelPackages` (so it tracks whatever
kernel the box runs — see the compat note in §32.9), loads the RDMA stack, and
materializes a declarative endpoint list.

Options (final shape tracked in the status doc):

- `services.urp.enable` — master switch.
- `services.urp.rdmaKernelModules` — default
  `[ "ib_core" "rdma_cm" "mlx5_ib" ]`.
- `services.urp.endpoints` — a list of submodules, each:
  `{ name; role = "acceptor" | "initiator"; connectPath | listenPath;
     bind | peer (IP:port); numQps; bufferCount; bufferSize; passwordFile?; }`.

Config it produces:

- `boot.extraModulePackages = [ (buildUrpKo config.boot.kernelPackages) ]`
- `boot.kernelModules = rdmaKernelModules ++ [ "urp" ]`
- `urp-cli` + `rdma-core` (and `perftest`, `mstflint` for baselines) in
  `environment.systemPackages`
- one **systemd oneshot per endpoint** — `After` network-online + the RDMA
  device; `ExecStart` = `urp add …`; `ExecStop` = `urp drain … && urp remove …`;
  `RemainAfterExit = true`. This mirrors the imperative sequence the microVM
  harness runs (`nix/microvms/{mkVm.nix,lib.nix}`).

Endpoint creation grammar (design 22/23):

- **acceptor** (hp1): `urp add pair_acceptor --connect-path /run/urp-echo.sock
  --bind 10.10.2.1:4791`
- **initiator** (hp3): `urp add pair_initiator --listen-path /run/urp.sock
  --peer 10.10.2.1:4791`
- teardown: `urp drain NAME` then `urp remove NAME`.

## 32.5 Deployment workflow (from `l`)

The hp NixOS repo (`~/nixos/hp/`) uses a fixed edit → sync → rebuild loop; urp
slots into it with **no new tooling**:

1. **Edit** the config on `l` (add the flake input + `services.urp` block).
2. **`git add`** in the nixos repo — the flake only sees the tracked tree.
3. **`make update`** in `hpN/` — `nix flake update` writes the uds-rdma-proxy
   rev + narHash into that host's own `flake.lock` (hp1 and hp3 have **separate**
   locks, pinned by github commit id + narHash, exactly like the existing `xdp2`
   input).
4. **`make sync`** — rsync the host dir to `hpN:`.
5. **ssh `hpN` → `make`** — `sudo nixos-rebuild switch --flake .`
   (`check_hostname` gates it to run on the box). hp3 also has
   `bootstrap`/`nixos-rebuild boot` if a reboot is needed.

Because the module builds against the already-built kernel tree on the box, the
rebuild is fast — it does **not** trigger a from-scratch kernel build.

## 32.6 The client matrix

The core deliverable. Over the standing `pair_acceptor` ↔ `pair_initiator`
session, we swap the acceptor-side UDS backend for a `urp-bench{,-rs} --listen`
and run a `--connect` generator on the initiator, scraping the byte-identical
`BENCH_OK lang=… mode=…` line from both ends — the Phase 10g pattern
(`nix/microvms/lib.nix`), minus the rxe caveat.

| listener \ generator | C (`urp-bench`) | Rust (`urp-bench-rs`) | Seastar |
|----------------------|-----------------|-----------------------|---------|
| **C**                | C↔C ✓           | C↔Rust ✓              | future (31a) |
| **Rust**             | Rust↔C ✓        | Rust↔Rust ✓           | future (31a) |
| **Seastar**          | future          | future                | future |

Each ✓ cell runs modes `blocking, uring-rw, uring-fixed, uring-bufring`
(plus C-only `uring-sendzc`), at `--verify full`, over the message-size × batch
matrix. The **C↔Rust** and **Rust↔C** cells are the live framing differential —
two independent implementations of the design-30 framing must interoperate
byte-for-byte across the wire.

**Seastar is deferred.** The C++/Seastar client (design 31a) does not exist yet;
its row is documented as future work, blocked on 31a being built. Shipping C+Rust
now is a deliberate scope decision.

### Additional client phases

- **RDMA-direct** — `urp-test-client 10.10.2.1 <port> {echo,throughput,latency,
  reorder,bigframe}` against a dedicated acceptor endpoint (e.g. `hwtest_acc` on
  a distinct port) — exercises the transport directly, bypassing the bench UDS
  clients.
- **Fast path** — `urp-fast-poc /dev/urp pair_acceptor` (design 31 `uring_cmd`
  PoC → `URP_FAST_POC_OK`). The hp kernel is ≥ 6.8, so the `/dev/urp` char
  device is live and the zero-copy path is finally testable on real hardware.

## 32.7 The `urp-hw-matrix` runner

A new `nix run .#urp-hw-matrix -- <acceptor-host> <initiator-host> <acceptor-ip>
[cells]` package (`nix/urp-hw-matrix.nix`, a `writeShellApplication` modeled on
`urp-bench-matrix.nix` + Phase 10g), driven over SSH from `l`.

Per cell (listener-lang × connector-lang × mode × size × batch):

1. `ssh acceptor` — kill any previous listener, start
   `urp-bench{,-rs} --listen /run/urp-echo.sock --id 2 …`.
2. `ssh initiator` — run `urp-bench{,-rs} --connect /run/urp.sock --id 1 …`.
3. Assert `BENCH_OK` on **both** ends (≤ 3 attempts to let the RC connection
   settle; a `BENCH_SKIP` for an unsupported mode → skip, not fail).

Then it prints the 4-way C/Rust delta table + interop verdicts, the same awk
table style as `urp-bench-matrix.nix`.

**Not in `ci-local`.** Like the microVM/soak/cross tiers, this needs real
hardware and is kept out of `nix run .#ci-local`; it is run by hand from `l`
against the lab boxes.

## 32.8 Time synchronization (PTP, for one-way latency)

Two kinds of latency are worth distinguishing, and they have very different
clock requirements:

- **Round-trip (RTT)** is measured on a **single clock** (the initiator's):
  stamp at send, stamp at return, halve. It needs **no** cross-host sync and is
  trustworthy on the current setup as-is — `urp-test-client … latency` and the
  `urp-bench` p50/p99 already work this way.
- **One-way latency** stamps on hp1 at send and on hp3 at receive, so it is only
  as good as the agreement between the two hosts' clocks.

Today both boxes run only **systemd-timesyncd** (SNTP; `configuration.nix`) —
tens of milliseconds of offset, which would swamp a single-digit-microsecond
one-way number. SNTP is the wrong tool. The right tool on this hardware is
**PTP over the NIC's hardware clock (PHC)**:

- The **ConnectX-4 Lx** exposes a PHC and hardware RX/TX timestamping
  (`ethtool -T enp1s0f1np1` confirms `hardware-transmit`/`hardware-receive` +
  the PHC index). PTP disciplines the two PHCs to sub-microsecond agreement,
  independent of and far tighter than any NTP path.
- Run **`linuxptp`** over the **back-to-back link B** (`10.10.3.0/29`,
  `enp1s0f1np1`) so PTP traffic never contends with the matrix on link A:
  `ptp4l -H` (hardware timestamping) with **hp1 as grandmaster** and **hp3 as
  slave** (hp1's PHC is simply the reference — for *latency* the clocks only
  need to *agree*, not to hold correct absolute time). `phc2sys` then steers the
  system clock from the disciplined PHC on each box.
- **Ownership.** This is **lab/host infrastructure, not a `urp` concern** — it
  lives in the hp1/hp3 NixOS configs (a small `ptp.nix` import: `linuxptp`
  package + a `ptp4l` unit bound to link B + a `phc2sys` unit), **not** inside
  `nixosModules.urp`. The module stays focused on the tunnel; the runner just
  reads the resulting clocks. Verify sync health with `pmc`
  (`GET CURRENT_DATA_SET` → `offsetFromMaster` in the low-hundreds-of-ns range)
  before trusting any one-way figure.

The `urp-hw-matrix` runner reports **RTT as the primary latency oracle**
(always valid) and the measured `pmc` `offsetFromMaster`. Because `urp-bench`'s
`BENCH_OK` currently carries only RTT (`p50_us`/`p99_us`, single-clock), PTP is
used here to **bound** the one-way estimate: with the clocks proven to agree to
within the reported offset, `RTT/2` is a defensible one-way figure to that
tolerance. A *direct* payload-timestamped one-way measurement (stamp
`CLOCK_REALTIME` in the payload on send, log it on receive) needs a urp-bench
change and is tracked as future work.

## 32.9 Oracles

- **Correctness** — every matrix cell runs at `--verify full` (per-byte payload
  verification); the C↔Rust cells additionally prove cross-implementation wire
  compatibility. `urp-test-client echo`/`bigframe` verify the raw transport.
- **Performance** — real `mbps` / `msgs_per_s` / p50 / p99 from `BENCH_OK`, at
  last as genuine 25 GbE numbers rather than rxe. This is where the design-30
  hypothesis (syscall batching is the win; AF_UNIX has no zero-copy) meets real
  hardware, and where design 31's fast path can show a real copy-elimination
  delta.
- **Latency** — **RTT** p50/p99 (single-clock, always valid); plus a
  **PTP-bounded one-way estimate** (`RTT/2` ± the measured `pmc` offset, §32.8).
  A direct payload-timestamped one-way probe is future work.
- **Zero-copy** — `URP_FAST_POC_OK` from the `/dev/urp` path.
- **Hygiene** — no `dmesg` splats after a run (`ssh hpN dmesg | grep -i urp`);
  clean endpoint teardown (the drain-order UAF fixed in PR #39 is exactly the
  teardown path a hardware run exercises repeatedly).

## 32.10 Risks & open items

- **Kernel compat (top risk)** — hp1/hp3 run a **custom net-next kernel** newer
  than the flake's "latest" pin. `urp.ko` must build against it; bleeding-edge
  net-next may have moved io_uring / RDMA / `page_pool` APIs the module uses.
  This is the blocking Phase 0 gate: build first, fix any breakage behind the
  existing `LINUX_VERSION_CODE` gate pattern, keep the 6.1/6.6/6.12/latest
  matrix green (`nix run .#ci-local`).
- **Single standing pair** — a known module gap (`status.md`): a second
  concurrent *initiator* endpoint does not start its CM. So the matrix rides
  **one** standing pair and swaps client backends per cell, rather than one
  endpoint per cell. `urp-test-client` modes need their own *acceptor* endpoint
  on a distinct port (fine — acceptors are unaffected).
- **RoCEv2 losslessness** — back-to-back at moderate load should work without
  PFC/ECN. If drops appear under load, enable PFC/DCQCN per design 15 (tuning,
  not a correctness blocker).
- **net-next source location** — the box's kernel is fetched from a local
  net-next checkout; that path must exist wherever the module build runs (the hp
  box on `make`, or `l` for a local Phase-0 build).
- **PTP sync health** — one-way latency (§32.8) is only trustworthy while
  `ptp4l`/`phc2sys` hold the PHCs in sync; the runner must gate the one-way
  figure on a `pmc` offset check and otherwise fall back to RTT-only, never
  publish a one-way number from an unsynced or free-running clock.

## 32.11 References

- Design 05 — RDMA transport (rdma_cm, QP config, buffer pool, CQ).
- Design 08a — QP health probes + PTP-based one-way latency measurement.
- Design 15 — RoCEv2 network config (PFC, ECN, DCQCN, DSCP, MTU).
- Design 22 / 23 — genl interface + `urp` CLI (endpoint grammar).
- Design 30 — `urp-bench` C+Rust clients, `BENCH_OK` grammar, mode matrix.
- Design 31 / 31a — `urp-fast` zero-copy fast path; the Seastar client (future).
- Appendix B — RoCEv2 security practices.

## 32.12 Results

_To be filled in by Phase 3 (real hardware run). Until then the only tunneled
numbers in the repo are the emulated-rxe correctness gates; this section will
carry the first genuine ConnectX-4 Lx 25 GbE mbps / p99 figures and the four
interop-cell verdicts._

| cell | mode | msg_size | mbps | msgs/s | RTT p50 | RTT p99 | 1-way p50 | verify | status |
|------|------|----------|------|--------|---------|---------|-----------|--------|--------|
| _(pending Phase 3)_ | | | | | | | | | |
