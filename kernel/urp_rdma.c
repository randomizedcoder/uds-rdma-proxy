// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- RDMA connection management
 *
 * Phase k0: Single QP, simple free-list buffer pool, RDMA CM connection.
 *
 * Initiator: rdma_resolve_addr() -> rdma_connect() to peer.
 * Acceptor:  rdma_listen() -> rdma_accept() from peer.
 *
 * Buffer pool: ep->num_bufs pages (from buffer_count), DMA-mapped, split into
 * send and recv pools.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"
#include <linux/inet.h>
#include <linux/ktime.h>
/*
 * page_pool was reorganised into net/page_pool/{helpers,types}.h in the
 * 6.6/6.7 era; the pre-split LTS line (6.1) still ships the single
 * net/page_pool.h. Detect the split via __has_include so we don't hinge
 * on an exact version boundary. The page_pool_params fields and the
 * create / dev_alloc_pages / put_page / destroy calls we use are present
 * in both layouts. sparse (__CHECKER__) cannot lex
 * __has_include(<...>), so it takes an explicit version-gate branch;
 * real compilers keep the exact-header probe.
 */
#if defined(__CHECKER__)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define URP_HAVE_SPLIT_PAGE_POOL 1
#endif
#elif defined(__has_include)
#if __has_include(<net/page_pool/helpers.h>)
#define URP_HAVE_SPLIT_PAGE_POOL 1
#endif
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define URP_HAVE_SPLIT_PAGE_POOL 1
#endif
#ifdef URP_HAVE_SPLIT_PAGE_POOL
#include <net/page_pool/helpers.h>
#include <net/page_pool/types.h>
#else
#include <net/page_pool.h>
#endif

/* ---- Buffer pool (Phase 4 Step 1: kernel page_pool) ----
 *
 * page_pool replaces our hand-rolled alloc_page with the standard
 * kernel pool that NIC drivers use. We deliberately do NOT use
 * PP_FLAG_DMA_MAP: software RDMA (rxe / siw) ib_devices have
 * `dma_device == NULL` and override `dma_ops`, so the DMA address has
 * to come from ib_dma_map_page rather than dma_map_page (which is what
 * PP_FLAG_DMA_MAP calls under the covers). For hardware RDMA NICs the
 * two paths happen to be equivalent, but doing it this way keeps the
 * rxe-based test environment working unchanged -- this was the plan's
 * "primary k2 engineering risk" (Plan section 4.1) and the answer is
 * "page_pool for page lifecycle, ib_dma_map_page for the mapping."
 *
 * Hot-path callers (urp_buf_alloc_send/recv) are unchanged: pages are
 * allocated up front, attached to urp_buffer slots, and the free list
 * keeps the existing spinlock semantics. page_pool_put_page is only
 * called at endpoint teardown, just before page_pool_destroy.
 */

static int urp_bufs_init(struct urp_endpoint *ep, struct ib_device *dev)
{
	struct page_pool_params pp = {
		.flags		= 0,	/* no DMA mapping -- see comment above */
		/*
		 * order covers the whole slot: for the default 4096-byte slot on
		 * a 4K-page host this is 0 (one page, unchanged); a larger
		 * buffer_size raises it so each slot is one physically-contiguous
		 * compound page. A high-order allocation can fail under memory
		 * fragmentation -- page_pool returns NULL and activation fails
		 * cleanly rather than corrupting anything.
		 */
		.order		= get_order(ep->buf_size),
		.pool_size	= ep->num_bufs,
		/*
		 * Phase 4 Step 2: NUMA-aware allocation. When the
		 * underlying NIC has a real PCI parent we can read its
		 * numa_node and steer page_pool to allocate close to the
		 * NIC. For rxe / siw the parent is missing or has no NUMA
		 * binding, so we fall back to NUMA_NO_NODE ("any node") --
		 * page_pool handles either input transparently.
		 */
		.nid		= (dev->dev.parent
				   ? dev_to_node(dev->dev.parent)
				   : NUMA_NO_NODE),
		.dev		= NULL,
		.dma_dir	= DMA_BIDIRECTIONAL,
		.max_len	= ep->buf_size,
		.offset		= 0,
	};
	int i;

	INIT_LIST_HEAD(&ep->send_free);
	INIT_LIST_HEAD(&ep->recv_free);
	spin_lock_init(&ep->send_lock);
	spin_lock_init(&ep->recv_lock);

	/* ep->num_bufs is resolved (and bounds-clamped) in urp_endpoint_activate */
	ep->bufs = kcalloc(ep->num_bufs, sizeof(*ep->bufs), GFP_KERNEL);
	if (!ep->bufs)
		return -ENOMEM;

	ep->page_pool = page_pool_create(&pp);
	if (IS_ERR(ep->page_pool)) {
		int ret = PTR_ERR(ep->page_pool);

		ep->page_pool = NULL;
		kfree(ep->bufs);
		ep->bufs = NULL;
		pr_err("page_pool_create failed: %d\n", ret);
		return ret;
	}

	for (i = 0; i < ep->num_bufs; i++) {
		struct urp_buffer *buf = &ep->bufs[i];

		buf->index = i;
		buf->page = page_pool_dev_alloc_pages(ep->page_pool);
		if (!buf->page) {
			pr_err("page_pool_dev_alloc_pages failed for buf %d\n", i);
			goto err;
		}

		buf->data = page_address(buf->page);
		buf->dma_addr = ib_dma_map_page(dev, buf->page, 0,
						ep->buf_size, DMA_BIDIRECTIONAL);
		if (ib_dma_mapping_error(dev, buf->dma_addr)) {
			pr_err("ib_dma_map_page failed for buf %d\n", i);
			page_pool_put_page(ep->page_pool, buf->page, -1, false);
			buf->page = NULL;
			goto err;
		}

		buf->sge.addr = buf->dma_addr;
		buf->sge.length = ep->buf_size;
		/* lkey set after PD creation */

		INIT_LIST_HEAD(&buf->list);

		/* Split: first half send, second half recv */
		if (i < ep->num_bufs / 2)
			list_add_tail(&buf->list, &ep->send_free);
		else
			list_add_tail(&buf->list, &ep->recv_free);
	}

	return 0;

err:
	for (i = i - 1; i >= 0; i--) {
		struct urp_buffer *buf = &ep->bufs[i];

		if (buf->page) {
			ib_dma_unmap_page(dev, buf->dma_addr, ep->buf_size,
					  DMA_BIDIRECTIONAL);
			page_pool_put_page(ep->page_pool, buf->page, -1, false);
			buf->page = NULL;
		}
	}
	page_pool_destroy(ep->page_pool);
	ep->page_pool = NULL;
	kfree(ep->bufs);
	ep->bufs = NULL;
	return -ENOMEM;
}

static void urp_bufs_cleanup(struct urp_endpoint *ep, struct ib_device *dev)
{
	int i;

	if (!ep->page_pool)
		return;

	for (i = 0; i < ep->num_bufs; i++) {
		struct urp_buffer *buf = &ep->bufs[i];

		if (!buf->page)
			continue;

		ib_dma_unmap_page(dev, buf->dma_addr, ep->buf_size,
				  DMA_BIDIRECTIONAL);
		page_pool_put_page(ep->page_pool, buf->page, -1, false);
		buf->page = NULL;
	}

	page_pool_destroy(ep->page_pool);
	ep->page_pool = NULL;
	kfree(ep->bufs);
	ep->bufs = NULL;
}

struct urp_buffer *urp_buf_alloc_send(struct urp_endpoint *ep)
{
	struct urp_buffer *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&ep->send_lock, flags);
	if (!list_empty(&ep->send_free)) {
		buf = list_first_entry(&ep->send_free, struct urp_buffer, list);
		list_del_init(&buf->list);
	}
	spin_unlock_irqrestore(&ep->send_lock, flags);

	return buf;
}

void urp_buf_free_send(struct urp_endpoint *ep, struct urp_buffer *buf)
{
	unsigned long flags;

	spin_lock_irqsave(&ep->send_lock, flags);
	list_add_tail(&buf->list, &ep->send_free);
	spin_unlock_irqrestore(&ep->send_lock, flags);
}

struct urp_buffer *urp_buf_alloc_recv(struct urp_endpoint *ep)
{
	struct urp_buffer *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&ep->recv_lock, flags);
	if (!list_empty(&ep->recv_free)) {
		buf = list_first_entry(&ep->recv_free, struct urp_buffer, list);
		list_del_init(&buf->list);
	}
	spin_unlock_irqrestore(&ep->recv_lock, flags);

	return buf;
}

void urp_buf_free_recv(struct urp_endpoint *ep, struct urp_buffer *buf)
{
	unsigned long flags;

	spin_lock_irqsave(&ep->recv_lock, flags);
	list_add_tail(&buf->list, &ep->recv_free);
	spin_unlock_irqrestore(&ep->recv_lock, flags);
}

/* ---- Deferred rdma_connect ---- */

/*
 * Phase 5 Step 3 fix: rdma_connect() can't be called from inside the
 * CM event handler because the rdma_cm core holds id->qp_mutex while
 * invoking the handler, and rdma_connect re-acquires it. We schedule
 * this work from ROUTE_RESOLVED; it runs after the handler returns
 * and the mutex is free.
 */
void urp_connect_work_fn(struct work_struct *w)
{
	struct urp_qp *qp = container_of(w, struct urp_qp, connect_work);
	struct urp_endpoint *ep = qp->ep;
	struct rdma_conn_param param = {};
	int ret;

	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.retry_count = 7;
	param.rnr_retry_count = 7;
	/*
	 * Phase 3b Step 8: include PSK auth payload in private_data when
	 * configured. Acceptor compares the hash against its own and
	 * rdma_reject's on mismatch.
	 */
	if (ep->has_password) {
		param.private_data = ep->auth_priv;
		param.private_data_len = sizeof(ep->auth_priv);
	}

	ret = rdma_connect(qp->cm_id, &param);
	if (ret)
		pr_err("rdma_connect failed on qp %u: %d\n",
		       qp->index, ret);
}

/* ---- Shared per-endpoint RDMA setup ---- */

/*
 * One-shot setup of resources shared across all QPs of an endpoint:
 * PD, buffer pool, and the two CQs that all QPs feed. Called from the
 * first cm-event that observes the ib_device (ROUTE_RESOLVED for the
 * initiator, CONNECT_REQUEST for the acceptor); subsequent invocations
 * are no-ops because ep->pd is already populated.
 */
static int urp_endpoint_setup_shared(struct urp_endpoint *ep,
				     struct ib_device *dev)
{
	u32 cq_entries;
	int i, ret;

	if (ep->pd)
		return 0;

	ep->ib_dev = dev;

	ep->pd = ib_alloc_pd(dev, 0);
	if (IS_ERR(ep->pd)) {
		ret = PTR_ERR(ep->pd);
		ep->pd = NULL;
		pr_err("ib_alloc_pd failed: %d\n", ret);
		return ret;
	}

	ret = urp_bufs_init(ep, dev);
	if (ret)
		goto err_pd;

	for (i = 0; i < ep->num_bufs; i++)
		ep->bufs[i].sge.lkey = ep->pd->local_dma_lkey;

	/*
	 * Shared CQs sized so completions don't back up under N QPs of
	 * sustained traffic. num_bufs*2 (send + recv halves) is the single-QP
	 * figure; scale it by num_qps. Clamp to the device's max_cqe so a large
	 * buffer_count x num_qps product can't push ib_alloc_cq past HW limits.
	 */
	cq_entries = min_t(u32, (u32)ep->num_bufs * 2 * ep->num_qps,
			   (u32)dev->attrs.max_cqe);

	ep->send_cq = ib_alloc_cq(dev, ep, cq_entries, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(ep->send_cq)) {
		ret = PTR_ERR(ep->send_cq);
		ep->send_cq = NULL;
		pr_err("ib_alloc_cq (send) failed: %d\n", ret);
		goto err_bufs;
	}

	ep->recv_cq = ib_alloc_cq(dev, ep, cq_entries, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(ep->recv_cq)) {
		ret = PTR_ERR(ep->recv_cq);
		ep->recv_cq = NULL;
		pr_err("ib_alloc_cq (recv) failed: %d\n", ret);
		goto err_send_cq;
	}

	/* SRQ shared across all QPs (Step 3). */
	ret = urp_srq_create(ep);
	if (ret)
		goto err_recv_cq;

	return 0;

err_recv_cq:
	ib_free_cq(ep->recv_cq);
	ep->recv_cq = NULL;
err_send_cq:
	ib_free_cq(ep->send_cq);
	ep->send_cq = NULL;
err_bufs:
	urp_bufs_cleanup(ep, dev);
err_pd:
	ib_dealloc_pd(ep->pd);
	ep->pd = NULL;
	ep->ib_dev = NULL;
	return ret;
}

/*
 * Create the QP for one cm_id and pin it into ep->qps[qp_index]. The
 * shared PD/CQs must already be set up. After this returns success the
 * QP is in INIT; rdma_connect / rdma_accept transitions it to RTS.
 */
static int urp_qp_create_on_cm_id(struct urp_endpoint *ep,
				  struct rdma_cm_id *cm_id, u32 qp_index)
{
	struct ib_qp_init_attr attr = {};
	int ret;

	attr.send_cq = ep->send_cq;
	attr.recv_cq = ep->recv_cq;
	attr.srq = ep->srq;			/* Step 3: shared RQ */
	/* SQ deep enough for the send half of the pool; never past HW limit. */
	attr.cap.max_send_wr = min_t(u32, ep->num_bufs,
				     (u32)ep->ib_dev->attrs.max_qp_wr);
	attr.cap.max_recv_wr = 0;		/* recvs flow through SRQ */
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.qp_type = IB_QPT_RC;
	attr.sq_sig_type = IB_SIGNAL_ALL_WR;

	ret = rdma_create_qp(cm_id, ep->pd, &attr);
	if (ret) {
		pr_err("rdma_create_qp[%u] failed: %d\n", qp_index, ret);
		return ret;
	}

	ep->qps[qp_index].qp = cm_id->qp;
	ep->qps[qp_index].cm_id = cm_id;
	return 0;
}

/*
 * CQ completion callbacks -- wired up after connection is established.
 * These are called from ib_poll_cq workqueue context.
 */
void urp_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_buffer *buf = container_of(wc->wr_cqe, struct urp_buffer, cqe);
	struct urp_endpoint *ep = cq->cq_context;

	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR)
		pr_err_ratelimited("send completion error: %s (%d)\n",
				   ib_wc_status_msg(wc->status), wc->status);

	/*
	 * Always return the buffer to the pool, including on flush after QP
	 * destruction. Without this, every QP teardown permanently
	 * leaks outstanding send buffers and the pool eventually exhausts.
	 */
	urp_buf_free_send(ep, buf);
}

/* urp_classify_frame() lives in urp_frame.c (design 28 E1) so it can be
 * compiled into the userspace fuzz harness as well as the module.
 */

/*
 * Deliver @len payload bytes at @data to the UDS socket @uds and account
 * the delivered frame. @s is the owning stream (NULL for the legacy k0
 * connection). Runs in the serialized recv-CQ workqueue context, so
 * kernel_sendmsg may sleep. Returns 0 or the sendmsg error.
 */
static int urp_rx_send_uds(struct urp_endpoint *ep, struct urp_stream *s,
			   struct socket *uds, const void *data, u32 len)
{
	struct msghdr msg = {};
	struct kvec iov;
	int ret;

	if (len == 0)
		return 0;	/* nothing to deliver (e.g. a bare FIN frame) */

	iov.iov_base = (void *)data;
	iov.iov_len = len;
	iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, len);

	ret = kernel_sendmsg(uds, &msg, &iov, 1, len);
	if (ret < 0) {
		pr_err_ratelimited("kernel_sendmsg failed: %d\n", ret);
		return ret;
	}

	atomic64_add(len, &ep->stats.rx_bytes);
	atomic64_inc(&ep->stats.rx_frames);
	if (s)
		atomic64_add(len, &s->rx_bytes);
	return 0;
}

/*
 * Stream delivery through the per-stream reorder buffer (design 29 Gap 1
 * fix). Multi-QP arrival skew means frames can complete out of sequence;
 * feeding them through s->reorder keyed by @seq and draining the in-order
 * prefix restores the byte stream before it reaches the UDS socket. On a
 * single QP the frame is always the next expected, so this reduces to
 * insert-then-immediately-drain (one buffer copy, no reordering).
 *
 * @scratch is an ep->buf_size staging buffer supplied by the caller (the
 * recv buffer itself, already copied out of by the insert). The reorder
 * buffer never yields more than this endpoint's max payload
 * (buf_size - header) < buf_size, so drain never reports -ENOBUFS.
 *
 * Serialization: the recv CQ is IB_POLL_WORKQUEUE, so completions for an
 * endpoint run one at a time -- the same invariant that lets the caller
 * touch @s without a per-stream ref. s->reorder is thus accessed by a
 * single context here, matching the reorder buffer's "external
 * serialization required" contract.
 */
static void urp_rx_deliver_stream(struct urp_endpoint *ep, struct urp_stream *s,
				  struct socket *uds, u64 seq,
				  const u8 *payload, u32 payload_len,
				  u8 *scratch)
{
	u64 expected = urp_reorder_next_expected(s->reorder);
	int ret = urp_reorder_insert(s->reorder, seq, payload, payload_len);

	if (ret == 0) {
		/* A frame that is not the next expected got buffered: real
		 * out-of-order arrival. In-order frames (seq == expected)
		 * drain immediately and are not counted as reorderings.
		 */
		if (seq != expected)
			atomic64_inc(&ep->stats.reorder_insertions);
	} else {
		/* -EEXIST (duplicate/retransmit), -ENOBUFS (window full), or
		 * -ENOMEM: this frame is dropped. Whatever is already in order
		 * still drains below.
		 */
		atomic64_inc(&ep->stats.reorder_drops);
	}

	for (;;) {
		u64 dseq;
		size_t dlen = ep->buf_size;

		if (urp_reorder_drain_next(s->reorder, &dseq, scratch, &dlen))
			break;	/* -ENOENT: no more in-order frames ready */
		urp_rx_send_uds(ep, s, uds, scratch, (u32)dlen);
	}
}

static void urp_recv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_buffer *buf = container_of(wc->wr_cqe, struct urp_buffer, cqe);
	struct urp_endpoint *ep = cq->cq_context;
	struct socket *uds = NULL;
	struct urp_stream *rx_stream = NULL;
	struct urp_rx_decoded dec;
	enum urp_rx_action action;
	u32 payload_len;
	u32 stream_id;
	int ret;

	if (wc->status != IB_WC_SUCCESS) {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			pr_err_ratelimited("recv completion error: %s (%d)\n",
					   ib_wc_status_msg(wc->status), wc->status);
		/* Return buffer to pool so it can be reposted. */
		urp_buf_free_recv(ep, buf);
		return;
	}

	/*
	 * Eager reap-on-close is intentionally NOT done here -- see the
	 * comment on urp_streams_reap() (urp_stream.c) for why, and where
	 * streams do get reaped.
	 */

	/*
	 * Classify + validate the frame (pure; design 28 E1). The action tells
	 * us what to do; dec holds the decoded header fields. Every length
	 * guard (design 27 27.8 #1) lives in urp_classify_frame.
	 */
	action = urp_classify_frame(wc->byte_len, buf->data, &dec);
	payload_len = dec.payload_len;
	stream_id = dec.stream_id;

	switch (action) {
	case URP_RX_DROP_SHORT:
		pr_err_ratelimited("short frame: %u bytes (< header)\n",
				   wc->byte_len);
		goto repost;
	case URP_RX_DROP_OVERSIZE:
		pr_err_ratelimited("received oversized frame: %u\n", payload_len);
		goto repost;
	case URP_RX_DROP_PAYLOAD_OVERRUN:
		pr_err_ratelimited("frame payload_len %u exceeds received %u\n",
				   payload_len,
				   wc->byte_len - URP_FRAME_HEADER_SIZE);
		goto repost;
	case URP_RX_DROP_SHORT_PROBE:
		pr_err_ratelimited("short PROBE frame: %u bytes\n", wc->byte_len);
		goto repost;
	case URP_RX_CREDIT: {
		/*
		 * CONTROL/CREDIT frame: peer is granting us send credits on the
		 * QP this completion came in on. No UDS delivery.
		 */
		int qp_idx = urp_qp_index_of(ep, wc->qp);

		if (qp_idx >= 0 && (dec.flags & URP_CTRL_FLAG_CREDIT))
			urp_credit_grant(&ep->qps[qp_idx].credit, dec.credits);
		goto repost;
	}
	case URP_RX_PROBE_PONG: {
		/* RTT sample -> per-QP EWMA (alpha = 0.2, integer math). */
		int qp_idx = urp_qp_index_of(ep, wc->qp);

		if (qp_idx >= 0) {
			struct urp_qp *q = &ep->qps[qp_idx];
			const void *pp = buf->data + URP_FRAME_HEADER_SIZE;
			u64 t_send_mono = urp_ping_decode_t_send_mono(pp);
			u64 now = ktime_get_ns();

			if (now > t_send_mono) {
				u64 rtt = now - t_send_mono;

				/* First sample seeds directly so we don't pull
				 * the baseline toward zero.
				 */
				if (q->rtt_ewma_ns == 0)
					q->rtt_ewma_ns = rtt;
				else
					q->rtt_ewma_ns =
						(q->rtt_ewma_ns * 4 + rtt) / 5;
				/* PONG for the outstanding PING: clear
				 * last_ping_ns, bump the pong streak, reset
				 * misses (consecutive_pongs drives a future
				 * Qualifying -> ACTIVE promotion).
				 */
				q->last_ping_ns = 0;
				q->consecutive_misses = 0;
				q->consecutive_pongs++;
			}
		}
		goto repost;
	}
	case URP_RX_PROBE_PING:
		urp_emit_pong_on(ep, wc->qp, buf->data + URP_FRAME_HEADER_SIZE);
		goto repost;
	case URP_RX_DELIVER_LEGACY:
		/* stream_id == 0: k0/legacy single connection. */
		if (ep->conn.active && ep->conn.uds_sock)
			uds = ep->conn.uds_sock;
		break;
	case URP_RX_DELIVER_STREAM: {
		struct urp_stream *s = NULL;
		bool need_backend = false;

		/*
		 * Dispatch runs in sleepable workqueue context (the recv CQ is
		 * IB_POLL_WORKQUEUE) and legitimately sleeps: SYN creates a
		 * stream (GFP_KERNEL alloc), FIN/RST take the per-stream mutex
		 * and RST calls kthread_stop(). It must therefore NOT run under
		 * rcu_read_lock. urp_stream_lookup brackets its own rhashtable
		 * traversal, and the insert/remove paths take their own bucket
		 * locks, so no outer RCU section is needed. Serialized recv
		 * completions + drain-after-quiesce keep `s` alive across this
		 * handler.
		 */
		(void)urp_stream_rx_dispatch(ep, stream_id, dec.flags, &s);
		if (s) {
			uds = s->uds_sock;
			need_backend = urp_stream_needs_backend(ep, s);
		}

		/*
		 * Acceptor: a freshly SYN'd stream has no backend UDS yet.
		 * The (blocking) connect is done here rather than inline in the
		 * SYN handler so it happens once, in one place.
		 */
		if (need_backend) {
			urp_stream_open_backend(s);
			uds = s->uds_sock;
		}
		rx_stream = s;
		break;
	}
	}

	if (!uds) {
		atomic64_inc(&ep->stats.buffer_alloc_fails);
		goto repost;
	}

	/*
	 * Deliver the payload to the UDS socket. Stream frames pass through
	 * the per-stream reorder buffer (design 29 Gap 1) so multi-QP arrival
	 * skew is corrected before delivery; the legacy k0 path is a single
	 * in-order QP and delivers directly. The recv buffer doubles as the
	 * reorder drain staging area -- safe because insert copies the payload
	 * out before we drain back into it.
	 */
	if (rx_stream)
		urp_rx_deliver_stream(ep, rx_stream, uds,
				      urp_frame_decode_seq(buf->data),
				      buf->data + URP_FRAME_HEADER_SIZE,
				      payload_len, buf->data);
	else
		urp_rx_send_uds(ep, NULL, uds,
				buf->data + URP_FRAME_HEADER_SIZE, payload_len);

	/*
	 * Per-QP RX accounting + credit for the frame received on wc->qp,
	 * independent of how many frames the reorder buffer released. Peer
	 * sends DATA -> we count a pending grant; once accumulated grants
	 * reach the threshold (initial_credits / 4) we emit a CONTROL/CREDIT
	 * frame back. Non-URP peers (the userspace test client) ignore it;
	 * URP peers consume it via the URP_RX_CREDIT branch above.
	 */
	{
		int qp_idx = urp_qp_index_of(ep, wc->qp);

		if (qp_idx >= 0) {
			struct urp_qp *qp = &ep->qps[qp_idx];
			atomic64_add(payload_len, &qp->rx_bytes);
			atomic64_inc(&qp->rx_frames);
			urp_credit_record_recv(&qp->credit);
			/*
			 * Only grant toward multi-stream peers (non-zero
			 * stream_id). Legacy stream_id == 0 traffic posts only
			 * as many recv WRs as its echo logic needs; an
			 * unsolicited CREDIT frame there causes RNR.
			 */
			if (stream_id != 0 &&
			    urp_credit_should_grant(&qp->credit)) {
				u16 grants = urp_credit_take_grants(&qp->credit);

				urp_emit_credit_frame(ep, qp, stream_id, grants);
			}
		}
	}

repost:
	/*
	 * Recycle the buffer back to the SRQ. With one shared pool per
	 * endpoint, recv completions on any QP refill the same SRQ.
	 */
	urp_buf_free_recv(ep, buf);
	buf = urp_buf_alloc_recv(ep);
	if (buf && ep->srq) {
		ret = urp_post_srq_recv(ep, buf);
		if (ret)
			urp_buf_free_recv(ep, buf);
	} else if (buf) {
		urp_buf_free_recv(ep, buf);
	}
}

/*
 * Alias so urp_srq.c can register the same callback without exposing
 * the static linkage of urp_recv_done. The cqe->done function pointer
 * is what dispatches, so the alias is purely a naming convenience.
 */
void urp_recv_done_for_srq(struct ib_cq *cq, struct ib_wc *wc)
{
	urp_recv_done(cq, wc);
}

/*
 * Destroy a cm_id together with the urp_cm_ctx hanging off its context
 * pointer, and clear the caller's pointer. Every cm_id we create gets a
 * kzalloc'd ctx, so the pair must always be freed together.
 */
static void urp_cm_id_destroy(struct rdma_cm_id **idp)
{
	struct urp_cm_ctx *ctx = (*idp)->context;

	rdma_destroy_id(*idp);
	kfree(ctx);
	*idp = NULL;
}

/*
 * Acceptor: handle one CONNECT_REQUEST event on the listener cm_id.
 *
 * For each peer-side QP, the peer initiates one CM connection which
 * arrives here as a separate CONNECT_REQUEST. We attach the next free
 * urp_qp slot to the child cm_id, create the QP, pre-post recvs, and
 * rdma_accept. On the very first connect we also stand up the UDS data
 * path and start the TX pump so that recvs go somewhere once the QP
 * transitions to RTS (see the comment in the previous single-QP code).
 */
static int urp_cm_accept_one(struct rdma_cm_id *child, struct urp_endpoint *ep,
			     const void *peer_priv, u8 peer_priv_len)
{
	struct urp_cm_ctx *child_ctx;
	struct rdma_conn_param param = {};
	u32 qp_index;
	int ret;

	/*
	 * Phase 3b Step 8: PSK validation. With ep->has_password, the
	 * peer must have included a matching 1+32-byte auth payload in
	 * its rdma_connect private_data. Reject otherwise.
	 */
	if (ep->has_password) {
		if (peer_priv_len < sizeof(ep->auth_priv) ||
		    memcmp(peer_priv, ep->auth_priv, sizeof(ep->auth_priv))) {
			atomic64_inc(&ep->stats.auth_failures);
			pr_warn("rejecting CONNECT_REQUEST with bad/missing PSK\n");
			rdma_reject(child, NULL, 0, 0);
			/* Step 9: multicast event so `urp monitor` users see
			 * auth failures alongside state transitions.
			 */
			urp_send_event(ep);
			return 0;
		}
	}

	qp_index = (u32)atomic_inc_return(&ep->qps_accepted) - 1;
	if (qp_index >= ep->num_qps) {
		atomic_dec(&ep->qps_accepted);
		pr_warn("rejecting extra CONNECT_REQUEST (%u >= %u QPs)\n",
			qp_index, ep->num_qps);
		rdma_reject(child, NULL, 0, 0);
		return 0;
	}

	ret = urp_endpoint_setup_shared(ep, child->device);
	if (ret)
		goto err_release_slot;

	/*
	 * If this slot was used by a previous (now-disconnected) connection,
	 * tear down the old QP + cm_id before reusing. DISCONNECTED only
	 * marks the slot free; we defer destruction until reuse or drain to
	 * avoid destroying a cm_id from inside its own event handler.
	 */
	if (ep->qps[qp_index].cm_id) {
		if (ep->qps[qp_index].qp) {
			ib_drain_qp(ep->qps[qp_index].qp);
			rdma_destroy_qp(ep->qps[qp_index].cm_id);
			ep->qps[qp_index].qp = NULL;
		}
		urp_cm_id_destroy(&ep->qps[qp_index].cm_id);
		ep->qps[qp_index].established = false;
	}

	child_ctx = kzalloc(sizeof(*child_ctx), GFP_KERNEL);
	if (!child_ctx) {
		ret = -ENOMEM;
		goto err_release_slot;
	}
	child_ctx->ep = ep;
	child_ctx->qp_index = qp_index;
	child_ctx->is_listener = false;
	child->context = child_ctx;

	ret = urp_qp_create_on_cm_id(ep, child, qp_index);
	if (ret)
		goto err_free_ctx;

	/*
	 * On the FIRST accepted child we bring up UDS + the TX pump. We do
	 * it here (before rdma_accept) so recvs landing the moment the QP
	 * transitions to RTS already have somewhere to go -- same reason as
	 * the k0 single-QP path. Subsequent accepted QPs share the same
	 * pump (it round-robins across qps[] via urp_qp_select_round_robin).
	 *
	 * Phase 4 Step 5: defensive cleanup of any prior ep->conn state.
	 * The matching DISCONNECTED handler should have torn this down
	 * already, but if events arrive out of order (the kernel doesn't
	 * always emit DISCONNECTED before the next CONNECT_REQUEST when
	 * the peer slams the connection), the previous pump kthread
	 * would otherwise survive and racing with the new one.
	 */
	if (qp_index == 0 && ep->connect_path[0]) {
		urp_socket_conn_cleanup(ep);
		ret = urp_connect_uds(ep, ep->connect_path);
		if (!ret)
			ret = urp_pump_start(ep);
		if (ret) {
			pr_err("acceptor data path setup failed: %d\n", ret);
			goto err_destroy_qp;
		}
	}

	/* Recv buffers are pre-posted to ep->srq inside
	 * urp_endpoint_setup_shared; no per-QP RQ to fill (Step 3).
	 */

	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.rnr_retry_count = 7;
	/*
	 * Phase 3b Step 8: echo our own auth_priv in the accept reply
	 * so the initiator can (in a future bidirectional check) also
	 * validate the acceptor. Initiator-validates-acceptor is not
	 * wired yet -- this just stages the payload on the wire.
	 */
	if (ep->has_password) {
		param.private_data = ep->auth_priv;
		param.private_data_len = sizeof(ep->auth_priv);
	}
	ret = rdma_accept(child, &param);
	if (ret)
		goto err_destroy_qp;

	return 0;

err_destroy_qp:
	rdma_destroy_qp(child);
	ep->qps[qp_index].qp = NULL;
	ep->qps[qp_index].cm_id = NULL;
err_free_ctx:
	child->context = NULL;
	kfree(child_ctx);
err_release_slot:
	atomic_dec(&ep->qps_accepted);
	return ret;
}

/*
 * Unified CM handler for every urp-managed rdma_cm_id. Dispatches on
 * ctx->is_listener (listener: CONNECT_REQUEST only) vs per-QP cm_ids
 * (full address/route/establish/teardown lifecycle).
 */
static int urp_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *event)
{
	struct urp_cm_ctx *ctx = id->context;
	struct urp_endpoint *ep = ctx->ep;
	int ret = 0;

	pr_info("CM event: %s (%d) [%s qp=%u]\n",
		rdma_event_msg(event->event), event->event,
		ctx->is_listener ? "listener" : "qp", ctx->qp_index);

	if (ctx->is_listener) {
		if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
			return urp_cm_accept_one(id, ep,
					event->param.conn.private_data,
					event->param.conn.private_data_len);
		return 0;
	}

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		ret = rdma_resolve_route(id, 2000);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ret = urp_endpoint_setup_shared(ep, id->device);
		if (ret) {
			pr_err("setup_shared failed on qp %u: %d\n",
			       ctx->qp_index, ret);
			break;
		}

		ret = urp_qp_create_on_cm_id(ep, id, ctx->qp_index);
		if (ret) {
			pr_err("qp_create_on_cm_id failed on qp %u: %d\n",
			       ctx->qp_index, ret);
			break;
		}

		/* Recvs are pre-posted to ep->srq in setup_shared (Step 3). */

		/*
		 * Phase 5 Step 3: rdma_connect() acquires id->qp_mutex, but
		 * the rdma_cm framework already holds it while invoking this
		 * handler. Calling rdma_connect inline self-deadlocks the
		 * cma_work_handler kworker (caught by hung_task_check after
		 * 120 s; QP stays in INIT, no ESTABLISHED ever fires).
		 * Defer to a work item so the connect runs from a context
		 * where it can take the mutex.
		 */
		schedule_work(&ep->qps[ctx->qp_index].connect_work);
		ret = 0;
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		if (ep->qps && ctx->qp_index < ep->num_qps) {
			ep->qps[ctx->qp_index].established = true;
			/* Step 5: skip the probe-driven QUALIFYING grace
			 * period for now and go straight to ACTIVE. The
			 * miss-counter in probe_work_fn still demotes to
			 * DRAINING on >= URP_QP_MISS_THRESHOLD misses.
			 */
			ep->qps[ctx->qp_index].health = URP_QP_STATE_ACTIVE;
			if ((u32)atomic_inc_return(&ep->qps_connected) ==
			    ep->num_qps) {
				ep->connected = true;
				complete(&ep->cm_done);
				pr_info("all %u QPs established\n",
					ep->num_qps);
			}
		}
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		if (ep->qps && ctx->qp_index < ep->num_qps) {
			if (ep->qps[ctx->qp_index].established) {
				ep->qps[ctx->qp_index].established = false;
				atomic_dec(&ep->qps_connected);
				/*
				 * Acceptor: release the slot so the next
				 * CONNECT_REQUEST can reuse it (the old QP +
				 * cm_id stay until the next reuse or drain,
				 * since we can't safely destroy the cm_id from
				 * inside its own handler).
				 */
				if (!ep->is_initiator)
					atomic_dec(&ep->qps_accepted);
			}
		}
		/*
		 * Phase 4 Step 5: on the acceptor's primary QP, tear down
		 * the legacy ep->conn before the next test client reconnects.
		 * Without this, urp_cm_accept_one for the new client overwrites
		 * conn->uds_sock + conn->tx_thread, orphaning the previous
		 * pump kthread and leaking ~16 KB stack + struct socket per
		 * connect/disconnect cycle. The 1-hour soak found this -- the
		 * leak rate was ~210 kB/cycle (the orphan pump plus auxiliary
		 * state). Pre-fix: 12 urp-tx kthreads after ~30 churn cycles.
		 */
		if (!ep->is_initiator && ctx->qp_index == 0)
			urp_socket_conn_cleanup(ep);
		ep->cm_status = event->status;
		complete(&ep->cm_done);
		pr_info("QP %u CM down: %s\n", ctx->qp_index,
			rdma_event_msg(event->event));
		break;

	default:
		pr_info("unhandled CM event: %s\n",
			rdma_event_msg(event->event));
		break;
	}

	return ret;
}

/* ---- Init / cleanup ---- */

/*
 * Allocate a cm_ctx, create the rdma_cm_id with our handler, and stash
 * the ctx in id->context. On failure both the ctx and id are released.
 */
static int urp_make_cm_id(struct urp_endpoint *ep, u32 qp_index,
			  bool is_listener, struct rdma_cm_id **out)
{
	struct urp_cm_ctx *ctx;
	struct rdma_cm_id *id;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->ep = ep;
	ctx->qp_index = qp_index;
	ctx->is_listener = is_listener;

	id = rdma_create_id(&init_net, urp_cm_handler, ctx,
			    RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(id)) {
		kfree(ctx);
		return PTR_ERR(id);
	}

	*out = id;
	return 0;
}

int urp_rdma_init(struct urp_endpoint *ep, const char *peer_addr,
		  int peer_port, int bind_port, bool is_initiator)
{
	struct sockaddr_in addr;
	int ret;
	u32 i;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	if (is_initiator) {
		addr.sin_port = htons(peer_port);
		addr.sin_addr.s_addr = in_aton(peer_addr);

		for (i = 0; i < ep->num_qps; i++) {
			struct rdma_cm_id *id;

			ret = urp_make_cm_id(ep, i, false, &id);
			if (ret) {
				pr_err("cm_id %u create failed: %d\n",
				       i, ret);
				goto err_destroy_all;
			}

			ep->qps[i].cm_id = id;

			ret = rdma_resolve_addr(id, NULL,
						(struct sockaddr *)&addr, 2000);
			if (ret) {
				pr_err("rdma_resolve_addr[%u] failed: %d\n",
				       i, ret);
				goto err_destroy_all;
			}
		}
		return 0;
	}

	/* Acceptor: single listener; CONNECT_REQUESTs populate ep->qps[]. */
	addr.sin_port = htons(bind_port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	ret = urp_make_cm_id(ep, 0, true, &ep->listen_id);
	if (ret) {
		pr_err("listener cm_id create failed: %d\n", ret);
		return ret;
	}

	ret = rdma_bind_addr(ep->listen_id, (struct sockaddr *)&addr);
	if (ret) {
		pr_err("rdma_bind_addr failed: %d\n", ret);
		goto err_destroy_listen;
	}

	ret = rdma_listen(ep->listen_id, ep->num_qps);
	if (ret) {
		pr_err("rdma_listen failed: %d\n", ret);
		goto err_destroy_listen;
	}

	pr_info("RDMA listening on port %d for %u QPs\n",
		bind_port, ep->num_qps);
	return 0;

err_destroy_listen:
	urp_cm_id_destroy(&ep->listen_id);
	return ret;

err_destroy_all:
	for (i = 0; i < ep->num_qps; i++) {
		if (ep->qps[i].cm_id)
			urp_cm_id_destroy(&ep->qps[i].cm_id);
	}
	return ret;
}

void urp_rdma_cleanup(struct urp_endpoint *ep)
{
	struct ib_device *dev = ep->ib_dev;
	u32 i;

	/* Disconnect all established QPs. */
	if (ep->qps) {
		for (i = 0; i < ep->num_qps; i++) {
			if (ep->qps[i].established && ep->qps[i].cm_id) {
				rdma_disconnect(ep->qps[i].cm_id);
				ep->qps[i].established = false;
			}
		}

		/* Drain and destroy each QP. */
		for (i = 0; i < ep->num_qps; i++) {
			if (ep->qps[i].qp) {
				ib_drain_qp(ep->qps[i].qp);
				rdma_destroy_qp(ep->qps[i].cm_id);
				ep->qps[i].qp = NULL;
			}
		}

		/* Free per-QP cm_ids (and their cm_ctx). */
		for (i = 0; i < ep->num_qps; i++) {
			if (ep->qps[i].cm_id)
				urp_cm_id_destroy(&ep->qps[i].cm_id);
		}
	}

	ep->connected = false;

	/* Shared resources -- only present once any QP setup completed. */
	urp_srq_destroy(ep);
	if (ep->send_cq) {
		ib_free_cq(ep->send_cq);
		ep->send_cq = NULL;
	}
	if (ep->recv_cq) {
		ib_free_cq(ep->recv_cq);
		ep->recv_cq = NULL;
	}
	if (ep->pd && dev) {
		urp_bufs_cleanup(ep, dev);
		ib_dealloc_pd(ep->pd);
		ep->pd = NULL;
	}
	ep->ib_dev = NULL;

	if (ep->listen_id)
		urp_cm_id_destroy(&ep->listen_id);
}
