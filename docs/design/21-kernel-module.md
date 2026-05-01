# Kernel Module Alternative

This document explores an alternative implementation of the UDS-RDMA proxy as a Linux kernel module, assessing the architectural trade-offs, Rust-in-kernel feasibility, code sharing strategy, and a phased approach that runs parallel to the userspace v0-v4 roadmap.

## 21.1 Motivation: Eliminating the UDS Copies

The userspace proxy's fundamental performance ceiling is **4 memory copies** end-to-end, as described in [Section 2.4](02-architecture.md#24-copy-analysis) and [Section 13.1](13-performance.md#131-the-copy-problem):

```
 Userspace proxy (4 copies):

 App write()  --[copy 1]-->  UDS kernel buf  --[copy 2]-->  proxy TX buf (MR)
                                                                  |
                                                         NIC DMA (0 copies)
                                                                  |
 proxy RX buf (MR)  --[copy 3]-->  UDS kernel buf  --[copy 4]-->  App read()
```

Copies 1-2 and 3-4 are the UDS hops: each crosses the user/kernel boundary via `write()` and `read()` system calls. The RDMA segment itself is zero-copy (NIC DMA), but the UDS bookends dominate latency.

A kernel module eliminates both UDS hops by intercepting socket operations before they leave kernel space:

```
 Kernel module (2 copies):

 App write()  --[copy 1]-->  kernel TX buf (DMA-mapped)
                                    |
                           NIC DMA (0 copies)
                                    |
 kernel RX buf (DMA-mapped)  --[copy 2]-->  App read()
```

With page flipping (see [Section 21.5](#215-zero-copy-potential)), even these 2 copies can potentially be reduced to 0-1:

```
 Kernel module + page flipping (0-1 copies):

 App write()  --[page pin]-->  NIC DMA from app page  (0 copies on send)
                                         |
 NIC DMA into pre-alloc page  --[page flip into socket buf]-->  App read()
                               (0-1 copies on receive, depending on alignment)
```

**Quantified benefit**: On a modern CPU, a 4KB `memcpy` takes approximately 100-250ns depending on cache state. Eliminating 2 copies saves 200-500ns per message. For a typical sub-microsecond RDMA RTT, this represents a **20-40% latency reduction** — a significant gain that cannot be achieved through any userspace optimization.

**Position relative to `splice()`**: The experimental `splice()` path ([Section 13.8](13-performance.md#138-experimental-splicevmsplice-zero-copy)) can reduce copies from 4 to 2-3 within the userspace model, but requires `splice()`-compatible socket types and cannot reach the zero-copy potential of a kernel module. The kernel module is more invasive but strictly more capable.


## 21.2 Architecture Comparison

### Userspace Proxy

```
 +-----------+     +--------------------------------------------------+     +----------+
 |           | UDS |  Userspace Proxy Process                          | UDS |          |
 |  App A    |<--->|  io_uring | frame codec | pump | ibverbs | CQ    |<--->|  App B   |
 |           |     |  buffers dual-registered (io_uring + ibverbs MR)  |     |          |
 +-----------+     +--------------------------------------------------+     +----------+
                          |                                |
                    2 copies per hop                  2 copies per hop
                    (user/kernel boundary)            (user/kernel boundary)
```

### Kernel Module

```
 +-----------+     +--------------------------------------------------+     +----------+
 |           | syscall  Kernel Module (in-kernel)                      | syscall       |
 |  App A    |---->|  socket ops | frame codec | pump | ib verbs | CQ |<----|  App B   |
 |           |     |  buffers DMA-mapped directly (ib_dma_map_page)   |     |          |
 +-----------+     +--------------------------------------------------+     +----------+
                          |                                |
                    1 copy (or 0)                    1 copy (or 0)
                    (app buf -> kernel buf)           (kernel buf -> app buf)
```

### Comparison Table

| Aspect | Userspace Proxy | Kernel Module |
|--------|----------------|---------------|
| **End-to-end copies** | 4 (minimum) | 2 (or 0-1 with page flipping) |
| **Syscalls per message** | 2+ (io_uring_enter, or 0 with SQPOLL) | 0 (already in kernel) |
| **Buffer registration** | Dual: io_uring + ibverbs MR | Single: ib_dma_map_page |
| **UDS I/O mechanism** | io_uring with registered buffers | kernel_recvmsg / kernel_sendmsg |
| **CQ polling** | Dedicated userspace thread | kthread or CQ completion callback |
| **Async framework** | tokio (control plane) | kthreads + work queues |
| **Reorder buffer** | BTreeMap (std) | kernel rbtree or BTreeMap (alloc) |
| **Metrics** | Prometheus HTTP endpoint | tracepoints + /proc + perf counters |
| **Configuration** | TOML file + CLI (clap) | GENL (`urp` CLI) + `/proc/urp/*` stats |
| **Debugging** | gdb, tracing crate, flamegraphs | ftrace, kprobes, crash dumps |
| **Deployment** | Binary copy, systemd, container | DKMS or per-kernel build, modprobe |
| **Crash impact** | Process dies, restart via systemd | Kernel panic (potentially) |
| **Language** | Rust (stable, full std) | C + Rust (limited, no_std + alloc) |
| **Update model** | Binary swap, graceful restart | Module reload (brief interruption) |
| **Transparency** | Full (apps see a normal UDS) | Full (apps see a normal UDS) |
| **Development velocity** | Fast (standard tooling) | Slow (kernel build cycle, VM testing) |


## 21.3 Kernel Module Architecture

### 21.3.1 Socket Interception

The kernel module routes UDS traffic to RDMA by installing custom `proto_ops` (protocol operations) on socket objects. When an application calls `write()` on such a socket, the kernel invokes our custom `sendmsg` handler instead of the standard Unix domain socket path. How the custom ops get installed depends on the implementation approach (see [Section 21.4](#214-implementation-approaches)) — the module can either swap ops on an existing socket (approach C) or create a virtual UDS endpoint that has custom ops from birth (approach D, recommended).

Key kernel structures:

```
 struct socket {
     ...
     const struct proto_ops *ops;  // custom ops installed by the module
     ...
 };

 struct proto_ops {
     int (*sendmsg)(struct socket *, struct msghdr *, size_t);
     int (*recvmsg)(struct socket *, struct msghdr *, size_t, int);
     int (*accept)(struct socket *, struct socket *, int, bool);
     unsigned int (*poll)(struct file *, struct socket *, poll_table *);
     ...
 };
```

The custom `sendmsg`:
1. Copies payload from the user's `iov` into a DMA-mapped kernel buffer (1 copy)
2. Encodes a frame header (from the shared protocol crate)
3. Calls `ib_post_send` to transmit via RDMA

The custom `recvmsg`:
1. Retrieves the next in-order frame from the reorder buffer
2. Copies the payload into the user's `iov` (1 copy)
3. Grants credits back to the sender

### 21.3.2 In-Kernel RDMA

The Linux kernel provides a full RDMA verbs API for in-kernel consumers (used by NFS/RDMA, iSER, SRP, etc.). This is the same API underlying userspace `libibverbs`, but called directly from kernel context.

Key kernel RDMA functions:

| Function | Purpose |
|----------|---------|
| `rdma_create_id` | Create an RDMA CM identifier |
| `rdma_listen` / `rdma_connect` | Connection management |
| `rdma_resolve_addr` / `rdma_resolve_route` | Address and route resolution |
| `ib_alloc_pd` | Allocate protection domain |
| `ib_create_cq` | Create completion queue |
| `ib_create_qp` | Create queue pair (RC type) |
| `ib_create_srq` | Create shared receive queue |
| `ib_post_send` / `ib_post_recv` | Post work requests |
| `ib_poll_cq` | Poll for completions |
| `ib_req_notify_cq` | Request CQ event notification |
| `ib_dma_map_page` / `ib_dma_unmap_page` | DMA-map kernel pages for RDMA |
| `ib_reg_mr` | Register memory region (if needed) |

These are mature, stable interfaces — the in-kernel RDMA consumer API has been used by storage protocols since the early days of the OFED stack. Reliability is not a concern.

### 21.3.3 Buffer Management

Kernel buffer management is simpler than userspace because there is no dual-registration requirement:

```
 Userspace:                              Kernel:
 mmap(MAP_HUGETLB) -> virtual addr       alloc_pages(GFP_KERNEL, order) -> struct page
 ibv_reg_mr(addr) -> MR lkey             ib_dma_map_page(page) -> DMA addr
 io_uring_register_buffers(addr)          (no io_uring registration needed)
 Must coordinate io_uring + ibverbs       Single owner, single DMA mapping
```

- **Allocation**: `alloc_pages(GFP_KERNEL, order)` with compound pages for large buffers. `alloc_pages_node(nid, ...)` for NUMA-aware allocation.
- **DMA mapping**: `ib_dma_map_page(device, page, offset, size, direction)` maps kernel pages for NIC DMA. No `ibv_reg_mr` overhead.
- **Huge pages**: Compound pages with high order (order 9 = 2MB) provide the same TLB benefits as userspace huge pages.
- **Free list**: `list_head` with `spinlock_t`, or per-CPU lists (`alloc_percpu`) for contention-free hot path (k0-k1). For k2, adopt the kernel's `page_pool` API — see [Section 21.9](#219-comparison-with-nic-driver-architecture).
- **Lifetime**: Simpler than userspace — a buffer is either in the free list, posted for RDMA send/recv, or being copied to/from a user iov. No io_uring completion phase.

> **k2 target: `page_pool`**: The kernel's `page_pool` API (since v4.18) was designed for exactly this use case — pre-allocating DMA-mapped pages and recycling them without unmapping. Creating a `page_pool` with `PP_FLAG_DMA_MAP_DEVICE` eliminates per-cycle `ib_dma_map_page`/`ib_dma_unmap_page` overhead, provides per-CPU caching (eliminating free-list contention without manual `alloc_percpu`), and handles refcount-based lifecycle automatically. See [Section 21.9](#219-comparison-with-nic-driver-architecture) for the full NIC driver parallel.

### 21.3.4 Pump Loop

Each proxied connection runs a bidirectional pump, implemented as kernel threads:

```
 +------------------+                              +------------------+
 | TX kthread       |                              | RX kthread       |
 |                  |                              |                  |
 | kernel_recvmsg() |     RDMA QP (RC)             | ib_poll_cq()     |
 | frame_encode()   |  ========================>   | reorder_insert() |
 | ib_post_send()   |                              | reorder_drain()  |
 | credit_check()   |  <========================   | kernel_sendmsg() |
 |                  |                              | credit_grant()   |
 +------------------+                              +------------------+
```

- **TX thread**: `kthread_create("urp-tx-%d", stream_id)` — reads from the UDS socket via `kernel_recvmsg`, encodes frames, posts RDMA sends. Sleeps via `wait_event_interruptible` when out of credits or buffers.
- **RX thread**: Can be driven by CQ completion callbacks (`ib_req_notify_cq` + `comp_handler`) rather than a dedicated polling thread. The completion handler wakes a `work_queue` item that drains the CQ, inserts frames into the reorder buffer, and writes in-order data to the UDS socket via `kernel_sendmsg`.
- **CPU binding**: `kthread_bind(thread, cpu)` for NUMA-local scheduling, analogous to the userspace proxy's CPU pinning.

### 21.3.5 Connection Table

The module maintains a connection table mapping UDS sockets to RDMA stream state:

```
 struct urp_connection {
     struct rhash_head  node;        // rhashtable linkage
     u32                stream_id;   // lookup key
     struct socket     *uds_sock;    // intercepted UDS socket
     struct ib_qp      *qp;         // RDMA queue pair
     struct urp_credits credits;     // flow control state
     struct rb_root     reorder;     // reorder buffer (rbtree)
     struct task_struct *tx_thread;  // TX pump kthread
     ...
 };
```

- **rhashtable**: Resizable hash table with RCU (Read-Copy-Update) for lock-free reads on the hot path. Lookups during `sendmsg`/`recvmsg` do not take locks.
- **RCU lifecycle**: Connection creation/destruction uses `rhashtable_insert_fast` / `rhashtable_remove_fast` with `synchronize_rcu` before freeing.


## 21.4 Implementation Approaches

### Approach A: Custom AF_RDMA Socket Family

Register a new address family (`AF_RDMA_PROXY`) via `sock_register()`. Applications would create sockets using `socket(AF_RDMA_PROXY, SOCK_STREAM, 0)` and connect them like normal stream sockets, but the kernel routes data over RDMA internally.

**Pros**: Clean separation, no hooks into existing socket code, straightforward implementation.
**Cons**: Not transparent — applications must be modified to use the new address family. Defeats the primary goal of drop-in UDS replacement.

### Approach B: BPF/sockmap Redirect

Use `BPF_MAP_TYPE_SOCKMAP` with `bpf_msg_redirect_map` to redirect UDS traffic to an internal kernel socket connected to the RDMA path.

**Pros**: Transparent to applications, leverages existing BPF infrastructure, no custom socket code.
**Cons**: `AF_UNIX` sockmap support is relatively recent and incomplete. Adds redirect latency. BPF programs have limited expressiveness — complex protocol logic (framing, reorder, credits) would still need a kernel module.

### Approach C: proto_ops Replacement

Replace the `proto_ops` pointer on targeted UDS sockets to intercept `sendmsg`/`recvmsg` directly. The module discovers existing `AF_UNIX` sockets (by path or inode) and swaps their `ops` pointer to redirect traffic.

**Pros**: Fully transparent to applications. Maximum control over the data path. Can intercept sockets the module did not create.
**Cons**: Race conditions — must intercept the socket after creation but before use. Lifecycle coupling — modifying someone else's socket object. Discovery problem — must locate the right socket to intercept. Must restore original ops on module unload. Must handle all `proto_ops` methods (poll, ioctl, etc.), not just sendmsg/recvmsg.

### Approach D: Virtual UDS Endpoint (Recommended)

The kernel module creates and owns a virtual UDS socket file at a configured filesystem path. Applications (e.g. Redpanda) connect to this path exactly as they would connect to any normal UDS socket. The module's custom `accept` handler returns connected sockets with RDMA-backed `proto_ops` from birth — no ops swapping required.

This is the most natural kernel translation of the userspace proxy architecture: the userspace proxy also creates a UDS socket, listens, and pumps data to/from RDMA. Approach D does the same thing entirely in kernel space.

**Lifecycle**:

```
 urp add redpanda --listen-path /var/run/urp/redpanda.sock --peer-address 192.168.1.2:4791
     |
     v
 GENL handler: urp_new_endpoint()
     |
     v
 sock_create_kern(AF_UNIX, SOCK_STREAM, 0, &listen_sock)
     |
     v
 kernel_bind(listen_sock, "/var/run/urp/redpanda.sock")
     |
     v
 kernel_listen(listen_sock, backlog)
     |                                        +---------------------+
     v                                        | Meanwhile:          |
 kthread: accept loop                         | rdma_connect() to   |
     |                                        | configured remote   |
     v                                        | peer, establish QPs |
 App connects to /var/run/urp/redpanda.sock   +---------------------+
     |
     v
 kernel_accept() -> new_sock
     |
     v
 Install custom proto_ops on new_sock (module owns it — no race)
     |
     v
 Allocate urp_connection: stream_id, QP, credits, reorder buf
     |
     v
 App's write()/read() -> urp_sendmsg()/urp_recvmsg() -> RDMA
```

**Data path** (identical to approach C once the socket is established):

```c
 // Custom sendmsg — installed on accepted sockets
 static int urp_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
 {
     struct urp_connection *conn = urp_lookup(sock);
     struct urp_buffer *buf = urp_buf_alloc(conn);

     // Copy from user iov into DMA-mapped buffer (1 copy)
     copy_from_iter(buf->data + FRAME_HEADER_SIZE, len, &msg->msg_iter);

     // Encode frame header (shared protocol crate)
     urp_frame_encode(buf->data, conn->stream_id, conn->next_seq++,
                      0 /* flags */, conn->credits_to_grant, len);
     conn->credits_to_grant = 0;

     // Post RDMA send
     struct ib_send_wr wr = {
         .opcode    = IB_WR_SEND,
         .sg_list   = &buf->sge,
         .num_sge   = 1,
         .send_flags = IB_SEND_SIGNALED,
     };
     return ib_post_send(conn->qp, &wr, NULL);
 }

 // Custom recvmsg — installed on accepted sockets
 static int urp_recvmsg(struct socket *sock, struct msghdr *msg,
                         size_t len, int flags)
 {
     struct urp_connection *conn = urp_lookup(sock);
     struct urp_buffer *buf;

     // Wait for in-order data from reorder buffer
     buf = urp_reorder_drain(conn);
     if (!buf)
         return -EAGAIN;

     // Copy from DMA-mapped buffer into user iov (1 copy)
     size_t payload_len = urp_frame_payload_len(buf->data);
     copy_to_iter(buf->data + FRAME_HEADER_SIZE, payload_len, &msg->msg_iter);

     // Grant credits back
     conn->credits_to_grant += 1;
     urp_buf_free(buf);

     return payload_len;
 }
```

**Configuration via Generic Netlink**:

The module registers a GENL family `"urp"` with 4 commands (`NEW`/`DEL`/`SET`/`GET_ENDPOINT`). Administrators manage endpoints using the `urp` CLI tool, which communicates with the kernel module via GENL. See [Section 22](22-genl-interface.md) for the full GENL interface specification and [Section 23](23-cli-tool.md) for the CLI tool design.

Endpoint creation at runtime:

```bash
 # Create a new RDMA-backed UDS endpoint
 urp add redpanda \
   --listen-path /var/run/urp/redpanda.sock \
   --peer-address 192.168.1.2:4791 \
   --num-qps 4 \
   --buffer-count 1024

 # Module creates /var/run/urp/redpanda.sock and starts listening
 # Redpanda connects to it as a normal UDS socket

 # Create a second endpoint for postgres
 urp add postgres \
   --listen-path /var/run/urp/postgres.sock \
   --peer-address 192.168.1.3:4791 \
   --num-qps 2

 # View all endpoints
 urp show

 # Graceful drain before removal
 urp drain redpanda
 urp remove redpanda
```

**Pros**: Fully transparent — apps connect to a normal-looking UDS path. Clean ownership — module creates and owns the socket, no ops swapping, no race conditions. Natural configuration model — explicit mapping of socket paths to RDMA endpoints. The same architecture as the userspace proxy, just in kernel space. Works with any application that supports configurable UDS paths (Redpanda, PostgreSQL, Redis, etc.). Runtime reconfiguration via GENL (add/remove/modify endpoints without module reload, multicast event notifications).

**Cons**: Requires the application to be configured to use the module's socket path (but most services already support configurable socket paths, and the userspace proxy has the same requirement). Must implement a full listening socket lifecycle (accept loop, connection tracking, graceful shutdown).

### Comparison

| Criterion | A: AF_RDMA | B: BPF/sockmap | C: proto_ops swap | D: Virtual UDS (rec.) |
|-----------|-----------|----------------|--------------------|-----------------------|
| Transparency | None (app changes) | Full | Full | Full (configurable path) |
| Complexity | Low | Medium | Medium-High | Medium |
| Zero-copy potential | Full | Limited | Full | Full |
| Kernel version deps | Minimal | v5.10+ for AF_UNIX sockmap | Minimal | Minimal |
| Maintenance burden | Low | Low (BPF portable) | Medium | Medium |
| Protocol expressiveness | Full | Limited (BPF constraints) | Full | Full |
| Socket ownership | Module creates | BPF redirects | Swaps on foreign socket | Module creates and owns |
| Race conditions | None | None | Yes (intercept window) | None |
| Runtime reconfig | No | Limited | No | Yes (GENL + `urp` CLI) |
| Lifecycle clarity | Clean | Clean | Complex (restore on unload) | Clean |


## 21.5 Zero-Copy Potential

The kernel module's most compelling advantage beyond copy reduction is the potential for true zero-copy operation.

### Send Side (App -> RDMA)

Instead of copying from the user's buffer, DMA-map the application's pages directly:

```
 App calls write(fd, buf, 4096)
     |
     v
 get_user_pages_fast(buf, 1, 0, &page)    // Pin app's page
     |
     v
 ib_dma_map_page(dev, page, offset, len, DMA_TO_DEVICE)
     |
     v
 ib_post_send(qp, &wr)                    // NIC DMAs from app's page
     |
     v
 (CQ completion) -> put_page(page)         // Unpin after send completes
```

**Constraint**: The page must remain pinned until the send completion CQE arrives. This requires tracking outstanding pages and unpinning them in the CQ completion handler. If the application reuses the buffer before the send completes, the NIC may DMA stale or corrupted data — so either the `write()` must block until completion, or a copy-on-write mechanism is needed.

### Receive Side (RDMA -> App)

Pre-allocate pages for receive buffers. When data arrives, instead of copying into the application's buffer, flip the page into the socket's receive buffer:

```
 NIC DMAs into pre-allocated page (posted via ib_post_recv)
     |
     v
 (CQ completion) -> reorder -> ready for delivery
     |
     v
 App calls read(fd, buf, 4096)
     |
     v
 Option A: copy_to_iter (1 copy — baseline)
 Option B: page flip — replace app's page mapping (0 copies, complex)
```

Page flipping (Option B) requires manipulating the process's page tables, which is feasible but complex. It works best when the payload is page-aligned and page-sized.

### Copy Count Summary

| Implementation | Send Copies | Receive Copies | Total | Notes |
|---------------|------------|----------------|-------|-------|
| Userspace proxy | 2 | 2 | **4** | Fundamental UDS boundary limit |
| Kernel module (baseline) | 1 | 1 | **2** | copy_from_iter / copy_to_iter |
| Kernel module + send pin | 0 | 1 | **1** | get_user_pages + DMA map |
| Kernel module + full page flip | 0 | 0 | **0** | Page pinning + page table manipulation |

**Complexity vs. gain**: Baseline kernel module (2 copies) delivers most of the benefit with manageable complexity. Send-side page pinning (1 copy) is practical with careful lifetime management. Full zero-copy (0 copies) requires page table manipulation and has strict alignment constraints — likely only worthwhile for large, page-aligned transfers.


## 21.6 Rust in the Linux Kernel

### Current State (as of kernel v6.7+)

Rust has been in the Linux kernel since v6.1 (December 2022), with growing subsystem support:

| Available | Status |
|-----------|--------|
| `core` crate | Full — `core::result`, `core::option`, iterators, etc. |
| `alloc` crate | `Vec`, `BTreeMap`, `Box`, `Arc` with kernel allocator (`GFP_KERNEL`) |
| Sync primitives | `Mutex<T>`, `SpinLock<T>`, `Arc<T>`, `CondVar` |
| Module macros | `module!`, `module_init`, `module_exit` |
| Error handling | `kernel::error::Result`, maps to kernel errno |
| String types | `CStr`, `CString` for kernel string handling |
| Device model | `Registration`, `Module` traits (emerging) |

| Missing / Incomplete | Impact |
|---------------------|--------|
| RDMA subsystem bindings | No Rust wrappers for `ib_*` APIs — must write FFI |
| Socket layer bindings | No Rust wrappers for `struct socket`, `proto_ops` |
| kthread / workqueue | No Rust abstractions for kernel threads or work queues |
| Async runtime | No kernel async/await equivalent (no tokio) |
| Network stack | Limited bindings for skbuff, netfilter, etc. |

### Feasibility Assessment

Writing the full kernel module in Rust would require approximately **2,000-5,000 lines of unsafe FFI wrappers** before any proxy logic could be written. These wrappers would cover:

- `ib_alloc_pd`, `ib_create_cq`, `ib_create_qp`, `ib_post_send`, `ib_post_recv`, `ib_poll_cq` (RDMA verbs)
- `struct socket`, `proto_ops`, `kernel_recvmsg`, `kernel_sendmsg` (socket layer)
- `kthread_create`, `kthread_bind`, `wake_up_process` (threading)
- `alloc_pages`, `__free_pages`, `page_address` (memory)
- `rhashtable_init`, `rhashtable_lookup_fast`, `rhashtable_insert_fast` (hash table)

This is a substantial investment with ongoing maintenance cost as kernel APIs evolve.

### Recommended Approach: C Integration Layer + Rust Protocol Library

```
 +-------------------------------------------------------+
 |  Kernel Module                                         |
 |                                                        |
 |  +---------------------------+  +-------------------+  |
 |  | C integration layer       |  | Rust static lib   |  |
 |  |                           |  | (no_std + alloc)  |  |
 |  | socket ops (proto_ops)    |  |                   |  |
 |  | RDMA verbs (ib_* calls)   |  | frame codec       |  |
 |  | kthreads / workqueues     |  | credit state      |  |
 |  | buffer alloc / DMA map    |  | reorder buffer    |  |
 |  | module init / cleanup     |  | QP selection      |  |
 |  | /proc + GENL netlink      |  | protocol consts   |  |
 |  |                           |  |                   |  |
 |  | Calls Rust via C FFI:     |  | Exports C ABI:    |  |
 |  |   urp_frame_encode(...)   |  |   #[no_mangle]    |  |
 |  |   urp_frame_decode(...)   |  |   pub extern "C"  |  |
 |  |   urp_reorder_insert(...) |  |                   |  |
 |  |   urp_credit_check(...)   |  |                   |  |
 |  +---------------------------+  +-------------------+  |
 +-------------------------------------------------------+
```

**Rationale**:
1. The C layer handles all kernel API interactions — these APIs are C-native, well-documented in C, and kernel developers read C
2. The Rust library contains pure protocol logic — the same code used in the userspace proxy, compiled as `no_std + alloc`
3. Boundary is clean: C calls Rust for encode/decode/reorder/credits; Rust never calls kernel APIs directly
4. If Rust kernel bindings mature, the C layer can be incrementally replaced

**Alternative: Pure C module** — more practical near-term, larger contributor pool, no Rust toolchain dependency in kernel build. But loses the code-sharing benefit and Rust's memory safety guarantees for the protocol logic.


## 21.7 Code Sharing Strategy

### Extracting `uds-rdma-protocol`

The key to code sharing is extracting a `uds-rdma-protocol` crate that compiles for both `std` (userspace) and `no_std + alloc` (kernel) targets.

```
 crates/
   uds-rdma-protocol/    # no_std + alloc core (shared between userspace and kernel)
   uds-rdma-proxy/       # userspace binary (depends on uds-rdma-protocol with std feature)
   uds-rdma-bench/       # load generator
   uds-rdma-kmod/        # kernel module Rust library (depends on uds-rdma-protocol, no_std)
```

### Shared Components (in `uds-rdma-protocol`)

```rust
#![no_std]
extern crate alloc;

// Feature gate for std-specific functionality
#[cfg(feature = "std")]
extern crate std;

// --- Frame codec ---
#[repr(C, packed)]
pub struct FrameHeader {
    pub stream_id: u32,
    pub sequence_number: u64,
    pub frame_type: u8,
    pub flags: u8,
    pub credits_granted: u16,
    pub payload_length: u32,
}

pub const FRAME_HEADER_SIZE: usize = 20;

pub fn frame_encode(buf: &mut [u8], header: &FrameHeader) -> Result<(), ProtocolError> { ... }
pub fn frame_decode(buf: &[u8]) -> Result<FrameHeader, ProtocolError> { ... }

// --- Frame type constants ---
pub const FRAME_TYPE_DATA: u8    = 0x00;
pub const FRAME_TYPE_CONTROL: u8 = 0x01;
pub const FRAME_TYPE_PROBE: u8   = 0x02;

// --- Per-type flag constants ---
pub const DATA_FLAG_SYN: u8 = 1 << 0;
pub const DATA_FLAG_FIN: u8 = 1 << 1;
pub const DATA_FLAG_RST: u8 = 1 << 2;

pub const CTRL_FLAG_CREDIT: u8     = 1 << 0;
pub const CTRL_FLAG_QP_DISABLE: u8 = 1 << 1;
pub const CTRL_FLAG_QP_ENABLE: u8        = 1 << 2;
pub const CTRL_FLAG_STREAM_WINDOW: u8    = 1 << 3;

pub const PROBE_FLAG_PONG: u8 = 1 << 0;

// --- Credit state machine ---
pub struct CreditState { ... }
impl CreditState {
    pub fn can_send(&self) -> bool { ... }
    pub fn consume(&mut self) -> Result<(), FlowControlError> { ... }
    pub fn grant(&mut self, n: u16) { ... }
    pub fn pending_grants(&self) -> u16 { ... }
}

// --- Reorder buffer ---
use alloc::collections::BTreeMap;

pub struct ReorderBuffer {
    expected_seq: u64,
    buffer: BTreeMap<u64, alloc::vec::Vec<u8>>,
    max_buffered: usize,
}

impl ReorderBuffer {
    pub fn insert(&mut self, seq: u64, data: alloc::vec::Vec<u8>) -> Result<(), ReorderError> { ... }
    pub fn drain(&mut self) -> impl Iterator<Item = alloc::vec::Vec<u8>> + '_ { ... }
    pub fn gap_count(&self) -> usize { ... }
}

// --- QP selection strategies ---
pub fn qp_select_round_robin(seq: u64, num_qps: u32) -> u32 { ... }
pub fn qp_select_hash(stream_id: u32, seq: u64, num_qps: u32) -> u32 { ... }

// --- MTU / payload sizing ---
pub const ROCEV2_HEADER_OVERHEAD: usize = 44;
pub fn max_payload_for_mtu(ethernet_mtu: usize) -> usize { ... }
```

### Non-Shared Components (I/O layer, platform-specific)

| Component | Userspace | Kernel |
|-----------|-----------|--------|
| UDS I/O | io_uring | kernel_recvmsg / kernel_sendmsg |
| RDMA verbs | ibverbs (rdma-sys crate) | ib_* kernel API (C) |
| Async runtime | tokio | kthreads + work queues |
| Metrics | Prometheus (metrics crate) | tracepoints + /proc |
| Config | TOML + clap | GENL (`urp` CLI) |
| Logging | tracing crate | printk / dynamic debug |
| Buffer alloc | mmap(MAP_HUGETLB) | alloc_pages(GFP_KERNEL) |

### Benefit to the Userspace Codebase

Extracting `uds-rdma-protocol` improves the userspace codebase regardless of whether the kernel module is ever built:

1. **Cleaner architecture**: Protocol logic decoupled from I/O layer
2. **Better testing**: Protocol crate can be tested independently, fuzzed in isolation
3. **Reusability**: Other tools (debug utilities, protocol analyzers) can use the crate
4. **Documentation**: The shared crate's API boundary documents the protocol contract


## 21.8 Component Mapping: Userspace to Kernel

| Component | Userspace Implementation | Kernel Equivalent |
|-----------|------------------------|-------------------|
| Buffer allocation | `mmap(MAP_HUGETLB, ...)` | `alloc_pages(GFP_KERNEL, order)` |
| Huge pages | 2MB/1GB via `MAP_HUGETLB` | Compound pages (order 9 = 2MB) |
| Buffer free list | `crossbeam::ArrayQueue` | `list_head` + `spinlock_t` (k0-k1) → `page_pool` (k2) |
| DMA registration | `ibv_reg_mr(pd, addr, len)` | `ib_dma_map_page(dev, page, ...)` |
| CQ polling | `ibv_poll_cq` in dedicated thread | `ib_poll_cq` in kthread or CQ callback |
| CQ notification | `ibv_req_notify_cq` + `ibv_get_cq_event` | `ib_req_notify_cq` + `comp_handler` |
| Reorder buffer | `std::collections::BTreeMap` | `alloc::collections::BTreeMap` or kernel `rbtree` |
| Connection table | `std::collections::HashMap` | `rhashtable` + RCU |
| Timers | `tokio::time::sleep` | `mod_timer` / `hrtimer_start` |
| Metrics export | Prometheus HTTP (`/metrics`) | tracepoints + `/proc/urp/*` + perf counters |
| Configuration | TOML file + `clap` | GENL (`urp` CLI) + `/proc/urp/*` stats |
| Logging | `tracing` crate | `pr_info` / `pr_err` + dynamic debug (`dyndbg`) |
| CPU pinning | `sched_setaffinity` (nix crate) | `kthread_bind(thread, cpu)` |
| NUMA allocation | `numactl` / `set_mempolicy` | `alloc_pages_node(nid, ...)` |
| Graceful shutdown | Signal handler + `tokio::select!` | `module_exit` + `kthread_stop` |
| Error propagation | `anyhow::Result` / `thiserror` | `kernel::error::Result` / errno |


## 21.9 Comparison with NIC Driver Architecture

The kernel module's architecture closely mirrors Linux NIC driver design patterns. Both are kernel-resident data-plane engines that move data between application buffers and DMA-capable hardware using pre-allocated, recycled buffer pools and completion-driven polling. This is not coincidental — RDMA NICs and Ethernet NICs face the same fundamental challenge: bridging software-managed memory with hardware DMA engines efficiently.

Understanding this parallel is valuable in two ways: it validates that our design follows decades of proven NIC driver engineering, and it identifies specific kernel APIs (notably `page_pool` and NAPI) that we should adopt rather than reimplementing.

### Architectural Mapping

| NIC Driver Pattern | NIC Kernel API | Our RDMA Module | RDMA Kernel API |
|---|---|---|---|
| RX descriptor ring | Ring of pre-allocated DMA pages | SRQ + pre-posted receive buffers | `ib_create_srq`, `ib_post_recv` |
| TX descriptor ring | TX ring with DMA-mapped buffers | Send queue + TX buffer pool | `ib_post_send` |
| Completion polling | NAPI `poll()` with budget | CQ polling with batch limit | `ib_poll_cq` |
| Interrupt → poll transition | `napi_schedule()` from IRQ handler | `comp_handler` from CQ event | `ib_req_notify_cq` |
| Adaptive coalescing | ethtool `rx-usecs`, `rx-frames` | Adaptive CQ polling (k2) | Busy-poll / event-driven hybrid |
| Buffer recycling | `page_pool` (DMA maps persist) | Free list → `page_pool` (k2) | `ib_dma_map_page` / `page_pool` |
| Per-CPU allocation | `page_pool` per-CPU caches | Per-CPU buffer pools (k2) | `page_pool` per-CPU caches |
| NUMA-aware alloc | `dev_alloc_pages_node()` | NUMA-aware buffer alloc (k2) | `alloc_pages_node(nid, ...)` |
| Zero-copy RX | XDP / page reference flip | Page flip to socket buffer (k2) | Page table manipulation |
| Zero-copy TX | `sendfile` / splice / page pinning | Send-side page pinning (k2) | `get_user_pages_fast` + `ib_dma_map_page` |
| Receive coalescing | GRO (Generic Receive Offload) | Reorder buffer + in-order drain | BTreeMap / rbtree by sequence number |
| Send segmentation | TSO (TCP Segmentation Offload) | NIC segments frames > PMTU | Transparent in RDMA layer |
| Flow control | TCP window / ECN / PFC | Credit state machine | Frame header `credits_granted` field |
| Multi-queue / RSS | Receive Side Scaling | Multi-QP with ECMP path diversity | Multiple QPs across ECMP paths |
| Device state | `struct net_device` + driver priv | `struct urp_connection` + rhashtable | RCU-safe hash table |
| Lock-free lookup | RCU over device/flow tables | RCU over connection hash | `rhashtable_lookup_fast` |
| Configuration | ethtool / netlink | GENL (`urp` CLI) | GENL family `"urp"` |

### NAPI and Adaptive CQ Polling

NAPI's core insight: under low load, use interrupts (event-driven, saves CPU); under high load, switch to polling (avoids interrupt storms, amortizes per-completion overhead). Our CQ polling follows the same pattern:

```
 NIC Driver (NAPI):                        RDMA Module (CQ polling):

 IRQ fires                                 ib_req_notify_cq callback fires
   → napi_schedule()                         → wake kthread or schedule work
   → disable NIC interrupt                   → (CQ events suppressed while polling)
   → poll(budget=64)                         → ib_poll_cq(batch_size)
   → process packets                         → process CQEs (decode, reorder, deliver)
   → if budget not exhausted:                → if no more CQEs:
       napi_complete() + re-enable IRQ           re-arm ib_req_notify_cq
```

NAPI's budget (default 64 packets per poll cycle) maps directly to our CQ batch polling — process N CQEs before yielding. Both prevent any single queue from monopolizing the CPU.

### `page_pool` and Buffer Recycling

The kernel's `page_pool` API (since v4.18) was designed to solve exactly the buffer recycling problem that NIC drivers face — and that our RDMA module inherits:

| Concern | Without page_pool | With page_pool |
|---------|------------------|----------------|
| DMA mapping | `ib_dma_map_page` on every alloc, `ib_dma_unmap_page` on every free | DMA map persists across recycles — map once, reuse indefinitely |
| Allocation contention | `spinlock_t` on shared free list | Per-CPU caches — lock-free on the fast path |
| Page lifecycle | Manual `list_head` management, explicit alloc/free tracking | Refcount-based: `refcnt == 1` → recycle to pool, `refcnt > 1` → free |
| NUMA awareness | Manual `alloc_pages_node` | Pool-level NUMA node affinity |
| Cache locality | No guarantee | Per-CPU caches keep hot pages local |

**Adoption path**: In k0-k1, use the simple `list_head + spinlock_t` free list (Section 21.3.3). In k2, replace with `page_pool`:

```c
 struct page_pool_params pp_params = {
     .flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
     .pool_size = buffer_count,
     .nid = dev_to_node(&rdma_dev->dev),
     .dev = &rdma_dev->dev,
     .dma_dir = DMA_BIDIRECTIONAL,
     .max_len = buffer_size,
 };
 struct page_pool *pool = page_pool_create(&pp_params);

 // Allocate (returns DMA-mapped page, potentially from per-CPU cache):
 struct page *page = page_pool_dev_alloc_pages(pool);

 // Use with RDMA:
 dma_addr_t dma = page_pool_get_dma_addr(page);  // cached, no IOMMU call
 // ... ib_post_recv / ib_post_send using dma addr ...

 // Recycle (page stays DMA-mapped, returns to per-CPU cache):
 page_pool_put_page(pool, page, buffer_size, true);
```

**Caveat**: `page_pool` was designed for `struct net_device` consumers. Using it with RDMA's `ib_device` may require adapting the DMA mapping callbacks, since RDMA uses `ib_dma_map_page` (which goes through the RDMA device's DMA ops) rather than the generic `dma_map_page`. This adaptation is the primary engineering risk and should be validated early in k2.

### XDP and Zero-Copy Receive

XDP (eXpress Data Path) processes packets in the NIC's original DMA buffer before any copy — the BPF program operates on the page the NIC DMA'd into. If the packet is redirected (`XDP_REDIRECT`), the page reference is transferred without copying.

Our zero-copy receive path ([Section 21.5](#215-zero-copy-potential)) follows the same principle: the NIC DMAs RDMA messages into pre-allocated pages, and instead of `copy_to_iter`, we flip the page into the receiving socket's buffer. AF_XDP takes this further — zero-copy all the way to userspace via shared umem — analogous to our userspace proxy's dual-registered buffers (io_uring + ibverbs MR) but in the reverse direction.

### Key Difference: Connection State

Standard NIC drivers are largely **stateless** at the driver level — connection state (TCP windows, sequence numbers, retransmission timers) lives in the TCP/IP stack above. The driver just moves frames between the wire and the kernel.

Our module is **stateful**: per-connection credit tracking, reorder buffers, sequence numbers, stream lifecycle (SYN/FIN/RST). This places our module's complexity between a NIC driver (simple, stateless frame mover) and a full protocol stack (TCP, with complex state machines). It is most comparable to a **TCP Offload Engine (TOE)** or the firmware running inside an RDMA NIC — a transport-aware, connection-aware data-plane engine.

This statefulness is the primary source of complexity and risk (Sections 21.10, 21.11) — a bug in a stateless NIC driver corrupts one packet; a bug in our stateful module can corrupt an entire connection's stream.


## 21.10 Advantages and Disadvantages

### Advantages

| Advantage | Detail |
|-----------|--------|
| **2 fewer copies** | Eliminates both UDS user/kernel boundary crossings (saves 200-500ns per message) |
| **Zero syscall overhead** | No `io_uring_enter`, `read`, `write` — already in kernel context |
| **Simpler buffer management** | No dual-registration; single DMA mapping per buffer |
| **Lower context switch cost** | No user/kernel transitions on the data path |
| **Zero-copy potential** | Page pinning + page flipping can eliminate all copies |
| **Kernel scheduling access** | Can use `kthread_bind`, priority inheritance, RT scheduling directly |
| **No CAP_IPC_LOCK needed** | Kernel can pin memory without the capability requirement |
| **Fully transparent** | Applications see a standard UDS — no code changes required |

### Disadvantages

| Disadvantage | Detail |
|-------------|--------|
| **Crash impact** | Bug in the module can panic the kernel — no process isolation |
| **Debugging difficulty** | No gdb attach, no easy printf-debugging; must use ftrace, kprobes, crash dumps |
| **Kernel API instability** | Internal kernel APIs have no stability guarantee; module may need updates each kernel release |
| **No standard library** | No `std`, no heap allocator (must use kernel's), no threads (kthreads), no async |
| **Deployment complexity** | Must build per-kernel-version (DKMS) or ship as out-of-tree module; harder than copying a binary |
| **Security surface** | Runs with kernel privilege; a vulnerability here is a kernel exploit |
| **Testing difficulty** | Must test in VMs (KUnit, MicroVM) — cannot run unit tests on host without risk |
| **Module reload risk** | Reloading the module drops all active connections (no graceful migration) |
| **Limited Rust bindings** | No RDMA or socket bindings in kernel Rust; must write 2,000-5,000 lines of FFI or use C |
| **Regulatory/compliance** | Some environments prohibit custom kernel modules (SOC2, FedRAMP, managed Kubernetes) |


## 21.11 Risk Assessment

### Technical Risks

| Risk | Severity | Likelihood | Mitigation |
|------|----------|-----------|------------|
| Kernel panic from buffer management bug | Critical | Medium | Extensive KUnit tests, Miri on shared crate, KASAN/KMEMLEAK in VMs |
| Memory leak in connection teardown | High | Medium | RCU + explicit resource tracking, `/proc` leak counters, KMEMLEAK |
| RDMA kernel API changes between versions | Medium | High | Pin to LTS kernels, CI matrix across kernel versions |
| Socket API changes (proto_ops) | Medium | Low | proto_ops has been stable for years; monitor kernel mailing list |
| Concurrency bugs (RCU, spinlocks) | High | Medium | KCSAN (kernel concurrency sanitizer), lockdep, provably correct ordering |
| Performance regression vs. userspace | Medium | Low | k0 PoC measures actual benefit before committing to full implementation |
| Container incompatibility | Medium | Medium | Must work with mount namespaces, network namespaces; test in container runtimes |
| SELinux/AppArmor policy conflicts | Medium | Medium | Document required security policy exceptions; provide reference profiles |
| Out-of-tree module taints kernel | Low | Certain | Accepted trade-off; document for operations teams; upstream long-term |

### Organizational Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Small kernel-developer pool | Slow development, review bottlenecks | C integration layer reduces Rust kernel expertise requirement |
| Upstream maintenance burden | Ongoing cost per kernel release | DKMS for wide support; target LTS kernels; upstream if adoption warrants |
| Kernel version test matrix | CI complexity | Nix flake manages kernel builds; MicroVM testing per version |


## 21.12 Phased Approach

The kernel module runs as a **parallel track**, independent of the userspace v0-v4 roadmap:

```
 Userspace track:
 v0 (TCP)  -->  v1 (rsockets)  -->  v2 (ibverbs)  -->  v3 (multi-QP)  -->  v4 (one-sided)
     |                                    |                                       |
     |                                    | extract shared crate                  |
     |                                    v                                       |
     |                            uds-rdma-protocol                               |
     |                            (no_std + alloc)                                |
     |                                    |                                       |
     |                                    v                                       v
 Kernel track:                    k0 (PoC) -------> k1 (functional) -------> k2 (optimized)
```

### Phase k0: Proof of Concept

**Goal**: Measure the actual benefit of eliminating UDS copies.

- Minimal C kernel module (~1,500 lines)
- Virtual UDS endpoint (approach D): module creates a single listening socket at a `module_param`-configured path (PoC only; replaced by GENL in k1)
- Single QP to a hardcoded remote peer, no flow control, no reorder, no multiplexing
- Direct `ib_post_send` / `ib_post_recv` with a small fixed buffer pool
- Custom `sendmsg`/`recvmsg` on accepted connections — validates the virtual endpoint data path
- Basic `/proc/urp/stats` for throughput and latency counters
- Build and test via MicroVM infrastructure (existing `nix/microvms/`)
- **Decision gate**: If measured latency improvement is <15% vs. userspace v2, the kernel module path may not justify its complexity — userspace `splice()` optimization may suffice

### Phase k1: Functional

**Goal**: Feature parity with userspace v2 (native ibverbs).

- Link Rust `uds-rdma-protocol` crate (compiled as static `no_std` library)
- Multi-QP support with reorder buffer (kernel `rbtree` or shared `BTreeMap`)
- QP health probes ([Section 8a](08a-qp-health-probes.md)): shared `PingPayload`/`PongPayload` structs, `QpProbeState` logic from protocol crate. Probe timer uses `hrtimer_start()` for sub-ms qualifying intervals. Timestamps via `ktime_get_ns()` / `ktime_get_real_ns()`.
- QP health state machine ([Section 8.9](08-multi-qp-ecmp.md#89-qp-health-state-machine)): shared `QpHealthState` enum (Qualifying/Active/Draining/Removed), adaptive selector (`qp_select_adaptive()` from shared crate), working set bitmap
- QP_DISABLE control frames decoded by shared crate's `frame_decode` (`frame_type = CONTROL`, `CTRL_FLAG_QP_DISABLE`)
- Credit-based flow control (shared `CreditState`)
- Stream multiplexing over QPs
- Connection lifecycle (SYN/FIN/RST via `frame_type = DATA`)
- GENL interface ([Section 22](22-genl-interface.md)): register `"urp"` GENL family with `NEW`/`DEL`/`SET`/`GET_ENDPOINT` commands, multicast `"events"` group for state-change notifications. Replaces k0's `module_param` — module starts empty, endpoints added via `urp add`
- `urp` CLI tool ([Section 23](23-cli-tool.md)): Rust binary using `neli` crate for GENL communication. Subcommands: `add`, `remove`, `set`, `show`, `stats`, `monitor`, `drain`
- `/proc/urp/*` metrics (connection count, throughput, credits, reorder depth, QP health state, probe RTT)
- KUnit test suite for in-kernel logic

### Phase k2: Optimized

**Goal**: Exploit kernel-only optimizations that userspace cannot achieve.

- Adopt `page_pool` API for buffer management ([Section 21.9](#219-comparison-with-nic-driver-architecture)): `page_pool_create()` with `PP_FLAG_DMA_MAP`, DMA map persistence across recycles, per-CPU caching (replaces `list_head + spinlock_t` free list)
- Zero-copy send via `get_user_pages_fast` + `ib_dma_map_page`
- Page flipping on receive (for aligned, page-sized payloads)
- NAPI-style adaptive CQ polling (busy-poll under load, event-driven when idle)
- NUMA-aware `page_pool` with `nid` affinity for buffer allocation
- Hardware RDMA performance comparison (ConnectX-6 or equivalent)
- Live endpoint tuning via `urp set` (buffer_count, num_qps)

### Prerequisites (Before k0)

| Prerequisite | Description |
|-------------|-------------|
| Extract `uds-rdma-protocol` | Create the `no_std + alloc` shared crate from existing protocol code |
| Validate `no_std` build | Ensure `BTreeMap`, `Vec`, frame codec compile without `std` |
| Kbuild integration | Add kernel module build rules to Nix flake |
| KUnit in MicroVMs | Configure MicroVM kernels with `CONFIG_KUNIT=y` |
| MicroVM RDMA | Verify `rdma_rxe` + in-kernel verbs work in MicroVM environment |


[Back to Design Overview](../DESIGN.md)
