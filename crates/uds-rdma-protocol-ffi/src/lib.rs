//! C-ABI staticlib bridge for `uds-rdma-protocol`, intended to be linked
//! into the `urp.ko` kernel module when `CONFIG_URP_REORDER_RUST=y`.
//!
//! This crate is **never** linked from userspace. It provides:
//!   * A `#[panic_handler]` that calls back into kernel-supplied `urp_panic_abort`.
//!   * A `#[global_allocator]` that calls back into kernel-supplied
//!     `urp_kalloc` / `urp_kfree`.
//!   * `#[no_mangle] extern "C"` exports for the `ReorderBuffer` API.
//!
//! Only the [`ReorderBuffer`] is bridged — `frame`, `credit`, and `qp`
//! selection are ported to C in the kernel module directly because their
//! state is small and stack-only. The reorder buffer is the one component
//! whose heap-backed `BTreeMap<u64, Vec<u8>>` makes a Rust implementation
//! interesting to benchmark against the C `rbtree` reference (see
//! `kernel/urp_reorder.c`).
//!
//! Lifetime / ownership rules:
//! * Handles are opaque pointers returned by `urp_rust_reorder_new()` and
//!   freed by `urp_rust_reorder_free()`. Concurrent calls on the same handle
//!   are unsafe; the kernel side serializes via the per-stream mutex.
//! * Frame payloads handed in via `urp_rust_reorder_insert()` are *copied*
//!   into Rust-managed storage (which on the kernel side ultimately calls
//!   `urp_kalloc`). The caller may reuse / free its buffer after the call.
//! * Frames produced by `urp_rust_reorder_drain_next()` are copied into the
//!   caller-provided output buffer; the Rust side frees its internal copy.

#![no_std]

extern crate alloc;

mod kernel_runtime;
pub mod ffi;

// Re-export the FFI symbols so they live in the staticlib.
#[allow(unused_imports)]
pub use ffi::*;
