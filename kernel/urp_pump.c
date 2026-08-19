// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- TX/RX data pump
 *
 * Phase k0: Simple bidirectional pump.
 *
 * TX kthread: kernel_recvmsg() from UDS -> frame_encode -> ib_post_send()
 * RX: CQ completion callback -> frame_decode -> kernel_sendmsg() to UDS
 *      (RX is handled inline in urp_recv_done in urp_rdma.c)
 *
 * No reorder buffer, no credits, no multiplexing.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"
#include "urp_retry_plan.h"
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>

/*
 * Upper bound on how long a TX pump sleeps waiting for a send buffer when the
 * pool is empty (design 35 §35.4, phase 1). In steady state urp_buf_free_send()
 * wakes the pump within microseconds of a completion; this timeout only bounds
 * the damage of a missed wakeup, so it is generous rather than tuned.
 */
#define URP_SEND_WAIT_MAX_MS	100

/*
 * DMA-sync and post one framed send buffer (@len bytes including the
 * header) on @qp. On failure the buffer is returned to the send pool;
 * the caller must not touch @buf after this call.
 */
static int urp_post_frame(struct urp_endpoint *ep, struct ib_qp *qp,
			  struct urp_buffer *buf, u32 len)
{
	struct ib_send_wr wr = {};
	const struct ib_send_wr *bad_wr;
	int ret;

	ib_dma_sync_single_for_device(ep->ib_dev, buf->dma_addr, len,
				      DMA_TO_DEVICE);

	buf->sge.length = len;
	buf->cqe.done = urp_send_done;
	wr.wr_cqe = &buf->cqe;
	wr.sg_list = &buf->sge;
	wr.num_sge = 1;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	ret = ib_post_send(qp, &wr, &bad_wr);
	if (ret)
		urp_buf_free_send(ep, buf);
	return ret;
}

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
		struct urp_qp *qp;
		struct msghdr msg = {};
		struct kvec iov;
		int ret;
		u32 len;
		/* Per-endpoint slot cap: buf->data is ep->buf_size bytes, so
		 * header + max_payload fills it exactly.
		 */
		u32 max_payload = urp_ep_max_payload(ep->buf_size);

		buf = urp_buf_alloc_send(ep);
		if (!buf) {
			/* No send buffers available, back off briefly */
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		/* Read from UDS into the buffer payload area (after header) */
		iov.iov_base = buf->data + URP_FRAME_HEADER_SIZE;
		iov.iov_len = max_payload;

		ret = kernel_recvmsg(conn->uds_sock, &msg, &iov, 1,
				     max_payload, 0);
		if (ret <= 0) {
			urp_buf_free_send(ep, buf);
			if (ret == 0) {
				/*
				 * Peer half-closed the write side. Stop reading
				 * (the pump exits below), but leave conn->active
				 * set and the uds_sock intact so the RX path can
				 * still forward the response back to the peer's
				 * read side. The eventual full close gets caught
				 * when the RX path's kernel_sendmsg returns
				 * -EPIPE, or when urp_socket_conn_cleanup runs
				 * on drain/CM disconnect. Self-exit is safe:
				 * urp_pump_start() pinned our task_struct, so
				 * the later kthread_stop() join cannot see a
				 * freed task.
				 */
				pr_debug("peer half-closed write side; pump exiting (rx still routes)\n");
			} else if (ret != -ERESTARTSYS && ret != -EAGAIN) {
				pr_err_ratelimited("kernel_recvmsg failed: %d\n", ret);
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

		/* Round-robin across all connected QPs (with num_qps=1
		 * this always picks qps[0]).
		 */
		qp = urp_qp_select_round_robin(ep);
		if (!qp) {
			urp_buf_free_send(ep, buf);
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		/*
		 * Per-QP credit consume. Best-effort -- if the peer hasn't
		 * replenished us yet (no CREDIT frame received), bump the
		 * stall counter and proceed anyway. The RC layer's
		 * rnr_retry handles transient overshoot; the credit
		 * accounting still tracks the imbalance so `urp show`
		 * reports stalls / send_credits realistically.
		 */
		if (urp_credit_consume(&qp->credit) == -EAGAIN)
			atomic64_inc(&ep->stats.credit_stalls);

		ret = urp_post_frame(ep, qp->qp, buf,
				     URP_FRAME_HEADER_SIZE + len);
		if (ret) {
			pr_err_ratelimited("ib_post_send failed: %d\n", ret);
			conn->active = false;
			break;
		}

		atomic64_add(len, &ep->stats.tx_bytes);
		atomic64_inc(&ep->stats.tx_frames);
		/* Per-QP counters surfaced via GENL */
		atomic64_add(len, &qp->tx_bytes);
		atomic64_inc(&qp->tx_frames);
	}

	pr_info("TX pump stopped\n");
	return 0;
}

int urp_pump_start(struct urp_endpoint *ep)
{
	struct urp_connection *conn = &ep->conn;

	conn->tx_thread = kthread_run(urp_tx_thread_fn, ep, "urp-tx");
	if (IS_ERR(conn->tx_thread)) {
		int ret = PTR_ERR(conn->tx_thread);

		conn->tx_thread = NULL;
		pr_err("kthread_run (tx) failed: %d\n", ret);
		return ret;
	}
	/*
	 * Pin the task so the kthread_stop() in urp_pump_stop() can never
	 * operate on a reaped task_struct, no matter when (or whether) the
	 * pump self-exits. kthread_stop() on an already-exited-but-pinned
	 * kthread is a safe join. Dropped in urp_pump_stop().
	 */
	get_task_struct(conn->tx_thread);

	pr_info("pump started\n");
	return 0;
}

void urp_pump_stop(struct urp_endpoint *ep)
{
	struct urp_connection *conn = &ep->conn;

	conn->active = false;

	if (conn->tx_thread) {
		kthread_stop(conn->tx_thread);
		put_task_struct(conn->tx_thread);
		conn->tx_thread = NULL;
	}

	pr_info("pump stopped\n");
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
		struct urp_qp *qp;
		struct msghdr msg = {};
		struct kvec iov;
		u8 flags = 0;
		int ret;
		u32 len;
		u32 max_payload = urp_ep_max_payload(ep->buf_size);

		if (!stream->uds_sock)
			break;

		buf = urp_buf_alloc_send(ep);
		if (!buf) {
			/*
			 * Send pool empty: block until a completion
			 * (urp_send_done) or an error path returns a buffer and
			 * wakes ep->send_wq, instead of the old 1 ms poll
			 * (design 35 §35.4, phase 1). wait_event evaluates the
			 * condition in this thread's context, so the
			 * alloc-in-predicate keeps the buffer it grabs. The
			 * bounded timeout is a safety net: kthread_stop() wakes
			 * this task directly (re-checked here and at the loop
			 * top), while a UDS close has no wakeup of its own and is
			 * caught within one timeout, then broken out of up top.
			 */
			wait_event_interruptible_timeout(ep->send_wq,
				(buf = urp_buf_alloc_send(ep)) != NULL ||
					kthread_should_stop() ||
					!stream->uds_sock,
				msecs_to_jiffies(URP_SEND_WAIT_MAX_MS));
			if (!buf)
				continue;
		}

		iov.iov_base = buf->data + URP_FRAME_HEADER_SIZE;
		iov.iov_len = max_payload;

		ret = kernel_recvmsg(stream->uds_sock, &msg, &iov, 1,
				     max_payload, 0);
		if (ret <= 0) {
			urp_buf_free_send(ep, buf);
			if (ret == 0)
				pr_debug("stream %u UDS closed by peer\n",
					 stream->id);
			else if (ret != -ERESTARTSYS && ret != -EAGAIN)
				pr_err_ratelimited("stream %u recvmsg failed: %d\n",
						   stream->id, ret);
			urp_stream_tx_fin(stream);
			/* Mark reapable and exit. Self-exit is safe: the
			 * task_struct was pinned in urp_stream_pump_start(),
			 * so urp_stream_destroy's kthread_stop() join cannot
			 * see a freed task.
			 */
			WRITE_ONCE(stream->tx_done, true);
			atomic_inc(&ep->pending_reap);
			break;
		}

		len = ret;

		if (send_syn) {
			flags |= URP_DATA_FLAG_SYN;
			send_syn = false;
		}

		urp_frame_encode(buf->data, stream->id, stream->tx_seq++,
				 URP_FRAME_TYPE_DATA, flags, 0, len);

		qp = urp_qp_select_round_robin(ep);
		if (!qp) {
			urp_buf_free_send(ep, buf);
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		/* Per-stream credit consume (best-effort, same rules as
		 * the legacy ep->conn pump above).
		 */
		if (urp_credit_consume(&stream->credit) == -EAGAIN)
			atomic64_inc(&ep->stats.credit_stalls);

		ret = urp_post_frame(ep, qp->qp, buf,
				     URP_FRAME_HEADER_SIZE + len);
		if (ret) {
			pr_err_ratelimited("stream %u ib_post_send failed: %d\n",
					   stream->id, ret);
			break;
		}

		atomic64_add(len, &ep->stats.tx_bytes);
		atomic64_inc(&ep->stats.tx_frames);
		atomic64_add(len, &stream->tx_bytes);
		atomic64_add(len, &qp->tx_bytes);
		atomic64_inc(&qp->tx_frames);
	}

	pr_debug("stream %u TX pump stopped\n", stream->id);
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
		pr_err("stream %u kthread_run failed: %d\n",
		       stream->id, ret);
		return ret;
	}
	/* Pin: see urp_pump_start(). Dropped in urp_stream_pump_stop(). */
	get_task_struct(stream->tx_thread);
	return 0;
}

void urp_stream_pump_stop(struct urp_stream *stream)
{
	if (stream->tx_thread) {
		kthread_stop(stream->tx_thread);
		put_task_struct(stream->tx_thread);
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

static int urp_emit_ping_on(struct urp_endpoint *ep, struct urp_qp *qp)
{
	struct urp_buffer *buf;
	u64 now_mono, now_real;
	int ret;

	if (!qp->established || !qp->qp)
		return 0;

	/* Miss detection. If the previous PING is still outstanding
	 * (last_ping_ns != 0), no PONG arrived in time, count a miss.
	 * On URP_QP_MISS_THRESHOLD consecutive misses demote the QP to
	 * DRAINING -- urp_qp_select_round_robin stops dispatching there
	 * until it recovers. PONGs reset last_ping_ns and
	 * consecutive_misses (see urp_recv_done).
	 */
	if (qp->last_ping_ns) {
		qp->consecutive_misses++;
		qp->consecutive_pongs = 0;
		if (qp->consecutive_misses >= URP_QP_MISS_THRESHOLD &&
		    qp->health == URP_QP_STATE_ACTIVE) {
			qp->health = URP_QP_STATE_DRAINING;
			pr_warn("QP %u demoted to DRAINING after %u misses\n",
				qp->index, qp->consecutive_misses);
			/*
			 * Design 33 Phase 1.5: on the initiator a demoted QP is
			 * a silent drop (hard peer reboot -- no CM event). Turn
			 * it into the same bounded connect-retry the CM error
			 * path uses, so the session self-heals. The retry tears
			 * this QP down, so stop here -- do NOT post another PING
			 * on it below.
			 */
			if (urp_silent_drop_should_reconnect(ep->is_initiator,
							     qp->established)) {
				urp_connect_retry_on_silent_drop(ep, qp);
				return 0;
			}
		}
	}

	buf = urp_buf_alloc_send(ep);
	if (!buf)
		return -ENOBUFS;

	now_mono = ktime_get_ns();
	now_real = ktime_get_real_ns();
	qp->probe_seq++;
	qp->last_ping_ns = now_mono;

	urp_frame_encode(buf->data, 0, qp->probe_seq, URP_FRAME_TYPE_PROBE,
			 0 /* flags == 0 means PING */, 0,
			 URP_PING_PAYLOAD_SIZE);
	urp_ping_encode(buf->data + URP_FRAME_HEADER_SIZE, qp->probe_seq,
			(u16)qp->index, now_mono, now_real);

	ret = urp_post_frame(ep, qp->qp, buf,
			     URP_FRAME_HEADER_SIZE + URP_PING_PAYLOAD_SIZE);
	if (ret)
		pr_warn_ratelimited("PING post_send on qp %u failed: %d\n",
				    qp->index, ret);
	return ret;
}

static void urp_probe_work_fn(struct work_struct *work)
{
	struct urp_endpoint *ep = container_of(to_delayed_work(work),
					       struct urp_endpoint, probe_work);
	u32 i;

	if (!ep->probe_active || ep->state != URP_STATE_ACTIVE)
		return;

	/*
	 * Design 33 Phase 1.5: multi-QP endpoints probe for per-QP health /
	 * load balancing; a single-QP initiator additionally probes purely as a
	 * silent-drop keepalive (a hard peer reboot delivers no CM event, so the
	 * missing PONGs are the only signal). A single-QP acceptor stays quiet.
	 */
	if (ep->qps && urp_should_emit_probes(ep->is_initiator, ep->num_qps)) {
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

	ret = urp_post_frame(ep, qp, buf,
			     URP_FRAME_HEADER_SIZE + URP_PONG_PAYLOAD_SIZE);
	if (ret)
		pr_warn_ratelimited("PONG post_send failed: %d\n", ret);
	return ret;
}

int urp_emit_credit_frame(struct urp_endpoint *ep, struct urp_qp *qp,
			  u32 stream_id, u16 grants)
{
	struct urp_buffer *buf;
	int ret;

	if (!qp || !qp->qp || grants == 0)
		return 0;

	buf = urp_buf_alloc_send(ep);
	if (!buf) {
		atomic64_inc(&ep->stats.credit_stalls);
		return -ENOBUFS;
	}

	urp_frame_encode(buf->data, stream_id, 0, URP_FRAME_TYPE_CONTROL,
			 URP_CTRL_FLAG_CREDIT, grants, 0);

	ret = urp_post_frame(ep, qp->qp, buf, URP_FRAME_HEADER_SIZE);
	if (ret)
		pr_warn_ratelimited("CREDIT frame post_send failed: %d\n", ret);
	return ret;
}
