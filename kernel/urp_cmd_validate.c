// SPDX-License-Identifier: GPL-2.0
/*
 * urp-fast command validators (design 31 section 31.10).
 *
 * The app->kernel trust boundary of the fast path lives here, deliberately
 * in its own translation unit with no kernel-subsystem dependencies, so the
 * SAME source compiles two ways:
 *
 *   - into urp.ko (kbuild, __KERNEL__ defined), where the ->uring_cmd
 *     handler in urp_cmd.c and the KUnit suite in urp_test.c call it; and
 *   - into a plain userspace binary (tools/urp-fast-validate-test.c and,
 *     later, the C client library), gated by the urp-fast-validate-units
 *     nix check.
 *
 * That dual build is the same "one wire contract, two implementations that
 * must agree" discipline design 30 used for the bench frame codec: a bug in
 * the boundary check is caught by a fast sandboxed run, not only by a slow
 * KUnit-in-VM pass. Keep these functions PURE -- no locks, no user access,
 * no allocation, no side effects.
 */

#ifndef __KERNEL__
#include "urp_cmd_compat.h"	/* u32/u16/u64, PAGE_SIZE, IS_ALIGNED, errnos */
#else
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/align.h>	/* IS_ALIGNED */
#include <linux/mm.h>		/* PAGE_SIZE  */
#endif

#include "include/uapi/linux/urp_cmd.h"
#include "urp_cmd.h"

int urp_cmd_validate_data(u32 cmd_op, const struct urp_cmd_data *in,
			  const struct urp_cmd_pool_geom *geom,
			  struct urp_cmd_req *out)
{
	u16 allowed_flags;

	if (cmd_op != URP_CMD_SEND && cmd_op != URP_CMD_RECV)
		return -EOPNOTSUPP;

	/* Reserved field is part of the ABI contract: it must be zero. */
	if (in->__resv != 0)
		return -EINVAL;

	/* FIN is a SEND-only marker; RECV must not set any flag (yet). */
	allowed_flags = (cmd_op == URP_CMD_SEND) ? URP_CMD_F_FIN : 0;
	if (in->flags & ~allowed_flags)
		return -EINVAL;

	/* A zero-length transfer is meaningless for both directions. */
	if (in->len == 0)
		return -EINVAL;

	if (geom->count == 0)
		return -ENXIO;			/* no pool registered */

	if (in->buf_index >= geom->count)
		return -ERANGE;

	if (in->len > geom->buf_size)
		return -EMSGSIZE;

	out->op		= cmd_op;
	out->buf_index	= in->buf_index;
	out->len	= in->len;
	out->stream_id	= in->stream_id;
	out->flags	= in->flags;
	return 0;
}

int urp_cmd_validate_reg(const struct urp_cmd_reg *r)
{
	if (r->flags != 0 || r->__resv != 0)
		return -EINVAL;

	/* Base must be page aligned so the pool pins on clean page boundaries. */
	if (r->base == 0 || !IS_ALIGNED(r->base, PAGE_SIZE))
		return -EINVAL;

	if (r->buf_size < URP_CMD_BUF_SIZE_MIN ||
	    r->buf_size > URP_CMD_BUF_SIZE_MAX)
		return -EINVAL;

	/* Positive, a whole number of buffers, and a whole number of pages. */
	if (r->len == 0 ||
	    r->len % r->buf_size != 0 ||
	    !IS_ALIGNED(r->len, PAGE_SIZE))
		return -EINVAL;

	if (r->count == 0)
		return -EINVAL;
	if (r->count > URP_CMD_POOL_COUNT_MAX)
		return -E2BIG;

	/* count must be exactly len / buf_size (no overflow: len fits u64). */
	if ((u64)r->count * r->buf_size != r->len)
		return -EINVAL;

	return 0;
}
