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
- **Control plane** — `urp add/remove/show/stats/drain/monitor` over generic
  netlink; per-endpoint `/proc/urp/<name>/stats`.
- **QP health probes + PSK auth** (SHA-256 in `rdma_cm` private_data).
- **Real Redpanda over UDS-over-RDMA** — a real `rpk` client ↔ real Redpanda
  broker: **metadata** *and* **full produce/consume** round-trip over the RDMA
  tunnel (payload verified byte-for-byte).
- **Testing** — single-host soft-RoCE integration, a 2-VM microVM pair harness,
  cross-arch (x86_64 KVM / aarch64 TCG), a kernel-version matrix (6.1/6.6/6.12/
  7.1), and a **KASAN + KMEMLEAK + lockdep** sanitizer pass (clean under a
  12-concurrent-stream burst).

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
```

To build the module against your **running** kernel, see the `buildUrpKo`
`--impure` recipe in `flake.nix`.

## Docs

- **[docs/DESIGN.md](docs/DESIGN.md)** — full design (framing, RDMA transport,
  multi-QP/ECMP, connection multiplexing, kernel module, GENL, CLI, and the
  netns/multi-tenancy + k8s provisioning design in §24–25).
- **[docs/KERNEL-MODULE-PLAN.md](docs/KERNEL-MODULE-PLAN.md)** — phased
  implementation plan; **[status.md](status.md)** — current status.

> Prototype / research code. Not production-hardened.
