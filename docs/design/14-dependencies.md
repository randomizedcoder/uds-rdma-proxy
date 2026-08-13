# Rust Crate Dependencies

> **Status: historical (userspace-proxy era, 2026-05).** This document
> describes the original *userspace Rust proxy* design, which was superseded:
> the project was implemented as a **Linux kernel module** instead — see
> [DESIGN.md](../DESIGN.md) and [21-kernel-module.md](21-kernel-module.md).
> Retained for design rationale and history. Details below (crates, io_uring,
> tokio, TOML config, Prometheus, the v0–v4 roadmap) do not match the
> implementation.

| Crate | Purpose | Notes |
|-------|---------|-------|
| `rdma-sys` | Raw ibverbs/rdma-cm FFI bindings | Low-level, maximum control over RDMA operations |
| `io-uring` | Low-level io_uring bindings | From tokio-rs, well-maintained |
| `tokio` | Async runtime | Primary runtime for connection management, control plane |
| `tokio-uring` | io_uring async runtime | For v0/v1 UDS I/O; evaluate overhead for v2+ |
| `metrics` | Metrics facade | Clean instrumentation API (`counter!()`, `gauge!()`, `histogram!()`) |
| `metrics-exporter-prometheus` | Prometheus exporter | HTTP endpoint for metrics scraping |
| `clap` | CLI argument parsing | Derive-based, de facto standard |
| `serde` + `toml` | Configuration parsing | TOML config file support |
| `tracing` | Structured logging | Async-aware, span-based, integrates with metrics |
| `tracing-subscriber` | Log output formatting | JSON and text output |
| `nix` | Unix/POSIX syscalls | `mmap`, `mlock`, CPU affinity, NUMA |
| `crossbeam` | Lock-free data structures | `ArrayQueue` for buffer pool free-list |
| `bytes` | Byte buffer utilities | For frame encoding/decoding |
| `thiserror` | Error types | Derive-based error types for library code |
| `anyhow` | Error handling | For application-level error propagation |
| `criterion` | Microbenchmarks | For buffer pool, frame codec, reorder buffer benchmarks |
| `proptest` | Property-based testing | Random input generation with shrinking for invariant checking |
| `cargo-fuzz` / `libfuzzer-sys` | Coverage-guided fuzzing | Fuzz targets for frame decode, reorder buffer, config parse |
| `arbitrary` | Structured fuzzing input | Derive `Arbitrary` for `FrameHeader`, `Frame`, config structs |
| `cargo-geiger` | `unsafe` usage tracking | Counts `unsafe` blocks across dependency tree |
| `cargo-audit` | Dependency vulnerability scan | Checks against RustSec advisory database (CI on every PR) |
| `cargo-deny` | License and advisory checks | Policy enforcement for dependency licenses and known advisories |

**Why raw `rdma-sys` instead of `async-rdma`**:
1. `async-rdma` assumes a specific async model that may conflict with our custom CQ polling loop
2. Credit-based flow control requires direct control over `post_recv` and send timing
3. Buffer pool management needs tight integration with both io_uring and ibverbs
4. Raw bindings provide transparency needed for performance debugging
5. Our dual-registration (io_uring + ibverbs) of the same buffers requires manual memory management


[Back to Design Overview](../DESIGN.md)
