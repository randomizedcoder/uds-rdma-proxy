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
#include <linux/version.h>
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
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/rhashtable.h>
#include <linux/rcupdate.h>
#include <linux/kref.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "include/uapi/linux/urp.h"
#include "urp_credit.h"
#include "urp_reorder.h"

/*
 * Compat: kernel_connect() / kernel_bind() took a `struct sockaddr *`
 * until Linux 7.0, which reworked them to take `struct sockaddr_unsized *`
 * (the unsized-sockaddr change). Cast UDS bind/connect call sites through
 * urp_sockaddr_t so the module builds against both the 6.1/6.6/6.12 LTS
 * line and 7.0+ mainline.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
typedef struct sockaddr_unsized urp_sockaddr_t;
#else
typedef struct sockaddr urp_sockaddr_t;
#endif

/*
 * Compat: strscpy() became a size-checked wrapper macro over the new
 * sized_strscpy() function in Linux 6.8. On 6.8+ the strscpy() macro
 * rejects a `const char *` source (its cstr type check trips on our
 * @path parameter), so we must call sized_strscpy() directly; on the
 * pre-6.8 LTS line sized_strscpy() does not exist and plain strscpy()
 * accepts the const source. urp_strscpy() picks the right one per version.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define urp_strscpy(dst, src, sz) sized_strscpy((dst), (src), (sz))
#else
#define urp_strscpy(dst, src, sz) strscpy((dst), (src), (sz))
#endif

/*
 * urp-fast zero-copy data path (design 31). Gated by the build-level
 * CONFIG_URP_FAST (default y in Kbuild; set =n to exclude the whole zero-copy
 * path for cert / attack-surface reasons) AND the >= 6.8 io_uring floor the
 * /dev/urp uring_cmd device needs (split <linux/io_uring/cmd.h>, 4-arg
 * pin_user_pages). URP_FAST_ENABLED is 1 only when both hold; the control plane
 * rejects a `fast` endpoint (-ENOTSUPP) otherwise, and urp_cmd.c stubs the
 * device. This is a compile-time "supported / not-supported" boundary, NOT a
 * runtime behavioral flag -- the copy (uds) path never depends on it.
 */
#if defined(CONFIG_URP_FAST) && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define URP_FAST_ENABLED 1
#else
#define URP_FAST_ENABLED 0
#endif

/*
 * Buffer pool sizing. The pool depth is per-endpoint (ep->num_bufs, resolved
 * from buffer_count at activation); CQ/SQ/SRQ depths and the credit window are
 * all derived from it at setup time (see urp_endpoint_setup_shared). URP_BUF_SIZE
 * is the default DMA slot size (frame header + max payload) used when buffer_size
 * is the default; the per-endpoint slot size is ep->buffer_size.
 */
#define URP_BUF_SIZE		4096	/* default slot: payload + header */
/*
 * URP_MAX_PAYLOAD is the ABSOLUTE, compile-time payload ceiling used by the
 * pure frame decoder (urp_frame.c, also dual-compiled into the fuzzers): the
 * largest payload any endpoint could ever carry = the biggest slot
 * (URP_BUFFER_SIZE_MAX) minus the header. Per-endpoint enforcement is tighter
 * and automatic: a recv buffer is posted with sge.length = ep->buf_size, so a
 * completion's byte_len can never exceed the endpoint's slot, and the decoder's
 * "payload_len > byte_len - header" check rejects anything larger than that
 * endpoint's real capacity. See urp_ep_max_payload().
 */
#define URP_MAX_PAYLOAD		(URP_BUFFER_SIZE_MAX - URP_FRAME_HEADER_SIZE)

/*
 * Pure per-endpoint sizing resolvers (table-tested in urp_test.c). Kept inline
 * and side-effect-free so the KUnit suite can pin every boundary without a live
 * RDMA device.
 *
 *   num_bufs  = buffer_count clamped to [MIN, MAX]      (pool depth)
 *   buf_size  = buffer_size  clamped to [MIN, MAX]      (DMA slot bytes)
 *   payload   = buf_size - header                       (max wire payload)
 */
static inline u32 urp_resolve_num_bufs(u32 buffer_count)
{
	return clamp_t(u32, buffer_count,
		       URP_BUFFER_COUNT_MIN, URP_BUFFER_COUNT_MAX);
}

static inline u32 urp_resolve_buf_size(u32 buffer_size)
{
	return clamp_t(u32, buffer_size,
		       URP_BUFFER_SIZE_MIN, URP_BUFFER_SIZE_MAX);
}

static inline u32 urp_ep_max_payload(u32 buf_size)
{
	/* buf_size >= URP_BUFFER_SIZE_MIN (= header) so this never underflows */
	return buf_size - URP_FRAME_HEADER_SIZE;
}

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
	/* Phase 3a Step 8: aggregate diagnostic counters. */
	atomic64_t	credit_stalls;
	atomic64_t	reorder_insertions;
	atomic64_t	reorder_drops;
	atomic64_t	buffer_alloc_fails;
	/* Phase 3b Step 9: incremented on PSK rdma_reject. */
	atomic64_t	auth_failures;
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
	 * Design 33 Bug 1: does this (acceptor) slot currently hold a claim
	 * on ep->qps_accepted? Set true once rdma_accept succeeds; cleared
	 * when the slot is released on CM teardown. Lets the teardown handler
	 * return the slot for a half-open child that never reached
	 * ESTABLISHED, without double-decrementing on a second event. Unused
	 * on the initiator (see urp_acceptor_should_release_slot).
	 */
	bool			accept_slot_held;

	/*
	 * Design 31 D1 interop: peer's advertised endpoint kind is FAST,
	 * learned from the CM private_data trailer (urp_conn_priv_peer_kind)
	 * at accept (initiator's connect payload) / ESTABLISHED (acceptor's
	 * accept payload). A fast peer has no pump to answer keepalive PINGs,
	 * so probing it churns the connection -- urp_probe_work_fn skips a QP
	 * whose peer is fast. Defaults false (peer unknown / pre-trailer build
	 * => treated as UDS => probed as before).
	 */
	bool			peer_is_fast;

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

	/*
	 * Phase 3b QP health-probe state (design 08a). probe_seq is the
	 * next outgoing PING sequence number. last_ping_ns is the
	 * monotonic timestamp of the last PING we sent on this QP, used
	 * to compute RTT when the PONG arrives. consecutive_misses
	 * counts PINGs that didn't get a PONG within the timeout (used
	 * by the qualifying / draining state machine). rtt_ewma_ns is
	 * the exponentially-weighted RTT in nanoseconds (alpha = 0.2);
	 * 0 until the first PONG lands.
	 */
	u32			probe_seq;
	u32			consecutive_misses;
	u32			consecutive_pongs;
	u64			last_ping_ns;
	u64			rtt_ewma_ns;

	/*
	 * Phase 3b Step 5: per-QP health state machine (design 08a section 8a.9).
	 * Values are from UAPI `enum urp_qp_state`
	 * (QUALIFYING / ACTIVE / DRAINING / REMOVED). For now QPs start
	 * directly in ACTIVE on RDMA_CM_EVENT_ESTABLISHED (Qualifying is
	 * skipped pending a slow-interval probe phase); on >= 3
	 * consecutive missed PONGs the state transitions to DRAINING and
	 * urp_qp_select_round_robin stops dispatching on that QP.
	 */
	u8			health;

	/*
	 * Phase 5 Step 3: rdma_connect() takes id->qp_mutex internally
	 * but the rdma_cm framework holds that same mutex while calling
	 * our CM event handler -- calling rdma_connect inline from the
	 * handler self-deadlocks the cma_work_handler kworker. Defer
	 * the connect to this work item; CM event ROUTE_RESOLVED queues
	 * it and returns 0, releasing the mutex, then the worker calls
	 * rdma_connect from a context where it can acquire the mutex.
	 */
	struct work_struct	connect_work;

	/*
	 * Design 33 Phase 1: bounded initiator connect-retry. connect_attempts
	 * counts re-dials since the last successful ESTABLISHED (reset to 0
	 * there); connect_retry_work re-dials from scratch after a capped
	 * exponential backoff. Deferred to a work item because the re-dial
	 * destroys and rebuilds this QP's cm_id, which must NOT happen from
	 * inside the cm_id's own event handler. Unused on the acceptor.
	 */
	unsigned int		connect_attempts;
	struct delayed_work	connect_retry_work;
};

#define URP_QP_MISS_THRESHOLD	3

/*
 * Design 33 Phase 1: initiator connect-retry tunables. These are the compiled
 * defaults; the live values are the urp_connect_* globals below, writable at
 * runtime via /proc/sys/urp/connect_* (see urp_sysctl.c). max_attempts=0
 * disables retry. Backoff is capped exponential (base << attempt, clamped to
 * ceil) -- ~10 min total window at the defaults.
 */
#define URP_CONNECT_MAX_ATTEMPTS_DEFAULT	300
#define URP_CONNECT_BACKOFF_BASE_MS_DEFAULT	100
#define URP_CONNECT_BACKOFF_CEIL_MS_DEFAULT	2000

extern unsigned int urp_connect_max_attempts;
extern unsigned int urp_connect_backoff_base_ms;
extern unsigned int urp_connect_backoff_ceil_ms;

/* urp_sysctl.c -- /proc/sys/urp/ runtime tunables (design 33 Phase 1). */
int  urp_sysctl_register(void);
void urp_sysctl_unregister(void);

/* Phase 3b: PSK auth (design 17 / Tier 0.5). */
#define URP_PSK_HASH_LEN	32	/* SHA-256 digest size */
#define URP_PSK_AUTH_METHOD_SHA256	1

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
 * spaces are per-stream (design 09 section 9.6); reorder buffer + credit
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

	atomic64_t		tx_bytes;	/* payload bytes sent on this stream */
	atomic64_t		rx_bytes;	/* payload bytes delivered to its UDS */

	struct urp_credit	credit;		/* per-stream flow control */
	struct urp_reorder	*reorder;	/* per-stream reorder buffer */

	struct socket		*uds_sock;	/* this stream's UDS endpoint */
	struct task_struct	*tx_thread;	/* per-stream TX kthread (Step 7c) */
	bool			tx_done;	/* TX pump exited (client closed);
						 * eligible for reap-on-close
						 */

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
	/* enum urp_ep_mode: k0 (legacy single conn) vs multistream (default).
	 * Gates the acceptor's eager ep->conn backend connect -- see
	 * urp_conn_plan.h / urp_acceptor_should_eager_connect().
	 */
	enum urp_ep_mode	mode;
	/* enum urp_ep_kind: uds copy path (default) vs fast zero-copy path.
	 * A fast endpoint suppresses the UDS pump and is driven by the app over
	 * /dev/urp (io_uring_cmd); design 31. Rejected without CONFIG_URP_FAST.
	 */
	enum urp_ep_kind	kind;
	/* raw PSK input from netlink; used at cfg time only */
	u8			password[URP_PASSWORD_MAX];
	bool			has_password;
	/*
	 * Phase 3b Step 7: SHA-256(password) lives here once
	 * urp_endpoint_create runs. The raw 16-byte input is discarded
	 * after hashing; ep->password is zeroed for the live endpoint.
	 */
	u8			password_hash[URP_PSK_HASH_LEN];
	/*
	 * Phase 3b Step 8: pre-built private_data buffer for rdma_connect
	 * / rdma_accept. Layout:
	 *   [0]    auth_method (URP_PSK_AUTH_METHOD_SHA256)
	 *   [1..]  password_hash
	 * Built once at endpoint create time.
	 */
	u8			auth_priv[1 + URP_PSK_HASH_LEN];
	/*
	 * Design 31 D1 interop: the CM private_data we actually put on the wire
	 * for rdma_connect / rdma_accept = the optional auth_priv (when
	 * has_password) followed by a [MAGIC][MAGIC][kind] trailer that advertises
	 * this endpoint's kind (urp_conn_priv_build). Prebuilt once at create.
	 */
	u8			conn_priv[1 + URP_PSK_HASH_LEN + 3];
					/* +3 == URP_CONN_PRIV_TRAILER_LEN
					 * (urp_frame.h, included below the
					 * struct so the literal is used here) */
	u8			conn_priv_len;
	u8			auth_len;	/* has_password ? sizeof(auth_priv) : 0 */

	/* Lifecycle */
	enum urp_endpoint_state	state;
	struct mutex		lock;			/* serializes state transitions */
	struct rhash_head	ht_node;		/* urp_endpoints rhashtable linkage */
	struct rcu_head		rcu;			/* deferred free */
	/*
	 * Reference count. Initialised to 1 (the table reference) at create;
	 * urp_endpoint_get() takes an additional ref under RCU so a looked-up
	 * pointer stays valid after the RCU section ends (the SET/GET/DEL
	 * handlers deref ep outside RCU). urp_endpoint_remove() drops the table
	 * reference exactly once (the rhashtable_remove winner); the object is
	 * freed via call_rcu only when the last reference is dropped. Closes the
	 * lookup-vs-DEL use-after-free (design 26 / 27.8).
	 */
	struct kref		refcount;
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

	/* Phase 4 Step 1: kernel page_pool backing the buffer cache. */
	struct page_pool	*page_pool;

	/* Multi-QP state (Phase 3a Step 2 scaffold; Step 2b fills it) */
	struct urp_qp	*qps;		/* array of num_qps entries; allocated in activate */
	atomic_t		qps_connected;	/* count of QPs in ESTABLISHED state */
	atomic_t		qps_accepted;	/* acceptor: count of CONNECT_REQUESTs processed */
	atomic_t		rr_counter;	/* round-robin selector cursor */

	/* Stream multiplexing (Phase 3a Step 6) */
	struct rhashtable	streams;	/* keyed by u32 stream_id */
	bool			streams_inited;
	atomic_t		next_stream_id;	/* monotonic per-direction allocator */
	atomic_t		pending_reap;	/* # of tx_done streams awaiting reap;
						 * swept from the serialized RX path
						 */

	/* Buffer pool -- depth is ep->num_bufs (resolved from buffer_count at
	 * activation, clamped to [URP_BUFFER_COUNT_MIN, URP_BUFFER_COUNT_MAX]).
	 * bufs is a kcalloc'd array of num_bufs slots; page_pool backs them.
	 */
	struct urp_buffer	*bufs;
	u32			num_bufs;
	u32			buf_size;	/* live DMA slot bytes (from buffer_size) */
	struct list_head	send_free;
	struct list_head	recv_free;
	spinlock_t		send_lock;	/* protects send_free */
	spinlock_t		recv_lock;	/* protects recv_free */
	/*
	 * TX pumps block here when the send pool is empty instead of the old
	 * 1 ms poll; urp_buf_free_send() wakes it on every buffer return
	 * (send completion or error path). Design 35 §35.4 (Option B, phase 1).
	 */
	wait_queue_head_t	send_wq;

	/* Runtime state */
	struct urp_stats	stats;
	struct completion	cm_done;
	int			cm_status;
	bool			connected;

	/*
	 * design 33 Phase 2: lazy connect-on-first-use. The initiator no longer
	 * dials RDMA-CM at `urp add`; the first UDS client accept fires a
	 * lifetime one-shot dial. connect_started is the latch (0 -> 1 via
	 * atomic_cmpxchg in the accept thread; never reset). connect_failed is
	 * set on the initiator's terminal retry-exhaustion paths so a late
	 * client fails fast instead of parking forever on a consumed cm_done --
	 * the endpoint stays dead until `urp remove`/`add`. Accessed lock-free
	 * (single writer per terminal event, READ_ONCE/WRITE_ONCE).
	 */
	atomic_t		connect_started;
	bool			connect_failed;

	/* Phase 3b: per-endpoint probe ticker. Fires every
	 * URP_PROBE_INTERVAL_MS on the system workqueue, emits one PING
	 * per established QP, reschedules itself. Cancelled in
	 * urp_endpoint_drain.
	 */
	struct delayed_work	probe_work;
	bool			probe_active;

	/* /proc/urp/<name>/stats entry (set by urp_endpoint_proc_create) */
	struct proc_dir_entry	*proc_dir;
};

/*
 * True for a zero-copy (fast) endpoint: no UDS pump, driven by the app over
 * /dev/urp (design 31). Suppresses the accept thread, the acceptor eager
 * backend connect, and (initiator) selects the dial-at-activate path. Always
 * false when URP_FAST_ENABLED is 0 -- such an endpoint is refused at create.
 */
static inline bool urp_ep_is_fast(const struct urp_endpoint *ep)
{
	return ep->kind == URP_EP_KIND_FAST;
}

/* Module-global endpoint store (defined in urp_endpoint.c) */
extern struct rhashtable	urp_endpoints;
extern bool			urp_endpoints_inited;

/* urp_endpoint.c -- lifecycle */
/*
 * Design 33 Phase 1: decode the endpoint's stored IPv4-mapped sockaddr_in6
 * into the (dotted-quad string, port) tuple urp_rdma re-dials with. Shared by
 * urp_endpoint_activate and the connect-retry work item.
 */
int  urp_endpoint_extract_v4(const struct sockaddr_in6 *addr, char *out_ip,
			     size_t out_len, int *out_port);
int  urp_endpoint_table_init(void);
void urp_endpoint_table_destroy(void);
/*
 * On success *out holds a caller reference (count = table ref + 1); the caller
 * must urp_endpoint_put() it when done.
 */
int  urp_endpoint_create(struct urp_endpoint *cfg, struct urp_endpoint **out);
int  urp_endpoint_activate(struct urp_endpoint *ep);
void urp_endpoint_drain(struct urp_endpoint *ep);
/*
 * Look up an endpoint by name and take a reference. Self-brackets RCU, so the
 * returned pointer is safe to dereference after this returns; the caller must
 * urp_endpoint_put() it. Returns NULL if not found (or being torn down).
 */
struct urp_endpoint *urp_endpoint_get(const char *name);
void urp_endpoint_put(struct urp_endpoint *ep);
/*
 * Unpublish from the table and tear down, exactly once. The thread that wins
 * the rhashtable removal drains the endpoint and drops the table reference;
 * concurrent removers return without double-draining. The object is freed via
 * call_rcu when its last reference is dropped.
 */
void urp_endpoint_remove(struct urp_endpoint *ep);
void urp_endpoint_drain_all(void);

/* urp_socket.c */
int  urp_socket_init(struct urp_endpoint *ep, const char *path);
void urp_socket_cleanup(struct urp_endpoint *ep);
void urp_socket_conn_cleanup(struct urp_endpoint *ep);
int  urp_connect_uds(struct urp_endpoint *ep, const char *path);
int  urp_stream_connect_uds(struct urp_stream *stream, const char *path);

/* urp_rdma.c */
/* Phase 5 Step 3: deferred rdma_connect (see urp_rdma.c). */
void urp_connect_work_fn(struct work_struct *w);
/* Design 33 Phase 1: deferred initiator connect-retry (backoff re-dial). */
void urp_connect_retry_work_fn(struct work_struct *w);
/* Design 33 Phase 1.5: probe-detected silent-drop -> connect-retry (no CM event). */
void urp_connect_retry_on_silent_drop(struct urp_endpoint *ep, struct urp_qp *qp);
/* Design 33 Phase 2: fire the deferred initiator dial on first UDS accept. */
void urp_lazy_connect_start(struct urp_endpoint *ep);

int  urp_rdma_init(struct urp_endpoint *ep, const char *peer_addr,
		   int peer_port, int bind_port, bool is_initiator);
void urp_rdma_cleanup(struct urp_endpoint *ep);
struct urp_buffer *urp_buf_alloc_send(struct urp_endpoint *ep);
void urp_buf_free_send(struct urp_endpoint *ep, struct urp_buffer *buf);
struct urp_buffer *urp_buf_alloc_recv(struct urp_endpoint *ep);
void urp_buf_free_recv(struct urp_endpoint *ep, struct urp_buffer *buf);

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
/* Reap tx_done (client-closed) streams. MUST be called only from the
 * serialized RX completion path (urp_recv_done) so it never races the
 * lock-free stream lookups there. Cheap no-op when nothing is pending.
 */
void urp_streams_reap(struct urp_endpoint *ep);
u32  urp_stream_next_id(struct urp_endpoint *ep);
int  urp_stream_create(struct urp_endpoint *ep, u32 stream_id,
		       struct urp_stream **out_stream);
struct urp_stream *urp_stream_lookup(struct urp_endpoint *ep, u32 stream_id);
void urp_stream_destroy(struct urp_endpoint *ep, struct urp_stream *s);

/*
 * Pure stream state-machine core (design 28 E2). The SYN/FIN/RST
 * transitions are extracted from the handlers below so they can be
 * table-tested in KUnit without sockets/kthreads, and mirrored 1:1 by
 * the Rust `stream` module (crates/uds-rdma-protocol/src/stream.rs) for
 * a differential check -- keep the two in lock-step.
 *
 * Events are internal (not on the wire); the states are the UAPI
 * enum urp_stream_state. urp_stream_next_state() is a pure function of
 * (current state, event): it returns the next state, a bitmask of side
 * effects the caller must apply (socket shutdown / destroy), and whether
 * the event was accepted (RX_SYN on a closing/closed stream is not).
 */
/*
 * The pure state-machine types + urp_stream_next_state() prototype live in
 * urp_stream_sm.h (extracted so the transition logic can be fuzzed in
 * userspace against the shim). enum urp_stream_state (UAPI) is already in
 * scope by this point.
 */
#include "urp_stream_sm.h"

/* urp_stream.c -- lifecycle handlers (Phase 3a Step 7) */
int  urp_stream_rx_syn(struct urp_endpoint *ep, u32 stream_id,
		       struct urp_stream **out_stream);
int  urp_stream_rx_fin(struct urp_stream *s);
int  urp_stream_rx_rst(struct urp_stream *s);
void urp_stream_tx_fin(struct urp_stream *s);
int  urp_stream_rx_dispatch(struct urp_endpoint *ep, u32 stream_id, u8 flags,
			    struct urp_stream **out_stream);
/* Acceptor-side backend-connect split (Step 7d): needs_backend() is a cheap
 * predicate safe to call under rcu_read_lock; open_backend() does the blocking
 * kernel_connect and must be called outside it.
 */
bool urp_stream_needs_backend(struct urp_endpoint *ep, struct urp_stream *s);
void urp_stream_open_backend(struct urp_stream *s);

/* urp_pump.c */
int  urp_pump_start(struct urp_endpoint *ep);
void urp_pump_stop(struct urp_endpoint *ep);
int  urp_stream_pump_start(struct urp_stream *stream);
void urp_stream_pump_stop(struct urp_stream *stream);
int  urp_emit_credit_frame(struct urp_endpoint *ep, struct urp_qp *qp,
			   u32 stream_id, u16 grants);
int  urp_emit_pong_on(struct urp_endpoint *ep, struct ib_qp *qp,
		      const void *ping_payload);
void urp_probe_work_start(struct urp_endpoint *ep);
void urp_probe_work_stop(struct urp_endpoint *ep);
/* Post one single-SGE IB_WR_SEND at @addr/@lkey (header included) completing
 * through @cqe. Shared by the pump (pool buffers) and the urp-fast zero-copy
 * SEND path (design 31, pool-wide MR). Caller owns DMA-sync and bookkeeping.
 */
int  urp_post_frame_raw(struct ib_qp *qp, u64 addr, u32 len, u32 lkey,
			struct ib_cqe *cqe);
/* Post one single-SGE recv WR at @addr/@lkey (frame landing space) completing
 * through @cqe. The urp-fast zero-copy RECV path (design 31) arms an app pool
 * buffer directly on the endpoint QP; caller owns the ownership bookkeeping.
 */
int  urp_post_recv_raw(struct ib_qp *qp, u64 addr, u32 len, u32 lkey,
		       struct ib_cqe *cqe);

/* CQ completion callbacks (urp_rdma.c) -- used by pump when posting sends */
void urp_send_done(struct ib_cq *cq, struct ib_wc *wc);
/* Recv completion for SRQ-posted buffers -- urp_srq.c wires it as the
 * cqe.done of every SRQ recv; lives in urp_rdma.c beside the buffer
 * pool helpers it shares with the non-SRQ path.
 */
void urp_recv_done_for_srq(struct ib_cq *cq, struct ib_wc *wc);

/* urp_proc.c */
int  urp_proc_init(void);
void urp_proc_cleanup(void);
int  urp_endpoint_proc_create(struct urp_endpoint *ep);
void urp_endpoint_proc_remove(struct urp_endpoint *ep);

/* urp_netlink.c */
int  urp_genl_register(void);
void urp_genl_unregister(void);
void urp_send_event(struct urp_endpoint *ep);

/*
 * Frame codec inlines + the RX frame classifier live in a standalone,
 * kernel-infrastructure-free header so they can also be compiled into the
 * userspace libFuzzer harness (design 27 F1 / design 28). urp.h's earlier
 * includes + the URP_BUF_SIZE/URP_MAX_PAYLOAD defines above satisfy its
 * includer contract.
 */
#include "urp_frame.h"

#endif /* _URP_H */
