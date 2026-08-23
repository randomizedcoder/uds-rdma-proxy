/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared reorder-buffer test scenarios (op-scripts), driven by BOTH the
 * in-kernel KUnit suite (kernel/urp_test.c) and the userspace units check
 * (tools/urp-reorder-units.c, nix check urp-reorder-units). One data set,
 * two drivers (KUNIT_EXPECT_* in-kernel, CHECK_EQ in userspace) -- that is
 * what keeps the default C rbtree backend's coverage in lock-step across
 * the VM and sandbox gates.
 *
 * Payload convention: an INSERT/DRAIN row's payload byte(i) = (u8)(seq + i),
 * so a DRAIN row verifies the seq AND the bytes without a separate payload
 * table. Trailing zero-initialised rows are ROP_END and terminate a script.
 *
 * The includer must have <linux/types.h>-style fixed-width types and errno
 * available; both are pulled in below (kernel vs tools/libc split).
 */
#ifndef _URP_REORDER_CASES_H
#define _URP_REORDER_CASES_H

#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

enum rop_kind {
	ROP_END = 0,
	ROP_INSERT,		/* insert @seq, @len payload bytes; expect @want_ret */
	ROP_DRAIN,		/* drain_next into a @bufsz buffer (0 => full):
				 * want_ret 0       => expect @seq + @len bytes;
				 * want_ret -ENOBUFS => expect required size == @len */
	ROP_EXP_NEXT,		/* assert next_expected == @seq */
	ROP_EXP_GAP,		/* assert gap_count (pending) == @seq */
	ROP_EXP_DRAINPEND,	/* assert drain_pending (drained-not-popped) == @seq */
};

struct rop {
	enum rop_kind	kind;
	u64		seq;
	u32		len;
	u32		bufsz;
	int		want_ret;
};

struct reorder_scenario {
	const char	*name;
	u64		initial;
	u32		max_buffered;
	struct rop	ops[24];
};

/*
 * Positive / boundary / corner scenarios. Folds the former one-scenario-per-
 * function KUnit tests (in_order, out_of_order, duplicate, already_delivered,
 * buffer_full, seq_saturates) in as named rows and adds the previously-
 * uncovered paths: deep out-of-order, drain_pending accounting,
 * initial_expected != 0, zero-length payloads, max_buffered == 1,
 * full-then-relieve backpressure, and the drain size-probe.
 */
static const struct reorder_scenario urp_reorder_scenarios[] = {
	{ "in_order", 0, 64, {
		{ ROP_INSERT, 0, 1 }, { ROP_DRAIN, 0, 1 },
		{ ROP_INSERT, 1, 1 }, { ROP_DRAIN, 1, 1 },
		{ ROP_INSERT, 2, 1 }, { ROP_DRAIN, 2, 1 },
		{ ROP_EXP_NEXT, 3 },
	} },
	{ "out_of_order", 0, 64, {
		{ ROP_INSERT, 2, 1 },
		{ ROP_DRAIN, 0, 0, 0, -ENOENT },
		{ ROP_EXP_GAP, 1 },
		{ ROP_INSERT, 0, 1 }, { ROP_DRAIN, 0, 1 },
		{ ROP_INSERT, 1, 1 },
		{ ROP_DRAIN, 1, 1 }, { ROP_DRAIN, 2, 1 },
		{ ROP_EXP_NEXT, 3 }, { ROP_EXP_GAP, 0 },
	} },
	{ "deep_descending", 0, 64, {
		{ ROP_INSERT, 4, 1 }, { ROP_INSERT, 3, 1 },
		{ ROP_INSERT, 2, 1 }, { ROP_INSERT, 1, 1 },
		{ ROP_EXP_GAP, 4 }, { ROP_EXP_NEXT, 0 },
		{ ROP_INSERT, 0, 1 },
		{ ROP_EXP_GAP, 0 }, { ROP_EXP_DRAINPEND, 5 },
		{ ROP_DRAIN, 0, 1 }, { ROP_DRAIN, 1, 1 }, { ROP_DRAIN, 2, 1 },
		{ ROP_DRAIN, 3, 1 }, { ROP_DRAIN, 4, 1 },
		{ ROP_EXP_NEXT, 5 },
	} },
	{ "drain_pending_accounting", 0, 64, {
		{ ROP_INSERT, 2, 1 }, { ROP_INSERT, 1, 1 },
		{ ROP_INSERT, 0, 1 },
		{ ROP_EXP_GAP, 0 }, { ROP_EXP_DRAINPEND, 3 },
		{ ROP_DRAIN, 0, 1 }, { ROP_EXP_DRAINPEND, 2 },
		{ ROP_DRAIN, 1, 1 }, { ROP_DRAIN, 2, 1 },
		{ ROP_EXP_DRAINPEND, 0 },
		{ ROP_DRAIN, 0, 0, 0, -ENOENT },
	} },
	{ "initial_nonzero", 100, 64, {
		{ ROP_INSERT, 100, 1 }, { ROP_DRAIN, 100, 1 },
		{ ROP_INSERT, 101, 1 }, { ROP_DRAIN, 101, 1 },
		{ ROP_EXP_NEXT, 102 },
		{ ROP_INSERT, 99, 1, 0, -EEXIST },
	} },
	{ "zero_length_payload", 0, 64, {
		{ ROP_INSERT, 0, 0 }, { ROP_DRAIN, 0, 0 },
		{ ROP_EXP_NEXT, 1 },
	} },
	{ "already_delivered_and_pending_dup", 0, 64, {
		{ ROP_INSERT, 0, 1 }, { ROP_DRAIN, 0, 1 },
		{ ROP_INSERT, 0, 1, 0, -EEXIST },	/* seq < next_expected */
		{ ROP_INSERT, 2, 1 },
		{ ROP_INSERT, 2, 1, 0, -EEXIST },	/* already pending */
	} },
	{ "buffer_full", 0, 2, {
		{ ROP_INSERT, 2, 1 }, { ROP_INSERT, 3, 1 },
		{ ROP_INSERT, 4, 1, 0, -ENOBUFS },
		{ ROP_INSERT, 0, 1 },			/* in-order accepted when full */
		{ ROP_EXP_NEXT, 1 },
	} },
	{ "max_buffered_one", 0, 1, {
		{ ROP_INSERT, 2, 1 },
		{ ROP_INSERT, 3, 1, 0, -ENOBUFS },
		{ ROP_INSERT, 0, 1 },
		{ ROP_EXP_NEXT, 1 }, { ROP_EXP_GAP, 1 },
		{ ROP_DRAIN, 0, 1 },
		{ ROP_INSERT, 1, 1 },
		{ ROP_EXP_NEXT, 3 },
		{ ROP_DRAIN, 1, 1 }, { ROP_DRAIN, 2, 1 },
	} },
	{ "full_then_relieve", 0, 3, {
		{ ROP_INSERT, 1, 1 }, { ROP_INSERT, 2, 1 }, { ROP_INSERT, 3, 1 },
		{ ROP_EXP_GAP, 3 },
		{ ROP_INSERT, 4, 1, 0, -ENOBUFS },	/* window full */
		{ ROP_INSERT, 0, 1 },			/* drains 0..3, frees space */
		{ ROP_EXP_NEXT, 4 }, { ROP_EXP_GAP, 0 },
		{ ROP_INSERT, 4, 1 },			/* now accepted */
		{ ROP_EXP_NEXT, 5 },
		{ ROP_DRAIN, 0, 1 }, { ROP_DRAIN, 1, 1 }, { ROP_DRAIN, 2, 1 },
		{ ROP_DRAIN, 3, 1 }, { ROP_DRAIN, 4, 1 },
	} },
	{ "size_probe", 0, 64, {
		{ ROP_INSERT, 0, 8 },
		{ ROP_DRAIN, 0, 8, 4, -ENOBUFS },	/* buf too small: required 8 */
		{ ROP_DRAIN, 0, 8, 8, 0 },		/* exact fit (off-by-one) */
		{ ROP_EXP_NEXT, 1 },
	} },
	{ "seq_saturates", U64_MAX, 16, {		/* Rust reorder_ops regression */
		{ ROP_INSERT, U64_MAX, 1 },
		{ ROP_EXP_NEXT, U64_MAX },
	} },
};

#endif /* _URP_REORDER_CASES_H */
