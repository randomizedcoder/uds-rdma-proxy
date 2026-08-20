/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp-fast char device + uring_cmd handler -- kernel-internal declarations
 * (design 31). The userspace-facing ABI is <uapi/linux/urp_cmd.h>; this
 * header holds the module-internal validator and lifecycle prototypes.
 *
 * The validators (urp_cmd_validate_data / urp_cmd_validate_reg) are PURE:
 * no locks, no user access, no allocation, no side effects. That is what
 * lets urp_test.c table-drive them under KUnit exactly like the frame,
 * credit, and reorder cores, and what makes the app->kernel trust boundary
 * (design 31 section 31.10) auditable in isolation.
 */
#ifndef _URP_CMD_H
#define _URP_CMD_H

#include <linux/types.h>

struct urp_cmd_data;	/* uapi: SEND/RECV inline args */
struct urp_cmd_reg;	/* uapi: REGISTER descriptor   */

/*
 * Registered-pool geometry the SEND/RECV validator checks against. A snapshot
 * the fop takes under its ctx lock, then validates lock-free.
 */
struct urp_cmd_pool_geom {
	u32	count;		/* buffers in the pool; 0 => none registered */
	u32	buf_size;	/* per-buffer size in bytes                  */
};

/*
 * A validated SEND/RECV request, ready for the data path (later PR). Named
 * _req rather than _op to avoid colliding with the uapi's enum urp_cmd_op
 * (struct/enum tags share one C namespace).
 */
struct urp_cmd_req {
	u32	op;		/* URP_CMD_SEND / URP_CMD_RECV */
	u32	buf_index;
	u32	len;
	u16	stream_id;
	u16	flags;
};

/*
 * Validate a SEND/RECV command against the pool geometry. Returns 0 and fills
 * *out on success, else a negative errno:
 *   -EOPNOTSUPP  cmd_op is not SEND/RECV
 *   -EINVAL      reserved field set, bad/zero flags, or len == 0
 *   -ENXIO       no pool registered (geom->count == 0)
 *   -ERANGE      buf_index >= count
 *   -EMSGSIZE    len (payload) > buf_size - URP_CMD_HEADER_RESV
 */
int urp_cmd_validate_data(u32 cmd_op, const struct urp_cmd_data *in,
			  const struct urp_cmd_pool_geom *geom,
			  struct urp_cmd_req *out);

/*
 * Validate a REGISTER descriptor's self-consistency (geometry only; the pin
 * itself is done by the caller). Returns 0 or a negative errno:
 *   -EINVAL  reserved set, base misaligned, buf_size out of range,
 *            len not a positive multiple of buf_size, or count mismatch
 *   -E2BIG   count exceeds URP_CMD_POOL_COUNT_MAX
 */
int urp_cmd_validate_reg(const struct urp_cmd_reg *r);

/* Char-device lifecycle (misc device), called from module init/exit. */
int  urp_cmd_dev_register(void);
void urp_cmd_dev_unregister(void);

#endif /* _URP_CMD_H */
