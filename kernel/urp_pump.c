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
#include <linux/ktime.h>
#include <linux/timekeeping.h>

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

		/*
		 * Step 4b: per-QP credit consume. Best-effort -- if the
		 * peer hasn't replenished us yet (no CREDIT frame
		 * received), bump the stall counter and proceed anyway.
		 * The RC layer's rnr_retry handles transient overshoot;
		 * the credit accounting still tracks the imbalance so
		 * `urp show` reports stalls / send_credits realistically.
		 */
		if (urp_credit_consume(&qps->credit) == -EAGAIN)
			atomic64_inc(&ep->stats.credit_stalls);

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

		/* Step 4b: per-stream credit consume (best-effort, same
		 * rules as the legacy ep->conn pump above). */
		if (urp_credit_consume(&stream->credit) == -EAGAIN)
			atomic64_inc(&ep->stats.credit_stalls);

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

/*
 * Emit a CONTROL/CREDIT frame to peer carrying @grants credits in the
 * frame header. Phase 3a Step 4b. The frame has no payload; flags is
 * URP_CTRL_FLAG_CREDIT, frame_type is URP_FRAME_TYPE_CONTROL,
 * credits_granted is the granted amount. Stream id mirrors the QP the
 * grant applies to (0 if endpoint-level legacy compat path).
 *
 * Best-effort: on no-buffer / no-QP we drop -- the peer can re-derive
 * credit state from its own send counter (the protocol tolerates
 * dropped CREDIT frames).
 */
/*
 * Phase 3b Step 2: probe emission. The per-endpoint delayed_work
 * fires every URP_PROBE_INTERVAL_MS (250ms) and posts one PING on
 * every established QP. Probes are gated on num_qps > 1 so the
 * single-QP test-client scenario (which expects only echo traffic
 * back) is unaffected; Phase 3b Step 4 will compute RTT when the
 * matching PONG arrives.
 */
#define URP_PROBE_INTERVAL_MS	250

static int urp_emit_ping_on(struct urp_endpoint *ep, struct urp_qp *qps)
{
	struct urp_buffer *buf;
	struct ib_send_wr wr = {};
	const struct ib_send_wr *bad_wr;
	u64 now_mono, now_real;
	int ret;

	if (!qps->established || !qps->qp)
		return 0;

	buf = urp_buf_alloc_send(ep);
	if (!buf)
		return -ENOBUFS;

	now_mono = ktime_get_ns();
	now_real = ktime_get_real_ns();
	qps->probe_seq++;
	qps->last_ping_ns = now_mono;

	urp_frame_encode(buf->data, 0, qps->probe_seq, URP_FRAME_TYPE_PROBE,
			 0 /* flags == 0 means PING */, 0,
			 URP_PING_PAYLOAD_SIZE);
	urp_ping_encode(buf->data + URP_FRAME_HEADER_SIZE, qps->probe_seq,
			(u16)qps->index, now_mono, now_real);

	ib_dma_sync_single_for_device(ep->ib_dev, buf->dma_addr,
				      URP_FRAME_HEADER_SIZE +
					      URP_PING_PAYLOAD_SIZE,
				      DMA_TO_DEVICE);

	buf->sge.length = URP_FRAME_HEADER_SIZE + URP_PING_PAYLOAD_SIZE;
	buf->cqe.done = urp_send_done;
	wr.wr_cqe = &buf->cqe;
	wr.sg_list = &buf->sge;
	wr.num_sge = 1;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	ret = ib_post_send(qps->qp, &wr, &bad_wr);
	if (ret) {
		pr_warn_ratelimited("urp: PING post_send on qp %u failed: %d\n",
				    qps->index, ret);
		urp_buf_free_send(ep, buf);
		return ret;
	}
	return 0;
}

static void urp_probe_work_fn(struct work_struct *work)
{
	struct urp_endpoint *ep = container_of(to_delayed_work(work),
					       struct urp_endpoint, probe_work);
	u32 i;

	if (!ep->probe_active || ep->state != URP_STATE_ACTIVE)
		return;

	if (ep->qps && ep->num_qps > 1) {
		for (i = 0; i < ep->num_qps; i++)
			urp_emit_ping_on(ep, &ep->qps[i]);
	}

	schedule_delayed_work(&ep->probe_work,
			      msecs_to_jiffies(URP_PROBE_INTERVAL_MS));
}

void urp_probe_work_start(struct urp_endpoint *ep)
{
	INIT_DELAYED_WORK(&ep->probe_work, urp_probe_work_fn);
	ep->probe_active = true;
	schedule_delayed_work(&ep->probe_work,
			      msecs_to_jiffies(URP_PROBE_INTERVAL_MS));
}

void urp_probe_work_stop(struct urp_endpoint *ep)
{
	if (ep->probe_active) {
		ep->probe_active = false;
		cancel_delayed_work_sync(&ep->probe_work);
	}
}

/*
 * Phase 3b Step 3: PONG emission. Called from the recv-completion
 * handler when a PING frame arrives on @qps. Echoes the PING fields
 * and appends our timestamps so the peer can compute RTT.
 * Best-effort: drops the PONG silently on buffer / post failure.
 */
int urp_emit_pong_on(struct urp_endpoint *ep, struct ib_qp *qp,
		     const void *ping_payload)
{
	struct urp_buffer *buf;
	struct ib_send_wr wr = {};
	const struct ib_send_wr *bad_wr;
	u64 t_recv_real, t_pong_mono, t_pong_real;
	int ret;

	if (!qp)
		return -EINVAL;

	buf = urp_buf_alloc_send(ep);
	if (!buf)
		return -ENOBUFS;

	t_recv_real = ktime_get_real_ns();
	t_pong_mono = ktime_get_ns();
	t_pong_real = t_recv_real;	/* "now" -- same wall-clock instant */

	urp_frame_encode(buf->data, 0, 0, URP_FRAME_TYPE_PROBE,
			 URP_PROBE_FLAG_PONG, 0, URP_PONG_PAYLOAD_SIZE);
	urp_pong_encode(buf->data + URP_FRAME_HEADER_SIZE, ping_payload,
			t_recv_real, t_pong_mono, t_pong_real);

	ib_dma_sync_single_for_device(ep->ib_dev, buf->dma_addr,
				      URP_FRAME_HEADER_SIZE +
					      URP_PONG_PAYLOAD_SIZE,
				      DMA_TO_DEVICE);

	buf->sge.length = URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE;
	buf->cqe.done = urp_send_done;
	wr.wr_cqe = &buf->cqe;
	wr.sg_list = &buf->sge;
	wr.num_sge = 1;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	ret = ib_post_send(qp, &wr, &bad_wr);
	if (ret) {
		pr_warn_ratelimited("urp: PONG post_send failed: %d\n", ret);
		urp_buf_free_send(ep, buf);
		return ret;
	}
	return 0;
}

int urp_emit_credit_frame(struct urp_endpoint *ep, struct urp_qp *qps,
			  u32 stream_id, u16 grants)
{
	struct urp_buffer *buf;
	struct ib_send_wr wr = {};
	const struct ib_send_wr *bad_wr;
	int ret;

	if (!qps || !qps->qp || grants == 0)
		return 0;

	buf = urp_buf_alloc_send(ep);
	if (!buf) {
		atomic64_inc(&ep->stats.credit_stalls);
		return -ENOBUFS;
	}

	urp_frame_encode(buf->data, stream_id, 0, URP_FRAME_TYPE_CONTROL,
			 URP_CTRL_FLAG_CREDIT, grants, 0);
	ib_dma_sync_single_for_device(ep->ib_dev, buf->dma_addr,
				      URP_FRAME_HEADER_SIZE, DMA_TO_DEVICE);

	buf->sge.length = URP_FRAME_HEADER_SIZE;
	buf->cqe.done = urp_send_done;
	wr.wr_cqe = &buf->cqe;
	wr.sg_list = &buf->sge;
	wr.num_sge = 1;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	ret = ib_post_send(qps->qp, &wr, &bad_wr);
	if (ret) {
		pr_warn("urp: CREDIT frame post_send failed: %d\n", ret);
		urp_buf_free_send(ep, buf);
		return ret;
	}
	return 0;
}
