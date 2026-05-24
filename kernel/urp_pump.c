// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) — TX/RX data pump
 *
 * Phase k0: Simple bidirectional pump.
 *
 * TX kthread: kernel_recvmsg() from UDS -> frame_encode -> ib_post_send()
 * RX: CQ completion callback -> frame_decode -> kernel_sendmsg() to UDS
 *      (RX is handled inline in urp_recv_done in urp_rdma.c)
 *
 * No reorder buffer, no credits, no multiplexing.
 */

#include "urp.h"
#include <linux/sched.h>

/*
 * TX pump kthread.
 *
 * Reads data from the UDS socket, wraps it in a frame header, and posts
 * it for RDMA send. Runs until the connection is shut down.
 */
static int urp_tx_thread_fn(void *data)
{
	struct urp_endpoint *ep = data;
	struct urp_connection *conn = &ep->conn;

	while (!kthread_should_stop() && conn->active) {
		struct urp_buffer *buf;
		struct urp_qp *qps;
		struct msghdr msg = {};
		struct kvec iov;
		int ret;
		u32 len;

		buf = urp_buf_alloc_send(ep);
		if (!buf) {
			/* No send buffers available, back off briefly */
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		/* Read from UDS into the buffer payload area (after header) */
		iov.iov_base = buf->data + URP_FRAME_HEADER_SIZE;
		iov.iov_len = URP_MAX_PAYLOAD;

		ret = kernel_recvmsg(conn->uds_sock, &msg, &iov, 1,
				     URP_MAX_PAYLOAD, 0);
		if (ret <= 0) {
			urp_buf_free_send(ep, buf);
			if (ret == 0) {
				pr_info("urp: UDS connection closed by peer\n");
				conn->active = false;
			} else if (ret != -ERESTARTSYS && ret != -EAGAIN) {
				pr_err("urp: kernel_recvmsg failed: %d\n", ret);
				conn->active = false;
			}
			break;
		}

		len = ret;

		/* Encode frame header */
		urp_frame_encode(buf->data,
				 0,		/* stream_id: k0 uses 0 */
				 conn->seq++,	/* sequence number */
				 URP_FRAME_TYPE_DATA,
				 0,		/* flags: no SYN/FIN/RST for k0 */
				 0,		/* credits: not used in k0 */
				 len);

		/* DMA sync for send */
		ib_dma_sync_single_for_device(ep->ib_dev,
					      buf->dma_addr,
					      URP_FRAME_HEADER_SIZE + len,
					      DMA_TO_DEVICE);

		/* Select a QP for this frame (round-robin across all
		 * connected QPs). With num_qps=1 this always picks qps[0];
		 * the abstraction exists so Step 2b's multi-cm-id work and
		 * Step 4's per-QP credit gate can plug in here without
		 * touching the pump.
		 */
		qps = urp_qp_select_round_robin(ep);
		if (!qps) {
			urp_buf_free_send(ep, buf);
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		/* Post RDMA send */
		{
			struct ib_send_wr wr = {};
			const struct ib_send_wr *bad_wr;

			buf->sge.length = URP_FRAME_HEADER_SIZE + len;
			buf->cqe.done = urp_send_done;

			wr.wr_cqe = &buf->cqe;
			wr.sg_list = &buf->sge;
			wr.num_sge = 1;
			wr.opcode = IB_WR_SEND;
			wr.send_flags = IB_SEND_SIGNALED;

			ret = ib_post_send(qps->qp, &wr, &bad_wr);
			if (ret) {
				pr_err("urp: ib_post_send failed: %d\n", ret);
				urp_buf_free_send(ep, buf);
				conn->active = false;
				break;
			}
		}

		atomic64_add(len, &ep->stats.tx_bytes);
		atomic64_inc(&ep->stats.tx_frames);
		/* Step 8: per-QP counters surfaced via GENL */
		atomic64_add(len, &qps->tx_bytes);
		atomic64_inc(&qps->tx_frames);
	}

	pr_info("urp: TX pump stopped\n");
	return 0;
}

int urp_pump_start(struct urp_endpoint *ep)
{
	struct urp_connection *conn = &ep->conn;

	conn->tx_thread = kthread_run(urp_tx_thread_fn, ep, "urp-tx");
	if (IS_ERR(conn->tx_thread)) {
		int ret = PTR_ERR(conn->tx_thread);

		conn->tx_thread = NULL;
		pr_err("urp: kthread_run (tx) failed: %d\n", ret);
		return ret;
	}

	pr_info("urp: pump started\n");
	return 0;
}

void urp_pump_stop(struct urp_endpoint *ep)
{
	struct urp_connection *conn = &ep->conn;

	conn->active = false;

	if (conn->tx_thread) {
		kthread_stop(conn->tx_thread);
		conn->tx_thread = NULL;
	}

	pr_info("urp: pump stopped\n");
}

/*
 * Per-stream TX kthread (Phase 3a Step 7c).
 *
 * Reads from the stream's UDS socket, encodes a frame with the
 * stream's id + tx_seq, sends via the endpoint's shared QP set
 * (round-robin). Mirrors urp_tx_thread_fn but pulls per-stream
 * sequence numbers + sets the SYN flag on the very first frame.
 */
static int urp_stream_tx_fn(void *data)
{
	struct urp_stream *stream = data;
	struct urp_endpoint *ep = stream->ep;
	bool send_syn = true;

	while (!kthread_should_stop()) {
		struct urp_buffer *buf;
		struct urp_qp *qps;
		struct msghdr msg = {};
		struct kvec iov;
		u8 flags = 0;
		int ret;
		u32 len;

		if (!stream->uds_sock)
			break;

		buf = urp_buf_alloc_send(ep);
		if (!buf) {
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		iov.iov_base = buf->data + URP_FRAME_HEADER_SIZE;
		iov.iov_len = URP_MAX_PAYLOAD;

		ret = kernel_recvmsg(stream->uds_sock, &msg, &iov, 1,
				     URP_MAX_PAYLOAD, 0);
		if (ret <= 0) {
			urp_buf_free_send(ep, buf);
			if (ret == 0)
				pr_info("urp: stream %u UDS closed by peer\n",
					stream->id);
			else if (ret != -ERESTARTSYS && ret != -EAGAIN)
				pr_err("urp: stream %u recvmsg failed: %d\n",
				       stream->id, ret);
			/* The stream goes to FIN_WAIT; full teardown happens
			 * in the drain path. */
			urp_stream_tx_fin(stream);
			break;
		}

		len = ret;

		if (send_syn) {
			flags |= URP_DATA_FLAG_SYN;
			send_syn = false;
		}

		urp_frame_encode(buf->data, stream->id, stream->tx_seq++,
				 URP_FRAME_TYPE_DATA, flags, 0, len);

		ib_dma_sync_single_for_device(ep->ib_dev, buf->dma_addr,
					      URP_FRAME_HEADER_SIZE + len,
					      DMA_TO_DEVICE);

		qps = urp_qp_select_round_robin(ep);
		if (!qps) {
			urp_buf_free_send(ep, buf);
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		{
			struct ib_send_wr wr = {};
			const struct ib_send_wr *bad_wr;

			buf->sge.length = URP_FRAME_HEADER_SIZE + len;
			buf->cqe.done = urp_send_done;
			wr.wr_cqe = &buf->cqe;
			wr.sg_list = &buf->sge;
			wr.num_sge = 1;
			wr.opcode = IB_WR_SEND;
			wr.send_flags = IB_SEND_SIGNALED;

			ret = ib_post_send(qps->qp, &wr, &bad_wr);
			if (ret) {
				pr_err("urp: stream %u ib_post_send failed: %d\n",
				       stream->id, ret);
				urp_buf_free_send(ep, buf);
				break;
			}
		}

		atomic64_add(len, &ep->stats.tx_bytes);
		atomic64_inc(&ep->stats.tx_frames);
		atomic64_add(len, &qps->tx_bytes);
		atomic64_inc(&qps->tx_frames);
	}

	pr_info("urp: stream %u TX pump stopped\n", stream->id);
	return 0;
}

int urp_stream_pump_start(struct urp_stream *stream)
{
	if (stream->tx_thread)
		return 0;
	stream->tx_thread = kthread_run(urp_stream_tx_fn, stream,
					"urp-tx-s%u", stream->id);
	if (IS_ERR(stream->tx_thread)) {
		int ret = PTR_ERR(stream->tx_thread);

		stream->tx_thread = NULL;
		pr_err("urp: stream %u kthread_run failed: %d\n",
		       stream->id, ret);
		return ret;
	}
	return 0;
}

void urp_stream_pump_stop(struct urp_stream *stream)
{
	if (stream->tx_thread) {
		kthread_stop(stream->tx_thread);
		stream->tx_thread = NULL;
	}
}
