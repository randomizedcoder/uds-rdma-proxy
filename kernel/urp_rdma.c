// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) — RDMA connection management
 *
 * Phase k0: Single QP, simple free-list buffer pool, RDMA CM connection.
 *
 * Initiator: rdma_resolve_addr() -> rdma_connect() to peer.
 * Acceptor:  rdma_listen() -> rdma_accept() from peer.
 *
 * Buffer pool: URP_NUM_BUFS pages, DMA-mapped, split into send and recv pools.
 */

#include "urp.h"
#include <linux/inet.h>
#include <linux/ktime.h>

/* ---- Buffer pool ---- */

static int urp_bufs_init(struct urp_endpoint *ep, struct ib_device *dev)
{
	int i;

	INIT_LIST_HEAD(&ep->send_free);
	INIT_LIST_HEAD(&ep->recv_free);
	spin_lock_init(&ep->send_lock);
	spin_lock_init(&ep->recv_lock);

	for (i = 0; i < URP_NUM_BUFS; i++) {
		struct urp_buffer *buf = &ep->bufs[i];

		buf->index = i;
		buf->page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!buf->page) {
			pr_err("urp: alloc_page failed for buf %d\n", i);
			goto err;
		}

		buf->data = page_address(buf->page);
		buf->dma_addr = ib_dma_map_page(dev, buf->page, 0,
						 URP_BUF_SIZE, DMA_BIDIRECTIONAL);
		if (ib_dma_mapping_error(dev, buf->dma_addr)) {
			pr_err("urp: DMA map failed for buf %d\n", i);
			__free_page(buf->page);
			buf->page = NULL;
			goto err;
		}

		buf->sge.addr = buf->dma_addr;
		buf->sge.length = URP_BUF_SIZE;
		/* lkey set after PD creation */

		INIT_LIST_HEAD(&buf->list);

		/* Split: first half send, second half recv */
		if (i < URP_NUM_BUFS / 2)
			list_add_tail(&buf->list, &ep->send_free);
		else
			list_add_tail(&buf->list, &ep->recv_free);
	}

	return 0;

err:
	/* Clean up already-allocated buffers */
	for (i = i - 1; i >= 0; i--) {
		struct urp_buffer *buf = &ep->bufs[i];

		if (buf->page) {
			ib_dma_unmap_page(dev, buf->dma_addr,
					  URP_BUF_SIZE, DMA_BIDIRECTIONAL);
			__free_page(buf->page);
			buf->page = NULL;
		}
	}
	return -ENOMEM;
}

static void urp_bufs_cleanup(struct urp_endpoint *ep, struct ib_device *dev)
{
	int i;

	for (i = 0; i < URP_NUM_BUFS; i++) {
		struct urp_buffer *buf = &ep->bufs[i];

		if (!buf->page)
			continue;

		ib_dma_unmap_page(dev, buf->dma_addr,
				  URP_BUF_SIZE, DMA_BIDIRECTIONAL);
		__free_page(buf->page);
		buf->page = NULL;
	}
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

/* ---- RDMA post operations ---- */

/* Forward declaration for recv CQE callback */
static void urp_recv_done(struct ib_cq *cq, struct ib_wc *wc);

int urp_post_recv(struct urp_endpoint *ep, struct ib_qp *qp,
		  struct urp_buffer *buf)
{
	struct ib_recv_wr wr = {};
	const struct ib_recv_wr *bad_wr;

	buf->sge.length = URP_BUF_SIZE;
	buf->cqe.done = urp_recv_done;

	wr.wr_cqe = &buf->cqe;
	wr.sg_list = &buf->sge;
	wr.num_sge = 1;

	return ib_post_recv(qp, &wr, &bad_wr);
}

/*
 * Post up to @count recv buffers from the endpoint's pool to @qp's RQ.
 * Step 2b posts a fixed slice per QP at QP-setup time; Step 3 will move
 * this work to the shared SRQ.
 */
int urp_post_recv_for_qp(struct urp_endpoint *ep, struct ib_qp *qp, u32 count)
{
	u32 posted = 0;

	while (posted < count) {
		struct urp_buffer *buf = urp_buf_alloc_recv(ep);
		int ret;

		if (!buf)
			break;	/* pool exhausted -- not fatal */

		ret = urp_post_recv(ep, qp, buf);
		if (ret) {
			urp_buf_free_recv(ep, buf);
			pr_err("urp: post_recv failed: %d\n", ret);
			return ret;
		}
		posted++;
	}

	pr_info("urp: posted %u recv buffers to QP\n", posted);
	return 0;
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
		pr_err("urp: ib_alloc_pd failed: %d\n", ret);
		return ret;
	}

	ret = urp_bufs_init(ep, dev);
	if (ret)
		goto err_pd;

	for (i = 0; i < URP_NUM_BUFS; i++)
		ep->bufs[i].sge.lkey = ep->pd->local_dma_lkey;

	/*
	 * Shared CQs sized so completions don't back up under N QPs of
	 * sustained traffic. URP_CQ_ENTRIES (URP_NUM_BUFS * 2) is the
	 * single-QP figure that worked in k0; scale it by num_qps.
	 */
	cq_entries = URP_CQ_ENTRIES * ep->num_qps;

	ep->send_cq = ib_alloc_cq(dev, ep, cq_entries, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(ep->send_cq)) {
		ret = PTR_ERR(ep->send_cq);
		ep->send_cq = NULL;
		pr_err("urp: ib_alloc_cq (send) failed: %d\n", ret);
		goto err_bufs;
	}

	ep->recv_cq = ib_alloc_cq(dev, ep, cq_entries, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(ep->recv_cq)) {
		ret = PTR_ERR(ep->recv_cq);
		ep->recv_cq = NULL;
		pr_err("urp: ib_alloc_cq (recv) failed: %d\n", ret);
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
	attr.cap.max_send_wr = URP_SQ_DEPTH;
	attr.cap.max_recv_wr = 0;		/* recvs flow through SRQ */
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.qp_type = IB_QPT_RC;
	attr.sq_sig_type = IB_SIGNAL_ALL_WR;

	ret = rdma_create_qp(cm_id, ep->pd, &attr);
	if (ret) {
		pr_err("urp: rdma_create_qp[%u] failed: %d\n", qp_index, ret);
		return ret;
	}

	ep->qps[qp_index].qp = cm_id->qp;
	ep->qps[qp_index].cm_id = cm_id;
	return 0;
}

/*
 * Post the per-QP slice of recv buffers. Step 3 will replace this with
 * SRQ-based watermark refill; until then each QP owns roughly
 * (recv_pool_size / num_qps) buffers on its dedicated RQ.
 */
static u32 urp_recvs_per_qp(struct urp_endpoint *ep)
{
	u32 recv_pool = URP_NUM_BUFS / 2;
	u32 per_qp = recv_pool / ep->num_qps;

	return per_qp ? per_qp : 1;
}

/*
 * CQ completion callbacks — wired up after connection is established.
 * These are called from ib_poll_cq workqueue context.
 */
void urp_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_buffer *buf = container_of(wc->wr_cqe, struct urp_buffer, cqe);
	struct urp_endpoint *ep = cq->cq_context;

	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR)
		pr_err("urp: send completion error: %s (%d)\n",
		       ib_wc_status_msg(wc->status), wc->status);

	/*
	 * Always return the buffer to the pool, including on flush after QP
	 * destruction. Without this, every QP teardown permanently
	 * leaks outstanding send buffers and the pool eventually exhausts.
	 */
	urp_buf_free_send(ep, buf);
}

static void urp_recv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_buffer *buf = container_of(wc->wr_cqe, struct urp_buffer, cqe);
	struct urp_endpoint *ep = cq->cq_context;
	struct socket *uds = NULL;
	struct msghdr msg = {};
	struct kvec iov;
	u32 payload_len;
	u32 stream_id;
	u8 flags;
	int ret;

	if (wc->status != IB_WC_SUCCESS) {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			pr_err("urp: recv completion error: %s (%d)\n",
			       ib_wc_status_msg(wc->status), wc->status);
		/* Return buffer to pool so it can be reposted. */
		urp_buf_free_recv(ep, buf);
		return;
	}

	payload_len = urp_frame_decode_payload_len(buf->data);
	if (payload_len > URP_MAX_PAYLOAD) {
		pr_err("urp: received oversized frame: %u\n", payload_len);
		goto repost;
	}

	/*
	 * Step 4b: handle CONTROL/CREDIT frames -- peer is granting us
	 * additional send credits on the QP this completion came in on.
	 * Apply to the QP's credit state and we're done; no UDS delivery.
	 */
	if (urp_frame_decode_type(buf->data) == URP_FRAME_TYPE_CONTROL) {
		int qp_idx = urp_qp_index_of(ep, wc->qp);
		u16 grants = urp_frame_decode_credits(buf->data);
		u8 cflags = urp_frame_decode_flags(buf->data);

		if (qp_idx >= 0 && (cflags & URP_CTRL_FLAG_CREDIT))
			urp_credit_grant(&ep->qps[qp_idx].credit, grants);
		goto repost;
	}

	/*
	 * Phase 3b PROBE handling.
	 *   flags == 0                       -> PING (Step 3): emit PONG
	 *   flags has URP_PROBE_FLAG_PONG    -> PONG (Step 4): compute
	 *                                       RTT, update per-QP EWMA,
	 *                                       reset consecutive_misses
	 * In both cases the frame is not delivered to UDS.
	 */
	if (urp_frame_decode_type(buf->data) == URP_FRAME_TYPE_PROBE) {
		u8 pflags = urp_frame_decode_flags(buf->data);
		const void *payload = buf->data + URP_FRAME_HEADER_SIZE;

		if (pflags & URP_PROBE_FLAG_PONG) {
			int qp_idx = urp_qp_index_of(ep, wc->qp);

			if (qp_idx >= 0) {
				struct urp_qp *q = &ep->qps[qp_idx];
				u64 t_send_mono =
					urp_ping_decode_t_send_mono(payload);
				u64 now = ktime_get_ns();

				if (now > t_send_mono) {
					u64 rtt = now - t_send_mono;
					/*
					 * EWMA alpha = 0.2 in integer math:
					 *   new = old * 4/5 + rtt * 1/5
					 * First sample (rtt_ewma_ns == 0)
					 * seeds directly so we don't pull the
					 * baseline toward zero.
					 */
					if (q->rtt_ewma_ns == 0)
						q->rtt_ewma_ns = rtt;
					else
						q->rtt_ewma_ns =
							(q->rtt_ewma_ns * 4 +
							 rtt) / 5;
					/* Step 5: signal "PONG arrived for
					 * the outstanding PING" by clearing
					 * last_ping_ns; bump pong streak;
					 * reset misses. A future Qualifying
					 * promotion step will use
					 * consecutive_pongs >= 3 to lift
					 * the QP to ACTIVE. */
					q->last_ping_ns = 0;
					q->consecutive_misses = 0;
					q->consecutive_pongs++;
				}
			}
		} else {
			urp_emit_pong_on(ep, wc->qp, payload);
		}
		goto repost;
	}

	/*
	 * Phase 3a Step 7b: dispatch DATA frames by stream_id. stream_id
	 * == 0 is the k0/legacy single-connection path -- frames go to
	 * ep->conn. Non-zero stream_ids look up a per-stream UDS socket;
	 * SYN-flagged frames may auto-create the stream entry via
	 * urp_stream_rx_syn (Step 7c on the acceptor side then opens the
	 * per-stream UDS + starts that stream's TX pump).
	 */
	stream_id = urp_frame_decode_stream_id(buf->data);
	flags = urp_frame_decode_flags(buf->data);

	if (stream_id == 0) {
		if (ep->conn.active && ep->conn.uds_sock)
			uds = ep->conn.uds_sock;
	} else {
		struct urp_stream *s = NULL;

		rcu_read_lock();
		(void)urp_stream_rx_dispatch(ep, stream_id, flags, &s);
		if (s)
			uds = s->uds_sock;
		rcu_read_unlock();
	}

	if (!uds) {
		atomic64_inc(&ep->stats.buffer_alloc_fails);
		goto repost;
	}

	/* Forward payload to UDS socket */
	iov.iov_base = buf->data + URP_FRAME_HEADER_SIZE;
	iov.iov_len = payload_len;
	iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, payload_len);

	ret = kernel_sendmsg(uds, &msg, &iov, 1, payload_len);
	if (ret < 0) {
		pr_err("urp: kernel_sendmsg failed: %d\n", ret);
	} else {
		int qp_idx;

		atomic64_add(payload_len, &ep->stats.rx_bytes);
		atomic64_inc(&ep->stats.rx_frames);
		/* Step 8: per-QP RX counters. wc->qp identifies the source
		 * QP; linear scan is fine for the supported num_qps range. */
		qp_idx = urp_qp_index_of(ep, wc->qp);
		if (qp_idx >= 0) {
			struct urp_qp *qps = &ep->qps[qp_idx];

			atomic64_add(payload_len, &qps->rx_bytes);
			atomic64_inc(&qps->rx_frames);

			/*
			 * Step 4b: record_recv + threshold-driven CREDIT
			 * frame emission. Peer sends DATA -> we count a
			 * pending grant; once accumulated grants reach
			 * threshold (initial_credits / 4) we emit a
			 * CONTROL/CREDIT frame back to peer carrying the
			 * granted count. Peers that don't speak credit
			 * (the userspace test client) ignore the CREDIT
			 * frame; URP-to-URP peers consume it via the
			 * URP_FRAME_TYPE_CONTROL branch above.
			 */
			urp_credit_record_recv(&qps->credit);
			/*
			 * Only emit CREDIT frames toward peers that speak
			 * the multi-stream protocol (non-zero stream_id).
			 * Legacy stream_id == 0 traffic (userspace
			 * urp-test-client and other non-URP peers) doesn't
			 * expect them and posts only as many recv WRs as
			 * its echo logic needs; an unsolicited CREDIT frame
			 * there causes RNR on the peer.
			 */
			if (stream_id != 0 &&
			    urp_credit_should_grant(&qps->credit)) {
				u16 grants = urp_credit_take_grants(&qps->credit);

				urp_emit_credit_frame(ep, qps, stream_id,
						      grants);
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
			pr_warn("urp: rejecting CONNECT_REQUEST with bad/missing PSK\n");
			rdma_reject(child, NULL, 0, 0);
			/* Step 9: multicast event so `urp monitor` users see
			 * auth failures alongside state transitions. */
			urp_send_event(ep);
			return 0;
		}
	}

	qp_index = (u32)atomic_inc_return(&ep->qps_accepted) - 1;
	if (qp_index >= ep->num_qps) {
		atomic_dec(&ep->qps_accepted);
		pr_warn("urp: rejecting extra CONNECT_REQUEST (%u >= %u QPs)\n",
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
		struct urp_cm_ctx *old_ctx = ep->qps[qp_index].cm_id->context;

		if (ep->qps[qp_index].qp) {
			ib_drain_qp(ep->qps[qp_index].qp);
			rdma_destroy_qp(ep->qps[qp_index].cm_id);
			ep->qps[qp_index].qp = NULL;
		}
		rdma_destroy_id(ep->qps[qp_index].cm_id);
		kfree(old_ctx);
		ep->qps[qp_index].cm_id = NULL;
		ep->qps[qp_index].established = false;
	}

	child_ctx = kzalloc(sizeof(*child_ctx), GFP_KERNEL);
	if (!child_ctx)
		return -ENOMEM;
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
	 */
	if (qp_index == 0 && ep->connect_path[0]) {
		ret = urp_connect_uds(ep, ep->connect_path);
		if (!ret)
			ret = urp_pump_start(ep);
		if (ret) {
			pr_err("urp: acceptor data path setup failed: %d\n", ret);
			goto err_destroy_qp;
		}
	}

	/* Recv buffers are pre-posted to ep->srq inside
	 * urp_endpoint_setup_shared; no per-QP RQ to fill (Step 3). */

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

	pr_info("urp: CM event: %s (%d) [%s qp=%u]\n",
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
		if (ret)
			break;

		ret = urp_qp_create_on_cm_id(ep, id, ctx->qp_index);
		if (ret)
			break;

		/* Recvs are pre-posted to ep->srq in setup_shared (Step 3). */

		{
			struct rdma_conn_param param = {};

			param.responder_resources = 1;
			param.initiator_depth = 1;
			param.retry_count = 7;
			param.rnr_retry_count = 7;
			/*
			 * Phase 3b Step 8: include PSK auth payload in
			 * private_data when configured. Acceptor compares
			 * the hash against its own and rdma_reject's on
			 * mismatch.
			 */
			if (ep->has_password) {
				param.private_data = ep->auth_priv;
				param.private_data_len = sizeof(ep->auth_priv);
			}
			ret = rdma_connect(id, &param);
		}
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		if (ep->qps && ctx->qp_index < ep->num_qps) {
			ep->qps[ctx->qp_index].established = true;
			/* Step 5: skip the probe-driven QUALIFYING grace
			 * period for now and go straight to ACTIVE. The
			 * miss-counter in probe_work_fn still demotes to
			 * DRAINING on >= URP_QP_MISS_THRESHOLD misses. */
			ep->qps[ctx->qp_index].health = URP_QP_STATE_ACTIVE;
			if ((u32)atomic_inc_return(&ep->qps_connected) ==
			    ep->num_qps) {
				ep->connected = true;
				complete(&ep->cm_done);
				pr_info("urp: all %u QPs established\n",
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
		ep->cm_status = event->status;
		complete(&ep->cm_done);
		pr_info("urp: QP %u CM down: %s\n", ctx->qp_index,
			rdma_event_msg(event->event));
		break;

	default:
		pr_info("urp: unhandled CM event: %s\n",
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
				pr_err("urp: cm_id %u create failed: %d\n",
				       i, ret);
				goto err_destroy_all;
			}

			ep->qps[i].cm_id = id;

			ret = rdma_resolve_addr(id, NULL,
						(struct sockaddr *)&addr, 2000);
			if (ret) {
				pr_err("urp: rdma_resolve_addr[%u] failed: %d\n",
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
		pr_err("urp: listener cm_id create failed: %d\n", ret);
		return ret;
	}

	ret = rdma_bind_addr(ep->listen_id, (struct sockaddr *)&addr);
	if (ret) {
		pr_err("urp: rdma_bind_addr failed: %d\n", ret);
		goto err_destroy_listen;
	}

	ret = rdma_listen(ep->listen_id, ep->num_qps);
	if (ret) {
		pr_err("urp: rdma_listen failed: %d\n", ret);
		goto err_destroy_listen;
	}

	pr_info("urp: RDMA listening on port %d for %u QPs\n",
		bind_port, ep->num_qps);
	return 0;

err_destroy_listen:
	{
		struct urp_cm_ctx *ctx = ep->listen_id->context;

		rdma_destroy_id(ep->listen_id);
		kfree(ctx);
		ep->listen_id = NULL;
	}
	return ret;

err_destroy_all:
	for (i = 0; i < ep->num_qps; i++) {
		if (ep->qps[i].cm_id) {
			struct urp_cm_ctx *ctx = ep->qps[i].cm_id->context;

			rdma_destroy_id(ep->qps[i].cm_id);
			kfree(ctx);
			ep->qps[i].cm_id = NULL;
		}
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
			if (ep->qps[i].cm_id) {
				struct urp_cm_ctx *ctx = ep->qps[i].cm_id->context;

				rdma_destroy_id(ep->qps[i].cm_id);
				kfree(ctx);
				ep->qps[i].cm_id = NULL;
			}
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

	if (ep->listen_id) {
		struct urp_cm_ctx *ctx = ep->listen_id->context;

		rdma_destroy_id(ep->listen_id);
		kfree(ctx);
		ep->listen_id = NULL;
	}
}
