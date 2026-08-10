/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Backend-agnostic interface for the per-stream multi-QP reorder
 * buffer.
 *
 * Phase 3a Step 5 introduces this interface. The default backend is a
 * native C rbtree-based implementation in kernel/urp_reorder.c.  Step 5b
 * will add an optional Rust backend behind CONFIG_URP_REORDER_RUST that
 * shims to the symbols declared in kernel/include/urp_ffi.h. Both
 * backends export this identical API so the data-path code can use one
 * abstraction.
 *
 * Semantics mirror uds_rdma_protocol::reorder::ReorderBuffer (the Rust
 * reference implementation in the protocol crate). Caller copies in
 * payloads on insert; caller copies out payloads on drain_next. The
 * buffer copies internally so callers don't have to worry about the
 * lifetime of the source buffer beyond the insert call.
 */
#ifndef _URP_REORDER_H
#define _URP_REORDER_H

#include <linux/types.h>

/* Opaque handle. Both backends allocate internally and return a ptr. */
struct urp_reorder;

/*
 * Allocate a new reorder buffer.
 *   @initial_expected: first sequence number expected (typically 0).
 *   @max_buffered:     maximum number of out-of-order frames to hold.
 * Returns NULL on allocation failure or invalid arguments.
 */
struct urp_reorder *urp_reorder_alloc(u64 initial_expected, u32 max_buffered);

/* Free a buffer allocated with urp_reorder_alloc(). NULL-safe. */
void urp_reorder_free(struct urp_reorder *rb);

/*
 * Insert a frame. The buffer copies @data internally.
 *   0:        inserted; any drainable in-order frames are now on the
 *             internal drain queue (pop via urp_reorder_drain_next).
 *   -EEXIST:  duplicate (seq < next_expected, or already pending).
 *   -ENOBUFS: buffer is at max_buffered capacity and seq != next_expected.
 *   -ENOMEM:  allocation failed.
 *   -EINVAL:  null handle or null data with non-zero length.
 */
int urp_reorder_insert(struct urp_reorder *rb, u64 seq,
		       const u8 *data, size_t data_len);

/*
 * Pop the next in-order frame off the drain queue. Caller provides
 * @out_data of size *inout_len; payload is copied in and *inout_len is
 * updated to the payload's length.
 *   0:        wrote a frame.
 *   -ENOENT:  drain queue is empty.
 *   -ENOBUFS: caller's buffer too small (*inout_len set to required size,
 *             frame remains queued).
 *   -EINVAL:  bad arguments.
 */
int urp_reorder_drain_next(struct urp_reorder *rb, u64 *out_seq,
			   u8 *out_data, size_t *inout_len);

/* The next sequence number expected for in-order delivery. */
u64 urp_reorder_next_expected(const struct urp_reorder *rb);

/* Number of frames currently buffered out-of-order. */
size_t urp_reorder_gap_count(const struct urp_reorder *rb);

/* Number of frames already drained but not yet popped via drain_next. */
size_t urp_reorder_drain_pending(const struct urp_reorder *rb);

#endif /* _URP_REORDER_H */
