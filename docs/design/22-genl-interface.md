# Generic Netlink Interface

The kernel module uses Generic Netlink (GENL) for all runtime configuration — creating, modifying, querying, and removing endpoints. This follows the standard Linux networking pattern established by WireGuard (`wg`), devlink, nl80211, and OVS.

GENL replaces the configfs-based approach initially considered in [Section 21.4D](21-kernel-module.md#214-implementation-approaches). The kernel community strongly favors GENL for dynamic network device configuration, reserving configfs for static device trees (USB gadgets, target subsystem). GENL provides atomic multi-attribute operations, structured request/response with type-safe attributes, and multicast event notifications — none of which configfs supports cleanly.

## 22.1 GENL Family Registration

The module registers a single GENL family `"urp"` during `module_init`:

```c
#include <net/genetlink.h>

static const struct genl_multicast_group urp_mcgrps[] = {
    { .name = "events" },
};

static const struct genl_split_ops urp_split_ops[] = {
    { .cmd = URP_CMD_NEW_ENDPOINT, .doit = urp_new_endpoint,
      .flags = GENL_ADMIN_PERM,
      .policy = urp_endpoint_policy, .maxattr = URP_ENDPOINT_A_MAX, },
    { .cmd = URP_CMD_DEL_ENDPOINT, .doit = urp_del_endpoint,
      .flags = GENL_ADMIN_PERM,
      .policy = urp_endpoint_policy, .maxattr = URP_ENDPOINT_A_MAX, },
    { .cmd = URP_CMD_SET_ENDPOINT, .doit = urp_set_endpoint,
      .flags = GENL_ADMIN_PERM,
      .policy = urp_endpoint_policy, .maxattr = URP_ENDPOINT_A_MAX, },
    { .cmd = URP_CMD_GET_ENDPOINT, .doit = urp_get_endpoint,
      .policy = urp_endpoint_policy, .maxattr = URP_ENDPOINT_A_MAX, },
    { .cmd = URP_CMD_GET_ENDPOINT, .dumpit = urp_dump_endpoints, },
};

static struct genl_family urp_genl_family = {
    .name       = URP_GENL_NAME,    /* "urp" */
    .version    = URP_GENL_VERSION, /* 1 */
    .maxattr    = URP_A_MAX,
    .policy     = urp_top_policy,
    .module     = THIS_MODULE,
    .split_ops      = urp_split_ops,
    .n_split_ops    = ARRAY_SIZE(urp_split_ops),
    .mcgrps     = urp_mcgrps,
    .n_mcgrps   = ARRAY_SIZE(urp_mcgrps),
};
```

Registration uses `genl_split_ops` (the modern kernel API, replacing the older combined `genl_ops`). This allows the same command (`URP_CMD_GET_ENDPOINT`) to have separate `doit` and `dumpit` handlers.

Write operations (`NEW`, `DEL`, `SET`) require `CAP_NET_ADMIN` via `GENL_ADMIN_PERM`. Read operations (`GET`) are unprivileged — any user can query endpoint state and stats.


## 22.2 Commands

Unlike WireGuard's 2-command model (`GET_DEVICE`, `SET_DEVICE`), we use **4 explicit commands** because endpoints are first-class resources with distinct create/delete semantics. WireGuard's `SET_DEVICE` implicitly creates peers if they don't exist and removes them via a `REMOVE_ME` flag — this overloading is appropriate for WireGuard's public-key-based peer model but awkward for named resource management.

| Command | Value | Handler | NLM Flags | Description |
|---------|-------|---------|-----------|-------------|
| `URP_CMD_NEW_ENDPOINT` | 1 | `doit` | `NLM_F_REQUEST` | Create endpoint: validate config, allocate `struct urp_endpoint`, set up RDMA CM, create virtual UDS socket |
| `URP_CMD_DEL_ENDPOINT` | 2 | `doit` | `NLM_F_REQUEST` | Drain active streams (FIN), tear down QPs, remove UDS socket file, free resources |
| `URP_CMD_SET_ENDPOINT` | 3 | `doit` | `NLM_F_REQUEST` | Modify mutable attributes on a live endpoint (e.g., add QPs, change password) |
| `URP_CMD_GET_ENDPOINT` | 4 | `doit` | `NLM_F_REQUEST` | Get one endpoint by name: config + state + per-QP stats + per-stream stats |
| `URP_CMD_GET_ENDPOINT` | 4 | `dumpit` | `NLM_F_REQUEST \| NLM_F_DUMP` | Dump all endpoints (iterates endpoint list, emits one `NLMSG` per endpoint) |

### Command Flow

```
 urp add peer-b --listen-path /var/run/urp/to-B.sock --peer-address 10.0.1.2:4791
   |
   v
 Userspace (urp CLI):
   socket(AF_NETLINK, SOCK_DGRAM, NETLINK_GENERIC)
   resolve family ID for "urp" via GENL controller (CTRL_CMD_GETFAMILY)
   |
   v
 Build request:
   nlmsghdr  { type=family_id, flags=NLM_F_REQUEST|NLM_F_ACK }
   genlmsghdr { cmd=URP_CMD_NEW_ENDPOINT, version=1 }
   NLA_NESTED(URP_A_ENDPOINT):
     NLA_STRING(URP_ENDPOINT_A_NAME, "peer-b")
     NLA_STRING(URP_ENDPOINT_A_LISTEN_PATH, "/var/run/urp/to-B.sock")
     NLA_BINARY(URP_ENDPOINT_A_PEER_ADDR, sockaddr_in6{10.0.1.2:4791})
   |
   v
 Kernel (urp_new_endpoint):
   1. Parse nested URP_A_ENDPOINT attributes via nla_parse_nested()
   2. Validate: name unique, at least one UDS path set, RDMA addr present
   3. Allocate struct urp_endpoint, populate from attributes
   4. Start RDMA CM: rdma_create_id(), rdma_resolve_addr()
   5. Create virtual UDS socket: sock_create_kern(), kernel_bind(), kernel_listen()
   6. Insert into endpoint hash table (rhashtable)
   7. Send multicast event to "events" group
   |
   v
 Reply:
   NLMSG_ERROR with error=0 (success) or -errno (failure + extack message)
```


## 22.3 Attribute Hierarchy

The attribute structure follows a **3-level nesting pattern** mirroring WireGuard's `device -> peer -> allowedip`:

```
Top-level (urp)  ->  Endpoint  ->  QP / Stream / Stats
```

### 22.3.1 Top-Level Attributes

```c
enum urp_attr {
    URP_A_UNSPEC,
    URP_A_ENDPOINT,          /* NLA_NESTED — contains endpoint attributes */
    __URP_A_MAX,
};
```

The top-level has a single attribute: `URP_A_ENDPOINT`. For `NEW`/`DEL`/`SET`/`GET` (`doit`), the message contains one nested endpoint. For `GET` (`dumpit`), the reply contains one endpoint per `NLMSG`.

### 22.3.2 Endpoint Attributes

```c
enum urp_endpoint_attr {
    URP_ENDPOINT_A_UNSPEC,

    /* Configuration (read-write on NEW, subset writable on SET) */
    URP_ENDPOINT_A_NAME,             /* NLA_NUL_STRING, max 16 — unique lookup key */
    URP_ENDPOINT_A_LISTEN_PATH,      /* NLA_NUL_STRING — UDS listen mode path */
    URP_ENDPOINT_A_CONNECT_PATH,     /* NLA_NUL_STRING — UDS connect mode path */
    URP_ENDPOINT_A_RDMA_DEVICE,      /* NLA_NUL_STRING — e.g. "mlx5_0" */
    URP_ENDPOINT_A_PEER_ADDR,        /* NLA_BINARY, sizeof(struct sockaddr_in6) — RDMA initiator target */
    URP_ENDPOINT_A_BIND_ADDR,        /* NLA_BINARY, sizeof(struct sockaddr_in6) — RDMA acceptor listen */
    URP_ENDPOINT_A_NUM_QPS,          /* NLA_U32 — number of QPs (1-32) */
    URP_ENDPOINT_A_BUFFER_COUNT,     /* NLA_U32 — total buffer pool slots */
    URP_ENDPOINT_A_BUFFER_SIZE,      /* NLA_U32 — per-buffer size in bytes */
    URP_ENDPOINT_A_PASSWORD,         /* NLA_NUL_STRING — Tier 0.5 PSK (write-only, max 16 chars) */

    /* State (read-only, returned by GET) */
    URP_ENDPOINT_A_STATE,            /* NLA_U8 — endpoint state enum */

    /* Nested children (read-only, returned by GET) */
    URP_ENDPOINT_A_QPS,              /* NLA_NESTED — array of QP attributes */
    URP_ENDPOINT_A_STREAMS,          /* NLA_NESTED — array of stream attributes */
    URP_ENDPOINT_A_STATS,            /* NLA_NESTED — aggregate statistics */

    __URP_ENDPOINT_A_MAX,
};
```

**Mutability rules**:

| Attribute | NEW | SET | GET | DEL |
|-----------|-----|-----|-----|-----|
| `NAME` | required | required (lookup key) | optional (filter) | required |
| `LISTEN_PATH` | optional | immutable | returned | - |
| `CONNECT_PATH` | optional | immutable | returned | - |
| `RDMA_DEVICE` | optional | immutable | returned | - |
| `PEER_ADDR` | optional | immutable | returned | - |
| `BIND_ADDR` | optional | immutable | returned | - |
| `NUM_QPS` | optional (default 1) | **mutable** (add QPs) | returned | - |
| `BUFFER_COUNT` | optional (default 1024) | **mutable** | returned | - |
| `BUFFER_SIZE` | optional (default 4076) | immutable | returned | - |
| `PASSWORD` | optional | **mutable** | **never returned** | - |
| `STATE` | - | via `urp drain` | returned | - |
| `QPS` | - | - | returned | - |
| `STREAMS` | - | - | returned | - |
| `STATS` | - | - | returned | - |

The `PASSWORD` attribute is **write-only** — it is accepted on `NEW` and `SET` but never included in `GET` responses. The kernel stores the SHA-256 hash, not the plaintext.

**Validation on NEW**:
- At least one of `LISTEN_PATH` or `CONNECT_PATH` must be set
- At least one of `PEER_ADDR` or `BIND_ADDR` must be set
- `PEER_ADDR` and `BIND_ADDR` are mutually exclusive (RDMA initiator vs. acceptor)
- `NAME` must be unique across all endpoints
- `NUM_QPS` must be 1-32
- `BUFFER_SIZE` must be >= `FRAME_HEADER_SIZE` (20 bytes) and <= PMTU limit

### 22.3.3 QP Attributes (read-only)

```c
enum urp_qp_attr {
    URP_QP_A_UNSPEC,
    URP_QP_A_INDEX,              /* NLA_U32 — QP index within the endpoint */
    URP_QP_A_STATE,              /* NLA_U8 — QP health state (Qualifying/Active/Draining/Removed) */
    URP_QP_A_RTT_NS,            /* NLA_U64 — last measured probe RTT in nanoseconds */
    URP_QP_A_TX_BYTES,          /* NLA_U64 — total bytes sent on this QP */
    URP_QP_A_RX_BYTES,          /* NLA_U64 — total bytes received on this QP */
    URP_QP_A_TX_FRAMES,         /* NLA_U64 — total frames sent on this QP */
    URP_QP_A_RX_FRAMES,         /* NLA_U64 — total frames received on this QP */
    __URP_QP_A_MAX,
};
```

Returned as a nested array inside `URP_ENDPOINT_A_QPS` — one nested block per QP.

### 22.3.4 Stream Attributes (read-only)

```c
enum urp_stream_attr {
    URP_STREAM_A_UNSPEC,
    URP_STREAM_A_ID,             /* NLA_U32 — stream ID */
    URP_STREAM_A_STATE,          /* NLA_U8 — SYN_SENT, ESTABLISHED, FIN_WAIT, CLOSED */
    URP_STREAM_A_TX_BYTES,       /* NLA_U64 — bytes sent on this stream */
    URP_STREAM_A_RX_BYTES,       /* NLA_U64 — bytes received on this stream */
    URP_STREAM_A_REORDER_DEPTH,  /* NLA_U32 — current out-of-order frames buffered */
    URP_STREAM_A_CREDITS_LOCAL,  /* NLA_U16 — local credits available */
    URP_STREAM_A_CREDITS_REMOTE, /* NLA_U16 — credits granted by remote */
    __URP_STREAM_A_MAX,
};
```

Returned as a nested array inside `URP_ENDPOINT_A_STREAMS` — one nested block per active stream. Closed streams are not included.

### 22.3.5 Aggregate Stats Attributes (read-only)

```c
enum urp_stats_attr {
    URP_STATS_A_UNSPEC,
    URP_STATS_A_ACTIVE_STREAMS,       /* NLA_U32 — current active stream count */
    URP_STATS_A_TX_BYTES,             /* NLA_U64 — total bytes transmitted */
    URP_STATS_A_RX_BYTES,             /* NLA_U64 — total bytes received */
    URP_STATS_A_TX_FRAMES,            /* NLA_U64 — total frames transmitted */
    URP_STATS_A_RX_FRAMES,            /* NLA_U64 — total frames received */
    URP_STATS_A_CREDIT_STALLS,        /* NLA_U64 — times a send blocked waiting for credits */
    URP_STATS_A_REORDER_INSERTIONS,   /* NLA_U64 — frames inserted into reorder buffer */
    URP_STATS_A_REORDER_DROPS,        /* NLA_U64 — frames dropped (buffer overflow, duplicate seq) */
    URP_STATS_A_BUFFER_ALLOC_FAILS,   /* NLA_U64 — buffer allocation failures */
    URP_STATS_A_AUTH_FAILURES,        /* NLA_U64 — PSK auth mismatches */
    __URP_STATS_A_MAX,
};
```

Aggregate counters across all QPs and streams for the endpoint. These are monotonic counters (never reset), suitable for rate computation in monitoring tools.


## 22.4 Kernel Policy Arrays

Each attribute level has a corresponding `nla_policy` array for kernel-side validation. The kernel's netlink infrastructure validates attributes against these policies before the handler is called — type mismatches, missing required attributes, and oversized strings are rejected automatically.

```c
static const struct nla_policy urp_top_policy[URP_A_MAX + 1] = {
    [URP_A_ENDPOINT]      = NLA_POLICY_NESTED(urp_endpoint_policy),
};

static const struct nla_policy urp_endpoint_policy[URP_ENDPOINT_A_MAX + 1] = {
    [URP_ENDPOINT_A_NAME]         = { .type = NLA_NUL_STRING, .len = 16 },
    [URP_ENDPOINT_A_LISTEN_PATH]  = { .type = NLA_NUL_STRING, .len = 108 }, /* sun_path max */
    [URP_ENDPOINT_A_CONNECT_PATH] = { .type = NLA_NUL_STRING, .len = 108 },
    [URP_ENDPOINT_A_RDMA_DEVICE]  = { .type = NLA_NUL_STRING, .len = 64 },
    [URP_ENDPOINT_A_PEER_ADDR]    = NLA_POLICY_EXACT_LEN(sizeof(struct sockaddr_in6)),
    [URP_ENDPOINT_A_BIND_ADDR]    = NLA_POLICY_EXACT_LEN(sizeof(struct sockaddr_in6)),
    [URP_ENDPOINT_A_NUM_QPS]      = NLA_POLICY_RANGE(NLA_U32, 1, 32),
    [URP_ENDPOINT_A_BUFFER_COUNT] = NLA_POLICY_MIN(NLA_U32, 16),
    [URP_ENDPOINT_A_BUFFER_SIZE]  = NLA_POLICY_RANGE(NLA_U32, 20, 65536),
    [URP_ENDPOINT_A_PASSWORD]     = { .type = NLA_NUL_STRING, .len = 16 },
    [URP_ENDPOINT_A_STATE]        = { .type = NLA_U8 },
    [URP_ENDPOINT_A_QPS]          = NLA_POLICY_NESTED(urp_qp_policy),
    [URP_ENDPOINT_A_STREAMS]      = NLA_POLICY_NESTED(urp_stream_policy),
    [URP_ENDPOINT_A_STATS]        = NLA_POLICY_NESTED(urp_stats_policy),
};

static const struct nla_policy urp_qp_policy[URP_QP_A_MAX + 1] = {
    [URP_QP_A_INDEX]      = { .type = NLA_U32 },
    [URP_QP_A_STATE]      = { .type = NLA_U8 },
    [URP_QP_A_RTT_NS]    = { .type = NLA_U64 },
    [URP_QP_A_TX_BYTES]  = { .type = NLA_U64 },
    [URP_QP_A_RX_BYTES]  = { .type = NLA_U64 },
    [URP_QP_A_TX_FRAMES] = { .type = NLA_U64 },
    [URP_QP_A_RX_FRAMES] = { .type = NLA_U64 },
};

static const struct nla_policy urp_stream_policy[URP_STREAM_A_MAX + 1] = {
    [URP_STREAM_A_ID]             = { .type = NLA_U32 },
    [URP_STREAM_A_STATE]          = { .type = NLA_U8 },
    [URP_STREAM_A_TX_BYTES]       = { .type = NLA_U64 },
    [URP_STREAM_A_RX_BYTES]       = { .type = NLA_U64 },
    [URP_STREAM_A_REORDER_DEPTH]  = { .type = NLA_U32 },
    [URP_STREAM_A_CREDITS_LOCAL]  = { .type = NLA_U16 },
    [URP_STREAM_A_CREDITS_REMOTE] = { .type = NLA_U16 },
};

static const struct nla_policy urp_stats_policy[URP_STATS_A_MAX + 1] = {
    [URP_STATS_A_ACTIVE_STREAMS]      = { .type = NLA_U32 },
    [URP_STATS_A_TX_BYTES]            = { .type = NLA_U64 },
    [URP_STATS_A_RX_BYTES]            = { .type = NLA_U64 },
    [URP_STATS_A_TX_FRAMES]           = { .type = NLA_U64 },
    [URP_STATS_A_RX_FRAMES]           = { .type = NLA_U64 },
    [URP_STATS_A_CREDIT_STALLS]       = { .type = NLA_U64 },
    [URP_STATS_A_REORDER_INSERTIONS]  = { .type = NLA_U64 },
    [URP_STATS_A_REORDER_DROPS]       = { .type = NLA_U64 },
    [URP_STATS_A_BUFFER_ALLOC_FAILS]  = { .type = NLA_U64 },
    [URP_STATS_A_AUTH_FAILURES]       = { .type = NLA_U64 },
};
```


## 22.5 UAPI Header

`include/uapi/linux/urp.h` — the user-kernel ABI contract. Can be hand-written or auto-generated from the YAML spec ([Section 22.6](#226-yaml-netlink-spec)).

```c
/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR MIT) */

#ifndef _UAPI_LINUX_URP_H
#define _UAPI_LINUX_URP_H

#define URP_GENL_NAME    "urp"
#define URP_GENL_VERSION  1

/* Commands */
enum urp_cmd {
    URP_CMD_NEW_ENDPOINT = 1,
    URP_CMD_DEL_ENDPOINT,
    URP_CMD_SET_ENDPOINT,
    URP_CMD_GET_ENDPOINT,
    __URP_CMD_MAX,
};
#define URP_CMD_MAX (__URP_CMD_MAX - 1)

/* Top-level attributes */
enum urp_attr {
    URP_A_UNSPEC,
    URP_A_ENDPOINT,          /* NLA_NESTED */
    __URP_A_MAX,
};
#define URP_A_MAX (__URP_A_MAX - 1)

/* Endpoint attributes */
enum urp_endpoint_attr {
    URP_ENDPOINT_A_UNSPEC,
    URP_ENDPOINT_A_NAME,             /* NLA_NUL_STRING, max 16 */
    URP_ENDPOINT_A_LISTEN_PATH,      /* NLA_NUL_STRING */
    URP_ENDPOINT_A_CONNECT_PATH,     /* NLA_NUL_STRING */
    URP_ENDPOINT_A_RDMA_DEVICE,      /* NLA_NUL_STRING */
    URP_ENDPOINT_A_PEER_ADDR,        /* NLA_BINARY, struct sockaddr_in6 */
    URP_ENDPOINT_A_BIND_ADDR,        /* NLA_BINARY, struct sockaddr_in6 */
    URP_ENDPOINT_A_NUM_QPS,          /* NLA_U32 */
    URP_ENDPOINT_A_BUFFER_COUNT,     /* NLA_U32 */
    URP_ENDPOINT_A_BUFFER_SIZE,      /* NLA_U32 */
    URP_ENDPOINT_A_PASSWORD,         /* NLA_NUL_STRING, write-only */
    URP_ENDPOINT_A_STATE,            /* NLA_U8, enum urp_endpoint_state */
    URP_ENDPOINT_A_QPS,              /* NLA_NESTED, array */
    URP_ENDPOINT_A_STREAMS,          /* NLA_NESTED, array */
    URP_ENDPOINT_A_STATS,            /* NLA_NESTED */
    __URP_ENDPOINT_A_MAX,
};
#define URP_ENDPOINT_A_MAX (__URP_ENDPOINT_A_MAX - 1)

/* QP attributes (nested inside URP_ENDPOINT_A_QPS) */
enum urp_qp_attr {
    URP_QP_A_UNSPEC,
    URP_QP_A_INDEX,              /* NLA_U32 */
    URP_QP_A_STATE,              /* NLA_U8, enum urp_qp_state */
    URP_QP_A_RTT_NS,            /* NLA_U64 */
    URP_QP_A_TX_BYTES,          /* NLA_U64 */
    URP_QP_A_RX_BYTES,          /* NLA_U64 */
    URP_QP_A_TX_FRAMES,         /* NLA_U64 */
    URP_QP_A_RX_FRAMES,         /* NLA_U64 */
    __URP_QP_A_MAX,
};
#define URP_QP_A_MAX (__URP_QP_A_MAX - 1)

/* Stream attributes (nested inside URP_ENDPOINT_A_STREAMS) */
enum urp_stream_attr {
    URP_STREAM_A_UNSPEC,
    URP_STREAM_A_ID,             /* NLA_U32 */
    URP_STREAM_A_STATE,          /* NLA_U8, enum urp_stream_state */
    URP_STREAM_A_TX_BYTES,       /* NLA_U64 */
    URP_STREAM_A_RX_BYTES,       /* NLA_U64 */
    URP_STREAM_A_REORDER_DEPTH,  /* NLA_U32 */
    URP_STREAM_A_CREDITS_LOCAL,  /* NLA_U16 */
    URP_STREAM_A_CREDITS_REMOTE, /* NLA_U16 */
    __URP_STREAM_A_MAX,
};
#define URP_STREAM_A_MAX (__URP_STREAM_A_MAX - 1)

/* Aggregate stats (nested inside URP_ENDPOINT_A_STATS) */
enum urp_stats_attr {
    URP_STATS_A_UNSPEC,
    URP_STATS_A_ACTIVE_STREAMS,       /* NLA_U32 */
    URP_STATS_A_TX_BYTES,             /* NLA_U64 */
    URP_STATS_A_RX_BYTES,             /* NLA_U64 */
    URP_STATS_A_TX_FRAMES,            /* NLA_U64 */
    URP_STATS_A_RX_FRAMES,            /* NLA_U64 */
    URP_STATS_A_CREDIT_STALLS,        /* NLA_U64 */
    URP_STATS_A_REORDER_INSERTIONS,   /* NLA_U64 */
    URP_STATS_A_REORDER_DROPS,        /* NLA_U64 */
    URP_STATS_A_BUFFER_ALLOC_FAILS,   /* NLA_U64 */
    URP_STATS_A_AUTH_FAILURES,        /* NLA_U64 */
    __URP_STATS_A_MAX,
};
#define URP_STATS_A_MAX (__URP_STATS_A_MAX - 1)

/* Endpoint state (URP_ENDPOINT_A_STATE) */
enum urp_endpoint_state {
    URP_STATE_CREATING,      /* RDMA CM connecting, QPs being set up */
    URP_STATE_ACTIVE,        /* RDMA connected, UDS socket listening, accepting streams */
    URP_STATE_DRAINING,      /* No new streams, existing streams finishing (FIN sent) */
    URP_STATE_STOPPED,       /* All streams closed, ready for removal */
};

/* QP health state (URP_QP_A_STATE) */
enum urp_qp_state {
    URP_QP_QUALIFYING,      /* Probing, not yet in active working set */
    URP_QP_ACTIVE,          /* Healthy, carrying traffic */
    URP_QP_DRAINING,        /* Marked for removal, draining in-flight frames */
    URP_QP_REMOVED,         /* Torn down */
};

/* Stream state (URP_STREAM_A_STATE) */
enum urp_stream_state {
    URP_STREAM_SYN_SENT,
    URP_STREAM_SYN_RECEIVED,
    URP_STREAM_ESTABLISHED,
    URP_STREAM_FIN_WAIT,
    URP_STREAM_CLOSE_WAIT,
    URP_STREAM_CLOSED,
};

#endif /* _UAPI_LINUX_URP_H */
```


## 22.6 YAML Netlink Spec

Modern kernel modules define their GENL interface in YAML (`Documentation/netlink/specs/`), which auto-generates the UAPI header and kernel policy arrays. This ensures the user-kernel ABI, kernel validation policy, and documentation stay synchronized.

```yaml
# Documentation/netlink/specs/urp.yaml
# SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR MIT)

name: urp
protocol: genetlink
uapi-header: linux/urp.h
doc: |
  UDS-RDMA Proxy kernel module.
  Tunnels Unix Domain Socket connections over RDMA (RoCEv2).
  Endpoints map local UDS socket paths to remote RDMA peers.

definitions:
  - type: enum
    name: urp-endpoint-state
    entries:
      - name: creating
      - name: active
      - name: draining
      - name: stopped

  - type: enum
    name: urp-qp-state
    entries:
      - name: qualifying
      - name: active
      - name: draining
      - name: removed

  - type: enum
    name: urp-stream-state
    entries:
      - name: syn-sent
      - name: syn-received
      - name: established
      - name: fin-wait
      - name: close-wait
      - name: closed

attribute-sets:
  - name: urp
    attributes:
      - name: endpoint
        type: nest
        nested-attributes: endpoint
        multi-attr: true

  - name: endpoint
    attributes:
      - name: name
        type: string
        doc: Unique endpoint identifier (max 16 chars)
        checks:
          max-len: 16
      - name: listen-path
        type: string
        doc: UDS socket path for listen mode (outbound tunnel)
        checks:
          max-len: 108
      - name: connect-path
        type: string
        doc: UDS socket path for connect mode (inbound tunnel delivery)
        checks:
          max-len: 108
      - name: rdma-device
        type: string
        doc: RDMA device name (e.g. mlx5_0)
        checks:
          max-len: 64
      - name: peer-addr
        type: binary
        doc: Remote RDMA peer address (struct sockaddr_in6, RDMA initiator)
        checks:
          exact-len: 28
      - name: bind-addr
        type: binary
        doc: Local RDMA bind address (struct sockaddr_in6, RDMA acceptor)
        checks:
          exact-len: 28
      - name: num-qps
        type: u32
        doc: Number of RDMA Queue Pairs (1-32)
        checks:
          min: 1
          max: 32
      - name: buffer-count
        type: u32
        doc: Total buffer pool slots
        checks:
          min: 16
      - name: buffer-size
        type: u32
        doc: Per-buffer size in bytes
        checks:
          min: 20
          max: 65536
      - name: password
        type: string
        doc: Tier 0.5 PSK password (write-only, max 16 chars)
        checks:
          max-len: 16
      - name: state
        type: u8
        enum: urp-endpoint-state
        doc: Endpoint lifecycle state (read-only)
      - name: qps
        type: nest
        nested-attributes: qp
        multi-attr: true
        doc: Per-QP state and counters (read-only)
      - name: streams
        type: nest
        nested-attributes: stream
        multi-attr: true
        doc: Per-stream state and counters (read-only)
      - name: stats
        type: nest
        nested-attributes: stats
        doc: Aggregate endpoint statistics (read-only)

  - name: qp
    attributes:
      - name: index
        type: u32
      - name: state
        type: u8
        enum: urp-qp-state
      - name: rtt-ns
        type: u64
        doc: Last measured probe RTT in nanoseconds
      - name: tx-bytes
        type: u64
      - name: rx-bytes
        type: u64
      - name: tx-frames
        type: u64
      - name: rx-frames
        type: u64

  - name: stream
    attributes:
      - name: id
        type: u32
      - name: state
        type: u8
        enum: urp-stream-state
      - name: tx-bytes
        type: u64
      - name: rx-bytes
        type: u64
      - name: reorder-depth
        type: u32
        doc: Current out-of-order frames buffered
      - name: credits-local
        type: u16
      - name: credits-remote
        type: u16

  - name: stats
    attributes:
      - name: active-streams
        type: u32
      - name: tx-bytes
        type: u64
      - name: rx-bytes
        type: u64
      - name: tx-frames
        type: u64
      - name: rx-frames
        type: u64
      - name: credit-stalls
        type: u64
      - name: reorder-insertions
        type: u64
      - name: reorder-drops
        type: u64
      - name: buffer-alloc-fails
        type: u64
      - name: auth-failures
        type: u64

operations:
  list:
    - name: new-endpoint
      doc: Create a new UDS-RDMA proxy endpoint
      attribute-set: urp
      flags: [admin-perm]
      do:
        request:
          attributes:
            - endpoint

    - name: del-endpoint
      doc: Remove an endpoint (drains active streams first)
      attribute-set: urp
      flags: [admin-perm]
      do:
        request:
          attributes:
            - endpoint

    - name: set-endpoint
      doc: Modify mutable attributes on a live endpoint
      attribute-set: urp
      flags: [admin-perm]
      do:
        request:
          attributes:
            - endpoint

    - name: get-endpoint
      doc: Get endpoint configuration, state, and statistics
      attribute-set: urp
      do:
        request:
          attributes:
            - endpoint
        reply:
          attributes:
            - endpoint
      dump:
        reply:
          attributes:
            - endpoint

mcast-groups:
  list:
    - name: events
```


## 22.7 Multicast Events

The `"events"` multicast group delivers asynchronous state-change notifications to subscribed userspace listeners (e.g., `urp monitor`). Events are unsolicited GENL messages using `URP_CMD_GET_ENDPOINT` as the command, containing the changed endpoint's current state.

### Event Triggers

| Event | Trigger | Included Attributes |
|-------|---------|-------------------|
| Endpoint state change | CREATING->ACTIVE, ACTIVE->DRAINING, etc. | NAME, STATE |
| QP health transition | Qualifying->Active, Active->Draining, etc. | NAME, QPS (changed QP only) |
| Auth failure | PSK hash mismatch on incoming connection | NAME, STATS (auth_failures counter) |
| Stream lifecycle | New stream established, stream closed | NAME, STREAMS (changed stream), STATS (active_streams) |

### Kernel-Side Event Emission

```c
static void urp_send_event(struct urp_endpoint *ep, enum urp_cmd cmd)
{
    struct sk_buff *skb;
    void *hdr;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
    if (!skb)
        return;

    hdr = genlmsg_put(skb, 0, 0, &urp_genl_family, 0, cmd);
    if (!hdr) {
        nlmsg_free(skb);
        return;
    }

    /* Serialize the endpoint's current state into the message */
    if (urp_fill_endpoint(skb, ep) < 0) {
        genlmsg_cancel(skb, hdr);
        nlmsg_free(skb);
        return;
    }

    genlmsg_end(skb, hdr);
    genlmsg_multicast(&urp_genl_family, skb, 0, 0, GFP_KERNEL);
}
```


## 22.8 Kernel Handler Design

### 22.8.1 NEW_ENDPOINT Handler

```c
static int urp_new_endpoint(struct sk_buff *skb, struct genl_info *info)
{
    struct nlattr *endpoint_attr = info->attrs[URP_A_ENDPOINT];
    struct nlattr *attrs[URP_ENDPOINT_A_MAX + 1];
    struct urp_endpoint *ep;
    int err;

    if (!endpoint_attr)
        return -EINVAL;

    err = nla_parse_nested(attrs, URP_ENDPOINT_A_MAX, endpoint_attr,
                           urp_endpoint_policy, info->extack);
    if (err)
        return err;

    /* Validate required attributes */
    if (!attrs[URP_ENDPOINT_A_NAME]) {
        NL_SET_ERR_MSG(info->extack, "endpoint name is required");
        return -EINVAL;
    }
    if (!attrs[URP_ENDPOINT_A_LISTEN_PATH] && !attrs[URP_ENDPOINT_A_CONNECT_PATH]) {
        NL_SET_ERR_MSG(info->extack, "at least one of listen-path or connect-path is required");
        return -EINVAL;
    }
    if (!attrs[URP_ENDPOINT_A_PEER_ADDR] && !attrs[URP_ENDPOINT_A_BIND_ADDR]) {
        NL_SET_ERR_MSG(info->extack, "peer-addr or bind-addr is required");
        return -EINVAL;
    }
    if (attrs[URP_ENDPOINT_A_PEER_ADDR] && attrs[URP_ENDPOINT_A_BIND_ADDR]) {
        NL_SET_ERR_MSG(info->extack, "peer-addr and bind-addr are mutually exclusive");
        return -EINVAL;
    }

    /* Allocate and populate endpoint */
    ep = urp_endpoint_create(attrs, info->extack);
    if (IS_ERR(ep))
        return PTR_ERR(ep);

    /* Insert into global endpoint table */
    err = rhashtable_insert_fast(&urp_endpoints, &ep->node, urp_rht_params);
    if (err) {
        NL_SET_ERR_MSG(info->extack, "endpoint name already exists");
        urp_endpoint_free(ep);
        return err;
    }

    /* Start async: RDMA CM connection + UDS socket creation */
    urp_endpoint_activate(ep);

    /* Notify listeners */
    urp_send_event(ep, URP_CMD_NEW_ENDPOINT);

    return 0;
}
```

### 22.8.2 GET_ENDPOINT Dump Handler

The `dumpit` handler iterates all endpoints, emitting one `NLMSG` per endpoint. Uses `cb->args[]` for resumable iteration (required for large responses that span multiple `sendmsg` calls):

```c
static int urp_dump_endpoints(struct sk_buff *skb, struct netlink_callback *cb)
{
    struct urp_endpoint *ep;
    struct rhashtable_iter iter;
    int err;

    rhashtable_walk_enter(&urp_endpoints, &iter);
    rhashtable_walk_start(&iter);

    while ((ep = rhashtable_walk_next(&iter)) != NULL) {
        if (IS_ERR(ep))
            continue;

        err = urp_fill_endpoint_msg(skb, ep, NETLINK_CB(cb->skb).portid,
                                    cb->nlh->nlmsg_seq, NLM_F_MULTI);
        if (err)
            break;
    }

    rhashtable_walk_stop(&iter);
    rhashtable_walk_exit(&iter);

    return skb->len;
}
```

### 22.8.3 Endpoint Serialization

`urp_fill_endpoint()` serializes an endpoint into netlink attributes — used by both `doit` GET and `dumpit`, and by multicast event emission:

```c
static int urp_fill_endpoint(struct sk_buff *skb, struct urp_endpoint *ep)
{
    struct nlattr *endpoint_nest, *qps_nest, *streams_nest, *stats_nest;

    endpoint_nest = nla_nest_start(skb, URP_A_ENDPOINT);
    if (!endpoint_nest)
        return -EMSGSIZE;

    /* Configuration */
    if (nla_put_string(skb, URP_ENDPOINT_A_NAME, ep->name) ||
        nla_put_u32(skb, URP_ENDPOINT_A_NUM_QPS, ep->num_qps) ||
        nla_put_u32(skb, URP_ENDPOINT_A_BUFFER_COUNT, ep->buffer_count) ||
        nla_put_u32(skb, URP_ENDPOINT_A_BUFFER_SIZE, ep->buffer_size) ||
        nla_put_u8(skb, URP_ENDPOINT_A_STATE, ep->state))
        goto nla_put_failure;

    if (ep->listen_path[0] &&
        nla_put_string(skb, URP_ENDPOINT_A_LISTEN_PATH, ep->listen_path))
        goto nla_put_failure;
    if (ep->connect_path[0] &&
        nla_put_string(skb, URP_ENDPOINT_A_CONNECT_PATH, ep->connect_path))
        goto nla_put_failure;

    /* NOTE: PASSWORD is never serialized (write-only) */

    /* Per-QP state */
    qps_nest = nla_nest_start(skb, URP_ENDPOINT_A_QPS);
    if (qps_nest) {
        /* ... iterate ep->qps[], emit nested QP attributes ... */
        nla_nest_end(skb, qps_nest);
    }

    /* Per-stream state */
    streams_nest = nla_nest_start(skb, URP_ENDPOINT_A_STREAMS);
    if (streams_nest) {
        /* ... iterate active streams via rhashtable, emit nested stream attributes ... */
        nla_nest_end(skb, streams_nest);
    }

    /* Aggregate stats */
    stats_nest = nla_nest_start(skb, URP_ENDPOINT_A_STATS);
    if (stats_nest) {
        nla_put_u32(skb, URP_STATS_A_ACTIVE_STREAMS, atomic_read(&ep->active_streams));
        nla_put_u64_64bit(skb, URP_STATS_A_TX_BYTES, atomic64_read(&ep->tx_bytes), 0);
        /* ... remaining stats ... */
        nla_nest_end(skb, stats_nest);
    }

    nla_nest_end(skb, endpoint_nest);
    return 0;

nla_put_failure:
    nla_nest_cancel(skb, endpoint_nest);
    return -EMSGSIZE;
}
```


## 22.9 Endpoint State Machine

```
  urp add         RDMA connected        urp drain         all streams closed
     |                  |                   |                    |
     v                  v                   v                    v
 CREATING ---------> ACTIVE -----------> DRAINING ----------> STOPPED
                       ^                                        |
                       |              urp remove                |
                       +--- urp set --+    |                    |
                                      v    v                    v
                                   [destroy endpoint, free resources]
```

- **CREATING**: Endpoint allocated, RDMA CM resolving address/route, QPs being created. The virtual UDS socket is not yet listening — applications cannot connect.
- **ACTIVE**: RDMA connected, virtual UDS socket listening, accepting new streams. Normal operating state.
- **DRAINING**: Triggered by `urp drain`. No new streams accepted (`accept()` returns `ECONNREFUSED`). Existing streams receive FIN frames. Module waits for all streams to close.
- **STOPPED**: All streams closed, QPs in RESET state. Endpoint can be removed via `urp remove` or reactivated (future).

The `urp remove` command on an ACTIVE endpoint triggers an implicit drain-then-destroy sequence.


## 22.10 Kernel Source Organization

The GENL interface code lives in `urp_netlink.c`, cleanly separated from the RDMA and socket logic:

```
kernel/
  urp_main.c          # module_init/exit: genl_register_family(), /proc init
  urp_netlink.c       # GENL command handlers, policy arrays, event emission
  urp_endpoint.c      # Endpoint lifecycle: create, activate, drain, destroy
  urp_rdma.c          # RDMA CM + verbs: QP setup, buffer pool, CQ
  urp_socket.c        # Virtual UDS endpoint: proto_ops, accept loop
  urp_pump.c          # Bidirectional data pump: TX/RX kthreads
  urp_proc.c          # /proc/urp/* stats export (supplement to GENL stats)
  include/uapi/linux/urp.h  # UAPI header (auto-generated or hand-written)
```

`urp_netlink.c` is approximately 400-600 lines — similar in scope to WireGuard's `netlink.c` (632 lines). The handlers delegate to `urp_endpoint.c` for actual resource management, keeping netlink serialization concerns separate from endpoint lifecycle logic.


[Back to Design Overview](../DESIGN.md)
