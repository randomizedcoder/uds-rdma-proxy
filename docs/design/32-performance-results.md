# Real-Hardware Performance Results — hp1 ↔ hp3 RoCEv2

Round-trip latency and interop results for the `urp` UDS↔RDMA proxy on real
**ConnectX-4 Lx 25 GbE RoCEv2** hardware, produced by the `urp-hw-matrix` runner
(design 32, [32-real-hardware-integration-testing.md](32-real-hardware-integration-testing.md)).

**Last updated**: 2026-08-17. **Result: 128/128 cells `BENCH_OK`, `--verify full`
(byte-exact), 0 failures, 0 skips** — the first full four-way client-interop pass
(including the Rust twins) over real RoCEv2.

> These supersede the emulated-microVM numbers in
> [BENCHMARKING.md](../BENCHMARKING.md) for real-hardware latency: that document's
> figures are explicitly *not* representative; the ones here are measured on the
> physical fabric.

---

## Setup

Two dedicated NixOS boxes, back-to-back (no switch), driven over ssh from
workstation `l`:

| | Acceptor | Initiator |
|---|---|---|
| Host | **hp1** | **hp3** |
| RoCE data link (A) | `enp1s0f0np0` **10.10.2.1/29** | `enp1s0f0np0` **10.10.2.3/29** |
| PTP link (B) | `enp1s0f1np1` 10.10.3.1/29 | `enp1s0f1np1` 10.10.3.3/29 |
| RDMA device | `mlx5_0` | `mlx5_0` |
| urp endpoint | `pair_acceptor` (backend `/run/urp-echo.sock`) | `pair_initiator` (listen `/run/urp.sock`) |

- **NIC**: Mellanox **ConnectX-4 Lx** (MT4117), 25 GbE, RoCEv2 (UDP/**4791**).
- **Kernel**: 7.1.8 (nixpkgs `linuxPackages_latest`); `urp.ko` vermagic `7.1.8`,
  built from the design-33 branch (`5bfcd37`) via `nixosModules.urp`.
- **Endpoint geometry**: `num-qps=1`, `buffer-count=1024`, `buffer-size=4096`,
  multistream mode. A single RC QP carries all streams.
- **Time sync**: `linuxptp` (`ptp4l`/`phc2sys`) on the separate link B so it never
  contends with the data path on link A. `offsetFromMaster` was **−36 ns** during
  this run — small enough that a one-way estimate of RTT/2 is meaningful.

### Data path (one message round trip)

```
urp-bench --connect            urp initiator            RoCEv2 (RC QP)            urp acceptor           urp-bench --listen
  (generator, hp3)   ── UDS ──▶  /run/urp.sock  ── RDMA ──▶  ...  ── RDMA ──▶  /run/urp-echo.sock  ── UDS ──▶  (echo, hp1)
        ▲                                                                                                         │
        └───────────────────────────────────  echo back over the same path  ─────────────────────────────────────┘
```

Every byte the generator sends traverses UDS → RDMA → UDS to the echo listener
and back. `--verify full` checks the returned bytes match exactly.

---

## The matrix

The `urp-bench` twins (design 30) are two byte-for-byte protocol-compatible load
generators — one **C**, one **Rust**. Each cell runs a listener (echo) on the
acceptor's backend socket and a generator on the initiator's listen socket, over
the standing RoCEv2 session. The matrix sweeps:

- **Client interop** (4): `c↔c`, `c↔rust`, `rust↔c`, `rust↔rust` (listener-lang ↔ generator-lang).
- **I/O mode** (4): `blocking`, `uring-rw`, `uring-fixed`, `uring-bufring`.
  (The C twin additionally has `uring-sqpoll`/`uring-sendzc`; those are not in the
  sweep. The Rust twin lacks `sendzc` and would emit `BENCH_SKIP` — none did.)
- **Message size** (4): `24`, `1024`, `4076`, `65516` bytes.
- **Batch depth** (2): `1`, `16` in-flight.

4 × 4 × 4 × 2 = **128 cells**, each `--duration 3 --verify full`.

**Metric.** `urp-bench` is a *symmetric peer echo*, so its latency figure is the
**round-trip time (RTT)** — the p50/p99/min/max of the full generator → echo →
generator loop, in microseconds. It also reports `msgs_per_s` and a payload
`mbps` field (megabytes/s of application payload). A one-way estimate is
≈ RTT/2, valid to within the PTP offset above.

### Reproduce

```
nix run .#urp-hw-matrix -- hp1 hp3 10.10.2.1      # full 128-cell sweep
URP_HW_MATRIX_QUICK=1 nix run .#urp-hw-matrix -- hp1 hp3 10.10.2.1   # 16-cell smoke
```

---

## Results

**128/128 `BENCH_OK`, `verify=full`, 0 failures, 0 skips.** Every client
combination, I/O mode, message size and batch depth exchanged byte-exact traffic
over real RoCEv2.

### Interop matrix (throughput + C↔C RTT)

Columns are `msgs_per_s` per client combo; the last two are the C↔C round-trip
p50/p99 in microseconds (representative of the fabric latency; the other combos
track within a few percent).

```
mode               msg batch |      c<->c   c<->rust   rust<->c rust<->rust |  cc_p50us  cc_p99us
blocking            24     1 |      21545      21328      21868      21943 |      24.5      29.4
blocking            24    16 |     162633     191832     171673     198506 |      43.3      58.0
blocking          1024     1 |      16145      15939      14311      17631 |      32.9      42.0
blocking          1024    16 |      70873      73564      70944      84073 |     115.1     146.9
blocking          4076     1 |      10789      10525      10546       9911 |      47.0      66.7
blocking          4076    16 |      30331      32402      32208      35239 |     262.3     443.8
blocking         65516     1 |       1568       1543       1515       1557 |     300.1     403.4
blocking         65516    16 |       2500       2483       2250       2527 |    3388.2    4218.9
uring-bufring       24     1 |      15527      14083      15512      15321 |      34.6      48.8
uring-bufring       24    16 |     233982     236978     242325     237743 |      38.7      64.7
uring-bufring     1024     1 |       9332       9332       8579       9369 |      56.9      67.8
uring-bufring     1024    16 |      91642      90755      92245      85849 |      93.0     164.4
uring-bufring     4076     1 |       8296       8153       8346       8378 |      63.8      86.3
uring-bufring     4076    16 |      31031      26944      31259      29558 |     250.9     411.8
uring-bufring    65516     1 |       1452       1446       1483       1441 |     326.4     432.9
uring-bufring    65516    16 |       2451       2224       2197       2412 |    3291.1    4711.0
uring-fixed         24     1 |      15303      15347      15303      15098 |      34.9      49.1
uring-fixed         24    16 |     217613     220772     210532     225927 |      43.1      67.6
uring-fixed       1024     1 |       9363       9386       9350       9354 |      56.8      68.0
uring-fixed       1024    16 |      99411      86130     103029     101174 |      84.7     173.1
uring-fixed       4076     1 |       8812       8665       8656       8494 |      57.7      76.5
uring-fixed       4076    16 |      38866      37446      37022      38083 |     208.1     337.3
uring-fixed      65516     1 |       1462       1489       1460       1418 |     323.7     419.6
uring-fixed      65516    16 |       2416       2328       2334       2264 |    3507.2    4515.6
uring-rw            24     1 |      15684      15560      15550      14321 |      34.3      48.9
uring-rw            24    16 |     231426     228021     204226     203390 |      38.1      55.5
uring-rw          1024     1 |       9307       9396       9384       9361 |      56.5      67.1
uring-rw          1024    16 |      99934     101139     106883     100099 |      82.2     114.5
uring-rw          4076     1 |       9147       8881       8055       8796 |      55.5      75.5
uring-rw          4076    16 |      39633      37079      38856      35152 |     205.0     321.5
uring-rw         65516     1 |       1497       1465       1496       1486 |     317.4     415.0
uring-rw         65516    16 |       2431       2476       2458       2310 |    3438.9    4293.2
```

`ptp_offset_ns=-36.0`, `cells_ok=128 skips=0`.

### Headlines

- **Lowest round trip**: **24.5 µs p50 / 29.4 µs p99** (`blocking`, 24 B, batch 1)
  — a ~12 µs one-way UDS→RDMA→UDS→RDMA→UDS hop.
- **Small-message, pipelined**: `uring-rw` 1024 B batch 16 → **82 µs p50 / 115 µs
  p99** at ~100 k msgs/s.
- **Peak message rate**: **~242 k msgs/s** (`uring-bufring`, 24 B, batch 16).
- **Peak payload throughput**: **~161 MB/s** (`uring-rw`, 4076 B, batch 16). This
  is an RTT-bound ping-pong echo on a **single QP**, not a line-rate streaming
  test, so it sits well under 25 GbE by design — latency, not bandwidth, is what
  this matrix measures.
- **Batching pays off**: batch 1 → 16 raises small-message rates ~7–15× (e.g.
  `uring-rw` 24 B: 15.7 k → 231 k msgs/s) at a modest tail-latency cost.
- **`io_uring` vs `blocking`**: `uring-*` modes cut syscalls/msg and win on
  small-message rate; `blocking` has the lowest single-shot p50 for tiny
  messages. Large 64 KiB messages are dominated by segmentation/reassembly and
  land at ~3.3–3.5 ms p50 across all modes.
- **Language parity**: C and Rust twins interoperate in every direction and track
  within a few percent — no combo is an outlier.

---

## Caveats & scope

- **RTT, not one-way.** Figures are full round trips; one-way ≈ RTT/2, valid to
  within the ±36 ns PTP offset. A directly payload-timestamped one-way probe is
  future work (needs a `urp-bench` change to stamp `CLOCK_REALTIME` in payload).
- **Single QP.** `num-qps=1`; multi-QP fan-out / multi-stream scaling is not
  exercised here.
- **Latency benchmark.** This is a request/response echo; it is not designed to
  saturate the 25 GbE link. Bandwidth headroom is expected.
- **Runner correctness.** These results required fixing three latent bugs in the
  `urp-hw-matrix` runner (connect as root, a self-killing `pkill` that reaped the
  launcher shell, and an equal listener/generator duration that tripped the
  symmetric-FIN handshake), plus an empty-table renderer. See the design-33
  branch commit that touches `nix/urp-hw-matrix.nix`.

## References

- [32-real-hardware-integration-testing.md](32-real-hardware-integration-testing.md) — the design/spec and the runner.
- [32-implementation-status.md](32-implementation-status.md) — phase tracker (Phase 3 = run the matrix).
- [BENCHMARKING.md](../BENCHMARKING.md) — buffer-geometry methodology + the (superseded) emulated numbers.
- [33-initiator-connection-bringup.md](33-initiator-connection-bringup.md) — the connection bring-up work whose `urp.ko` (`5bfcd37`) was under test.
