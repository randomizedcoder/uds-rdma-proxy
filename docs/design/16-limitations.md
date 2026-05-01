# Limitations & Non-Goals

## 16.1 Fundamental Limitations

| Limitation | Description | Mitigation |
|-----------|-------------|------------|
| **SCM_RIGHTS** | File descriptor passing over UDS cannot be tunneled. FDs are kernel-local objects. | Two modes: **Strict** (return `EOPNOTSUPP`, refuse the connection) or **Graceful** (strip ancillary data, forward payload, log high-severity alert). See below. |
| **SCM_CREDENTIALS** | Process credential passing is kernel-local. | Can be emulated by including credential info in the framing protocol, but the kernel won't enforce it on the remote side. |
| **Copy overhead** | Minimum 4 copies end-to-end (2 per UDS hop). Inherent to the UDS + userspace proxy model. | Minimize everything else (syscalls, allocation, TLB misses). |
| **No encryption** | RoCEv2 has no built-in encryption or authentication. | Assumed trusted data center fabric. If needed, add application-level encryption or IPsec (but this defeats RDMA's kernel bypass). |

#### UDS Ancillary Data Handling Modes

Applications often send `SCM_RIGHTS` or `SCM_CREDENTIALS` as part of a single `sendmsg()` call alongside regular data. Simply "rejecting" the connection is difficult because the ancillary data is interleaved with the byte stream. The proxy supports two configurable modes:

**Strict Mode** (`ancillary_data = "strict"`):
- The proxy uses `recvmsg()` instead of `read()` to detect ancillary data.
- If any control message (`cmsg`) is present, the proxy immediately returns `EOPNOTSUPP` to the application (via RST on the UDS connection) and closes the stream.
- Use this mode when data integrity is paramount and the application is known not to use FD passing.

**Graceful Mode** (`ancillary_data = "graceful"`):
- The proxy detects ancillary data via `recvmsg()` and **strips it** -- only the regular payload bytes are forwarded through RDMA.
- A high-severity alert is logged (and a `uds_rdma_proxy_ancillary_data_stripped_total` counter is incremented) each time ancillary data is detected.
- The connection stays alive. This prevents total connection crashes for applications that send non-critical metadata (e.g., `SCM_CREDENTIALS` for logging purposes) alongside their main data stream.
- Use this mode when the application might send ancillary data that is not essential to correctness.

**Default**: Strict mode. Graceful mode is opt-in because silently stripping metadata can cause subtle application bugs that are harder to diagnose than a clear rejection.

**Implementation note**: Using `recvmsg()` instead of `read()` adds slight overhead (larger syscall struct). In the hot path, the proxy should detect whether the UDS peer has requested credential passing (`SO_PASSCRED`) at connection time and only use `recvmsg()` when needed. If the socket is not configured for ancillary data, the fast `read()` path is used.

## 16.2 Non-Goals (v1)

- **SOCK_DGRAM UDS**: Stream sockets only. Datagram support could be added later (actually maps more naturally to RDMA SEND/RECV since message boundaries are preserved).
- **Abstract namespace sockets**: Supported for connecting, but the proxy itself always creates a filesystem-path UDS socket for its listener.
- **Windows/macOS**: Linux only. io_uring and RDMA are Linux-specific.
- **Userspace TCP (DPDK/AF_XDP)**: The UDS hop is always through the kernel. True kernel bypass on the UDS side would require a different approach entirely.


[Back to Design Overview](../DESIGN.md)
