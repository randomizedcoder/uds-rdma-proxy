/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UDS-RDMA Proxy (urp) UAPI definitions
 *
 * Phase k0: module_param configuration only.
 * Full GENL UAPI comes in Phase k1/Phase 2.
 */
#ifndef _UAPI_LINUX_URP_H
#define _UAPI_LINUX_URP_H

/* Frame header size (matches uds-rdma-protocol crate) */
#define URP_FRAME_HEADER_SIZE	20

/* Frame types */
#define URP_FRAME_TYPE_DATA	0x00
#define URP_FRAME_TYPE_CONTROL	0x01
#define URP_FRAME_TYPE_PROBE	0x02

/* Data flags */
#define URP_DATA_FLAG_SYN	(1 << 0)
#define URP_DATA_FLAG_FIN	(1 << 1)
#define URP_DATA_FLAG_RST	(1 << 2)

/* Default port for RDMA CM */
#define URP_DEFAULT_PORT	4791

/* /proc paths */
#define URP_PROC_DIR		"urp"
#define URP_PROC_STATS		"stats"

#endif /* _UAPI_LINUX_URP_H */
