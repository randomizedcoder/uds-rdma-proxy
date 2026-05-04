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

int urp_post_recv(struct urp_endpoint *ep, struct urp_buffer *buf)
{
	struct ib_recv_wr wr = {};
	const struct ib_recv_wr *bad_wr;

	buf->sge.length = URP_BUF_SIZE;
	buf->cqe.done = urp_recv_done;

	wr.wr_cqe = &buf->cqe;
	wr.sg_list = &buf->sge;
	wr.num_sge = 1;

	return ib_post_recv(ep->qp, &wr, &bad_wr);
}

int urp_post_recv_all(struct urp_endpoint *ep)
{
	struct urp_buffer *buf;
	int count = 0;
	int ret;

	while ((buf = urp_buf_alloc_recv(ep)) != NULL) {
		ret = urp_post_recv(ep, buf);
		if (ret) {
			urp_buf_free_recv(ep, buf);
			pr_err("urp: post_recv failed: %d\n", ret);
			return ret;
		}
		count++;
	}

	pr_info("urp: posted %d recv buffers\n", count);
	return 0;
}

/* ---- QP creation ---- */

static int urp_create_qp(struct urp_endpoint *ep)
{
	struct ib_qp_init_attr attr = {};
	int ret;

	ep->send_cq = ib_alloc_cq(ep->cm_id->device, ep,
				   URP_CQ_ENTRIES, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(ep->send_cq)) {
		pr_err("urp: ib_alloc_cq (send) failed\n");
		return PTR_ERR(ep->send_cq);
	}

	ep->recv_cq = ib_alloc_cq(ep->cm_id->device, ep,
				   URP_CQ_ENTRIES, 0, IB_POLL_WORKQUEUE);
	if (IS_ERR(ep->recv_cq)) {
		pr_err("urp: ib_alloc_cq (recv) failed\n");
		ib_free_cq(ep->send_cq);
		return PTR_ERR(ep->recv_cq);
	}

	attr.event_handler = NULL; /* k0: no QP event handling */
	attr.send_cq = ep->send_cq;
	attr.recv_cq = ep->recv_cq;
	attr.cap.max_send_wr = URP_SQ_DEPTH;
	attr.cap.max_recv_wr = URP_RQ_DEPTH;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.qp_type = IB_QPT_RC;
	attr.sq_sig_type = IB_SIGNAL_ALL_WR;

	ret = rdma_create_qp(ep->cm_id, ep->pd, &attr);
	if (!ret)
		ep->qp = ep->cm_id->qp;
	return ret;
}

/* ---- RDMA CM event handling ---- */

static int urp_cm_setup_connection(struct urp_endpoint *ep)
{
	struct ib_device *dev = ep->cm_id->device;
	int i, ret;

	ep->pd = ib_alloc_pd(dev, 0);
	if (IS_ERR(ep->pd)) {
		pr_err("urp: ib_alloc_pd failed\n");
		return PTR_ERR(ep->pd);
	}

	ret = urp_bufs_init(ep, dev);
	if (ret)
		goto err_pd;

	/* Set lkey on all buffers now that PD exists */
	for (i = 0; i < URP_NUM_BUFS; i++)
		ep->bufs[i].sge.lkey = ep->pd->local_dma_lkey;

	ret = urp_create_qp(ep);
	if (ret)
		goto err_bufs;

	ret = urp_post_recv_all(ep);
	if (ret)
		goto err_qp;

	return 0;

err_qp:
	rdma_destroy_qp(ep->cm_id);
	ib_free_cq(ep->recv_cq);
	ib_free_cq(ep->send_cq);
err_bufs:
	urp_bufs_cleanup(ep, dev);
err_pd:
	ib_dealloc_pd(ep->pd);
	return ret;
}

/*
 * CQ completion callbacks — wired up after connection is established.
 * These are called from ib_poll_cq workqueue context.
 */
void urp_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_buffer *buf = container_of(wc->wr_cqe, struct urp_buffer, cqe);
	struct urp_endpoint *ep = cq->cq_context;

	if (wc->status != IB_WC_SUCCESS) {
		pr_err("urp: send completion error: %s (%d)\n",
		       ib_wc_status_msg(wc->status), wc->status);
		return;
	}

	urp_buf_free_send(ep, buf);
}

static void urp_recv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_buffer *buf = container_of(wc->wr_cqe, struct urp_buffer, cqe);
	struct urp_endpoint *ep = cq->cq_context;
	struct msghdr msg = {};
	struct kvec iov;
	u32 payload_len;
	int ret;

	if (wc->status != IB_WC_SUCCESS) {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			pr_err("urp: recv completion error: %s (%d)\n",
			       ib_wc_status_msg(wc->status), wc->status);
		return;
	}

	payload_len = urp_frame_decode_payload_len(buf->data);
	if (payload_len > URP_MAX_PAYLOAD) {
		pr_err("urp: received oversized frame: %u\n", payload_len);
		goto repost;
	}

	if (!ep->conn.active || !ep->conn.uds_sock)
		goto repost;

	/* Forward payload to UDS socket */
	iov.iov_base = buf->data + URP_FRAME_HEADER_SIZE;
	iov.iov_len = payload_len;
	iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, payload_len);

	ret = kernel_sendmsg(ep->conn.uds_sock, &msg, &iov, 1, payload_len);
	if (ret < 0) {
		pr_err("urp: kernel_sendmsg failed: %d\n", ret);
	} else {
		atomic64_add(payload_len, &ep->stats.rx_bytes);
		atomic64_inc(&ep->stats.rx_frames);
	}

repost:
	/* Return buffer to receive pool and repost */
	urp_buf_free_recv(ep, buf);
	buf = urp_buf_alloc_recv(ep);
	if (buf) {
		ret = urp_post_recv(ep, buf);
		if (ret)
			urp_buf_free_recv(ep, buf);
	}
}

static int urp_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *event)
{
	struct urp_endpoint *ep = id->context;
	int ret = 0;

	pr_info("urp: CM event: %s (%d)\n",
		rdma_event_msg(event->event), event->event);

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		ret = rdma_resolve_route(id, 2000);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ret = urp_cm_setup_connection(ep);
		if (ret)
			break;

		{
			struct rdma_conn_param param = {};

			param.responder_resources = 1;
			param.initiator_depth = 1;
			param.retry_count = 7;
			param.rnr_retry_count = 7;
			ret = rdma_connect(id, &param);
		}
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		ep->connected = true;
		complete(&ep->cm_done);
		pr_info("urp: RDMA connection established\n");
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		/* Acceptor side: save listener, switch to child CM ID */
		ep->listen_id = ep->cm_id;
		ep->cm_id = id;
		id->context = ep;

		ret = urp_cm_setup_connection(ep);
		if (ret)
			break;

		/* Connect UDS and start pump BEFORE rdma_accept.
		 * rdma_accept transitions QP to RTR/RTS, enabling data flow.
		 * If we delay UDS setup, recv completions race ahead and
		 * urp_recv_done drops data (conn.active is still false).
		 * The TX pump blocks on kernel_recvmsg until echo data arrives,
		 * so it won't attempt ib_post_send before the QP is ready. */
		if (ep->connect_path[0]) {
			ret = urp_connect_uds(ep, ep->connect_path);
			if (!ret)
				ret = urp_pump_start(ep);
			if (ret) {
				pr_err("urp: acceptor data path setup failed: %d\n", ret);
				break;
			}
		}

		{
			struct rdma_conn_param param = {};

			param.responder_resources = 1;
			param.initiator_depth = 1;
			param.rnr_retry_count = 7;
			ret = rdma_accept(id, &param);
		}
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		ep->connected = false;
		ep->cm_status = event->status;
		complete(&ep->cm_done);
		pr_info("urp: RDMA connection down: %s\n",
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

int urp_rdma_init(struct urp_endpoint *ep, const char *peer_addr,
		  int peer_port, int bind_port, bool is_initiator)
{
	struct sockaddr_in addr;
	int ret;

	ep->cm_id = rdma_create_id(&init_net, urp_cm_handler, ep,
				   RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(ep->cm_id)) {
		pr_err("urp: rdma_create_id failed\n");
		return PTR_ERR(ep->cm_id);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	if (is_initiator) {
		/* Initiator: resolve peer address and connect */
		addr.sin_port = htons(peer_port);
		addr.sin_addr.s_addr = in_aton(peer_addr);

		ret = rdma_resolve_addr(ep->cm_id, NULL,
					(struct sockaddr *)&addr, 2000);
		if (ret) {
			pr_err("urp: rdma_resolve_addr failed: %d\n", ret);
			goto err_destroy;
		}
	} else {
		/* Acceptor: bind and listen for incoming RDMA connections */
		addr.sin_port = htons(bind_port);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);

		ret = rdma_bind_addr(ep->cm_id, (struct sockaddr *)&addr);
		if (ret) {
			pr_err("urp: rdma_bind_addr failed: %d\n", ret);
			goto err_destroy;
		}

		ret = rdma_listen(ep->cm_id, 1);
		if (ret) {
			pr_err("urp: rdma_listen failed: %d\n", ret);
			goto err_destroy;
		}

		pr_info("urp: RDMA listening on port %d\n", bind_port);
	}

	return 0;

err_destroy:
	rdma_destroy_id(ep->cm_id);
	ep->cm_id = NULL;
	return ret;
}

void urp_rdma_cleanup(struct urp_endpoint *ep)
{
	if (!ep->cm_id)
		return;

	/* Disconnect and drain outstanding work before freeing resources */
	if (ep->connected) {
		rdma_disconnect(ep->cm_id);
		ep->connected = false;
	}

	if (ep->qp) {
		ib_drain_qp(ep->qp);
		rdma_destroy_qp(ep->cm_id);
		ep->qp = NULL;
	}

	if (ep->send_cq) {
		ib_free_cq(ep->send_cq);
		ep->send_cq = NULL;
	}
	if (ep->recv_cq) {
		ib_free_cq(ep->recv_cq);
		ep->recv_cq = NULL;
	}

	if (ep->pd) {
		struct ib_device *dev = ep->cm_id->device;

		urp_bufs_cleanup(ep, dev);
		ib_dealloc_pd(ep->pd);
		ep->pd = NULL;
	}

	rdma_destroy_id(ep->cm_id);
	ep->cm_id = NULL;

	if (ep->listen_id) {
		rdma_destroy_id(ep->listen_id);
		ep->listen_id = NULL;
	}
}
