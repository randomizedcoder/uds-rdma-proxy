# Provisioning Layer (trusted multi-cluster) — outline

Per the scope decision in [§24.0](24-network-namespaces.md), the trusted
multi-cluster deployment needs **no kernel change** — the module already supports
multiple host-side UDS endpoints. The remaining work is a **userspace / ops
provisioning layer** that wires Redpanda pods to the fast path. This document is
an *outline* to remember what to build; it is not yet a full design.

> Status: **outline / not started.** Prerequisite reading:
> [24 §24.0](24-network-namespaces.md), [18-deployment.md](18-deployment.md),
> and the Redpanda metadata-only finding in the `test-redpanda-uds` harness.

## 25.1 Components

1. **`urp-agent` (privileged host daemon; systemd unit or k8s DaemonSet)**
   - Reconciles a *desired set of tunnels* (from a ConfigMap / CRD / static file)
     against live endpoints by calling the existing `urp` CLI (`add` / `remove` /
     `drain` / `show`).
   - Loads the module + sets up soft-RoCE (`rdma_rxe`) or binds the hardware
     device; ensures `ib_core`/`rdma_cm`/`rdma_rxe`.
   - Owns **bind addr:port allocation** per endpoint and records the
     broker→node→bind mapping so peers can be told where to connect.
   - Manages **PSK** material (from a k8s Secret) for connection auth.
   - Handles teardown/GC when a broker pod or link goes away.

2. **Socket sharing (host ↔ pod)**
   - Per-tenant host directory (e.g. `/run/urp/<tenant>/`) exposed to the pod as a
     **shared bind-mount volume** (hostPath or CSI), so host-`urp` and in-pod
     Redpanda see the same socket inode (works both directions; pathname AF_UNIX
     ignores netns for the rendezvous — see [24 §24.4.1](24-network-namespaces.md)).
   - Directory ownership/mode scoped so only that tenant's pod mounts it.

3. **Redpanda wiring**
   - `kafka_api` entry with `unix_path` pointing **into the shared volume** (plus a
     TCP entry, since UDS is non-advertisable).
   - **Open data-plane item:** rpk/franz uses UDS only for the *initial metadata
     fetch*; produce/consume follows the advertised **TCP** endpoint. Full
     produce/consume over the fast path therefore needs the advertised port bridged
     too (see the "full produce/consume" follow-up in the personal plan / harness
     notes). Cross-broker **RPC** (Seastar `rpc_server`) is a separate bridge and
     an open question for the mesh case.

4. **Kubernetes integration**
   - DaemonSet: module load + `urp-agent` + rxe/device setup (privileged /
     `CAP_NET_ADMIN`, host RDMA access).
   - A **CRD or ConfigMap** describing tunnels (tenant, broker pair, socket paths,
     peer node/bind). Volume manifests for the socket dirs. PSK Secret.
   - Peer discovery: map each broker's advertised-RPC/Kafka identity to the node +
     bind where its `urp` acceptor lives.

## 25.2 Security & observability
- Host agent holds the capabilities; pods stay unprivileged + RDMA-free.
- Socket-dir perms as the pod-facing boundary; PSK per tenant.
- Map `urp show` / `/proc/urp/<name>/stats` to per-tenant metrics.

## 25.3 Open questions (carry forward)
- Full **produce/consume over RDMA** (advertised TCP data-plane bridging) — the
  metadata-only limitation is a real gap for a data-plane speedup.
- **Inter-broker RPC** over the fast path (mesh) — bridging `rpc_server`.
- Peer/bind **addressing scheme** across nodes and how the agent discovers it.
- Lifecycle/GC coupling to pod lifetimes (who removes an endpoint when a pod dies).
