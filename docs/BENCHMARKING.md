# Benchmarking the buffer geometry

**Status:** initial results from the emulated microVM harness (2026-08-12).
**⚠️ The numbers below are NOT representative of real performance** — see
[The big caveat](#the-big-caveat) — real-hardware numbers are a
[TODO](#todo-real-hardware-rocev2). This document records *how* the buffer
geometry (`buffer_count` / `buffer_size`) is exercised and benchmarked, what the
emulated harness reported, and how to reproduce it — in a VM today, on real
RoCEv2 hardware later.

Related: [design 22 §22.3](design/22-genl-interface.md) (the attributes),
[design 28 §28.8](design/28-testability.md) (the liveness oracle), and the
design-29 Gap-2 wiring commits (`buffer_count` / `buffer_size` through the data
path).

## What is under test

`buffer_count` and `buffer_size` are per-endpoint attributes that size the whole
data path:

- **`buffer_count`** → the page-pool depth, the send/recv buffer split, the
  shared CQ / SRQ / per-QP SQ depths, and the credit window.
- **`buffer_size`** → the DMA slot bytes (header + max payload), the page-pool
  `order` (slots larger than a page become one contiguous compound page), every
  recv `sge.length`, and the wire max-payload the UDS pump reads per frame.

The benchmark sweeps a range of geometries end to end and, for each, sends
fixed-size application messages through the full path
(`client → RDMA → acceptor → UDS backend → echo → back`) and measures the
round trip.

## How the testing is done

### The nix target

The whole thing runs as a two-VM pair test:

```sh
# functional + benchmark run (normal kernel — fast, cached)
nix run .#urp-microvm-pair-test

# same, under the KASAN / KMEMLEAK / lockdep sanitizer kernel
# (memory-safety + race coverage; slower, rebuilds the debug kernel)
nix run .#urp-microvm-pair-test-debug
```

Each spins up two microVMs wired by an emulated link, loads `urp.ko` on both,
sets up a soft-RoCE (`rxe`) device, and walks a series of phases. The buffer
geometry benchmark is **Phase 10f**.

### Phase 10f: buffer geometry

For each `(buffer_count, buffer_size, payload)` point, Phase 10f:

1. adds a dedicated acceptor endpoint with that `--buffer-count` / `--buffer-size`;
2. asserts `urp show` reports the **effective** geometry (closes the design 28
   observability gap — the attributes used to be inert);
3. from the second VM runs `urp-test-client … bigframe <payload> <count>`, which
   sends `count` single DATA frames of `payload` bytes each and verifies every
   echo comes back **byte-exact** (a payload larger than the old fixed 4076-byte
   ceiling only survives if the slot and max-payload were sized from
   `buffer_size`);
4. asserts the acceptor delivered every frame (`rx-frames` / `rx-bytes`);
5. records per-frame RTT, MB/s, and messages/sec.

The current sweep spans six points, with `buffer_size` matched to the payload
(the realistic "size the pool for the workload" config), so it exercises the
whole page-pool `order` range — from small order-0 slots near
`URP_BUFFER_SIZE_MIN` up to order-3 slots larger than a page:

| buffer_count | buffer_size | payload | slot order (4 KiB pages) |
|---:|---:|---:|:--|
| 64 | 512 | 256 | 0 |
| 64 | 1024 | 512 | 0 |
| 64 | 2048 | 1024 | 0 |
| 64 | 4096 | 2048 | 0 |
| 32 | 16384 | 8000 | 2 |
| 64 | 32768 | 12000 | 3 |

### The `bigframe` client mode

`urp-test-client <ip> <port> bigframe [payload_bytes] [count]` is the benchmark
driver. It sends `count` single frames of `payload_bytes`, verifies each echo
byte-for-byte, and prints one machine-parseable line:

```
BIGFRAME_OK payload=8000 frames=32 rtt_us=42071.7 mbps=0.2 msgs_per_s=24
```

The client's other modes (`echo`, `throughput`, `latency`, `reorder`) remain
available for ad-hoc runs.

### Unit-level sizing coverage

The pure geometry resolvers (`urp_resolve_num_bufs`, `urp_resolve_buf_size`,
`urp_ep_max_payload`) are table-tested in KUnit, pinning every clamp boundary
(0, `<MIN`, `MIN`, default, `MAX`, `>MAX`, saturated, header-only slot):

```sh
sudo nix run .#test-kmod-k0   # loads urp.ko in a VM; KUnit runs at module init
```

## Results (emulated microVM harness, 2026-08-12)

All six geometries delivered **byte-exact** (`rx-bytes == payload × frames` at
every size), including the small order-0 slots, on both the normal and the
KASAN/KMEMLEAK/lockdep sanitizer kernels (sanitizer run clean — no reports).

Benchmark table (32 frames per point):

```
 bufsize  payload     rtt_us       mbps     msgs/s
     512      256    42003.1        0.0         24
    1024      512    41981.0        0.0         24
    2048     1024    41773.6        0.0         24
    4096     2048    42138.1        0.0         24
   16384     8000    42071.7        0.2         24
   32768    12000    42331.5        0.3         24
```

### What the numbers say (and don't)

- **RTT is a flat ~42 ms floor across every payload** (256 B and 12 kB are within
  ~1 % of each other), and **messages/sec pins to ~24 ≈ 1 / 42 ms**. The workload
  is entirely round-trip-latency-bound.
- Only **MB/s** moves (0.0 → 0.3), and only because throughput = payload × a fixed
  ~24 msg/s.

In other words: the *data-movement* cost that should make a 256 B message cheaper
than a 12 kB one is a rounding error next to the fixed per-round-trip cost on
this harness. The expected small-message advantage (lower latency, far more
messages/sec) is **real in principle but invisible here** — see below.

## The big caveat

**These VMs are fully emulated, and the RDMA transport is soft-RoCE (`rxe`), not
a real NIC.** The ~42 ms round-trip floor is dominated by:

- software RoCE packet processing in the kernel (`rxe`), not hardware offload;
- TCG/emulated-CPU overhead in the microVMs;
- `socat`/`cat` context switches on the UDS echo backend.

None of that is representative of a real RoCEv2 deployment, where the whole point
is hardware kernel-bypass. **Treat the absolute latency/throughput numbers as
meaningless** — they exist only to prove the code is *correct and memory-safe*
across the full buffer-geometry range, and to validate the measurement harness
itself. The benchmark's *shape* (flat, latency-bound) is an artifact of the
emulator, not of the proxy.

## TODO: real-hardware RoCEv2

We have multiple machines with **Mellanox 25 GbE RoCEv2 NICs**, so real
point-to-point numbers are a planned follow-up.

Plan:

- Run the same `urp-test-client bigframe` / `throughput` / `latency` drivers
  between two physical hosts over the real NICs (real `mlx5` RDMA device, not
  `rxe`), outside VMs.
- Repeat the geometry sweep (256 B … 12 kB, and larger) plus a dedicated
  small-message run at high frame counts.

What we expect to see on hardware (and cannot see on the emulator):

- **Single-digit-microsecond RTT** for small messages instead of ~42 ms — the
  kernel-bypass latency win becomes obvious.
- **Messages/sec in the millions** for small payloads, falling as payload grows
  — the small-message regime is where RDMA shines, at the cost of poor byte
  efficiency (per-message header overhead dominates).
- A genuine **latency-vs-throughput tradeoff** as `buffer_size` grows: larger
  slots amortize per-frame overhead (higher MB/s) at a modestly higher per-frame
  latency — the curve the emulator flattens.

Until then, the microVM harness is the regression gate (correctness +
memory-safety across the geometry range); the numbers here are **not** a
performance baseline.

## io_uring UDS benchmark (design 30)

**Status: designed, not yet implemented.**
[Design 30](design/30-urp-bench-io-uring.md) specifies `urp-bench`, a
symmetric C + Rust benchmark pair that drives the **UDS side** of the tunnel
with io_uring (registered buffers, batched submissions, SQPOLL, a SEND_ZC
probe) and sweeps message size × batch size × io_uring mode. Its results —
the mode/size/batch matrix, the C-vs-Rust delta table, and the
`BENCH_MEMCPY` copy-cost yardstick — will land in this document alongside
the geometry sweep above, under the same emulated-numbers caveat (the direct
no-tunnel topology on a real host is the meaningful substrate for the
io_uring deltas).
