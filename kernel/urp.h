/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UDS-RDMA Proxy (urp) internal header
 *
 * Phase 2: multi-endpoint via GENL. Endpoints are stored in a global
 * rhashtable keyed by name. Configuration arrives via netlink, not
 * module_param. Stream multiplexing (multiple connections per endpoint)
 * remains Phase 3 work; k0 single-connection model still applies inside
 * each endpoint.
 */
#ifndef _URP_H
#define _URP_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/un.h>
#include <linux/in6.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/rhashtable.h>
#include <linux/rcupdate.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "include/uapi/linux/urp.h"
#include "urp_credit.h"
#include "urp_reorder.h"

/* Buffer pool sizing -- defaults; per-endpoint values override at create */
#define URP_NUM_BUFS		64
#define URP_BUF_SIZE		4096	/* payload + header */
#define URP_MAX_PAYLOAD		(URP_BUF_SIZE - URP_FRAME_HEADER_SIZE)
#define URP_CQ_ENTRIES		(URP_NUM_BUFS * 2)	/* send + recv */
#define URP_SQ_DEPTH		URP_NUM_BUFS
#define URP_RQ_DEPTH		URP_NUM_BUFS

/*
 * struct urp_buffer - DMA-mapped buffer for RDMA send/recv
 * @list:     free list linkage
 * @page:     backing kernel page
 * @dma_addr: DMA address for RDMA operations
 * @data:     kernel virtual address (page_address(page))
 * @sge:      scatter-gather entry (pre-filled for RDMA)
 * @cqe:      CQ completion callback (per-buffer, set to send_done or recv_done)
 * @index:    buffer index (for debugging)
 */
struct urp_buffer {
	struct list_head	list;
	struct page		*page;
	dma_addr_t		dma_addr;
	void			*data;
	struct ib_sge		sge;
	struct ib_cqe		cqe;
	u32			index;
};

/*
 * struct urp_stats - per-endpoint statistics
 * Atomics for lock-free updates from TX/RX paths.
 */
struct urp_stats {
	atomic64_t	tx_bytes;
	atomic64_t	rx_bytes;
	atomic64_t	tx_frames;
	atomic64_t	rx_frames;
	atomic64_t	connections;
	/* Phase 3a Step 8: aggregate diagnostic counters (RTT and
	 * auth_failures stay 0 until Phase 3b probes/PSK land). */
	atomic64_t	credit_stalls;
	atomic64_t	reorder_insertions;
	atomic64_t	reorder_drops;
	atomic64_t	buffer_alloc_fails;
};

struct urp_endpoint;	/* forward decl for struct urp_qp */

/*
 * struct urp_qp - per-QP runtime state
 *
 * One entry per QP in ep->qps[]. Holds the verbs QP handle (set after
 * rdma_create_qp), per-QP rdma_cm_id, connectedness, and a back-pointer
 * to the owning endpoint for CM-event dispatch. Phase 3a Step 4 will
 * add credit tracking; Phase 3b will add probe state (RTT EWMA,
 * consecutive_misses).
 */
struct urp_qp {
	struct urp_endpoint	*ep;		/* back-pointer */
	struct rdma_cm_id	*cm_id;		/* per-QP CM id */
	struct ib_qp		*qp;
	u32			index;		/* position in ep->qps[] */
	bool			established;	/* set on RDMA_CM_EVENT_ESTABLISHED */

	/*
	 * Step 4: per-QP credit-based flow control state. Initialized on
	 * QP allocation; not yet wired into TX/RX (Step 4b adds the
	 * consume-and-block + grant-frame emission).
	 */
	struct urp_credit	credit;

	/* Step 8: per-QP counters surfaced via GENL urp_fill_endpoint */
	atomic64_t		tx_bytes;
	atomic64_t		rx_bytes;
	atomic64_t		tx_frames;
	atomic64_t		rx_frames;
};

/*
 * struct urp_cm_ctx - context attached to every rdma_cm_id created by urp
 *
 * Allocated by urp_rdma_init (or by the listener handler when a child
 * cm_id is accepted) and stashed in id->context. The CM handler reads
 * this on every event so the per-cm-id work can find both the owning
 * endpoint and (for per-QP cm_ids) the QP slot to update.
 *
 * Freed at cm_id destroy time. For the listener cm_id, is_listener is
 * true and qp_index is unused.
 */
struct urp_cm_ctx {
	struct urp_endpoint	*ep;
	u32			qp_index;
	bool			is_listener;
};

/*
 * struct urp_connection - per-UDS-connection state (k0: single connection)
 * @uds_sock:   accepted UDS socket
 * @tx_thread:  TX pump kthread
 * @seq:        next sequence number for outgoing frames
 * @active:     connection is active (cleared on shutdown)
 */
struct urp_connection {
	struct socket		*uds_sock;
	struct task_struct	*tx_thread;
	u64			seq;
	bool			active;
};

/*
 * struct urp_stream - one multiplexed connection over the endpoint's
 *                     QP set (Phase 3a Step 6).
 *
 * Each accepted UDS connection (Step 7) maps to one stream.  Sequence
 * spaces are per-stream (design 09 §9.6); reorder buffer + credit
 * state are per-stream because multi-QP delivery across an endpoint is
 * shared by all of its streams.
 *
 * Lifecycle is mediated by call_rcu so concurrent rhashtable walks see
 * a stable view until their read-side critical section ends, matching
 * the pattern used by the endpoint table.
 */
struct urp_stream {
	struct urp_endpoint	*ep;		/* back-pointer */
	u32			id;		/* rhashtable key */
	enum urp_stream_state	state;
	struct mutex		lock;		/* serializes state changes */

	u64			tx_seq;		/* next outgoing seq */
	u64			rx_next;	/* next expected incoming seq */

	struct urp_credit	credit;		/* per-stream flow control */
	struct urp_reorder	*reorder;	/* per-stream reorder buffer */

	struct socket		*uds_sock;	/* this stream's UDS endpoint */
	struct task_struct	*tx_thread;	/* per-stream TX kthread (Step 7c) */

	struct rhash_head	ht_node;
	struct rcu_head		rcu;
};

/*
 * struct urp_endpoint - one configured proxy endpoint
 *
 * Multiple endpoints exist concurrently; lookup is by name via the global
 * urp_endpoints rhashtable. Lifecycle transitions are serialized by @lock;
 * teardown uses kfree_rcu so dump walks see consistent state.
 */
struct urp_endpoint {
	/* Identity / configuration -- set at create, immutable except where noted */
	char			name[URP_NAME_MAX];	/* lookup key */
	char			listen_path[URP_PATH_MAX_LEN];
	char			connect_path[URP_PATH_MAX_LEN];
	char			rdma_device[URP_DEVICE_MAX];	/* "" = auto */
	struct sockaddr_in6	peer_addr;
	struct sockaddr_in6	bind_addr;
	bool			has_peer_addr;
	bool			has_bind_addr;
	u32			num_qps;		/* mutable via SET */
	u32			buffer_count;		/* mutable via SET */
	u32			buffer_size;
	u8			password[URP_PASSWORD_MAX];	/* mutable via SET, write-only */
	bool			has_password;

	/* Lifecycle */
	enum urp_endpoint_state	state;
	struct mutex		lock;			/* serializes state transitions */
	struct rhash_head	ht_node;		/* urp_endpoints rhashtable linkage */
	struct rcu_head		rcu;			/* deferred free */
	bool			is_initiator;		/* derived from listen_path != "" */

	/* UDS side */
	struct socket		*listen_sock;
	struct task_struct	*accept_thread;
	struct urp_connection	conn;

	/* RDMA side -- shared across all QPs of this endpoint */
	struct rdma_cm_id	*listen_id;	/* acceptor: listener CM ID */
	struct ib_device	*ib_dev;	/* cached on first QP setup */
	struct ib_pd		*pd;
	struct ib_cq		*send_cq;
	struct ib_cq		*recv_cq;
	struct ib_srq		*srq;		/* shared receive queue (Step 3) */
	u32			srq_pool_target;

	/* Multi-QP state (Phase 3a Step 2 scaffold; Step 2b fills it) */
	struct urp_qp	*qps;		/* array of num_qps entries; allocated in activate */
	atomic_t		qps_connected;	/* count of QPs in ESTABLISHED state */
	atomic_t		qps_accepted;	/* acceptor: count of CONNECT_REQUESTs processed */
	atomic_t		rr_counter;	/* round-robin selector cursor */

	/* Stream multiplexing (Phase 3a Step 6) */
	struct rhashtable	streams;	/* keyed by u32 stream_id */
	bool			streams_inited;
	atomic_t		next_stream_id;	/* monotonic per-direction allocator */

	/* Buffer pool */
	struct urp_buffer	bufs[URP_NUM_BUFS];
	struct list_head	send_free;
	struct list_head	recv_free;
	spinlock_t		send_lock;
	spinlock_t		recv_lock;

	/* Runtime state */
	struct urp_stats	stats;
	struct completion	cm_done;
	int			cm_status;
	bool			connected;

	/* RX work */
	struct work_struct	rx_work;
	struct workqueue_struct	*rx_wq;

	/* /proc/urp/<name>/stats entry (set by urp_endpoint_proc_create) */
	struct proc_dir_entry	*proc_dir;
};

/* Module-global endpoint store (defined in urp_endpoint.c) */
extern struct rhashtable	urp_endpoints;
extern bool			urp_endpoints_inited;

/* urp_endpoint.c -- lifecycle */
int  urp_endpoint_table_init(void);
void urp_endpoint_table_destroy(void);
int  urp_endpoint_create(struct urp_endpoint *cfg, struct urp_endpoint **out);
int  urp_endpoint_activate(struct urp_endpoint *ep);
void urp_endpoint_drain(struct urp_endpoint *ep);
void urp_endpoint_destroy(struct urp_endpoint *ep);
struct urp_endpoint *urp_endpoint_lookup(const char *name);
void urp_endpoint_drain_all(void);

/* urp_socket.c */
int  urp_socket_init(struct urp_endpoint *ep, const char *path);
void urp_socket_cleanup(struct urp_endpoint *ep);
int  urp_connect_uds(struct urp_endpoint *ep, const char *path);
int  urp_stream_connect_uds(struct urp_stream *stream, const char *path);

/* urp_rdma.c */
int  urp_rdma_init(struct urp_endpoint *ep, const char *peer_addr,
		   int peer_port, int bind_port, bool is_initiator);
void urp_rdma_cleanup(struct urp_endpoint *ep);
struct urp_buffer *urp_buf_alloc_send(struct urp_endpoint *ep);
void urp_buf_free_send(struct urp_endpoint *ep, struct urp_buffer *buf);
struct urp_buffer *urp_buf_alloc_recv(struct urp_endpoint *ep);
void urp_buf_free_recv(struct urp_endpoint *ep, struct urp_buffer *buf);
int  urp_post_recv(struct urp_endpoint *ep, struct ib_qp *qp, struct urp_buffer *buf);
int  urp_post_recv_for_qp(struct urp_endpoint *ep, struct ib_qp *qp, u32 count);

/* urp_srq.c -- Shared Receive Queue (Phase 3a Step 3) */
int  urp_srq_create(struct urp_endpoint *ep);
void urp_srq_destroy(struct urp_endpoint *ep);
int  urp_srq_post_initial(struct urp_endpoint *ep);
int  urp_post_srq_recv(struct urp_endpoint *ep, struct urp_buffer *buf);

/* urp_qp.c -- per-QP state and selection (Phase 3a Step 2) */
int  urp_qps_init(struct urp_endpoint *ep);
void urp_qps_destroy(struct urp_endpoint *ep);
struct urp_qp *urp_qp_select_round_robin(struct urp_endpoint *ep);
int  urp_qp_index_of(struct urp_endpoint *ep, struct ib_qp *qp);

/* urp_stream.c -- stream multiplexing core (Phase 3a Step 6) */
int  urp_streams_init(struct urp_endpoint *ep);
void urp_streams_destroy_all(struct urp_endpoint *ep);
u32  urp_stream_next_id(struct urp_endpoint *ep);
int  urp_stream_create(struct urp_endpoint *ep, u32 stream_id,
		       struct urp_stream **out_stream);
struct urp_stream *urp_stream_lookup(struct urp_endpoint *ep, u32 stream_id);
void urp_stream_destroy(struct urp_endpoint *ep, struct urp_stream *s);

/* urp_stream.c -- lifecycle handlers (Phase 3a Step 7) */
int  urp_stream_rx_syn(struct urp_endpoint *ep, u32 stream_id,
		       struct urp_stream **out_stream);
int  urp_stream_rx_fin(struct urp_stream *s);
int  urp_stream_rx_rst(struct urp_stream *s);
void urp_stream_tx_fin(struct urp_stream *s);
void urp_stream_tx_rst(struct urp_stream *s);
int  urp_stream_rx_dispatch(struct urp_endpoint *ep, u32 stream_id, u8 flags,
			    struct urp_stream **out_stream);

/* urp_pump.c */
int  urp_pump_start(struct urp_endpoint *ep);
void urp_pump_stop(struct urp_endpoint *ep);
int  urp_stream_pump_start(struct urp_stream *stream);
void urp_stream_pump_stop(struct urp_stream *stream);

/* CQ completion callbacks (urp_rdma.c) -- used by pump when posting sends */
void urp_send_done(struct ib_cq *cq, struct ib_wc *wc);

/* urp_proc.c */
int  urp_proc_init(void);
void urp_proc_cleanup(void);
int  urp_endpoint_proc_create(struct urp_endpoint *ep);
void urp_endpoint_proc_remove(struct urp_endpoint *ep);

/* urp_netlink.c */
int  urp_genl_register(void);
void urp_genl_unregister(void);
void urp_send_event(struct urp_endpoint *ep);

/* Frame encode/decode (inline, matches shared Rust crate wire format) */

/*
 * Wire format (20 bytes, little-endian):
 *   [0..4)   stream_id       u32
 *   [4..12)  sequence_number u64
 *   [12]     frame_type      u8
 *   [13]     flags           u8
 *   [14..16) credits_granted u16
 *   [16..20) payload_length  u32
 */

static inline void urp_frame_encode(void *buf, u32 stream_id, u64 seq,
				    u8 frame_type, u8 flags,
				    u16 credits, u32 payload_len)
{
	u8 *p = buf;

	put_unaligned_le32(stream_id, p);
	put_unaligned_le64(seq, p + 4);
	p[12] = frame_type;
	p[13] = flags;
	put_unaligned_le16(credits, p + 14);
	put_unaligned_le32(payload_len, p + 16);
}

static inline u32 urp_frame_decode_payload_len(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le32(p + 16);
}

static inline u32 urp_frame_decode_stream_id(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le32(p);
}

static inline u64 urp_frame_decode_seq(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le64(p + 4);
}

static inline u8 urp_frame_decode_type(const void *buf)
{
	const u8 *p = buf;

	return p[12];
}

static inline u8 urp_frame_decode_flags(const void *buf)
{
	const u8 *p = buf;

	return p[13];
}

#endif /* _URP_H */
