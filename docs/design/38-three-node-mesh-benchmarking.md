# 38. Three-Node Full-Mesh Benchmarking — the "2-way" concurrency regime

Status: **Infrastructure landed + first measured results (2026-08-31).** The
RoCEv2 testbed grew from a back-to-back **pair** (hp1↔hp3) to a **3-node full
mesh** (hp1/hp2/hp3, every host directly cabled to the other two). This is the
first time `urp` is exercised where a single node drives/serves **two concurrent
RDMA sessions on its two ports to two different peers at once** — a regime the
back-to-back pair could not reach. All correctness gates stay green
(`reorder_drops=0`, `verify=full` byte-exact); the new results are about
**aggregate ceilings and per-flow fairness under concurrent load**.

This doc extends [32. Real-Hardware Integration Testing](32-real-hardware-integration-testing.md)
(which describes the 2-host pair) with the mesh topology, the new
`urp-mesh-matrix` runner, and the measured numbers.

---

## 38.1 Topology

hp1/hp2/hp3 are identical AMD Ryzen 5 PRO boxes, each with a dual-port
Mellanox ConnectX-4 Lx (25 GbE, `mlx5_core`, fw 14.27.4000), running NixOS on
stock kernel **7.2.0** with jumbo MTU **9700** on the fabric ports. Management
is on a separate 1 GbE (`eno1`); the fabric carries no IP but the RoCEv2 links.

The re-cable put each host's **two** ports onto **different** neighbors, so the
three hosts form a triangle. IPs use a **node-pair /29 scheme**: the third octet
encodes the two node IDs the link joins, the host octet is the node number.

| Edge | Subnet | end A | end B |
|---|---|---|---|
| hp1↔hp2 | `10.10.12.0/29` | hp1 `enp1s0f1np1` = `.1` | hp2 `enp1s0f0np0` = `.2` |
| hp1↔hp3 | `10.10.13.0/29` | hp1 `enp1s0f0np0` = `.1` | hp3 `enp1s0f1np1` = `.3` |
| hp2↔hp3 | `10.10.23.0/29` | hp2 `enp1s0f1np1` = `.2` | hp3 `enp1s0f0np0` = `.3` |

```
            10.10.12.0/29
      hp1 ------------------- hp2
        \                    /
 10.10.13 \                / 10.10.23
            \            /
             \          /
               hp3
```

Each 25 GbE link is full-duplex (25 Gb/s each way). Each **node** therefore has
2×25 = 50 Gb/s of port bandwidth, but — as the results show — a single node's
`urp` copy-path/CPU tops out well below that.

## 38.2 The runner: `urp-mesh-matrix`

`nix run .#urp-mesh-matrix -- [h1 h2 h3]` (default `hp1 hp2 hp3`), source in
[`nix/urp-mesh-matrix.nix`](../../nix/urp-mesh-matrix.nix). It is modelled on
`urp-f2-matrix` — it creates its **own** per-run endpoints (`ma_*` acceptors /
`mi_*` initiators) and never touches the declarative `services.urp` pair, so it
composes with a live deployment. Unlike the two-host runners it tracks a
**per-flow acceptor host**, so sinks can live on different nodes simultaneously.

A **directed flow** `s→d` makes `d` the acceptor/sink (binds its edge IP, runs
`urp-bench --listen`) and `s` the initiator/source (`urp-bench --connect`); data
flows `s→d`. Scenarios are just lists of `(s,d)` pairs, launched concurrently;
the runner starts every sink (waits for each UDS to bind — the acceptor
lazy-connects on the first frame and does not retry), launches all sources at
once, then sums each sink's `BENCH_OK mbps` and diffs `reorder-drops`.

Scenarios (`URP_MESH_SCENARIOS` to filter):

| Name | Flows | What it stresses |
|---|---|---|
| `per-edge` | 1 (×3, sequential) | single-link baseline, one edge at a time |
| `hub-rx` | 2 (×3 hubs) | one node **receiving** from both neighbors at once (RX 2-way) |
| `hub-tx` | 2 (×3 hubs) | one node **sending** to both neighbors at once (TX 2-way) |
| `ring` | 3 | `1→2→3→1`: every node sends on one port, receives on the other |
| `all2all` | 6 | all directed flows: every node sends **and** receives on both ports |

Defaults: `bufsize=65516`, `bufcount=1024`, `num_qps=1`, `dur=5`, `verify=none`
(gate on `reorder_drops=0` + all sinks reporting). Env overrides documented in
the file header.

## 38.3 Correctness (all edges)

`nix run .#urp-reorder-matrix` on each of the three edges — buffer_size
{68, 4096, 65516} × num_qps {1, 4, 8}, `verify=full`:

| Edge | cells | result |
|---|---|---|
| hp1↔hp3 | 9/9 PASS | `reorder_drops=0`, byte-exact |
| hp1↔hp2 | 9/9 PASS | `reorder_drops=0`, byte-exact |
| hp2↔hp3 | 9/9 PASS | `reorder_drops=0`, byte-exact |

Multi-QP reorder is exercised (`insertions>0` at qps 4/8, `=0` at qps 1) and
delivered byte-exact on every edge, including with **hp2 as acceptor** — the
newly-aligned node behaves identically to the original pair.

## 38.4 Single-edge baseline (`urp-bw-matrix`, verify=none)

Sink-measured goodput, `num_qps=1`, one stream per edge:

| buffer | hp1↔hp3 | hp1↔hp2 | hp2↔hp3 | % of 25 GbE |
|---|---|---|---|---|
| 4 KiB | 1285 MB/s | 1426 | 1291 | ~41–46% |
| 16 KiB | 2472 | 2528 | 2476 | ~79–81% |
| 64 KiB | **2862** | **2992** | **2855** | **~91–96%** |

The three edges track each other to within noise — hp2's edges are first-class.
(The 4 KiB point is copy-path/frame-rate bound; 64 KiB approaches line. See
[34](34-bulk-throughput.md)/[37](37-high-throughput-clustering.md) for the
single-stream copy-vs-fast story.)

## 38.5 Concurrent 2-way results (`urp-mesh-matrix`, bufsize=65516)

Steady-state (median of repeated runs; `reorder_drops=0` on every point):

| Scenario | per-flow MB/s | aggregate MB/s | note |
|---|---|---|---|
| per-edge (1 flow) | 2982–3333 | 2982–3333 | one node, one RX stream |
| **hub-rx** (2 flows→1 node) | 1750–1955 | **3620–3901** | one node, **2 RX** streams |
| **hub-tx** (1 node→2) | 1810–2032 | **3752–3842** | one node, **2 TX** streams |
| **ring** (3 flows) | 1667–1795 | **~5100** | every node 1 RX + 1 TX |
| **all2all** (6 flows) | 858–960 | **~5500** | every node 2 RX + 2 TX |

### The headline: a per-node processing ceiling ≈ 3.7–3.9 GB/s

- A node **receiving on both ports at once** (`hub-rx`) aggregates ~3.6–3.9
  GB/s — **more** than a single stream (~3.3) but nowhere near 2× (which would
  be ~6.6). The two flows split it fairly (e.g. 1946 vs 1955 MB/s).
- A node **sending on both ports** (`hub-tx`) shows the same ~3.75–3.84 GB/s
  ceiling, also fairly split.
- `ring` and `all2all` aggregate **higher** (5.1 / 5.5 GB/s) precisely because
  the load is spread across **all three nodes** — but each individual node is
  still working at ~its ceiling: in `all2all` each node carries 2 RX + 2 TX ≈
  4 × 0.9 ≈ 3.6 GB/s, matching the hub number.

Byte-correctness holds under concurrency too: re-running `ring` and `all2all`
with `URP_MESH_VERIFY=full` (every sink byte-verifies its stream) keeps all
sinks `BENCH_OK` with `reorder_drops=0` — aggregate drops to ~1.3–1.8 GB/s
because per-byte verification is CPU-heavy and competes with the copy path,
which only reinforces the node-CPU-bound picture below.

So the mesh is **node-CPU / copy-path bound, not fabric bound**: the fabric
offers 150 Gb/s of directed capacity across six half-links, but three nodes each
capped near ~3.9 GB/s cap the system at ~5.5 GB/s aggregate. This is the same
copy-path membw ceiling seen single-stream in design 37, now shown to be a
**per-node** budget shared across a node's concurrent flows on both ports. The
zero-copy fast path (design 31) is the lever that should lift it — pointing
`urp-fast-f2-matrix`-style fast endpoints at the mesh edges is the next step.

### Fairness is good in steady state, with an intermittent startup latch

When all flows warm up cleanly, per-flow fairness is good: hub/ring/all2all
split their node's budget evenly (within ~5–10%). **However**, intermittently
(~1 in 3 `all2all` runs, and once in `ring`) a **single directed flow latches
into a starved ~4.9 MB/s state at startup** and stays there for the whole run,
while its peers — including the reverse direction on the same physical link —
run full and absorb the slack. Observed: `hp1→hp2` most often; once the whole
`hp1↔hp3` edge. Critically, **`reorder_drops=0` and bytes stay exact** even when
a flow is starved, and re-running clears it — so this is a **startup
credit/scheduling race**, not a steady-state fairness bug or a correctness
problem. It is a genuinely new behavior the pair testbed never surfaced.

**Follow-up (open):** characterize and fix the startup starvation latch —
suspected a credit/pump warm-up ordering race when a node brings up concurrent
RX+TX endpoints on both ports at once (the loser latches at ~1 credit batch).
Repro: `URP_MESH_SCENARIOS=all2all nix run .#urp-mesh-matrix -- hp1 hp2 hp3`,
re-run a few times. Not a data-integrity issue (drops=0), so low urgency, but it
caps worst-case QoS under full mesh load.

## 38.6 How to reproduce

```
# correctness, per edge
nix run .#urp-reorder-matrix -- hp1 hp3 10.10.13.1
nix run .#urp-reorder-matrix -- hp1 hp2 10.10.12.1
nix run .#urp-reorder-matrix -- hp2 hp3 10.10.23.2

# single-edge goodput baseline
nix run .#urp-bw-matrix -- hp1 hp3 10.10.13.1     # (etc. per edge)

# the full concurrent 2-way campaign
nix run .#urp-mesh-matrix -- hp1 hp2 hp3
# or one scenario:
URP_MESH_SCENARIOS=hub-rx nix run .#urp-mesh-matrix -- hp1 hp2 hp3
```

All runners SSH in as root, assert `urp.ko` is loaded (the `services.urp`
NixOS module does that), stage the bench closure with `nix copy`, and clean up
their endpoints on exit. None is in `ci-local` — they need the real hardware.
