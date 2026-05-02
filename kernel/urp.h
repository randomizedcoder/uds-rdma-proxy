/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UDS-RDMA Proxy (urp) internal header
 *
 * Phase k0: single endpoint, single QP, no credits, no reorder.
 */
#ifndef _URP_H
#define _URP_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/un.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/completion.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "include/uapi/linux/urp.h"

/* Buffer pool sizing */
#define URP_NUM_BUFS		64
#define URP_BUF_SIZE		4096	/* payload + header */
#define URP_MAX_PAYLOAD		(URP_BUF_SIZE - URP_FRAME_HEADER_SIZE)
#define URP_CQ_ENTRIES		(URP_NUM_BUFS * 2)	/* send + recv */
#define URP_SQ_DEPTH		URP_NUM_BUFS
#define URP_RQ_DEPTH		URP_NUM_BUFS

/* Module parameter path length */
#define URP_PATH_MAX		108	/* sizeof(struct sockaddr_un.sun_path) */

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
 * struct urp_endpoint - top-level module state
 * @listen_sock:   UDS listening socket
 * @accept_thread: kthread running the accept loop
 * @conn:          current connection (k0: only one)
 *
 * RDMA resources:
 * @cm_id:     RDMA CM identifier
 * @pd:        protection domain
 * @send_cq:   send completion queue
 * @recv_cq:   receive completion queue
 * @qp:        queue pair
 *
 * Buffer pool:
 * @bufs:       array of all buffers
 * @free_list:  available buffers
 * @free_lock:  protects free_list
 *
 * State:
 * @stats:      /proc counters
 * @cm_done:    RDMA CM event completion
 * @connected:  RDMA connection is up
 */
struct urp_endpoint {
	/* UDS side */
	struct socket		*listen_sock;
	struct task_struct	*accept_thread;
	struct urp_connection	conn;

	/* RDMA side */
	struct rdma_cm_id	*cm_id;		/* active connection (or listener before connect) */
	struct rdma_cm_id	*listen_id;	/* acceptor: listener CM ID (kept for cleanup) */
	struct ib_pd		*pd;
	struct ib_cq		*send_cq;
	struct ib_cq		*recv_cq;
	struct ib_qp		*qp;

	/* Buffer pool */
	struct urp_buffer	bufs[URP_NUM_BUFS];
	struct list_head	send_free;
	struct list_head	recv_free;
	spinlock_t		send_lock;
	spinlock_t		recv_lock;

	/* State */
	struct urp_stats	stats;
	struct completion	cm_done;
	int			cm_status;
	bool			connected;
	bool			is_initiator;
	char			uds_path[URP_PATH_MAX];

	/* RX work */
	struct work_struct	rx_work;
	struct workqueue_struct	*rx_wq;
};

/* Global endpoint (k0: single instance) */
extern struct urp_endpoint *urp_ep;

/* urp_socket.c */
int urp_socket_init(struct urp_endpoint *ep, const char *path);
void urp_socket_cleanup(struct urp_endpoint *ep);
int urp_connect_uds(struct urp_endpoint *ep, const char *path);

/* urp_rdma.c */
int urp_rdma_init(struct urp_endpoint *ep, const char *peer_addr,
		  int peer_port, int bind_port, bool is_initiator);
void urp_rdma_cleanup(struct urp_endpoint *ep);
struct urp_buffer *urp_buf_alloc_send(struct urp_endpoint *ep);
void urp_buf_free_send(struct urp_endpoint *ep, struct urp_buffer *buf);
struct urp_buffer *urp_buf_alloc_recv(struct urp_endpoint *ep);
void urp_buf_free_recv(struct urp_endpoint *ep, struct urp_buffer *buf);
int urp_post_recv(struct urp_endpoint *ep, struct urp_buffer *buf);
int urp_post_recv_all(struct urp_endpoint *ep);

/* urp_pump.c */
int urp_pump_start(struct urp_endpoint *ep);
void urp_pump_stop(struct urp_endpoint *ep);

/* CQ completion callbacks (urp_rdma.c) — used by pump when posting sends */
void urp_send_done(struct ib_cq *cq, struct ib_wc *wc);

/* urp_proc.c */
int urp_proc_init(void);
void urp_proc_cleanup(void);

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
