# uds-rdma-proxy (`urp`)

Tunnel **Unix Domain Socket** traffic over **RDMA** (RoCEv2), so any program that
speaks UDS can talk to a peer on another host over the RDMA fast path — with no
application changes.

```
App A  <--UDS-->  urp  <==RDMA/RoCEv2==>  urp  <--UDS-->  App B
```

`urp` is implemented as a **Linux kernel module** (`kernel/`, C) that moves bytes
between a Unix socket and RDMA queue pairs in kernel space, plus a Rust control
CLI (`urp`) that drives it over generic netlink. Everything is built and tested
with Nix; **no RDMA hardware is required** — the test suites run on soft-RoCE
(`rdma_rxe`).

## Status

Actively developed. Highlights validated so far (see **[status.md](status.md)**
for the full picture):

- **Kernel data path** — k0 echo → multi-QP + SRQ + credit flow control +
  per-stream reorder → **initiator multi-stream** (many concurrent UDS
  connections multiplexed over a shared QP set).
- **Byte-windowing flow control** — a blocking per-stream sender gate bounding
  in-flight bytes, with cumulative-absolute `CREDIT-BYTES` grants (capability-
  negotiated, so it falls back to frame credits with an older peer). This is what
  makes the **multi-QP data path stable under sustained load on real hardware**
  (see [docs/design/35-windowing-flow-control.md](docs/design/35-windowing-flow-control.md)).
- **Zero-copy fast path** (optional, `CONFIG_URP_FAST`; `urp add --kind fast`) —
  an `io_uring` `uring_cmd` interface that REGISTERs a userspace buffer pool and
  moves payload with no kernel copy
  ([docs/design/31-urp-fast-zero-copy.md](docs/design/31-urp-fast-zero-copy.md)).
- **Control plane** — `urp add/remove/show/stats/drain/monitor` over generic
  netlink; per-endpoint `/proc/urp/<name>/stats`.
- **QP health probes + PSK auth** (SHA-256 in `rdma_cm` private_data).
- **Real Redpanda over UDS-over-RDMA** — a real `rpk` client ↔ real Redpanda
  broker: **metadata** *and* **full produce/consume** round-trip over the RDMA
  tunnel (payload verified byte-for-byte).
- **Kernel version support** — the module builds and is tested against the
  three current LTS series **6.1 / 6.6 / 6.12** *and* the latest mainline
  (nixos-unstable `linuxPackages_latest`, currently **7.2**). CI compiles
  `urp.ko` against every one of them on each push; the microVM pair test boots
  the latest kernel and the hardware testbed runs it too (see the version
  matrix in [ci.yml](.github/workflows/ci.yml)).
- **Testing** — single-host soft-RoCE integration, a 2-VM microVM pair harness,
  cross-arch (x86_64 KVM / aarch64 TCG), the kernel-version matrix above, and a
  **KASAN + KMEMLEAK + lockdep** sanitizer pass (clean under a
  12-concurrent-stream burst).
- **Real RoCEv2 hardware** — validated end-to-end on a two-host 25 GbE testbed
  (Mellanox `mlx5`): byte-exact `urp-bench` transfers and a multi-QP reorder
  sweep (`num_qps ∈ {1,4,8}` × frame sizes) that passes `BENCH_OK verify=full`
  with zero drops. See
  [docs/design/32-real-hardware-integration-testing.md](docs/design/32-real-hardware-integration-testing.md)
  and [docs/design/34-bulk-throughput.md](docs/design/34-bulk-throughput.md).
- **Static analysis** — hermetic Nix targets for sparse, smatch,
  checkpatch --strict, W=1, coccicheck, clippy, rustfmt
  (`nix build .#analysis-all`); all clean except a small documented
  checkpatch residual (see
  [docs/design/26-upstream-readiness.md](docs/design/26-upstream-readiness.md)).
- **Fuzzing** — a comprehensive program covering every attack surface (see
  [docs/design/27-fuzz-testing.md](docs/design/27-fuzz-testing.md)): hermetic
  libFuzzer+ASAN/UBSan harnesses that compile the real kernel C (frame
  classifier, RX state-machine pipeline, C reorder backend), cargo-fuzz on the
  shared Rust crate, and live-VM fuzzers (coverage-guided netlink via KCOV,
  concurrent netlink racer, hostile-peer RDMA wire fuzzer) that run inside the
  KASAN/KMEMLEAK/lockdep pair test. Three real memory-safety bugs found and
  fixed so far.
- **CI** — every push: Nix build matrix + fuzz smoke ([ci.yml](.github/workflows/ci.yml));
  nightly: 10-minute fuzz runs, both microVM pair tests (incl. sanitizers),
  the 1-hour soak, and cross-arch gates ([nightly.yml](.github/workflows/nightly.yml)).

## Quick start (Nix)

```sh
nix build .#urp-ko          # build the kernel module
nix build .#urp-cli         # build the urp CLI
nix run  .#test-kmod-k0     # single-host soft-RoCE integration test (root)
nix run  .#urp-microvm-pair-test        # 2-VM URP-to-URP pair test
nix run  .#urp-microvm-pair-test-debug  # ...under KASAN/KMEMLEAK/lockdep

# Redpanda over UDS-over-RDMA (needs root; builds the broker via the redpanda
# fork flake the first time):
nix run  .#test-redpanda-uds                # metadata round-trip
nix run  .#test-redpanda-produce-consume    # full produce/consume

# Hermetic fuzzing (libFuzzer + ASAN/UBSan on the real kernel C):
nix run  .#fuzz-classify -- -max_total_time=60   # RX frame classifier
nix run  .#fuzz-rx-seq   -- -max_total_time=60   # RX state-machine pipeline
nix run  .#fuzz-reorder  -- -max_total_time=60   # C reorder backend

# Benchmark + real-hardware sweeps (the *-matrix runners take two hosts + an IP):
nix run  .#urp-bench-local                              # single-host io_uring bench
nix run  .#urp-reorder-matrix -- <acc-host> <init-host> <acc-ip>  # multi-QP reorder sweep
nix run  .#urp-bw-matrix      -- <acc-host> <init-host> <acc-ip>  # bulk-throughput sweep
```

To build the module against your **running** kernel, see the `buildUrpKo`
`--impure` recipe in `flake.nix`.

## Docs

- **[docs/DESIGN.md](docs/DESIGN.md)** — full design (framing, RDMA transport,
  multi-QP/ECMP, connection multiplexing, kernel module, GENL, CLI, and the
  netns/multi-tenancy + k8s provisioning design in §24–25).
- **[docs/KERNEL-MODULE-PLAN.md](docs/KERNEL-MODULE-PLAN.md)** — phased
  implementation plan; **[status.md](status.md)** — current status.
- **[docs/BENCHMARKING.md](docs/BENCHMARKING.md)** — how the buffer geometry
  (`buffer_count`/`buffer_size`) is benchmarked (soft-RoCE VM numbers are a
  functional harness, not a performance baseline).
- **Performance & flow control** — the `urp-bench` io_uring benchmark
  ([design 30](docs/design/30-urp-bench-io-uring.md)), the zero-copy fast path
  ([design 31](docs/design/31-urp-fast-zero-copy.md)), real-hardware results
  ([design 32](docs/design/32-real-hardware-integration-testing.md)), bulk
  throughput ([design 34](docs/design/34-bulk-throughput.md)), and byte-windowing
  flow control ([design 35](docs/design/35-windowing-flow-control.md)).

> Prototype / research code. Not production-hardened.
