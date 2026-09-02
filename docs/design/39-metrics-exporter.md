# 39. Metrics Exporter — a lightweight Prometheus surface for `urp`

Status: **PR1–PR4 implemented + deployed (2026-09-01).** The exporter is built
(`crates/urp-exporter`), hardened-module-packaged (`nixosModules.urp-exporter`),
stress/fuzz/audit-verified, and running on hp1/hp2/hp3 feeding Prometheus +
Grafana. io_uring GET fan-out (PR4) was implemented and **benchmarked: blocking
wins at every measured N** (§39.4), so it stays opt-in and blocking is the
default. Remaining: PR5 hardware footprint acceptance test + 8h soak. The `urp`
kernel module
already tracks a small, fixed set of per-endpoint / per-QP / per-stream counters
and exposes them over generic netlink (read today by `urp show --json`). This
doc specifies a **standalone, dependency-light Prometheus exporter** that scrapes
that netlink surface and serves `/metrics`, so we can graph UDS-over-RDMA
throughput/latency/fairness and **alarm on the error counters**
(`reorder_drops`, `buffer_alloc_fails`, `auth_failures`, credit stalls).

The overriding design constraint is **cost**: the mesh is *per-node-CPU / copy-path
bound* — a single node tops out near ~3.9 GB/s (see
[38. Three-Node Full-Mesh Benchmarking](38-three-node-mesh-benchmarking.md) §38.5).
An observability agent that steals measurable CPU from the copy path would show
up directly as lost goodput. So "extremely efficient and as lightweight as
possible" is not a nicety here — it is a **correctness budget** we test against
(§39.9).

This supersedes the naming/taxonomy sketch in
[11. Metrics](11-metrics.md) (historical, userspace-proxy era — its
`uds_rdma_proxy_*` metric tables predate the kernel-module rewrite and name
metrics the module does not count). We keep 11's *intent* and reuse the useful
metric names, but the source of truth for what exists is `crates/urp-netlink`'s
`Stats`/`Qp`/`Stream` structs.

---

## 39.1 What the module already exposes (the scrape source)

The module tracks counters in `struct urp_stats` (per-endpoint), `struct urp_qp`,
and `struct urp_stream` (`kernel/urp.h`), all `atomic64_t`, and exposes them via
**generic netlink** (family `"urp"`, `URP_CMD_GET_ENDPOINT`). Two other surfaces
exist but are inferior for scraping: `/proc/urp/<name>/stats` (per-endpoint
*aggregate only* — omits `credit_stalls` / reorder / auth counters), and the CLI
`urp show|stats --json`. **There is no sysfs, no debugfs, no `/metrics` today.**

The exporter reuses the existing, blocking, tokio-free `crates/urp-netlink`
client — `UrpSocket::connect()` + `fetch_endpoints()` — which decodes the netlink
reply into these `#[derive(Serialize)]` structs (`crates/urp-netlink/src/format.rs`):

| Struct | Counter fields (→ Prometheus counters) | Gauge fields |
|---|---|---|
| `Stats` (per-endpoint) | `tx_bytes` `rx_bytes` `tx_frames` `rx_frames` `credit_stalls` `reorder_insertions` `reorder_drops` `buffer_alloc_fails` `auth_failures` | `active_streams` |
| `Qp` (per-QP) | `tx_bytes` `rx_bytes` `tx_frames` `rx_frames` | `rtt_ns`, `state` |
| `Stream` (per-stream) | `tx_bytes` `rx_bytes` | `reorder_depth` `credits_local` `credits_remote`, `state` |
| `Endpoint` | `connections` (in `Stats`) | `state`, `num_qps`, `buffer_count`, `buffer_size` |

### The one scrape gotcha: `dumpit` omits stats

`fetch_endpoints(sock, None)` issues a netlink **`dumpit`** that returns every
endpoint's *scalar* config + state but **no `stats`/`qps`/`streams` nests** — the
verbose fill is gated to single-endpoint **`doit`** GETs
(`kernel/urp_netlink.c` verbose branch). So a full scrape is:

```
1× dump            → list of endpoint names (+ config, state)
N× verbose GET     → per-endpoint Stats + Qp[] + Stream[]   (one per name)
```

i.e. **1 + N round-trips per scrape**. This is the entire basis of the io_uring
discussion in §39.5 (batching the N GETs) and the kernel follow-up in §39.11
(teach `dumpit` to carry stats and the N GETs vanish).

---

## 39.2 Metric model

Prefix **`urp_`** (short — every byte is in the `/metrics` payload we render on
every scrape). Counters carry the `_total` suffix; gauges do not. Enumerated
states use the Prometheus **stateset** idiom (one series per state, value 0/1).

**Per-endpoint** — labels `{endpoint, device}` (`device` = `rdma_device`):

```
urp_endpoint_tx_bytes_total          urp_endpoint_credit_stalls_total
urp_endpoint_rx_bytes_total          urp_endpoint_reorder_insertions_total
urp_endpoint_tx_frames_total         urp_endpoint_reorder_drops_total        # ← alarm
urp_endpoint_rx_frames_total         urp_endpoint_buffer_alloc_fails_total   # ← alarm
urp_endpoint_connections_total       urp_endpoint_auth_failures_total        # ← alarm
# gauges
urp_endpoint_active_streams          urp_endpoint_num_qps
urp_endpoint_state{state="active|draining|..."}                              # stateset
urp_endpoint_info{listen_path,connect_path,peer,bind,buffer_size,buffer_count} 1  # info
```

**Per-QP** — labels `{endpoint, device, qp}` (`qp` = `index`):

```
urp_qp_tx_bytes_total  urp_qp_rx_bytes_total  urp_qp_tx_frames_total  urp_qp_rx_frames_total
urp_qp_rtt_ns                       # gauge (EWMA; 0 until first PONG)
urp_qp_state{state="healthy|degraded|draining|..."}   # stateset
```

**Per-stream** — labels `{endpoint, stream}` — **behind `--per-stream`, OFF by
default.** Streams are ephemeral (one per accepted UDS connection); exporting
per-stream series churns label cardinality and litters TSDB with dead series.
Default granularity is endpoint + QP.

```
urp_stream_tx_bytes_total  urp_stream_rx_bytes_total
urp_stream_reorder_depth  urp_stream_credits_local  urp_stream_credits_remote
```

**Exporter self-metrics** — labels none / `{error}`:

```
urp_exporter_build_info{version,git} 1
urp_up 1                                            # module loaded + netlink reachable
urp_exporter_scrape_duration_seconds               # gauge, last scrape
urp_exporter_scrape_errors_total{error="..."}      # counter
urp_exporter_netlink_requests_total                # counter (dump + N GETs)
urp_exporter_endpoints                             # gauge
urp_exporter_cache_hits_total                      # counter (§39.4 min-interval cache)
```

### Cardinality budget

Per node the standing config is a handful of endpoints (mesh runs create low-tens
of transient `ma_*`/`mi_*` endpoints, §38.2). Worst case per endpoint:
~10 endpoint series + `num_qps`×6 QP series (≤ 8 QPs → ≤ 48) ≈ **~60 series/endpoint**
with per-stream off. The renderer enforces a hard `--max-series` cap and, on
exceeding it, emits `urp_exporter_series_capped_total` and a `# capped` comment
rather than an unbounded payload. This cap is a unit-tested table (§39.8).

---

## 39.3 Architecture & the efficiency budget

**Model: pull, single-threaded, blocking, no async runtime, no HTTP framework.**

```
                ┌──────────────── urp-exporter (one thread) ────────────────┐
  Prometheus ──▶│ TcpListener.accept ─▶ parse GET ─▶ /metrics?             │
   (scrape)     │        │                              │                   │
                │        │                    cache fresh (< ttl)? ──yes──▶ serve buf
                │        │                              │no                  │
                │        │                    scrape(): dump + N GET ───────┼──▶ netlink (urp)
                │        │                              │                   │
                │        │                    render into reused buf ◀──────┘
                │        ▼                              │                   │
                │   write buf, close ◀──────────────────┘                   │
                └────────────────────────────────────────────────────────────┘
```

Efficiency rules, each with a test or bench that enforces it (§39.8/§39.9):

1. **No async runtime, no HTTP crate.** The listener is `std::net::TcpListener`
   in a blocking accept loop; requests are handled one at a time (Prometheus
   scrapes a target serially and infrequently). The HTTP/1.1 responder is
   hand-rolled (~a few hundred lines): parse request line + headers with a
   bounded read (max header bytes + read timeout → slow-loris resistant),
   route `GET /metrics` and `GET /`, everything else → 404/405/400. **No hyper,
   axum, tiny_http, tokio.** This keeps the dependency set to essentially
   `urp-netlink` + `libc`.

2. **Text rendered by hand — no `serde_json` in the binary.** We reuse
   `urp-netlink`'s decode but render Prometheus exposition text directly into a
   reused `String`/`Vec<u8>`; we do **not** pull `serde_json` into the exporter
   binary. Smaller binary, no intermediate `Value` allocation.

3. **Inherit the workspace `opt-level = "z"` release profile** (`Cargo.toml`
   `[profile.release]`: `panic="abort"`, `lto=true`, `opt-level="z"`, `strip=true`).
   An exporter is latency-insensitive at the µs level; a tiny binary and small
   RSS matter more. (Contrast `urp-bench`, which opts back to `opt-level=3`.)

4. **Reused buffers, zero steady-state heap churn.** The render buffer and the
   scrape scratch are allocated once and `clear()`ed, never reallocated per
   scrape. Enforced by an **allocation-counting global allocator in tests/bench**
   (§39.9) asserting 0 allocations on the steady-state render path. Caveat:
   `urp-netlink` today allocates a `Vec` per `recv`; §39.11 tracks either a
   `fetch_into(&mut scratch)` API on `urp-netlink` or accepting a bounded,
   measured per-scrape allocation.

5. **Min-interval scrape cache.** If the last scrape is younger than
   `--cache-ttl` (default 250 ms), serve the cached render. This bounds netlink
   load under multiple/over-eager scrapers and under the stress harness, and is
   the mechanism that makes the exporter safe to co-locate with the data path.
   `urp_exporter_cache_hits_total` counts it.

6. **CPU-bounded by construction.** The `services.urp-exporter` unit (§39.6) sets
   `CPUQuota` and `MemoryMax` so the exporter *cannot* steal more than a few
   percent of one core even under pathological scrape load — the systemd cgroup
   is the backstop behind the code-level budget.

7. **Unprivileged.** `URP_CMD_GET_ENDPOINT` is unprivileged; the exporter needs
   no `CAP_NET_ADMIN`. It binds `127.0.0.1:<port>` by default (front with a
   reverse proxy for remote scrape / TLS).

Config is a minimal hand-rolled arg/env parser (no `clap`): `--listen`,
`--cache-ttl-ms`, `--per-qp`/`--per-stream`, `--max-series`, `--scrape-timeout-ms`.

---

## 39.4 io_uring: where it helps, where it doesn't, and how we decide

The user asked whether the exporter should read the kernel metrics via io_uring.
Answer: **it's a real, thematically-consistent optimization for the `N`-GET
fan-out — but it is an *optional, feature-gated, benchmark-justified* path, not
the default.** Honest analysis:

**Where io_uring helps.** A full scrape is `1 dump + N verbose GETs` (§39.1). On
the blocking path those `N` GETs are *serial*: `sendto → wait → recv`, repeated,
so scrape latency ≈ `N ×` round-trip. The `N` GETs are independent (distinct
`nlmsg_seq`), so they can be pipelined:

- **Batched submission** — submit all `N` `SENDMSG` + `RECVMSG` SQEs in one
  `io_uring_enter`; syscalls drop from ~`2N` to ~1–2, and scrape wall-time
  collapses from `N` round-trips to ≈ 1.
- **Registered fixed buffers + fixed fd** (`IORING_REGISTER_BUFFERS` /
  `IORING_REGISTER_FILES`) — eliminate the per-`recv` buffer allocation that
  `urp-netlink` does today, and the per-op fd refcount. Directly serves rule #4.
- Precedent exists: `urp-bench` already uses the low-level `io-uring = "0.7"`
  crate (no runtime in between) — see [30. urp-bench io_uring](30-urp-bench-io-uring.md).

**Where it does not help.**
- **N = 1** (single endpoint): one round-trip either way; ring setup is pure
  overhead. The blocking path wins.
- The **in-kernel genl message build cost** dominates for large `N`; io_uring
  reduces syscalls and scheduling, not the kernel's fill work.
- The initial **dump is multipart** (reassemble until `NLMSG_DONE`), which is
  fiddlier under io_uring's completion model — so we keep the dump on the
  blocking path and use io_uring *only* for the `N` single-reply GET fan-out
  (each GET reply is one message → trivial seq→buffer mapping).

**Decision procedure (measured, not assumed).**
- Default build = **blocking** (`urp-netlink` as-is). Simplest, smallest,
  correct, and best at the realistic `N` (a node runs a handful to low-tens of
  endpoints).
- Add an **`io-uring` cargo feature** (off by default) that replaces the GET
  fan-out with one submit/reap using registered buffers + fixed fd.
- Benchmark both at `N ∈ {1, 4, 16, 64, 256}` (§39.9). Ship io_uring **only** if
  it shows a material scrape-CPU/latency win at the `N` we actually run; keep it
  as an opt-in for large-fan-out *aggregator* deployments (one exporter scraping
  many endpoints).

**Higher-leverage alternative (noted, kernel-side, out of scope for the exporter
PRs).** Teaching `dumpit` to include the `stats` nest (behind a request flag)
returns everything in *one* multipart dump and makes the `N` GETs — and thus most
of the io_uring argument — disappear. Tracked in §39.11.

**Measured (PR4, 2026-09-01, hp1, `URP_EXPORTER_BENCH`, 200 iters/N).** The
`io-uring` feature was implemented (`urp-netlink::send_batch_uring`: two-phase —
submit all `N` sends, confirm each queued its one reply, then drain `N` recvs
mapped back by `nlmsg_seq`, with a timeout guard; per-batch ring, plain
`Send`/`Recv`, bumped `SO_RCVBUF`) and benchmarked against the blocking serial
GETs by hammering one endpoint `N` times:

| N | blocking | io_uring | speedup |
|---|---|---|---|
| 1 | 5.8 µs | 32.3 µs | **0.18×** |
| 4 | 21.7 µs | 45.0 µs | 0.48× |
| 16 | 88.7 µs | 112.6 µs | 0.79× |
| 64 | 359.9 µs | 387.1 µs | 0.93× |
| 256 | 1445.9 µs | 1475.7 µs | 0.98× |

**io_uring never wins.** At the realistic `N` (a node runs 1–low-tens of
endpoints) it is 2–5× *slower* — the ring setup + two `io_uring_enter`s dominate;
by `N = 256` it only reaches ~parity because the kernel's per-message genl-fill
cost (which io_uring cannot reduce) dominates both paths, exactly as predicted.
**Decision: blocking is the shipped default; the `io-uring` feature stays
off-by-default and opt-in.** It is kept in-tree (feature + the
`urp-exporter-iouring` package) so the benchmark is reproducible and the path can
be re-evaluated if a warm/cached ring or the kernel-side `dumpit`-with-stats
lands — but on today's numbers there is no `N` at which enabling it is justified.

---

## 39.5 Crate + modular nix files

House pattern (from the conventions of `nix/urp-cli.nix` + `nix/checks.nix`):
`rustPlatform.buildRustPackage` + `cargoLock.lockFile = ../Cargo.lock` (vendored,
no crane/naersk/cargoHash), optional `rustToolchain` arg, per-crate `-p` flags,
and a hand-written `src` fileset allowlist replicated in every crate `.nix`.

New / touched files:

| File | Purpose |
|---|---|
| `crates/urp-exporter/` | new binary crate; added to workspace `members` in `Cargo.toml`. Deps: `urp-netlink` (workspace), `libc`; optional `io-uring` (feature). **No** tokio/hyper/serde_json/clap. |
| **`nix/urp-exporter.nix`** (new) | byte-for-byte copy of `nix/urp-cli.nix` with `pname`/`mainProgram`/`cargoBuildFlags`/`cargoTestFlags` → `urp-exporter`, and the `src` filter allowlist extended with `baseName == "urp-exporter"`. |
| `flake.nix` | (1) import block `urpExporter = import ./nix/urp-exporter.nix { inherit pkgs; inherit (packages) rustToolchain; };` (2) `packages.urp-exporter = urpExporter;` (3) `apps.urp-exporter` (4) `checks.urp-exporter-tests`. |
| `nix/checks.nix` | new `urp-exporter-tests` sentinel check (sandboxed `buildRustPackage`, `cargoTestFlags = ["-p" "urp-exporter"]`, `installPhase` → `touch $out/passed`); extend the shared `src` filter allowlist (checks.nix:20-31) with `urp-exporter`. |
| `nix/ci-local.nix` | add an `urp-exporter` build row + the `urp-exporter-tests` check row (so `nix run .#ci-local` covers it — it needs no hardware). |
| **`nix/nixos-exporter-module.nix`** (new) or a `services.urp-exporter` block in `nix/nixos-module.nix` | hardened systemd unit (below). |
| **`nix/urp-exporter-stress.nix`** (new) | mock-backed + HW stress/soak runner (§39.9). |
| `nix/analysis/rust-lints.nix` | add `cargo clippy -p urp-exporter --all-targets --offline 2>&1 \|\| true` and include the crate in the `cargo fmt --all --check`. |
| `nix/shell-functions/build.nix` | extend `run-miri` with `cargo miri test -p urp-exporter`. |
| `docs/design/39-metrics-exporter.md` | this doc. `status.md` gets an observability line. Memory note. |
| `nix/urp-exporter-alerts.yml` + dashboard json | Prometheus alert rules (§39.10) + a Grafana dashboard. |

**`services.urp-exporter` NixOS module** — hardened, CPU-bounded, unprivileged:

```nix
systemd.services.urp-exporter = {
  after = [ "systemd-modules-load.service" ];   # urp.ko loaded by services.urp
  serviceConfig = {
    ExecStart = "${pkg}/bin/urp-exporter --listen 127.0.0.1:${toString cfg.port}";
    DynamicUser = true;
    NoNewPrivileges = true;
    ProtectSystem = "strict";  ProtectHome = true;  PrivateTmp = true;
    RestrictAddressFamilies = [ "AF_NETLINK" "AF_INET" "AF_INET6" ];
    SystemCallFilter = [ "@system-service" ];
    MemoryMax = cfg.memoryMax;   # e.g. "64M" — leak backstop
    CPUQuota = cfg.cpuQuota;     # e.g. "10%"  — the copy-path-theft backstop (§39.3 rule 6)
    Restart = "on-failure";
  };
};
```

---

## 39.6 Static analysis — same bar as the rest of the tree

The repo's lint contract is **advisory** (report-only, `|| true`), not
deny-gated; parity means the exporter is *held to and clean under* the same
tools, wired into the same files:

- **clippy** — `cargo clippy -p urp-exporter --all-targets --offline` line added
  to `nix/analysis/rust-lints.nix`; keep it warning-clean (no `clippy.toml`,
  no `-D warnings` in-tree — advisory, matching every other crate).
- **rustfmt** — `cargo fmt --all --check` clean (default style; no `rustfmt.toml`).
- **miri** — `cargo miri test -p urp-exporter` via the devshell `run-miri`
  helper; the hand-rolled HTTP parser + any `unsafe` in the io_uring path must be
  miri-clean (UB-free).
- **Test check in CI** — `urp-exporter-tests` runs as a sandboxed Nix check under
  `nix flake check` / `ci-local`, exactly like `urp-netlink-tests`.
- **Fuzz** — two `cargo-fuzz`/libFuzzer targets under the design-27 fuzz tier
  (`nix/fuzz-rust.nix`): `fuzz_http_request` (raw bytes → request parser must
  never panic/UB) and `fuzz_netlink_reply` (raw bytes → `Endpoint::parse_top`
  decode must never panic). See [27. Fuzz Testing](27-fuzz-testing.md).

**Gap called out honestly (new, because this is the tree's first network-facing
listener):** the repo has **no `cargo-deny`/`cargo-audit`** today. An exporter
opens a socket and (optionally) adds the `io-uring` dep, i.e. new supply-chain +
attack surface. This doc **recommends adding `cargo-audit` (advisory) as part of
PR3** — a `nix/analysis/rust-audit.nix` in the report-only analysis tier — scoped
initially to the exporter's dependency closure. Not a blocker for the MVP, but
the right place to introduce it.

---

## 39.7 Table-driven unit tests

House style (from `crates/urp-netlink/src/lib.rs:63-93`): plain
`#[cfg(test)] mod tests` with a single `#[test]` iterating `let cases = [ … ]`,
cases labeled *positive / boundary / negative / corner*, a design-number comment,
`.into_iter().enumerate()`, and a `"case {i}: …"` assertion message. No `rstest`,
no `proptest`. Each table below is one such test.

| Test (truth table) | Cases cover |
|---|---|
| `render_endpoint_exposition` | `Endpoint` fixture → exact `/metrics` lines. **positive**: full endpoint w/ stats + 2 QPs; **boundary**: `num_qps=0`, empty `streams`; **negative**: `stats=None` (a dump-only endpoint) → emit gauges but **no** counter series; **corner**: `Option` config fields absent → `info` metric omits them. Also asserts `# HELP`/`# TYPE` emitted exactly once per metric family. |
| `escape_label_value` | raw → escaped: `\` → `\\`, `"` → `\"`, `\n` → `\\n`; **corner**: empty string, a name at the 15-byte kernel limit. |
| `state_to_stateset` | endpoint/QP state string → which `{state=…}` series is `1` and the rest `0`; **corner**: unknown/empty state → an `unknown` series, never a panic. |
| `cardinality_cap` | `(num_qps, num_streams, per_stream, max_series)` → series count ≤ cap, `series_capped_total` incremented past the cap; **boundary**: exactly at cap. |
| `counter_reset_tolerated` | a counter that went *backwards* (endpoint recreated between scrapes) renders as-is (Prometheus handles resets); exporter must not panic or clamp. |
| `http_request_route` | raw request bytes → `{200 /metrics, 200 /, 404, 405 non-GET, 400 malformed, 400 oversized-header, 408 read-timeout}`; **corner**: missing CRLF, pipelined bytes, `HEAD`. |
| `netlink_reply_to_metrics` | decoded `Stats`/`Qp` fixtures → exact metric *values* (golden), guarding the field→metric mapping (`reorder_drops` → `urp_endpoint_reorder_drops_total`, `rtt_ns` → gauge, …). |
| `cache_ttl_gate` | `(elapsed_ms, ttl_ms)` → served-from-cache vs refresh; **boundary**: `elapsed == ttl`. |

The exposition fixtures double as **golden files** — a mismatch is a readable
diff, and they document the exact wire format for dashboard authors.

---

## 39.8 Benchmarking & stress — proving it's fast *and* stable

Four layers, from pure-CPU micro up to 8h-on-hardware:

**(a) Render micro-bench (no hardware, no deps).** A small deterministic bench in
the `urp-bench` spirit (hand-rolled loop, not criterion, to avoid a heavy
dev-dep): render a synthetic fleet of `N ∈ {1,4,16,64,256}` endpoints × `Q ∈
{1,4,8}` QPs into the reused buffer, report `renders/s`, `ns/render`, bytes/render,
and **allocations/render** via a wrapping `GlobalAlloc` counter installed in the
bench. Acceptance: **0 allocations** on the steady-state render path (rule #4);
`ns/render` flat in `N` (linear, no super-linear blowup).

**(b) Mock-backed scrape stress (CI-able, no RDMA).** A **mock netlink server**
test double that speaks the `urp` genl family and returns a configurable synthetic
fleet, so the *exporter* (socket + parse + render + HTTP) can be stressed without
the module or hardware — runnable in a microVM/CI. `nix/urp-exporter-stress.nix`
drives `/metrics` at high frequency (e.g. 100 Hz) for a fixed duration against the
mock and records exporter **CPU%, RSS, fd count, p50/p99 scrape latency, payload
size, and 5xx count**. Gate: RSS flat (no leak), fd count stable (no leak), zero
5xx, p99 under budget.

**(c) Negligible-footprint acceptance test (hardware — the key one).** Run the
exporter scraping at 1 Hz on hp1/hp2/hp3 *during* a `urp-mesh-matrix all2all`
run and assert the aggregate-goodput delta vs the no-exporter baseline is within
measurement noise (target **< 1%**). Because the mesh is per-node-CPU-bound (§38.5),
any CPU the exporter steals surfaces as lost goodput — this test is the direct,
quantitative proof of "extremely efficient." Wired as a mode of
`nix/urp-exporter-stress.nix` (HW branch), reusing the mesh runner.

**(d) 8h soak alongside the mesh soak.** Co-run the exporter with the existing
`urp-mesh-soak` (§ the soak runner from design 38 work); sample exporter RSS/fd
each iteration (mirrors the soak's per-host slab sampling) → RED on RSS growth
> a small threshold or any fd leak over 8h. Bonus synergy: the soak can *scrape
the exporter* to record `urp_endpoint_reorder_drops_total` etc. as a
cross-check of its own CSV.

**(e) Fuzz** (from §39.6): `fuzz_http_request` + `fuzz_netlink_reply` run in the
fuzz tier; a network-facing parser must be fuzzed.

---

## 39.9 Alarms

The error counters map straight to alert rules (shipped as
`nix/urp-exporter-alerts.yml`):

| Alert | Expression (sketch) | Severity |
|---|---|---|
| `URPReorderDrops` | `rate(urp_endpoint_reorder_drops_total[5m]) > 0` | **critical** — loss; the testbed gates on `drops=0` everywhere |
| `URPBufferAllocFails` | `rate(urp_endpoint_buffer_alloc_fails_total[5m]) > 0` | warning — pool exhaustion |
| `URPAuthFailures` | `rate(urp_endpoint_auth_failures_total[5m]) > 0` | warning — PSK/security |
| `URPCreditStallsHigh` | `rate(urp_endpoint_credit_stalls_total[1m]) > <thresh>` | info — flow-control backpressure |
| `URPEndpointNotActive` | `max_over_time(urp_endpoint_state{state="active"}[1m]) == 0` | warning |
| `URPQpUnhealthy` | `urp_qp_state{state="healthy"} == 0` | warning |
| `URPExporterDown` | `up{job="urp"} == 0` | warning |

Graphs the same data unlocks: per-endpoint goodput
(`rate(urp_endpoint_rx_bytes_total)`), frame rate, **per-QP throughput split**
(the exact view that would have *visualized* the mesh startup fairness latch,
§38.5), per-QP `rtt_ns`, reorder-queue depth, live credit levels.

---

## 39.10 Phasing

1. **PR1 — MVP (no hardware, CI-green).** `crates/urp-exporter/` (blocking scrape
   via `urp-netlink` + hand-rolled HTTP + text exposition), the §39.8 table-driven
   tests, `nix/urp-exporter.nix`, flake wiring, `urp-exporter-tests` check,
   `ci-local` rows, clippy/fmt lines.
2. **PR2 — deploy surface.** `services.urp-exporter` hardened module (CPUQuota /
   MemoryMax / DynamicUser), `nix/urp-exporter-alerts.yml`, Grafana dashboard.
3. **PR3 — stress + supply chain.** Mock netlink backend, `nix/urp-exporter-stress.nix`
   (render micro-bench + mock scrape stress), the two fuzz targets, and
   `cargo-audit` in the analysis tier.
4. **PR4 — io_uring (measured, optional).** `io-uring` feature for the GET
   fan-out + the `N`-sweep benchmark; **ship only if it wins** at realistic `N`.
5. **PR5 — hardware validation.** Deploy to hp1/hp2/hp3; run the
   negligible-footprint acceptance test during the mesh and an 8h soak; append
   measured numbers here and flip the `Status:` line.

## 39.11 Follow-ups (open)

- **Kernel: `dumpit`-with-stats.** Add stats/qps to the multipart dump behind a
  request flag → one dump replaces `1 + N` round-trips, largely mooting §39.4.
  Highest-leverage efficiency lever; kernel change, separate from the exporter PRs.
- **`urp-netlink` zero-alloc scrape.** A `fetch_into(&mut scratch)` API so the
  scrape path allocates nothing (today `recv_raw` allocates a `Vec` per receive).
- **Latency/credit gaps.** The module exposes only `rtt_ewma_ns` — no latency
  histograms, no credit-*grant* counters (design 35/36 wishlist in
  [11-metrics.md](11-metrics.md)). Those need new kernel counters before the
  exporter can surface them; out of scope here.

## 39.12 How to reproduce (once implemented)

```
# build + unit tests (no hardware)
nix build -L .#urp-exporter
nix build -L .#checks.x86_64-linux.urp-exporter-tests
nix run   .#ci-local                       # includes the exporter build + tests

# run it against a live module (needs urp.ko loaded)
nix run .#urp-exporter -- --listen 127.0.0.1:9975
curl -s localhost:9975/metrics | grep urp_endpoint_reorder_drops_total

# stress (mock-backed — no RDMA), and the HW footprint/soak checks
nix run .#urp-exporter-stress                 # mock scrape stress + render bench
nix run .#urp-exporter-stress -- --hw hp1 hp2 hp3   # goodput-delta during the mesh
```

Only the `--hw` stress mode and PR5 acceptance/soak need the real testbed; the
crate, tests, mock stress, benches, and fuzz targets are hardware-free and CI-able.
