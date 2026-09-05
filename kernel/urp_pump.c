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
 * gap #6 Phase 2 (PR3): byte-window sender-gate liveness backstop. After this
 * many URP_SEND_WAIT_MAX_MS timeouts with no window room (5 s), post
 * best-effort to break a potential send-pool-exhaustion deadlock (see the gate
 * in urp_stream_tx_fn). Never reached under normal grant flow.
 */
#define URP_WINDOW_STALL_BUDGET	50

/*
 * Post one IB_WR_SEND of @len bytes (header included) at DMA/MR address @addr
 * with local key @lkey on @qp, completing through @cqe. A single SGE: the
 * pump's pool buffers are physically contiguous compound pages, and the
 * urp-fast zero-copy path (design 31) addresses any frame size through a
 * pool-wide MR, so one gather entry covers the whole frame either way. Does no
 * DMA-sync and no buffer bookkeeping -- the caller owns both. Returns the
 * ib_post_send result.
 */
int urp_post_frame_raw(struct ib_qp *qp, u64 addr, u32 len, u32 lkey,
		       struct ib_cqe *cqe)
{
	struct ib_send_wr wr = {};
	struct ib_sge sge = {};

	sge.addr = addr;
	sge.length = len;
	sge.lkey = lkey;
	wr.wr_cqe = cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	return ib_post_send(qp, &wr, NULL);
}

/*
 * Post one IB_WR_SEND gathering @num_sge chunks from @sges (each entry's addr,
 * length and lkey already set by the caller) completing through @cqe. This is
 * the chunked pool-buffer poster (design 37 path Y): a logical frame spans
 * num_chunks physically-separate chunks. At num_sge == 1 it is exactly
 * urp_post_frame_raw. Does no DMA-sync/bookkeeping -- the caller owns both.
 */
int urp_post_frame_sg(struct ib_qp *qp, struct ib_sge *sges, u32 num_sge,
		      struct ib_cqe *cqe)
{
	struct ib_send_wr wr = {};

	wr.wr_cqe = cqe;
	wr.sg_list = sges;
	wr.num_sge = num_sge;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	return ib_post_send(qp, &wr, NULL);
}

/*
 * Post one recv WR of @len bytes landing at DMA/MR address @addr with local key
 * @lkey on @qp, completing through @cqe. The urp-fast zero-copy RECV path
 * (design 31 PR4) arms an app-donated pool buffer directly on the endpoint QP
 * (no SRQ), so each completion maps back to the donating op via @cqe. A single
 * SGE covers the whole frame (header + payload) through the pool-wide MR. Does
 * no bookkeeping -- the caller owns the ownership state machine. Returns the
 * ib_post_recv result.
 */
int urp_post_recv_raw(struct ib_qp *qp, u64 addr, u32 len, u32 lkey,
		      struct ib_cqe *cqe)
{
	struct ib_recv_wr wr = {};
	struct ib_sge sge = {};

	sge.addr = addr;
	sge.length = len;
	sge.lkey = lkey;
	wr.wr_cqe = cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	return ib_post_recv(qp, &wr, NULL);
}

/*
 * DMA-sync and post one framed send buffer (@len bytes including the
 * header) on @qp. On failure the buffer is returned to the send pool;
 * the caller must not touch @buf after this call.
 */
static int urp_post_frame(struct urp_endpoint *ep, struct ib_qp *qp,
			  struct urp_buffer *buf, u32 len)
{
	u32 remaining = len;
	u32 n;
	int ret;

	/*
	 * Spread the framed bytes (@len, header included) across the buffer's
	 * chunks: chunk 0 first (it carries the 20-byte header), then as many
	 * more chunks as @len needs. Set each used SGE's length and DMA-sync
	 * exactly those bytes. At num_chunks == 1 this is one SGE of @len,
	 * byte-identical to the pre-chunked path.
	 */
	for (n = 0; n < buf->num_chunks && remaining; n++) {
		u32 clen = min(remaining, ep->chunk_size);

		buf->sges[n].length = clen;
		ib_dma_sync_single_for_device(ep->ib_dev, buf->sges[n].addr,
					      clen, DMA_TO_DEVICE);
		remaining -= clen;
	}

	buf->cqe.done = urp_send_done;
	ret = urp_post_frame_sg(qp, buf->sges, n, &buf->cqe);
	if (ret)
		urp_buf_free_send(ep, buf);
	return ret;
}

/*
 * Build the scatter kvec list to read one frame's payload off the UDS socket
 * into @buf's chunks (design 37 path Y). The 20-byte header reserves the front
 * of chunk 0, so chunk 0 takes chunk_size - header bytes and chunks 1..N-1 take
 * a full chunk each. Returns the kvec count (== buf->num_chunks). Total
 * capacity is num_chunks*chunk_size - header >= buf_size - header = the
 * max_payload the caller passes to kernel_recvmsg, so a full frame always fits.
 * At num_chunks == 1 this is one kvec of ep->buf_size - header -- unchanged.
 */
/*
 * Build a kvec[] covering exactly @payload_len payload bytes spread across
 * the buffer's chunks. Chunk 0 skips the URP_FRAME_HEADER_SIZE header; each
 * subsequent chunk contributes its full chunk_size until @payload_len is
 * satisfied (last chunk short). Unused chunks are dropped. Returns the number
 * of kvecs written (>= 1 for a non-empty payload; 0 for payload_len == 0).
 * Used both to size a TX read (payload_len == max_payload) and to deliver a
 * received frame straight from the recv chunks (the in-order RX bypass).
 */
u32 urp_frame_fill_kvecs_len(struct urp_endpoint *ep, struct urp_buffer *buf,
			     struct kvec *kv, u32 payload_len)
{
	u32 off = URP_FRAME_HEADER_SIZE;
	u32 remaining = payload_len;
	u32 nkv = 0;
	u32 j;

	for (j = 0; j < buf->num_chunks && remaining; j++) {
		u32 avail = ep->chunk_size - off;
		u32 take = min(remaining, avail);

		kv[nkv].iov_base = (u8 *)page_address(buf->pages[j]) + off;
		kv[nkv].iov_len = take;
		nkv++;
		remaining -= take;
		off = 0;
	}
	return nkv;
}

static u32 urp_frame_fill_kvecs(struct urp_endpoint *ep, struct urp_buffer *buf,
				struct kvec *kv)
{
	/* Read capacity == header-less payload across every chunk. */
	return urp_frame_fill_kvecs_len(ep, buf, kv,
					urp_ep_max_payload(ep->buf_size));
}

/*
 * design 40 §40.2: on a sampled DATA frame whose connection negotiated TSTAMP,
 * set URP_DATA_FLAG_TSTAMP and append the 8-byte sender-CLOCK_REALTIME trailer
 * after @len payload bytes. Returns the extra bytes to post (0 or 8) so the
 * caller extends the send length. @seq is the frame's sequence number (sampling
 * gate seq % period == 0). Only single-chunk buffers are stamped: placing the
 * trailer past a payload that scatters across path-Y chunks would need
 * boundary-straddle logic, and the TX pump reserves 8 tail bytes
 * (urp_ep_tx_max_payload) so HEADER + len + 8 always fits a single-chunk buffer.
 * Jumbo (multi-chunk) flows are throughput-oriented and simply report no OWD.
 */
static u32 urp_tx_maybe_stamp(struct urp_endpoint *ep, struct urp_buffer *buf,
			      u64 seq, u32 len)
{
	u32 period = READ_ONCE(urp_latency_sample_period);

	if (!ep->tstamp_negotiated || buf->num_chunks != 1 || !period ||
	    (seq % period))
		return 0;
	((u8 *)buf->data)[13] |= URP_DATA_FLAG_TSTAMP;
	urp_frame_tstamp_encode((u8 *)buf->data + URP_FRAME_HEADER_SIZE + len,
				ktime_get_real_ns());
	return URP_TSTAMP_TRAILER_LEN;
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
		struct kvec kv[URP_MAX_SGE];
		u32 nkv;
		int ret;
		u32 len;
		u64 seq;
		u32 tstamp;	/* OWD trailer bytes to post: 0 or 8 */
		/* Per-endpoint frame cap: header + max_payload == buf_size,
		 * scattered across the buffer's chunks (design 37 path Y).
		 * design 40 §40.2: reserve the 8-byte OWD trailer at the tail.
		 */
		u32 max_payload = urp_ep_tx_max_payload(ep->buf_size);

		buf = urp_buf_alloc_send(ep);
		if (!buf) {
			/* No send buffers available, back off briefly */
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		/* Read from UDS into the buffer chunks (after the header). */
		nkv = urp_frame_fill_kvecs(ep, buf, kv);

		ret = kernel_recvmsg(conn->uds_sock, &msg, kv, nkv,
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
		seq = conn->seq++;
		urp_frame_encode(buf->data,
				 0,		/* stream_id: k0 uses 0 */
				 seq,		/* sequence number */
				 URP_FRAME_TYPE_DATA,
				 0,		/* flags: no SYN/FIN/RST for k0 */
				 0,		/* credits: not used in k0 */
				 len);
		/* design 40 §40.2: OWD trailer on sampled frames (0 or 8 bytes). */
		tstamp = urp_tx_maybe_stamp(ep, buf, seq, len);

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
				     URP_FRAME_HEADER_SIZE + len + tstamp);
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
		struct kvec kv[URP_MAX_SGE];
		u32 nkv;
		u8 flags = 0;
		int ret;
		u32 len;
		u64 seq;
		u32 tstamp;	/* OWD trailer bytes to post: 0 or 8 */
		/* design 40 §40.2: reserve the 8-byte OWD trailer at the tail. */
		u32 max_payload = urp_ep_tx_max_payload(ep->buf_size);

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

		nkv = urp_frame_fill_kvecs(ep, buf, kv);

		ret = kernel_recvmsg(stream->uds_sock, &msg, kv, nkv,
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

		seq = stream->tx_seq++;
		urp_frame_encode(buf->data, stream->id, seq,
				 URP_FRAME_TYPE_DATA, flags, 0, len);
		/* design 40 §40.2: OWD trailer on sampled frames (0 or 8 bytes). */
		tstamp = urp_tx_maybe_stamp(ep, buf, seq, len);

		qp = urp_qp_select_round_robin(ep);
		if (!qp) {
			urp_buf_free_send(ep, buf);
			schedule_timeout_interruptible(msecs_to_jiffies(1));
			continue;
		}

		if (READ_ONCE(ep->window_negotiated)) {
			/*
			 * gap #6 Phase 2 (PR3): byte-window sender gate (design
			 * 35 §35.3). Block until this frame fits the in-flight
			 * window instead of the best-effort frame-credit consume.
			 * tx_bytes is this kthread's own counter (single writer);
			 * tx_bytes_acked is bumped by the receive path and read
			 * here (READ_ONCE). The stall is counted once per episode;
			 * the bounded wait re-checks terminal conditions (stop /
			 * UDS close / disconnect) so a peer that stops granting
			 * cannot wedge the pump forever (§35.8).
			 */
			if (!urp_window_has_room(atomic64_read(&stream->tx_bytes),
						 READ_ONCE(stream->tx_bytes_acked),
						 READ_ONCE(stream->window_bytes),
						 len)) {
				/*
				 * Liveness safety valve (§35.8): a window-blocked
				 * sender holds this send buffer, so if EVERY send
				 * buffer of an endpoint is held by blocked senders
				 * the receive path could fail to allocate a buffer
				 * to emit grants -- a pool-exhaustion deadlock. The
				 * grant emit is non-blocking + cumulative so it
				 * self-heals the instant any buffer frees, but as a
				 * hard backstop we cap the total block time and then
				 * post best-effort (degrading to the pre-PR3
				 * behaviour only under genuine grant starvation,
				 * never under normal grant flow). Grants arrive in
				 * sub-ms on a healthy fabric, so the budget is never
				 * reached in practice.
				 */
				unsigned int stalls = 0;

				atomic64_inc(&ep->stats.credit_stalls);
				while (!urp_window_has_room(
						atomic64_read(&stream->tx_bytes),
						READ_ONCE(stream->tx_bytes_acked),
						READ_ONCE(stream->window_bytes),
						len)) {
					wait_event_interruptible_timeout(ep->send_wq,
						urp_window_has_room(
							atomic64_read(&stream->tx_bytes),
							READ_ONCE(stream->tx_bytes_acked),
							READ_ONCE(stream->window_bytes),
							len) ||
						kthread_should_stop() ||
						!stream->uds_sock ||
						!READ_ONCE(ep->connected),
						msecs_to_jiffies(URP_SEND_WAIT_MAX_MS));
					if (kthread_should_stop() ||
					    !stream->uds_sock ||
					    !READ_ONCE(ep->connected)) {
						urp_buf_free_send(ep, buf);
						goto stream_tx_stop;
					}
					if (++stalls >= URP_WINDOW_STALL_BUDGET) {
						pr_warn_ratelimited(
							"stream %u window stalled %ums; posting best-effort\n",
							stream->id,
							stalls * URP_SEND_WAIT_MAX_MS);
						break;
					}
				}
			}
		} else {
			/* Per-stream credit consume (best-effort, same rules as
			 * the legacy ep->conn pump above).
			 */
			if (urp_credit_consume(&stream->credit) == -EAGAIN)
				atomic64_inc(&ep->stats.credit_stalls);
		}

		ret = urp_post_frame(ep, qp->qp, buf,
				     URP_FRAME_HEADER_SIZE + len + tstamp);
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

stream_tx_stop:	/* byte-window gate aborted on stop / UDS close / disconnect */
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
		for (i = 0; i < ep->num_qps; i++) {
			/*
			 * Design 31 D1: a fast peer has no pump to answer a PING,
			 * so probing it would rack up missed PONGs and trip the
			 * silent-drop reconnect in a loop, never letting the fast
			 * side's REGISTER stabilize. Skip it -- a fast peer's
			 * liveness is RC + app-completion driven, not probe based.
			 */
			if (ep->qps[i].peer_is_fast)
				continue;
			urp_emit_ping_on(ep, &ep->qps[i]);
		}
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

/*
 * gap #6 Phase 2 (PR3): emit a CONTROL/CREDIT-BYTES grant. Unlike the u16
 * header credit above, this carries the receiver's cumulative rx_bytes_delivered
 * high-water as a u64 in the frame payload (design 35 §35.3). The sender applies
 * it with max(), so a lost/reordered/duplicate grant is idempotent -- there is
 * no retransmit or ack of the grant itself.
 */
int urp_emit_credit_bytes_frame(struct urp_endpoint *ep, struct urp_qp *qp,
				u32 stream_id, u64 cumulative_bytes)
{
	struct urp_buffer *buf;
	int ret;

	if (!qp || !qp->qp)
		return 0;

	buf = urp_buf_alloc_send(ep);
	if (!buf) {
		atomic64_inc(&ep->stats.credit_stalls);
		return -ENOBUFS;
	}

	urp_frame_encode(buf->data, stream_id, 0, URP_FRAME_TYPE_CONTROL,
			 URP_CTRL_FLAG_CREDIT_BYTES, 0,
			 URP_CREDIT_BYTES_PAYLOAD_SIZE);
	urp_credit_bytes_encode(buf->data + URP_FRAME_HEADER_SIZE,
				cumulative_bytes);

	ret = urp_post_frame(ep, qp->qp, buf,
			     URP_FRAME_HEADER_SIZE + URP_CREDIT_BYTES_PAYLOAD_SIZE);
	if (ret)
		pr_warn_ratelimited("CREDIT-BYTES frame post_send failed: %d\n",
				    ret);
	return ret;
}
