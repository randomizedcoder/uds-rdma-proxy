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

/*
 * QP health-probe payload sizes (carried inside a URP_FRAME_TYPE_PROBE frame;
 * byte layout + encoders in kernel/urp_frame.h). Wire constants -- they match
 * crates/uds-rdma-protocol/src/probe.rs. A PONG is the largest control frame
 * urp ever emits (URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE == 68), which
 * sets the receive-buffer floor below (URP_BUFFER_SIZE_MIN).
 */
#define URP_PING_PAYLOAD_SIZE	32
#define URP_PONG_PAYLOAD_SIZE	48

/* Data flags */
#define URP_DATA_FLAG_SYN	(1 << 0)
#define URP_DATA_FLAG_FIN	(1 << 1)
#define URP_DATA_FLAG_RST	(1 << 2)

/* Control flags (frame_type == URP_FRAME_TYPE_CONTROL) */
#define URP_CTRL_FLAG_CREDIT	(1 << 0)	/* credits_granted carries grant */
/*
 * gap #6 Phase 2 (PR2): byte-denominated flow-control grant. Carries a u64
 * cumulative rx_bytes_delivered in the CONTROL *payload* (payload_length == 8)
 * -- the header's u16 credits_granted cannot hold a byte count. Bit 5 is used
 * because bits 1..4 are reserved by the Rust protocol twin
 * (crates/uds-rdma-protocol/src/constants.rs: QP_DISABLE/QP_ENABLE/
 * STREAM_WINDOW/AUTH); keep the two in lock-step. Codec only in PR2 (encode/
 * decode + tests); emit/apply lands in PR3 with the blocking sender gate.
 */
#define URP_CTRL_FLAG_CREDIT_BYTES	(1 << 5)
#define URP_CREDIT_BYTES_PAYLOAD_SIZE	8	/* u64 cumulative byte count */

/*
 * gap #6 Phase 2 (PR2): connection capability bits, advertised in the CM
 * private_data trailer (urp_conn_priv_build_full) and negotiated by BOTH peers
 * before the byte-window path activates (design 35 §35.3 interop gate). A
 * byte-gated sender talking to a frame-credit-only receiver would block
 * forever, so the window is honored only when both sides advertise support.
 */
#define URP_CONN_CAP_WINDOW_BYTES	(1 << 0)	/* peer honors byte-windowing */

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
/*
 * A receive buffer must hold the largest frame the peer can send. DATA frames
 * are bounded by buffer_size itself (payload = buffer_size - header), but the
 * fixed-size QP health-probe PONG is URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE
 * (== 68) regardless of buffer_size. A smaller buffer overflows on the very first
 * PONG -- the receiver raises an ib "local length error", NAKs, and the sender's
 * QP tears down into a reconnect crash-loop (observed on hp1<->hp3 at
 * buffer_size=64: only 64 of a 68-byte PONG fits). So the floor is the PONG size,
 * not the bare header. The netlink policy (urp_buffer_size_range, urp_netlink.c)
 * enforces this floor -- a request below it is rejected with ERANGE rather than
 * silently accepted -- and urp_resolve_buf_size() also clamps as a backstop.
 */
#define URP_BUFFER_SIZE_MIN	(URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE)
/*
 * The ceiling is a software/allocation choice, not a wire or hardware limit:
 * payload_length on the wire is a u32 (urp_frame.h), and RoCEv2 RC segments a
 * single large message into PMTU packets in the NIC (ConnectX-4 Lx reports
 * max_msg_sz = 1 GiB). Since the copy pump is frame-rate-bound (design 34
 * §34.5.1), a larger slot moves proportionally more bytes per frame. 1 MiB is
 * the current cap; slots this large are backed by high-order compound pages
 * (page_pool order = get_order(buffer_size)), so pair a large buffer_size with a
 * small buffer_count and expect high-order allocation to be best-effort. A
 * scatter-gather (multi-SGE, max_sge=30) large-frame path is the production
 * follow-up that removes the high-order-alloc dependency (design 37).
 */
#define URP_BUFFER_SIZE_MAX	1048576

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

	/* NLA_U8 -- enum urp_ep_mode; optional config, immutable, default
	 * multistream. Placed after the read-only children to keep the older
	 * config attr numbers (1..10) stable.
	 */
	URP_ENDPOINT_A_MODE		= 15,

	/* NLA_U8 -- enum urp_ep_kind; optional config, immutable, default uds
	 * (the copy path). "fast" selects the zero-copy alternative path, which
	 * suppresses the UDS pump and is driven by the app over /dev/urp
	 * (io_uring_cmd). Requires CONFIG_URP_FAST; rejected otherwise.
	 */
	URP_ENDPOINT_A_KIND		= 16,

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

/*
 * Endpoint operating mode (per-endpoint, immutable, set at `urp add`).
 *
 * Selects how the acceptor wires its backend UDS:
 *   MULTISTREAM (default) -- one backend connection per stream, opened lazily
 *      when that stream's SYN arrives (urp_stream_open_backend). The acceptor
 *      must NOT eagerly open the legacy single ep->conn connection, which would
 *      steal a single-accept backend from the real stream.
 *   K0 -- legacy single-connection mode (stream_id 0). The acceptor eagerly
 *      opens ep->conn at CONNECT_REQUEST so stream_id==0 traffic has a backend
 *      ready. Used by test-kmod-k0 / urp-test-client's echo/throughput modes.
 *
 * Gated by urp_acceptor_should_eager_connect() (kernel/urp_conn_plan.h).
 */
enum urp_ep_mode {
	URP_EP_MODE_MULTISTREAM	= 0,	/* per-stream backend connect (default) */
	URP_EP_MODE_K0		= 1,	/* legacy single ep->conn eager connect */
	__URP_EP_MODE_MAX,
};

#define URP_EP_MODE_MAX (__URP_EP_MODE_MAX - 1)

/*
 * Endpoint kind (per-endpoint, immutable, set at `urp add`).
 *
 * Selects the data path an endpoint uses:
 *   UDS (default) -- the copy path. The kernel runs an AF_UNIX pump; unaware
 *      apps talk to the listen/connect UDS socket and the kernel copies each
 *      frame into a registered pool buffer before posting it. Always available.
 *   FAST -- the zero-copy alternative path (design 31, urp-fast). No UDS pump;
 *      an aware app hands its own pinned buffer pool to the module over
 *      /dev/urp (io_uring_cmd) and the NIC DMAs straight into/out of the app
 *      pages. Requires CONFIG_URP_FAST; the control plane rejects a fast
 *      endpoint (-ENOTSUPP) when the module is built without it.
 *
 * `kind` is orthogonal to `mode` (multistream vs k0 backend wiring).
 */
enum urp_ep_kind {
	URP_EP_KIND_UDS		= 0,	/* copy path via the AF_UNIX pump (default) */
	URP_EP_KIND_FAST	= 1,	/* zero-copy path via /dev/urp (io_uring_cmd) */
	__URP_EP_KIND_MAX,
};

#define URP_EP_KIND_MAX (__URP_EP_KIND_MAX - 1)

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
