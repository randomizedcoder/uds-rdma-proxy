# Network Namespaces & Multi-Tenancy

This document specifies how the `urp` kernel module becomes **network-namespace
aware** so that independent tenants — e.g. multiple Redpanda clusters running as
Kubernetes pods on one host — can each use the RDMA fast path in isolation. It
audits every place the module currently hard-codes `init_net`, sets out the
relevant kernel facilities, proposes a design, and gives a phased implementation
plan.

> Status: **design / deferred.** See the Scope decision below — for the currently
> targeted deployment (trusted multi-cluster) **no kernel change is required**.
> The netns/exclusive-device work (Phase 7, §24.10) is retained as the design for
> a *future* untrusted-multi-tenant requirement and is not scheduled.

---

## 24.0 Scope decision (2026-08) — no module change needed today

A key realization refined the scope: because the UDS half of an endpoint stays
**host-side** (§24.5.1 — the operator's chosen "create `unix_path` in the default
namespace and pass it into the container" model), the pod never uses RDMA. The
module's netns-awareness therefore does **not** change any pod's data path. Its
*only* effect would be **hardware-enforced isolation of the RDMA fabric between
mutually-distrusting tenants** sharing one NIC (separate devices → separate QP
number spaces, PDs, buffer pools, resource/DoS domains).

The targeted deployment is **trusted multi-cluster**: several Redpanda clusters
operated by the same (cooperating) operator, needing logical separation but not
hostile-neighbor hardware isolation. For that case:

- **The module already does everything needed.** It supports many named endpoints,
  each with its own UDS path and RDMA bind (exercised today by `test-kmod-k0`'s
  multi-endpoint test). All RDMA can share `init_net` / one device.
- **The remaining work is provisioning, not kernel code:**
  1. a privileged **host agent** (systemd unit or k8s DaemonSet) that calls
     `urp add` to wire up each broker link;
  2. per-tenant socket directories exposed to pods as **shared bind-mount volumes**
     (hostPath/CSI), so host-`urp` and in-pod Redpanda see the same socket inode.
     This works in both directions — whoever creates the socket (urp when it
     listens, Redpanda when it listens), the other side reaches it via the shared
     inode, and pathname AF_UNIX ignores netns for the rendezvous (§24.4.1);
  3. bind-address/port allocation + connection auth (PSK) for logical separation.
- **Accepted trade-off:** all tenants share one RDMA device, so isolation is
  *logical* (auth + addressing + volume mounts), and a buggy/greedy tenant can
  contend for the shared device. Acceptable under a trusted-operator model.

**When to revisit Phase 7:** if the requirement ever becomes *untrusted*
multi-tenant (different customers, hostile neighbors) needing hardware-enforced
RDMA isolation. The full design below stands ready for that day.

---

## 24.1 Motivation

The module today assumes a single host-wide tunnel in the initial network
namespace. The target world is different:

- **Redpanda brokers as pods.** Each broker runs in its own pod (its own network
  + mount namespace) and should reach peer brokers over the RDMA fast path
  instead of TCP.
- **Multi-tenant.** Several *independent* Redpanda clusters (tenants) may run on
  the same Kubernetes cluster — and the same node. Tenant A's traffic and RDMA
  resources must be isolated from tenant B's.
- **Pods should stay simple.** A pod ideally speaks only its normal Kafka Unix
  socket and knows nothing about RDMA, netns modes, or the fabric.

This makes netns the natural **isolation boundary**, and requires the module to
create and track RDMA resources per tenant rather than globally.

## 24.2 Requirements & non-goals

**Requirements**

- **R1 — Per-tenant RDMA isolation.** Each tenant's queue pairs, PDs, CQs, and CM
  IDs live on an RDMA device scoped to that tenant's network namespace
  (hardware-enforced; see §24.4.2). *This is the chosen isolation model.*
- **R2 — Coexistence.** One loaded `urp.ko` serves many tenants concurrently;
  endpoint names collide only *within* a tenant, not across tenants.
- **R3 — Pods speak only UDS.** The pod's application (Redpanda) connects to a
  normal Unix socket; no RDMA lives inside the pod. *(Chosen UDS model — see
  §24.5.1.)*
- **R4 — Lifecycle safety.** When a tenant's netns is destroyed (pod dies), the
  module tears down that tenant's endpoints, QPs, and sockets automatically and
  leaks nothing.
- **R5 — Backward compatible.** With no netns specified, behavior is identical to
  today (everything in `init_net`), so the single-host and existing microVM/rxe
  tests keep working unchanged.

**Non-goals**

- **No per-frame tenant tagging.** The 20-byte wire header is unchanged; identity
  is signaled once at connection setup, not per frame (§24.5.5, §24.8).
- **No RDMA inside unprivileged pods.** RDMA setup (exclusive mode, device
  assignment) is a privileged, host-side operation performed by an agent
  (§24.5.3), not by the pod.
- Not changing the data-path copy model, framing, credit, reorder, or probe
  protocols.

## 24.3 Current `init_net` assumptions (audit)

Every hard-coded namespace touchpoint that must change or be consciously kept:

| Area | Location | Today | Needed |
|---|---|---|---|
| GENL family | `urp_netlink.c:714-724` (`.netnsok = false`) | global, init-only, `GENL_ADMIN_PERM` | tolerate a netns reference on `add`; see §24.5.3 |
| Endpoint store | `urp_endpoint.c:19-33` | one global `rhashtable` keyed by `name` | per-net tables **or** key `(net, name)` (§24.5.6) |
| `/proc` | `urp_proc.c:56` `proc_mkdir("urp", NULL)` | global `/proc/urp/<name>` | per-netns `/proc/net/urp` (§24.5.7) |
| UDS sockets | `urp_socket.c:92,134,172` `sock_create_kern(&init_net,…)` | init_net | **stays host/init_net** by design (§24.5.1) |
| RDMA CM | `urp_rdma.c:929` `rdma_create_id(&init_net,…)` | init_net | use the endpoint's tenant `net` (§24.5.4) |
| Wire header | `urp.h:452` | no tenant field | **unchanged** (§24.8) |
| Conn auth | PSK in `rdma_cm` private_data (Phase 3b) | PSK only | optionally carry tenant token (§24.5.5) |

The key insight from the audit: the **UDS side and the RDMA side of an endpoint
live in different namespaces**, and only the RDMA side needs to become
netns-aware.

## 24.4 Kernel background

### 24.4.1 AF_UNIX and network namespaces

- **Pathname** Unix sockets (`/run/…/kafka.sock`) are identified by a filesystem
  inode. Two processes in *different* network namespaces can communicate over the
  same pathname socket as long as they can both see the inode — AF_UNIX pathname
  sockets are **not** isolated by netns. (Only **abstract** `@name` sockets are
  netns-scoped.)
- A kernel thread runs in `init_net` and in **init's mount namespace / fs
  context**. So `kernel_bind`/`kernel_connect` on a *pathname* from a kernel
  thread resolves that path in the *host* filesystem — it cannot see a pod's
  private mount namespace.

This is exactly why the UDS problem is a *mount*-namespace problem, not a netns
problem, and why the chosen solution (§24.5.1) keeps the socket host-side.

### 24.4.2 RDMA and network namespaces

- `rdma_create_id(struct net *net, …)` is netns-aware: address resolution,
  routing, and device selection all happen within `net`.
- The RDMA subsystem has a host-global **netns mode**:
  - **shared / compat** (default): every RDMA device is visible in every netns.
  - **exclusive**: a device belongs to exactly one netns and is invisible
    elsewhere. Set with `rdma system set netns exclusive`; a device is then moved
    in with `rdma dev set <dev> netns <pid|name>`.
- Exclusive mode is the mechanism that makes R1 *hardware*-enforced: a tenant's
  device (an SR-IOV VF in production, or a per-netns `rdma_rxe` over a veth in
  test) is only usable from inside that tenant's netns. QPs/PDs/CQs created on it
  are therefore automatically tenant-private — **"separate QPs per namespace"
  falls out for free** once the module creates them in the right `net`.
- **Operational caveat:** exclusive mode is a host-wide, effectively one-way
  switch that must be set before devices are namespaced. It is an operator/CNI
  responsibility, not the module's.

### 24.4.3 Per-netns state & lifecycle

- `register_pernet_subsys(&ops)` + `net_generic(net, id)` give each netns a slab
  of module-private state, with `.init(net)` on creation and `.exit(net)` /
  `.exit_batch()` on destruction — the natural home for a per-tenant endpoint
  table and its teardown.
- A `struct net *` obtained for an endpoint must be released with `put_net()`.
  **Do not** hold a long-lived `get_net()` on a tenant's netns — that would pin
  the namespace and block pod teardown. Instead, tear endpoints down from the
  pernet `.exit` hook (§24.5.8).
- Resolving a caller-supplied namespace: `get_net_ns_by_fd(fd)` (an nsfd such as
  `/proc/<pid>/ns/net`) or `get_net_ns_by_pid(pid)`, evaluated in the caller's
  process context inside the GENL `doit` handler.

## 24.5 Design

### 24.5.1 The two-plane split (the core idea)

An endpoint has two sides, and they deliberately live in **different
namespaces**:

```
   host (init_net + host mount ns)                 tenant netns (exclusive RDMA dev)
   ────────────────────────────────                ───────────────────────────────
   UDS pathname socket  ── created by ──►  urp.ko  ── RDMA CM/QP/PD/CQ in ep->net ──► RoCEv2
   /run/urp/<tenant>/kafka.sock                     (rdma_rxe / SR-IOV VF moved
        │  (bind-mounted as a k8s volume)            into the tenant netns)
        ▼
   pod mount ns: Redpanda connects to
   its local /var/lib/redpanda/kafka.sock
```

- **UDS side stays host-side.** The module creates the pathname socket in
  `init_net` / the host filesystem — which the kernel thread *can* do — and the
  host directory is shared into the pod as a volume (a hostPath/CSI-style
  bind-mount, the same pattern k8s already uses for CRI/CSI sockets). Redpanda in
  the pod connects to it as an ordinary local socket (§24.4.1 makes this legal
  across namespaces). **This is the user's "set up `unix_path` in the default
  namespace and pass it into the container" model**, and it means `sock_create_kern`
  can *keep* using `&init_net`.
- **RDMA side is tenant-scoped.** All `rdma_cm`/verbs calls use the endpoint's
  tenant `net`, so QPs land on the tenant's exclusive device (R1).

Net effect: **only the RDMA half of the module becomes netns-aware.** The pod
never touches RDMA (R3).

### 24.5.2 Endpoint gains a tenant `net`

`struct urp_endpoint` gains `struct net *net;` (the RDMA-side namespace).
`--listen-path`/`--connect-path` remain **host** paths. When no namespace is
supplied, `ep->net = &init_net` (R5).

### 24.5.3 Control-plane model: host agent + nsfd reference

Because the UDS lives host-side, `urp add` is naturally driven by a **privileged
host-side agent** (a Kubernetes DaemonSet, or the CNI/device plugin), not from
inside the pod. The agent already has `CAP_NET_ADMIN` on the host and can open
the tenant's `/proc/<pid>/ns/net`.

- New GENL attribute **`URP_A_NETNS_FD`** (an nsfd) — optionally **`URP_A_NETNS_PID`**.
  The `doit` handler resolves it with `get_net_ns_by_fd()` /
  `get_net_ns_by_pid()`, stores the `struct net *` on the endpoint, and
  `put_net()`s it on destroy.
- Absent attribute ⇒ `init_net` (R5).
- GENL family: keep host-agent auth (`GENL_ADMIN_PERM`, CAP_NET_ADMIN on the
  host). *(Alternative — `.netnsok = true` + `GENL_UNS_ADMIN_PERM` + `genl_info_net()`
  so a pod manages its own endpoints from inside its netns — is documented in
  §24.11 as a rejected/optional variant, because it conflicts with the host-side
  UDS model.)*

This keeps the trust boundary clean: one privileged host component programs the
fabric; pods stay unprivileged and RDMA-free.

### 24.5.4 Data-plane changes

Thread `ep->net` through the RDMA layer:

- `rdma_create_id(ep->net, …)` (`urp_rdma.c:929`) instead of `&init_net`.
- Everything downstream (PD, CQ, QP, SRQ, MR, buffer pool) is created on the
  `ib_device` selected within `ep->net`, so it is tenant-private automatically.
- UDS calls in `urp_socket.c` keep `&init_net` (host-side, §24.5.1).

### 24.5.5 Identity signaling: at connection setup, not per frame

RC QPs are connection-scoped, so the acceptor already knows *which tenant* a
frame belongs to from the connection it arrived on. Therefore:

- **No per-frame tenant id.** The 20-byte frame header is unchanged (R-non-goal,
  §24.8).
- **Routing** to the correct acceptor endpoint is by RDMA **bind address/port**
  (each endpoint listens on its own bind), which is already how connections find
  their endpoint.
- **Authorization / identity** is signaled once, in `rdma_cm` **private_data** at
  CM setup — extend the existing Phase 3b PSK blob with an optional **tenant
  token** so the acceptor can reject a connection that targets an endpoint it is
  not entitled to. With fully exclusive per-netns devices (R1) this is
  defense-in-depth; it becomes load-bearing if a device is ever shared.

### 24.5.6 Endpoint store: per-net tables

Two options; **per-net tables** are recommended:

- **(chosen) Per-net `rhashtable`** held in `net_generic` pernet state. Names are
  unique per tenant (R2); teardown is a single table walk from `.exit` (§24.5.8);
  no cross-tenant name leakage.
- (alt) One global table keyed by `(net, name)` — simpler diff but complicates
  netns-exit cleanup and mixes tenants in one structure.

### 24.5.7 `/proc`: per-netns via `/proc/net/urp`

Move the proc root from global `/proc/urp` to **`/proc/net/urp`** (registered via
`proc_net` in the pernet `.init`), so each netns sees only its own endpoints'
stats. A host tool inspecting a tenant enters its netns (`ip netns exec` /
`nsenter`) to read them — matching how all per-netns `/proc/net` state works.

### 24.5.8 Lifecycle & cleanup

- `register_pernet_subsys`: `.init(net)` creates the per-net endpoint table +
  `/proc/net/urp`; `.exit(net)` drains and destroys every endpoint in that net
  (tear down QPs/CM, close the host UDS, free buffers), then removes the proc
  dir.
- Endpoints do **not** pin their tenant `net` with a long-lived `get_net()`
  (§24.4.3). The `.exit` hook runs as the netns is dismantled and is where
  teardown happens; any transient `struct net *` reference is `put_net()`d
  promptly.
- The host-side UDS file (created in `init_net`) is unlinked during endpoint
  destroy regardless of which tenant net is going away — the two planes are torn
  down together.

### 24.5.9 Deployment topologies

- **(A) Host-side per-tenant fabric netns (recommended for Redpanda).** The agent
  creates one netns per *tenant* (not per pod), moves an exclusive RDMA device
  into it, and creates all of that tenant's endpoints there. Pods have **no**
  RDMA and no special CNI — they only get the host-path socket volume. Simplest
  and matches R3 directly.
- **(B) Pod-netns RDMA.** The tenant netns *is* the pod's own netns, with an
  SR-IOV VF / `rdma-cni`-assigned device. Stronger locality but requires RDMA
  plumbing in every pod. Supported by the same `URP_A_NETNS_FD` mechanism, chosen
  by the agent.

Both are the same code path — the agent just decides which `net` an endpoint's
RDMA side binds to.

## 24.6 UAPI / CLI changes

- **UAPI** (`include/uapi/linux/urp.h`): add `URP_A_NETNS_FD` (u32 nsfd) and/or
  `URP_A_NETNS_PID` (u32) to the endpoint attribute set; bump `URP_GENL_VERSION`.
- **CLI** (`crates/urp-cli`): `urp add … --rdma-netns <PATH|PID>` (e.g.
  `--rdma-netns /var/run/netns/tenant-a` or `--rdma-netns-pid 12345`); the CLI
  opens the nsfd and passes it. `--listen-path`/`--connect-path` documented as
  **host** paths. Absent flag ⇒ `init_net`.
- **`urp show`**: report the endpoint's tenant netns (inode id) for observability.

## 24.7 Security

- Isolation is hardware-enforced by exclusive-mode per-netns devices (R1).
- Trust boundary: only the privileged host agent programs endpoints; pods are
  unprivileged and RDMA-free (§24.5.3).
- Host UDS files live under a per-tenant directory (`/run/urp/<tenant>/`) with
  restrictive ownership/mode; only that tenant's pod gets the directory
  bind-mounted.
- CM-setup PSK + optional tenant token authorize each connection (§24.5.5).

## 24.8 Wire protocol impact

**None to the frame format.** The 20-byte header (`urp.h:452`) is untouched. The
only wire change is optional additional bytes in the `rdma_cm` **private_data**
exchanged during connection establishment (a tenant token alongside the existing
PSK). This is the deliberate, argued alternative to per-frame namespace signaling.

## 24.9 Testing

- Extend the single-host harness (`nix/test-kmod-k0.nix` style): create ≥2 named
  netns, put a `rdma_rxe` device in each (exclusive mode where the host allows
  it), `urp add … --rdma-netns nsA` and `--rdma-netns nsB`, and assert:
  - endpoints in nsA are invisible in nsB (`/proc/net/urp` and `urp show`);
  - a tenant-A initiator cannot establish to a tenant-B acceptor (token/bind
    isolation);
  - destroying nsA tears down only nsA's endpoints (leak check on the survivor).
- microVM: a per-tenant Redpanda whose Kafka UDS is a host-path volume, reachable
  over the tenant's fabric netns; two tenants side by side proving isolation.
- Caveat: `rdma system set netns exclusive` is host-global — the harness documents
  this prerequisite and can fall back to shared-mode + token isolation for CI
  where exclusive mode is impractical.

## 24.10 Phased implementation plan (Phase 7 — DEFERRED)

> **Deferred** per §24.0. This plan applies only if/when untrusted-multi-tenant
> hardware isolation is required. It is *not* scheduled for the trusted
> multi-cluster deployment, which needs no module change.


- **7.1 — Endpoint netns plumbing (no behavior change).** Add `URP_A_NETNS_FD`/
  `PID` UAPI + CLI `--rdma-netns`; resolve to `struct net *`, store on the
  endpoint, `put_net()` on destroy. Default `init_net`. *DoD:* existing 23/23
  `test-kmod-k0` still green; `urp show` prints netns; absent flag identical to
  today.
- **7.2 — Data-plane uses `ep->net`.** Switch `rdma_create_id` and downstream
  verbs to `ep->net`; UDS stays `init_net`. *DoD:* endpoint in a non-init netns
  with a per-netns `rdma_rxe` establishes CM and passes echo.
- **7.3 — Per-net tables + pernet lifecycle + `/proc/net/urp`.** `register_pernet_subsys`,
  per-net endpoint table, netns-exit teardown, proc move. *DoD:* two-netns
  isolation + netns-delete cleanup (no leaks under KMEMLEAK).
- **7.4 — Exclusive-mode isolation.** Integrate device-per-netns; isolation tests
  (A-can't-reach-B). *DoD:* cross-tenant establishment refused; QPs confined to
  tenant device.
- **7.5 — Connection-time tenant token.** Extend PSK private_data with the token +
  acceptor authorization. *DoD:* mismatched token rejected with observable error.
- **7.6 — Kubernetes integration + e2e.** DaemonSet `urp-agent`, host-path socket
  volumes, exclusive-mode/device-plugin setup, and a multi-tenant Redpanda e2e
  (two clusters, fast path, isolated). *DoD:* `rpk` metadata over the fast path in
  each tenant; tenants provably isolated.

## 24.11 Open questions / decisions

1. **Control-plane trust model.** Recommended: host-agent + `URP_A_NETNS_FD`
   (§24.5.3), which fits the host-side UDS. The rejected variant is caller-netns
   (`.netnsok=true` + `GENL_UNS_ADMIN_PERM` + `genl_info_net()`), which would let a
   pod self-manage but contradicts host-side socket creation. **Confirm host-agent
   model.**
2. **Primary deployment topology.** Recommended: (A) host-side per-tenant fabric
   netns so pods carry no RDMA. (B) pod-netns RDMA remains available. **Confirm A
   as default.**
3. **Tenant token when devices are fully exclusive.** Keep it as defense-in-depth
   (cheap, and the only guard if a device is ever shared)? Recommended: yes.
4. **`/proc/net/urp` vs keeping global `/proc/urp` with a tenant subdir.**
   Recommended: `/proc/net/urp` (true per-netns semantics).
