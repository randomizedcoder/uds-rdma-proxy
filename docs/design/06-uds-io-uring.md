# UDS I/O Layer (io_uring)

## 6.1 Why io_uring

Traditional `read()`/`write()` syscalls on UDS incur per-call overhead: user-to-kernel transition, argument validation, scheduling. For a proxy processing millions of messages per second, this overhead is significant.

`io_uring` provides:

- **Syscall batching**: Multiple I/O operations can be submitted with a single `io_uring_enter()` call, or zero calls with SQ polling.
- **Registered buffers**: `IORING_REGISTER_BUFFERS` pre-registers userspace buffers with the kernel, eliminating per-I/O `get_user_pages()` calls.
- **Kernel-side polling**: `IORING_SETUP_SQPOLL` makes a kernel thread poll the submission queue, eliminating even the `io_uring_enter()` syscall.
- **Completion batching**: Multiple completions can be reaped in a single pass.

## 6.2 Integration with RDMA Buffers

The key optimization is **dual-registering** the same buffer pool with both io_uring and ibverbs:

```
 +-----------------------------------+
 | Buffer Pool (mmap'd, huge pages)  |
 |                                   |
 | Registered with:                  |
 |   1. ibv_reg_mr()  (RDMA NIC)    |
 |   2. IORING_REGISTER_BUFFERS     |
 |      (kernel io_uring)            |
 +-----------------------------------+

 UDS Read Path:
   io_uring SQE (IORING_OP_READ_FIXED, buf_index=slot_idx)
     -> kernel writes UDS data directly into registered buffer slot
     -> CQE fires
     -> encode frame header in same buffer (prepend 20 bytes)
     -> ibv_post_send() from same buffer
     -> NIC DMAs from same buffer

 Result: ONE buffer, no intermediate copies between io_uring and RDMA
```

> **Kernel module variant**: The kernel module alternative ([Section 21](21-kernel-module.md)) eliminates dual-registration entirely. There is no io_uring layer in kernel space — the module reads from UDS via `kernel_recvmsg` directly into DMA-mapped buffers managed by the kernel's `page_pool` API. Buffers are DMA-mapped once and recycled without remapping, providing the same zero-copy benefit with simpler lifetime management ([Section 21.9](21-kernel-module.md#219-comparison-with-nic-driver-architecture)).

## 6.3 Buffer Lifetime Management

A buffer slot must remain valid (not returned to the pool) until all operations on it are complete:

```
 Slot allocated from pool
       |
       v
 io_uring read submitted (SQE)
       |
       v
 io_uring read completed (CQE)  -- buffer now contains UDS data
       |
       v
 Frame header encoded in buffer
       |
       v
 ibv_post_send() submitted       -- NIC may DMA at any time
       |
       v
 ibv_poll_cq() returns send CQE  -- NIC is done with buffer
       |
       v
 Slot returned to pool            -- SAFE to reuse
```

The pump must track both the io_uring CQE and the RDMA send CQE before releasing the buffer. In the receive direction, the buffer is held from `recv CQE` until the `io_uring write CQE` confirms the UDS write completed.

## 6.4 Crate Selection

| Phase | Crate | Rationale |
|-------|-------|-----------|
| v0-v1 | `tokio-uring` | Async runtime with io_uring backend. Easier integration, good for prototyping. |
| v2+ | `io-uring` (raw) | Low-level bindings from tokio-rs. Maximum control over SQE/CQE batching, registered buffers, and poll modes. Evaluate if tokio-uring overhead is measurable. |

The UDS I/O is abstracted behind a trait so both backends can be supported:

```rust
trait UdsIo {
    async fn read_into(&self, buf: &mut RegisteredBuffer) -> io::Result<usize>;
    async fn write_from(&self, buf: &RegisteredBuffer, len: usize) -> io::Result<()>;
    async fn shutdown(&self) -> io::Result<()>;
}
```


[Back to Design Overview](../DESIGN.md)
