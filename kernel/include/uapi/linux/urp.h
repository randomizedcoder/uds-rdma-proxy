/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UDS-RDMA Proxy (urp) UAPI definitions
 *
 * Phase 2: GENL family "urp" replaces module_param configuration.
 * Endpoints are created/managed at runtime via the urp CLI tool.
 */
#ifndef _UAPI_LINUX_URP_H
#define _UAPI_LINUX_URP_H

/* ---------------------------------------------------------------- */
/* Frame wire format (matches uds-rdma-protocol crate)              */
/* ---------------------------------------------------------------- */

/* Frame header size */
#define URP_FRAME_HEADER_SIZE	20

/* Frame types */
#define URP_FRAME_TYPE_DATA	0x00
#define URP_FRAME_TYPE_CONTROL	0x01
#define URP_FRAME_TYPE_PROBE	0x02

/* Data flags */
#define URP_DATA_FLAG_SYN	(1 << 0)
#define URP_DATA_FLAG_FIN	(1 << 1)
#define URP_DATA_FLAG_RST	(1 << 2)

/* Control flags (frame_type == URP_FRAME_TYPE_CONTROL) */
#define URP_CTRL_FLAG_CREDIT	(1 << 0)	/* credits_granted carries grant */

/* Default port for RDMA CM */
#define URP_DEFAULT_PORT	4791

/* /proc paths */
#define URP_PROC_DIR		"urp"
#define URP_PROC_STATS		"stats"

/* ---------------------------------------------------------------- */
/* Generic netlink family                                           */
/* ---------------------------------------------------------------- */

#define URP_GENL_NAME		"urp"
#define URP_GENL_VERSION	1

/* Multicast group for state-change notifications */
#define URP_GENL_MCGRP_EVENTS	"events"

/* Length limits */
#define URP_NAME_MAX		16	/* endpoint name */
#define URP_PATH_MAX_LEN	108	/* sizeof(sockaddr_un.sun_path) */
#define URP_DEVICE_MAX		64	/* RDMA device name */
#define URP_PASSWORD_MAX	16	/* PSK length */

/* Configuration limits */
#define URP_NUM_QPS_MAX		32
#define URP_NUM_QPS_MIN		1
#define URP_BUFFER_COUNT_MIN	16
#define URP_BUFFER_COUNT_MAX	65536	/* hard cap: bounds the pool allocation */
#define URP_BUFFER_SIZE_MIN	URP_FRAME_HEADER_SIZE
#define URP_BUFFER_SIZE_MAX	65536

/* Defaults applied when attribute absent in NEW_ENDPOINT */
#define URP_NUM_QPS_DEFAULT	1
#define URP_BUFFER_COUNT_DEFAULT 1024
#define URP_BUFFER_SIZE_DEFAULT	4096	/* slot = header + 4076 payload */

/*
 * GENL commands
 *
 * NEW_ENDPOINT, DEL_ENDPOINT, SET_ENDPOINT require CAP_NET_ADMIN.
 * GET_ENDPOINT is unprivileged. GET supports both doit (single) and dumpit (all).
 */
enum urp_cmd {
	URP_CMD_UNSPEC		= 0,
	URP_CMD_NEW_ENDPOINT	= 1,
	URP_CMD_DEL_ENDPOINT	= 2,
	URP_CMD_SET_ENDPOINT	= 3,
	URP_CMD_GET_ENDPOINT	= 4,
	__URP_CMD_MAX,
};

#define URP_CMD_MAX (__URP_CMD_MAX - 1)

/* Top-level attributes */
enum urp_attr {
	URP_A_UNSPEC		= 0,
	URP_A_ENDPOINT		= 1,	/* NLA_NESTED -- endpoint attrs */
	__URP_A_MAX,
};

#define URP_A_MAX (__URP_A_MAX - 1)

/*
 * Endpoint attributes (nested inside URP_A_ENDPOINT)
 *
 * Mutability:
 *   NEW: NAME required; LISTEN_PATH or CONNECT_PATH required (one of)
 *   SET: NAME (lookup), and only NUM_QPS / BUFFER_COUNT / PASSWORD / STATE
 *        are mutable. LISTEN_PATH/CONNECT_PATH/PEER_ADDR/BIND_ADDR/BUFFER_SIZE
 *        are immutable after creation.
 *   GET: returns NAME + all immutable config + STATE + nested QPS/STREAMS/STATS.
 *        PASSWORD is write-only and never returned.
 *   DEL: NAME only.
 */
enum urp_endpoint_attr {
	URP_ENDPOINT_A_UNSPEC		= 0,

	/* Configuration */
	URP_ENDPOINT_A_NAME		= 1,	/* NLA_NUL_STRING, max 16 */
	URP_ENDPOINT_A_LISTEN_PATH	= 2,	/* NLA_NUL_STRING, max 108 (initiator UDS path) */
	URP_ENDPOINT_A_CONNECT_PATH	= 3,	/* NLA_NUL_STRING, max 108 (acceptor UDS path) */
	/* NLA_NUL_STRING, max 64 -- e.g. "mlx5_0"; optional, auto-pick if absent */
	URP_ENDPOINT_A_RDMA_DEVICE	= 4,
	/* NLA_BINARY, exact 28 bytes (struct sockaddr_in6) -- initiator target */
	URP_ENDPOINT_A_PEER_ADDR	= 5,
	URP_ENDPOINT_A_BIND_ADDR	= 6,	/* NLA_BINARY, exact 28 bytes -- acceptor bind */
	URP_ENDPOINT_A_NUM_QPS		= 7,	/* NLA_U32, range 1..32 */
	URP_ENDPOINT_A_BUFFER_COUNT	= 8,	/* NLA_U32, min 16 */
	URP_ENDPOINT_A_BUFFER_SIZE	= 9,	/* NLA_U32, range 20..65536 */
	URP_ENDPOINT_A_PASSWORD		= 10,	/* NLA_NUL_STRING, max 16, write-only */

	/* Read-only state */
	URP_ENDPOINT_A_STATE		= 11,	/* NLA_U8 -- enum urp_endpoint_state */

	/* Read-only nested children (returned by GET only) */
	URP_ENDPOINT_A_QPS		= 12,	/* NLA_NESTED array of urp_qp_attr sets */
	URP_ENDPOINT_A_STREAMS		= 13,	/* NLA_NESTED array of urp_stream_attr sets */
	URP_ENDPOINT_A_STATS		= 14,	/* NLA_NESTED -- urp_stats_attr set */

	__URP_ENDPOINT_A_MAX,
};

#define URP_ENDPOINT_A_MAX (__URP_ENDPOINT_A_MAX - 1)

/* QP attributes (nested array inside URP_ENDPOINT_A_QPS) */
enum urp_qp_attr {
	URP_QP_A_UNSPEC		= 0,
	URP_QP_A_INDEX		= 1,	/* NLA_U32 */
	URP_QP_A_STATE		= 2,	/* NLA_U8 -- enum urp_qp_state */
	URP_QP_A_RTT_NS		= 3,	/* NLA_U64 */
	URP_QP_A_TX_BYTES	= 4,	/* NLA_U64 */
	URP_QP_A_RX_BYTES	= 5,	/* NLA_U64 */
	URP_QP_A_TX_FRAMES	= 6,	/* NLA_U64 */
	URP_QP_A_RX_FRAMES	= 7,	/* NLA_U64 */
	__URP_QP_A_MAX,
};

#define URP_QP_A_MAX (__URP_QP_A_MAX - 1)

/* Stream attributes (nested array inside URP_ENDPOINT_A_STREAMS) */
enum urp_stream_attr {
	URP_STREAM_A_UNSPEC		= 0,
	URP_STREAM_A_ID			= 1,	/* NLA_U32 */
	URP_STREAM_A_STATE		= 2,	/* NLA_U8 -- enum urp_stream_state */
	URP_STREAM_A_TX_BYTES		= 3,	/* NLA_U64 */
	URP_STREAM_A_RX_BYTES		= 4,	/* NLA_U64 */
	URP_STREAM_A_REORDER_DEPTH	= 5,	/* NLA_U32 */
	URP_STREAM_A_CREDITS_LOCAL	= 6,	/* NLA_U16 */
	URP_STREAM_A_CREDITS_REMOTE	= 7,	/* NLA_U16 */
	__URP_STREAM_A_MAX,
};

#define URP_STREAM_A_MAX (__URP_STREAM_A_MAX - 1)

/* Aggregate stats attributes (nested inside URP_ENDPOINT_A_STATS) */
enum urp_stats_attr {
	URP_STATS_A_UNSPEC		= 0,
	URP_STATS_A_ACTIVE_STREAMS	= 1,	/* NLA_U32 */
	URP_STATS_A_TX_BYTES		= 2,	/* NLA_U64 */
	URP_STATS_A_RX_BYTES		= 3,	/* NLA_U64 */
	URP_STATS_A_TX_FRAMES		= 4,	/* NLA_U64 */
	URP_STATS_A_RX_FRAMES		= 5,	/* NLA_U64 */
	URP_STATS_A_CREDIT_STALLS	= 6,	/* NLA_U64 */
	URP_STATS_A_REORDER_INSERTIONS	= 7,	/* NLA_U64 */
	URP_STATS_A_REORDER_DROPS	= 8,	/* NLA_U64 */
	URP_STATS_A_BUFFER_ALLOC_FAILS	= 9,	/* NLA_U64 */
	URP_STATS_A_AUTH_FAILURES	= 10,	/* NLA_U64 */
	__URP_STATS_A_MAX,
};

#define URP_STATS_A_MAX (__URP_STATS_A_MAX - 1)

/* Endpoint lifecycle states */
enum urp_endpoint_state {
	URP_STATE_CREATING	= 0,	/* RDMA setup in progress */
	URP_STATE_ACTIVE	= 1,	/* RDMA up, UDS listening */
	URP_STATE_DRAINING	= 2,	/* No new streams, finishing existing */
	URP_STATE_STOPPED	= 3,	/* All resources released, ready for removal */
	__URP_STATE_MAX,
};

#define URP_STATE_MAX (__URP_STATE_MAX - 1)

/* QP state (Phase 3 will populate; k0 reports ACTIVE for the single QP) */
enum urp_qp_state {
	URP_QP_STATE_QUALIFYING	= 0,
	URP_QP_STATE_ACTIVE	= 1,
	URP_QP_STATE_DRAINING	= 2,
	URP_QP_STATE_REMOVED	= 3,
	__URP_QP_STATE_MAX,
};

#define URP_QP_STATE_MAX (__URP_QP_STATE_MAX - 1)

/* Stream state (Phase 3 multiplexing; k0 emits ESTABLISHED if connected) */
enum urp_stream_state {
	URP_STREAM_STATE_SYN_SENT	= 0,
	URP_STREAM_STATE_SYN_RECEIVED	= 1,
	URP_STREAM_STATE_ESTABLISHED	= 2,
	URP_STREAM_STATE_FIN_WAIT	= 3,
	URP_STREAM_STATE_CLOSE_WAIT	= 4,
	URP_STREAM_STATE_CLOSED		= 5,
	__URP_STREAM_STATE_MAX,
};

#define URP_STREAM_STATE_MAX (__URP_STREAM_STATE_MAX - 1)

#endif /* _UAPI_LINUX_URP_H */
