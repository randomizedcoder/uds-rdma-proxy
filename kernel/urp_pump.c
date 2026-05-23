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
		ib_dma_sync_single_for_device(ep->cm_id->device,
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
