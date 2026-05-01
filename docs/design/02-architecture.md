# High-Level Architecture

## 2.1 End-to-End System Overview

### Unidirectional Modes

A proxy can operate in one of two UDS modes, or both simultaneously:

**UDS listen mode** — the proxy creates a UDS socket and accepts connections from local applications:

```
 Machine A                                                    Machine B
 +-------------+     +--------------------+   RoCEv2 RC QPs   +--------------------+     +-------------+
 |             |     |                    |  (1..N QPs over    |                    |     |             |
 |  App A      | UDS |  uds-rdma-proxy    |   ECMP paths)     |  uds-rdma-proxy    | UDS |  App B      |
 |  (client)   |---->|  listen_path       |<=================>|  connect_path      |---->|  (server)   |
 |             |     |                    |  RDMA initiator    |                    |     |             |
 +-------------+     +--------------------+                    +--------------------+     +-------------+
                      creates & binds                           connects to app's
                      /var/run/urp/app.sock                     existing UDS socket
```

**UDS connect mode** — the proxy connects to an existing UDS socket where a local application is listening:

```
 Machine A                                                    Machine B
 +-------------+     +--------------------+   RoCEv2 RC QPs   +--------------------+     +-------------+
 |             |     |                    |  (1..N QPs over    |                    |     |             |
 |  App A      | UDS |  uds-rdma-proxy    |   ECMP paths)     |  uds-rdma-proxy    | UDS |  App B      |
 |  (server)   |<----|  connect_path      |<=================>|  listen_path       |<----|  (client)   |
 |             |     |                    |  RDMA acceptor     |                    |     |             |
 +-------------+     +--------------------+                    +--------------------+     +-------------+
                      connects to app's                         creates & binds
                      existing UDS socket                       /var/run/urp/app.sock
```

### Bidirectional Mode (Cluster Deployments)

For peer-to-peer cluster deployments (e.g., Redpanda, ClickHouse), each node must both accept and initiate connections to its peers. A single proxy instance handles **both directions simultaneously** using two different UDS socket paths:

```
 Machine A (Redpanda node)                                   Machine B (Redpanda node)
 +-------------+     +--------------------+   RoCEv2 RC QPs   +--------------------+     +-------------+
 |             |     |                    |  (1..N QPs over    |                    |     |             |
 |  Redpanda   |<-+->|  uds-rdma-proxy    |   ECMP paths)     |  uds-rdma-proxy    |<-+->|  Redpanda   |
 |             |  |  |                    |<=================>|                    |  |  |             |
 +-------------+  |  +--------------------+                    +--------------------+  |  +-------------+
                  |   listen_path:                              listen_path:           |
                  |    /var/run/urp/to-B.sock                    /var/run/urp/to-A.sock |
                  |   connect_path:                             connect_path:           |
                  +--- /var/run/redpanda/rpc.sock                /var/run/redpanda/rpc.sock ---+
```

In bidirectional mode:
- **Outbound** (A wants to reach B): Redpanda on A connects to the proxy's `listen_path`. The proxy tunnels the connection over RDMA.
- **Inbound** (B wants to reach A): The remote proxy tunnels the connection over RDMA. A's proxy connects to Redpanda's `connect_path` to deliver it.

Both proxy instances run the **same binary**. The proxy's behavior is determined by its configuration — specifically, which UDS paths are set:

- **`listen_path`** set → proxy creates and binds a UDS socket, accepts local app connections (outbound direction)
- **`connect_path`** set → proxy connects to an existing UDS socket to deliver remote connections (inbound direction)
- **Both set** → bidirectional mode (both directions simultaneously)

RDMA directionality is configured separately:
- **RDMA initiator**: `peer_address` is set — the proxy calls `rdma_connect()` to reach the remote peer
- **RDMA acceptor**: `bind_address` is set — the proxy calls `rdma_listen()` to accept incoming RDMA connections

### Deployment Model: One Instance Per Peer

For a 3-node Redpanda cluster (A, B, C), Machine A runs **2 proxy instances** (one per remote peer):

```
 Machine A
 ┌──────────────────────────────────────────────────────────────┐
 │  ┌──────────────┐                                           │
 │  │              │  UDS   ┌────────────────────────────┐      │
 │  │              │◄──────►│ proxy (peer B)              │──RDMA──► Machine B
 │  │              │        │ listen:  /var/run/urp/to-B  │      │
 │  │              │        │ connect: /var/run/rp/rpc    │      │
 │  │  Redpanda    │        └────────────────────────────┘      │
 │  │              │                                           │
 │  │              │  UDS   ┌────────────────────────────┐      │
 │  │              │◄──────►│ proxy (peer C)              │──RDMA──► Machine C
 │  │              │        │ listen:  /var/run/urp/to-C  │      │
 │  │              │        │ connect: /var/run/rp/rpc    │      │
 │  └──────────────┘        └────────────────────────────┘      │
 └──────────────────────────────────────────────────────────────┘
```

Multiple proxy instances share the same `connect_path` (Redpanda's server socket) — each incoming RDMA stream results in a new `connect()` call, which Redpanda accepts as a normal client connection. Each instance has its own `listen_path` so Redpanda can direct outbound traffic to the correct peer.

## 2.2 Internal Component Architecture

```
 +------------------------------------------------------------------------+
 |  uds-rdma-proxy                                                        |
 |                                                                        |
 |  +---------------------+          +-------------------------+          |
 |  | UDS Endpoint        |          | RDMA Connection Manager |          |
 |  | (io_uring)          |          | (rdma_cm)               |          |
 |  +----------+----------+          +------------+------------+          |
 |             |                                  |                       |
 |  +----------v----------------------------------v-----------+           |
 |  |                  Connection Table                       |           |
 |  |   stream_id -> (uds_fd, qp_set, buffers, credits)      |           |
 |  +----------+---------------------+-----------------------+           |
 |             |                     |                                    |
 |  +----------v----------+ +-------v-----------+                        |
 |  | UDS -> RDMA Pump    | | RDMA -> UDS Pump  |                        |
 |  |                     | |                    |                        |
 |  | io_uring read       | | CQ poll            |                        |
 |  | frame encode        | | frame decode       |                        |
 |  | seq# assign         | | reorder buffer     |                        |
 |  | QP select           | | io_uring write     |                        |
 |  | ibv_post_send       | | credit grant       |                        |
 |  +---------------------+ +--------------------+                        |
 |                                                                        |
 |  +---------------------+  +---------------------+  +--------------+   |
 |  | Buffer Pool         |  | Reorder Engine      |  | Metrics      |   |
 |  | (pre-reg MRs,       |  | (BTreeMap per-conn) |  | (Prometheus) |   |
 |  |  huge pages,        |  | seq tracking        |  |              |   |
 |  |  lock-free alloc)   |  | gap detection       |  |              |   |
 |  +---------------------+  +---------------------+  +--------------+   |
 +------------------------------------------------------------------------+
```

## 2.3 Data Flow (Single Direction: UDS-to-RDMA)

```
 Application write()
       |
       v
 +------------------+
 | UDS Socket       |  (kernel copies app data to UDS buffer)
 +--------+---------+
          |
          v
 +------------------+
 | io_uring CQE     |  (read completes into pre-registered buffer)
 +--------+---------+
          |
          v
 +------------------+
 | Frame Encoder    |  (write header: stream_id, seq#, flags, len)
 | (in-place in     |  (header + payload contiguous in same buffer)
 |  TX buffer)      |
 +--------+---------+
          |
          v
 +------------------+
 | QP Selector      |  (round-robin / adaptive / hash-affinity)
 +--------+---------+
          |
          v
 +------------------+
 | ibv_post_send()  |  (NIC DMAs from registered buffer)
 +--------+---------+
          |
     ═════╪═══════════  RoCEv2 wire  ═══════════════════
          |
          v
 +------------------+
 | ibv_poll_cq()    |  (recv CQE on remote side)
 +--------+---------+
          |
          v
 +------------------+
 | Frame Decoder    |  (extract stream_id, seq#, payload)
 +--------+---------+
          |
          v
 +------------------+
 | Reorder Buffer   |  (BTreeMap: sort by seq#, deliver in order)
 +--------+---------+
          |
          v
 +------------------+
 | io_uring SQE     |  (write to UDS, payload from RX buffer)
 +--------+---------+
          |
          v
 +------------------+
 | UDS Socket       |  (kernel copies to app's UDS buffer)
 +--------+---------+
          |
          v
 Application read()
```

## 2.4 Copy Analysis

The minimum copy path through the proxy:

```
 App write() --[copy 1: app->kernel UDS buf]--> kernel --[copy 2: kernel->user MR buf]--> proxy TX buffer
                                                                                              |
                                                                                     (NIC DMA, 0 copies)
                                                                                              |
 proxy RX buffer --[copy 3: user MR buf->kernel UDS buf]--> kernel --[copy 4: kernel->app]--> App read()
```

There are **4 memory copies** end-to-end (2 per direction per hop). This is inherent to the UDS + RDMA proxy model. The RDMA network segment itself is zero-copy (NIC DMA), but the UDS segments each incur a kernel-userspace copy. Using io_uring with registered buffers and `IORING_SETUP_SQPOLL` can reduce the syscall overhead but not the copies themselves.

For comparison, TCP has 4+ copies too (plus protocol processing), so the proxy wins on CPU/latency even with the same copy count.


[Back to Design Overview](../DESIGN.md)
