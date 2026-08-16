/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * urp-fast: io_uring uring_cmd ABI for the zero-copy fast path (design 31).
 *
 * An aware application opens the urp character device (/dev/urp) and drives
 * the data plane entirely through IORING_OP_URING_CMD submissions on that
 * fd. This header is the shared contract between kernel/urp_cmd.c and the
 * userspace client library (C in tools/, Rust in crates/urp-fast, and the
 * C++/Seastar client of design 31a). It NEVER redeclares the frame wire
 * format -- payload framing stays in <uapi/linux/urp.h> and the
 * uds-rdma-protocol crate (design 21 section 21.7).
 *
 * Design references:
 *   - 31 section 31.3  interface choice (uring_cmd into /dev/urp)
 *   - 31 section 31.4  buffer pool lifecycle (REGISTER pins once)
 *   - 31 section 31.2  ownership model (submit/complete = handoff)
 *   - 31 section 31.10 the app->kernel trust boundary this ABI must guard
 *
 * The opcode travels in the SQE's cmd_op field (ioucmd->cmd_op). The 16-byte
 * inline SQE cmd area carries the per-op arguments below. REGISTER is a cold
 * path and passes a userspace pointer to a larger descriptor; SEND and RECV
 * are hot and fit their arguments inline.
 */
#ifndef _UAPI_LINUX_URP_CMD_H
#define _UAPI_LINUX_URP_CMD_H

#include <linux/types.h>

/* Character device the fast path is driven through. */
#define URP_CMD_DEVICE_NAME	"urp"
#define URP_CMD_DEVICE_PATH	"/dev/urp"

/*
 * uring_cmd opcodes, carried in sqe->cmd_op (== ioucmd->cmd_op).
 * Numbered from 1 so a zero cmd_op is always an obvious "unset" bug.
 */
enum urp_cmd_op {
	URP_CMD_REGISTER	= 1,	/* pin + register an app buffer pool     */
	URP_CMD_UNREGISTER	= 2,	/* dereg + unpin the pool                */
	URP_CMD_SEND		= 3,	/* hand buffer to NIC as TX source       */
	URP_CMD_RECV		= 4,	/* donate buffer as RX landing space     */
	__URP_CMD_OP_MAX,
};

#define URP_CMD_OP_MAX		(__URP_CMD_OP_MAX - 1)

/* Per-op flags (urp_cmd_data.flags). Reserved bits must be zero. */
#define URP_CMD_F_FIN		(1 << 0)	/* SEND: last frame of a message */

/* Pool geometry limits (mirrors the tunnel buffer geometry, design 21). */
#define URP_CMD_BUF_SIZE_MIN	64u
#define URP_CMD_BUF_SIZE_MAX	(1u << 20)	/* 1 MiB, matches BENCH_PAYLOAD_MAX */
#define URP_CMD_POOL_COUNT_MAX	65536u		/* index fits u16 stream demux headroom */

/* Endpoint-name field width (matches URP_NAME_MAX in <uapi/linux/urp.h>). */
#define URP_CMD_NAME_MAX	16

/*
 * SEND / RECV inline argument block. Lives in the 16-byte SQE cmd area
 * (io_uring_sqe_cmd()). Exactly 16 bytes; the kernel enforces this with a
 * BUILD_BUG_ON via the io_uring_sqe_cmd() accessor.
 *
 *   buf_index  index into the registered pool (0 .. count-1)
 *   len        SEND: payload bytes to transmit from buf[buf_index]
 *              RECV: max bytes the donated buffer may receive (<= buf_size)
 *   stream_id  multiplexed stream this op belongs to (design 09)
 *   flags      URP_CMD_F_*
 *   __resv     must be zero
 */
struct urp_cmd_data {
	__u32	buf_index;
	__u32	len;
	__u16	stream_id;
	__u16	flags;
	__u32	__resv;
};

/*
 * REGISTER descriptor. The SQE cmd area carries a pointer to this (see
 * struct urp_cmd_reg_sqe); the kernel copy_from_user()s it once on the cold
 * path. base must be page aligned; len must be a positive multiple of
 * buf_size; count must equal len / buf_size. endpoint names the (connected)
 * urp endpoint whose RDMA device the pool is DMA-mapped against -- the pool
 * shares that endpoint's protection domain, so the fast-path data posts land
 * on its QP (design 31 section 31.4). NUL-terminated within the field.
 */
struct urp_cmd_reg {
	__u64	base;		/* pool base userspace address (page aligned) */
	__u64	len;		/* total pool bytes                           */
	__u32	buf_size;	/* per-buffer size (URP_CMD_BUF_SIZE_MIN..MAX)*/
	__u32	count;		/* buffer count (== len / buf_size)           */
	__u32	flags;		/* reserved, must be zero                     */
	__u32	__resv;		/* reserved, must be zero                     */
	char	endpoint[URP_CMD_NAME_MAX];	/* target endpoint name       */
};

/* REGISTER inline argument block: a pointer to struct urp_cmd_reg. */
struct urp_cmd_reg_sqe {
	__u64	arg;		/* userspace pointer to struct urp_cmd_reg */
	__u64	__resv;		/* reserved, must be zero                  */
};

#endif /* _UAPI_LINUX_URP_CMD_H */
