/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp_ffi.h -- C prototypes for the Rust ReorderBuffer staticlib.
 *
 * The matching Rust definitions live in
 * crates/uds-rdma-protocol-ffi/src/ffi.rs. The build wiring is in
 * nix/urp-protocol-ffi.nix.
 *
 * This header is only meaningful when CONFIG_URP_REORDER_RUST=y. The
 * default kernel build uses kernel/urp_reorder.c (a native rbtree-based
 * implementation) and never includes this header.
 */

#ifndef _URP_FFI_H
#define _URP_FFI_H

#ifdef CONFIG_URP_REORDER_RUST

#include <linux/types.h>
#include <linux/compiler_attributes.h>	/* __noreturn */

/* Opaque handle type. The Rust side returns pointers to a heap-allocated
 * `UrpRustReorder`; C only ever passes the pointer around.
 */
struct urp_rust_reorder;

/*
 * Symbols the kernel module *exports* to the Rust staticlib. The Rust
 * side declares these as `extern "C"` and the linker resolves them when
 * urp.ko is built. Implementations live in kernel/urp_reorder_rust.c.
 *
 * urp_kalloc / urp_kfree: GFP_KERNEL-equivalent allocations. Must be
 *   called only from sleepable context (the per-stream mutex serializes,
 *   and the data-path RX work runs in a workqueue).
 * urp_panic_abort: terminal abort path. Must not return; calls BUG().
 */
void *urp_kalloc(size_t size, size_t align);
void urp_kfree(void *ptr, size_t size, size_t align);
void __noreturn urp_panic_abort(void);

/*
 * Symbols the Rust staticlib *exports* back to the kernel module. See
 * the rustdoc comments on each function in
 * crates/uds-rdma-protocol-ffi/src/ffi.rs for full semantics; the
 * one-line summaries below are reminders.
 */

/* Allocate a new reorder buffer. Returns NULL on allocation failure. */
struct urp_rust_reorder *urp_rust_reorder_new(u64 initial_expected,
					      size_t max_buffered);

/* Free a previously allocated buffer. Passing NULL is a no-op. */
void urp_rust_reorder_free(struct urp_rust_reorder *rb);

/* Insert a frame. Returns 0 on success or a negative errno (-EEXIST,
 * -ENOBUFS, -ENOMEM, -EINVAL). Drained in-order frames go onto an
 * internal queue; pop them with urp_rust_reorder_drain_next().
 */
int urp_rust_reorder_insert(struct urp_rust_reorder *rb, u64 seq,
			    const u8 *data, size_t data_len);

/* Pop the next in-order frame from the drain queue.
 *   0:        wrote *out_seq, copied payload into out_data, set *inout_len
 *             to the payload length.
 *   -ENOENT:  no frame available.
 *   -ENOBUFS: payload doesn't fit; *inout_len is set to required size and
 *             the frame remains queued. Caller may retry with a larger buf.
 *   -EINVAL:  argument error (NULL handle, NULL out pointers, or NULL
 *             out_data with non-zero *inout_len).
 */
int urp_rust_reorder_drain_next(struct urp_rust_reorder *rb, u64 *out_seq,
				u8 *out_data, size_t *inout_len);

/* The next sequence number expected for in-order delivery. */
u64 urp_rust_reorder_next_expected(const struct urp_rust_reorder *rb);

/* The number of frames currently buffered out-of-order. */
size_t urp_rust_reorder_gap_count(const struct urp_rust_reorder *rb);

/* The number of frames already drained but not yet consumed via
 * urp_rust_reorder_drain_next().
 */
size_t urp_rust_reorder_drain_pending(const struct urp_rust_reorder *rb);

#endif /* CONFIG_URP_REORDER_RUST */

#endif /* _URP_FFI_H */
