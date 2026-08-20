/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp-fast per-buffer ownership state machine (design 31 section 31.2).
 *
 * A registered pool buffer is either APP_OWNED or KERNEL_OWNED, and the
 * SQE/CQE pair is the handoff: SEND/RECV flips APP -> KERNEL (the NIC now owns
 * the pages), completion flips KERNEL -> APP (the app may reuse the buffer).
 * A double submit of a buffer already in flight, or a completion for a buffer
 * the app already owns, is a protocol violation the transitions below reject.
 *
 * This is a PURE core, exactly like urp_cmd_validate.c and urp_frame.h: it
 * operates on a caller-supplied bitmap (bit set == KERNEL_OWNED) with plain,
 * non-atomic word ops, so the SAME source compiles into urp.ko (KUnit in
 * urp_test.c) and into the userspace validator units. The kernel caller
 * serialises the transitions with a spinlock (ctx->own_lock in urp_cmd.c); the
 * core itself takes no lock and does no allocation.
 *
 * Includer contract -- before including this header, provide:
 *   - fixed-width type u32 (kernel: <linux/types.h>; userspace: urp_cmd_compat.h),
 *   - bool/true/false (kernel: <linux/types.h>; userspace: <stdbool.h>),
 *   - errno values ERANGE/EBUSY/EINVAL (kernel: <linux/errno.h>; userspace: <errno.h>).
 */
#ifndef _URP_CMD_OWN_H
#define _URP_CMD_OWN_H

/* Bits per bitmap word, portable across the kernel and userspace builds. */
#define URP_OWN_BPW	(8u * (unsigned int)sizeof(unsigned long))

/* Words needed to track @count buffers (== BITS_TO_LONGS in the kernel). */
static inline u32 urp_own_words(u32 count)
{
	return (count + URP_OWN_BPW - 1) / URP_OWN_BPW;
}

/*
 * Claim @idx for the kernel (APP -> KERNEL), i.e. the SEND/RECV submit path.
 * Returns 0 on success, -ERANGE if @idx is out of range, -EBUSY if the buffer
 * is already KERNEL_OWNED (a double submit while a prior op is in flight).
 */
static inline int urp_own_claim(unsigned long *own, u32 count, u32 idx)
{
	unsigned long *word;
	unsigned long mask;

	if (idx >= count)
		return -ERANGE;

	word = &own[idx / URP_OWN_BPW];
	mask = 1UL << (idx % URP_OWN_BPW);
	if (*word & mask)
		return -EBUSY;
	*word |= mask;
	return 0;
}

/*
 * Release @idx back to the app (KERNEL -> APP), i.e. the completion path.
 * Returns 0 on success, -ERANGE if @idx is out of range, -EINVAL if the buffer
 * was already APP_OWNED (a completion for a buffer the app already holds -- a
 * double free / spurious completion).
 */
static inline int urp_own_release(unsigned long *own, u32 count, u32 idx)
{
	unsigned long *word;
	unsigned long mask;

	if (idx >= count)
		return -ERANGE;

	word = &own[idx / URP_OWN_BPW];
	mask = 1UL << (idx % URP_OWN_BPW);
	if (!(*word & mask))
		return -EINVAL;
	*word &= ~mask;
	return 0;
}

/*
 * True if any buffer is still KERNEL_OWNED (in flight). The teardown-quiesce
 * path (design 31 D4) uses this to refuse/drain UNREGISTER while the NIC may
 * still touch the pinned pages. Unused high bits of the final word stay clear
 * (claim rejects idx >= count), so a plain non-zero word test is exact.
 */
static inline bool urp_own_any_kernel(const unsigned long *own, u32 count)
{
	u32 w, nwords = urp_own_words(count);

	for (w = 0; w < nwords; w++)
		if (own[w])
			return true;
	return false;
}

#endif /* _URP_CMD_OWN_H */
