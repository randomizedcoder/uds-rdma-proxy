// SPDX-License-Identifier: GPL-2.0
/*
 * urp-bench — io_uring UDS benchmark shell (design 30 §30.6, work item B3).
 *
 * Thin by design: all protocol/accounting logic lives in the pure core
 * (urp-bench-core.c, unit-tested + fuzzed); this file owns sockets, the
 * io_uring rings, and the event loop. Modes (§30.3):
 *
 *   blocking       poll(2) + nonblocking read/write — the control
 *   uring-rw       IORING_OP_RECV/SEND, batched submits
 *   uring-fixed    READ_FIXED/WRITE_FIXED on registered buffers
 *   uring-bufring  multishot RECV + provided buffer ring
 *   uring-sqpoll   IORING_SETUP_SQPOLL
 *   uring-sendzc   SEND_ZC probe — records the AF_UNIX copied-fallback
 *                  evidence (IORING_NOTIF_USAGE_ZC_COPIED)
 *
 * Echo discipline: messages that arrive intact in the receive buffer are
 * echoed IN PLACE (flip the ECHO flag byte, send the same bytes); only
 * messages that spanned receive chunks are re-encoded from the assembly
 * buffer (counted as reassembled). Receives are re-armed before new
 * writes are queued — the §30.5 deadlock-freedom invariant.
 *
 * Unsupported mode/kernel combinations print "BENCH_SKIP mode=… reason=…"
 * and exit 0 — a matrix cell is measured honestly or visibly absent.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "urp-bench-core.h"
#include "include/uapi/linux/urp_cmd.h"	/* fast (uring-cmd) ABI, design 31 */

#define RECV_BUF_SZ	65536u
#define ECHO_SCRATCH_SZ	(BENCH_HDR_SIZE + BENCH_PAYLOAD_MAX)
#define SAMPLE_CAP	200000
#define FIN_TIMEOUT_S	10
#define BENCH_BGID	0	/* provided-buffer group id (bufring mode) */

/* user_data encoding: kind in the top byte, index below. */
#define UD_KIND_RECV	1
#define UD_KIND_SEND	2
#define UD(kind, idx)	(((uint64_t)(kind) << 56) | (uint64_t)(idx))
#define UD_KIND(ud)	((int)((ud) >> 56))
#define UD_IDX(ud)	((uint32_t)((ud) & 0xffffffffu))

/*
 * SOCK_STREAM invariant: exactly ONE recv SQE is outstanding at a time —
 * concurrent recvs on a stream socket complete in arbitrary order and
 * would permute the byte stream. The pool exists only so a buffer whose
 * in-place echoes are still in flight can be parked while another one
 * takes over receiving.
 */
struct recv_buf {
	uint8_t *data;
	int pending_echoes;	/* in-place echo sends referencing us */
	int in_recv;		/* the (single) recv SQE targets us */
};

struct send_op {
	uint8_t *ptr;		/* remaining bytes to send */
	uint32_t remaining;
	int recv_idx;		/* recv buf this echoes out of, -1 = none */
	int slot;		/* own-send slot to release, -1 = none */
	int scratch;		/* echo-scratch slot to release, -1 = none */
	int fixed_idx;		/* registered-buffer index, -1 = none */
	int zc_notifs;		/* SEND_ZC: notification CQEs still due */
	int in_use;
};

struct run {
	const struct bench_config *cfg;
	int fd;
	struct io_uring ring;
	int use_uring;
	int use_fixed;		/* fixed buffers registered */
	int use_sendzc;
	int use_bufring;	/* provided buffer ring + multishot recv */
	int use_fast;		/* uring-cmd zero-copy fast path (design 31) */
	void *fast;		/* struct fast_ctx * — reflect path reaches it via on_msg */
	const char *fast_endpoint;	/* urp `--kind fast` endpoint to REGISTER against */

	/* provided-buffer ring (bufring mode) */
	struct io_uring_buf_ring *br;
	uint32_t br_mask;
	uint32_t br_avail;	/* buffers currently in the ring (kernel-owned) */
	int ms_armed;		/* the single multishot recv SQE is live */

	/* buffers */
	struct recv_buf *rbufs;
	uint32_t n_rbufs;
	uint8_t *send_slots;	/* batch slots x msg_size */
	uint8_t *slot_free;	/* [batch] */
	uint8_t **scratch;	/* reassembled-echo pool, [n_scratch] */
	uint8_t *scratch_busy;
	uint32_t n_scratch;
	uint32_t scratch_sz;
	uint8_t *asm_buf;

	struct send_op *sends;
	uint32_t n_sends;

	/* core state */
	struct bench_deframer df;
	struct bench_tracker tracker;
	struct bench_stats stats;

	/* progress */
	uint64_t next_seq;
	uint64_t sent_originals;
	uint64_t goal;		/* count mode; ~0 in duration mode */
	uint64_t deadline_ns;	/* duration mode; 0 = none */
	int own_fin_sent;
	uint32_t own_fin_seq;
	int own_fin_echoed;
	int peer_fin_seen;
	int peer_closed;

	/* counters */
	uint64_t syscalls;
	uint64_t bytes_echoed_back;	/* own original bytes confirmed */
	uint64_t msgs_rx_total;
	uint64_t zc_copied;
	uint64_t zc_sends;

	int recv_wanted;	/* no recv armed: every buffer had echoes */

	/* --pattern stream (§34.4). do_generate: this side sources traffic
	 * (echo pattern, or the connect side of a stream). do_echo: reflect
	 * peer originals (echo pattern only). */
	int do_generate;
	int do_echo;
	int src_carry_empty;	/* stream source: outbound fully flushed */
	uint64_t tx_wire_bytes;	/* stream source: wire bytes handed to the socket */
	uint64_t rx_data_bytes;	/* stream sink: wire bytes of data delivered */
	uint64_t rx_data_msgs;	/* stream sink: data frames delivered */
	uint64_t t_first_rx_ns;	/* stream sink: first data frame (goodput window) */
	uint64_t t_stream_end_ns;	/* stream sink: peer FIN seen */

	int64_t rc;		/* first fatal error */
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void skip(const struct bench_config *cfg, const char *reason)
{
	printf("BENCH_SKIP lang=c mode=%s msg_size=%u batch=%u reason=%s\n",
	       bench_mode_str(cfg->mode), cfg->msg_size, cfg->batch, reason);
	exit(0);
}

static void fail(const struct bench_config *cfg, const char *reason, int err)
{
	printf("BENCH_FAIL lang=c mode=%s msg_size=%u batch=%u reason=%s err=%d\n",
	       bench_mode_str(cfg->mode), cfg->msg_size, cfg->batch, reason,
	       err);
	exit(1);
}

/* ---- sockets ---------------------------------------------------------- */

static int uds_listen_accept(const char *path)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	int lfd, fd;

	if (strlen(path) >= sizeof(sun.sun_path))
		return -1;
	strcpy(sun.sun_path, path);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		return -1;
	unlink(path);
	if (bind(lfd, (struct sockaddr *)&sun, sizeof(sun)) < 0 ||
	    listen(lfd, 1) < 0) {
		close(lfd);
		return -1;
	}
	fd = accept(lfd, NULL, NULL);
	close(lfd);
	return fd;
}

static int uds_connect(const char *path)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	int fd, i;

	if (strlen(path) >= sizeof(sun.sun_path))
		return -1;
	strcpy(sun.sun_path, path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	/* The listener may still be starting: retry ~5 s. */
	for (i = 0; i < 100; i++) {
		if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) == 0)
			return fd;
		usleep(50000);
	}
	close(fd);
	return -1;
}

/* ---- shared message handling ------------------------------------------ */

struct msg_ctx {
	struct run *r;
	/* chunk bounds of the current recv completion (in-place test) */
	const uint8_t *chunk;
	size_t chunk_len;
	int recv_idx;
};

static void queue_echo_uring(struct run *r, struct msg_ctx *mc,
			     const struct bench_hdr *hdr,
			     const uint8_t *payload);
static void queue_echo_blocking(struct run *r, struct msg_ctx *mc,
				const struct bench_hdr *hdr,
				const uint8_t *payload);
static void queue_echo_fast(struct run *r, struct msg_ctx *mc,
			    const struct bench_hdr *hdr,
			    const uint8_t *payload);
static void recycle_provided(struct run *r, uint32_t buf_id);

static int on_msg(void *ctx, const struct bench_hdr *hdr,
		  const uint8_t *payload)
{
	struct msg_ctx *mc = ctx;
	struct run *r = mc->r;

	r->msgs_rx_total++;

	if (hdr->flags & BENCH_FLAG_ECHO) {
		/* an echo of one of our originals */
		int64_t rtt = bench_track_echo(&r->tracker, hdr->seq,
					       now_ns());

		if (rtt < 0)
			return (int)rtt;
		bench_stats_add(&r->stats, (uint64_t)rtt);
		r->bytes_echoed_back += BENCH_HDR_SIZE + hdr->payload_len;
		if (r->cfg->verify == BENCH_VERIFY_FULL &&
		    bench_verify_payload(payload, hdr->payload_len,
					 hdr->origin_id, hdr->seq) < 0)
			return -BENCH_ECORRUPT;
		if ((hdr->flags & BENCH_FLAG_FIN) && r->own_fin_sent &&
		    hdr->seq == r->own_fin_seq)
			r->own_fin_echoed = 1;
		return 0;
	}

	/* a peer original */
	if (r->cfg->verify == BENCH_VERIFY_FULL &&
	    bench_verify_payload(payload, hdr->payload_len, hdr->origin_id,
				 hdr->seq) < 0)
		return -BENCH_ECORRUPT;

	if (!r->do_echo) {
		/* stream sink: count delivered bytes, never echo. Goodput is
		 * measured from the first data frame to the peer's FIN. */
		if (hdr->flags & BENCH_FLAG_FIN) {
			r->peer_fin_seen = 1;
			r->t_stream_end_ns = now_ns();
		} else {
			if (r->t_first_rx_ns == 0)
				r->t_first_rx_ns = now_ns();
			r->rx_data_bytes += BENCH_HDR_SIZE + hdr->payload_len;
			r->rx_data_msgs++;
		}
		return 0;
	}

	/* echo pattern: reflect it back */
	if (hdr->flags & BENCH_FLAG_FIN)
		r->peer_fin_seen = 1;
	if (r->use_fast)
		queue_echo_fast(r, mc, hdr, payload);
	else if (r->use_uring)
		queue_echo_uring(r, mc, hdr, payload);
	else
		queue_echo_blocking(r, mc, hdr, payload);
	return r->rc < 0 ? (int)r->rc : 0;
}

/* Is payload an in-place slice of the current recv chunk? */
static int payload_in_chunk(const struct msg_ctx *mc,
			    const struct bench_hdr *hdr,
			    const uint8_t *payload)
{
	const uint8_t *start = payload - BENCH_HDR_SIZE;

	return hdr->payload_len > 0 && start >= mc->chunk &&
	       payload + hdr->payload_len <= mc->chunk + mc->chunk_len;
}

/* ---- io_uring backend -------------------------------------------------- */

static struct io_uring_sqe *get_sqe(struct run *r)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);

	if (!sqe) {
		/* SQ full: flush synchronously and retry once. */
		r->syscalls++;
		io_uring_submit(&r->ring);
		sqe = io_uring_get_sqe(&r->ring);
		if (!sqe) {
			r->rc = -BENCH_EFULL;
			return NULL;
		}
	}
	return sqe;
}

static int alloc_send_op(struct run *r)
{
	uint32_t i;

	for (i = 0; i < r->n_sends; i++)
		if (!r->sends[i].in_use)
			return (int)i;
	return -1;
}

static void prep_send(struct run *r, int op_idx)
{
	struct send_op *op = &r->sends[op_idx];
	struct io_uring_sqe *sqe = get_sqe(r);

	if (!sqe)
		return;
	if (r->use_sendzc) {
		io_uring_prep_send_zc(sqe, 0, op->ptr, op->remaining,
				      MSG_NOSIGNAL, 0);
		op->zc_notifs++;
		r->zc_sends++;
	} else if (r->use_fixed && op->fixed_idx >= 0) {
		io_uring_prep_write_fixed(sqe, 0, op->ptr, op->remaining, 0,
					  op->fixed_idx);
	} else {
		io_uring_prep_send(sqe, 0, op->ptr, op->remaining,
				   MSG_NOSIGNAL);
	}
	sqe->flags |= IOSQE_FIXED_FILE;	/* fd 0 = the registered socket */
	io_uring_sqe_set_data64(sqe, UD(UD_KIND_SEND, (uint32_t)op_idx));
}

static void prep_recv(struct run *r, uint32_t rb_idx)
{
	struct recv_buf *rb = &r->rbufs[rb_idx];
	struct io_uring_sqe *sqe = get_sqe(r);

	if (!sqe)
		return;
	if (r->use_fixed)
		io_uring_prep_read_fixed(sqe, 0, rb->data, RECV_BUF_SZ, 0,
					 (int)rb_idx);
	else
		io_uring_prep_recv(sqe, 0, rb->data, RECV_BUF_SZ, 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	io_uring_sqe_set_data64(sqe, UD(UD_KIND_RECV, rb_idx));
	rb->in_recv = 1;
}

/* Arm the single next recv: reuse the drained buffer if its echoes are
 * gone, else any echo-free buffer, else wait for release_send. */
static void arm_next_recv(struct run *r, uint32_t prefer)
{
	uint32_t i;

	if (r->rbufs[prefer].pending_echoes == 0) {
		prep_recv(r, prefer);
		return;
	}
	for (i = 0; i < r->n_rbufs; i++) {
		if (!r->rbufs[i].in_recv && r->rbufs[i].pending_echoes == 0) {
			prep_recv(r, i);
			return;
		}
	}
	r->recv_wanted = 1;
}

static void queue_echo_uring(struct run *r, struct msg_ctx *mc,
			     const struct bench_hdr *hdr,
			     const uint8_t *payload)
{
	int op_idx = alloc_send_op(r);
	struct send_op *op;

	if (op_idx < 0) {
		r->rc = -BENCH_EFULL;
		return;
	}
	op = &r->sends[op_idx];
	memset(op, 0, sizeof(*op));
	op->in_use = 1;
	op->recv_idx = -1;
	op->slot = -1;
	op->scratch = -1;
	op->fixed_idx = -1;

	if (payload_in_chunk(mc, hdr, payload)) {
		/* In-place: flip the ECHO flag byte, send the same bytes. */
		uint8_t *start = (uint8_t *)(payload - BENCH_HDR_SIZE);

		start[5] |= BENCH_FLAG_ECHO;
		op->ptr = start;
		op->remaining = BENCH_HDR_SIZE + hdr->payload_len;
		op->recv_idx = mc->recv_idx;
		op->fixed_idx = mc->recv_idx;	/* recv bufs are [0..RB) */
		r->rbufs[mc->recv_idx].pending_echoes++;
	} else {
		/* Reassembled (or header-only): rebuild in a scratch slot. */
		struct bench_hdr e = *hdr;
		uint32_t s;

		for (s = 0; s < r->n_scratch; s++)
			if (!r->scratch_busy[s])
				break;
		if (s == r->n_scratch) {
			/* pool exhausted — fail loudly, never stall */
			op->in_use = 0;
			r->rc = -BENCH_EFULL;
			return;
		}
		e.flags |= BENCH_FLAG_ECHO;
		bench_hdr_encode(&e, r->scratch[s]);
		if (hdr->payload_len)
			memcpy(r->scratch[s] + BENCH_HDR_SIZE, payload,
			       hdr->payload_len);
		r->scratch_busy[s] = 1;
		op->ptr = r->scratch[s];
		op->remaining = BENCH_HDR_SIZE + hdr->payload_len;
		op->scratch = (int)s;
		op->fixed_idx = r->use_fixed ?
			(int)(r->n_rbufs + r->cfg->batch + s) : -1;
	}
	prep_send(r, op_idx);
}

/*
 * Queue one original (or the FIN). Returns 0 on success, 1 for
 * "back off": the tracker slot for next_seq is still occupied by
 * next_seq - window (echoes completed out of order) — not an error,
 * just stop topping up until that echo lands.
 */
static int queue_original_uring(struct run *r, int fin)
{
	uint32_t slot;
	int op_idx;
	struct send_op *op;
	struct bench_hdr h;
	uint8_t *buf;
	uint64_t t = now_ns();

	if (bench_track_sent(&r->tracker, (uint32_t)r->next_seq, t) < 0)
		return 1;	/* oldest window slot still in flight */

	op_idx = alloc_send_op(r);
	if (op_idx < 0) {
		r->rc = -BENCH_EFULL;
		return 0;
	}
	for (slot = 0; slot < r->cfg->batch; slot++)
		if (r->slot_free[slot])
			break;
	if (slot == r->cfg->batch) {
		r->rc = -BENCH_EFULL;	/* planner said there was room */
		return 0;
	}
	r->slot_free[slot] = 0;
	buf = r->send_slots + (size_t)slot * r->cfg->msg_size;

	h.magic = BENCH_MAGIC;
	h.version = BENCH_VERSION;
	h.flags = fin ? BENCH_FLAG_FIN : 0;
	h.origin_id = r->cfg->id;
	h.payload_len = fin ? 0 : r->cfg->msg_size - BENCH_HDR_SIZE;
	h.seq = (uint32_t)r->next_seq;
	h.t_send_ns = t;
	bench_hdr_encode(&h, buf);
	/*
	 * Payload bytes are only generated under --verify full (smoke);
	 * perf cells send whatever the slot holds — the kernel copies the
	 * same number of bytes either way, and fill cost would contaminate
	 * the copy measurement (§30.5).
	 */
	if (r->cfg->verify == BENCH_VERIFY_FULL && h.payload_len)
		bench_fill_payload(buf + BENCH_HDR_SIZE, h.payload_len,
				   h.origin_id, h.seq);

	if (fin) {
		r->own_fin_sent = 1;
		r->own_fin_seq = h.seq;
	}
	r->next_seq++;
	r->sent_originals++;

	op = &r->sends[op_idx];
	memset(op, 0, sizeof(*op));
	op->in_use = 1;
	op->recv_idx = -1;
	op->slot = (int)slot;
	op->scratch = -1;
	op->fixed_idx = r->use_fixed ? (int)(r->n_rbufs + slot) : -1;
	op->ptr = buf;
	op->remaining = fin ? BENCH_HDR_SIZE : r->cfg->msg_size;
	prep_send(r, op_idx);
	return 0;
}

static void release_send(struct run *r, struct send_op *op)
{
	if (op->recv_idx >= 0) {
		struct recv_buf *rb = &r->rbufs[op->recv_idx];

		rb->pending_echoes--;
		if (rb->pending_echoes == 0) {
			if (r->use_bufring)
				/* last echo out of this provided buffer done:
				 * hand it back to the ring (re-arms if stalled). */
				recycle_provided(r, (uint32_t)op->recv_idx);
			else if (r->recv_wanted) {
				r->recv_wanted = 0;
				prep_recv(r, (uint32_t)op->recv_idx);
			}
		}
	}
	if (op->scratch >= 0)
		r->scratch_busy[op->scratch] = 0;
	if (op->slot >= 0)
		r->slot_free[op->slot] = 1;
	op->in_use = 0;
}

static void handle_send_cqe(struct run *r, struct io_uring_cqe *cqe)
{
	uint32_t op_idx = UD_IDX(cqe->user_data);
	struct send_op *op = &r->sends[op_idx];

	if (r->use_sendzc && (cqe->flags & IORING_CQE_F_NOTIF)) {
		/* zero-copy notification: one send's buffer guard drops */
		if (cqe->res & IORING_NOTIF_USAGE_ZC_COPIED)
			r->zc_copied++;
		op->zc_notifs--;
		if (op->remaining == 0 && op->zc_notifs == 0)
			release_send(r, op);
		return;
	}
	if (cqe->res < 0) {
		if (r->use_sendzc && cqe->res == -EOPNOTSUPP) {
			/*
			 * The probe's entire purpose (§30.3): this kernel
			 * refuses SEND_ZC on AF_UNIX. Record the evidence
			 * and skip the cell.
			 */
			printf("BENCH_ZC sends=%llu copied=%llu result=eopnotsupp\n",
			       (unsigned long long)r->zc_sends,
			       (unsigned long long)r->zc_copied);
			skip(r->cfg, "sendzc_eopnotsupp");
		}
		r->rc = cqe->res;
		return;
	}
	if ((uint32_t)cqe->res < op->remaining) {
		/* partial send: push the remainder (another notif if zc) */
		op->ptr += cqe->res;
		op->remaining -= (uint32_t)cqe->res;
		prep_send(r, (int)op_idx);
		return;
	}
	op->remaining = 0;
	if (r->use_sendzc && op->zc_notifs > 0)
		return;		/* wait for the notification CQE(s) */
	release_send(r, op);
}

static void handle_recv_cqe(struct run *r, struct io_uring_cqe *cqe)
{
	uint32_t rb_idx = UD_IDX(cqe->user_data);
	struct recv_buf *rb = &r->rbufs[rb_idx];
	struct msg_ctx mc = { .r = r, .recv_idx = (int)rb_idx };
	int ret;

	rb->in_recv = 0;
	if (cqe->res == 0 || cqe->res == -ECONNRESET) {
		r->peer_closed = 1;
		return;
	}
	if (cqe->res < 0) {
		r->rc = cqe->res;
		return;
	}
	mc.chunk = rb->data;
	mc.chunk_len = (size_t)cqe->res;
	ret = bench_deframe_feed(&r->df, rb->data, (size_t)cqe->res, on_msg,
				 &mc);
	if (ret < 0) {
		r->rc = ret;
		return;
	}
	/* §30.5 invariant: re-arm the receive before new writes queue. */
	arm_next_recv(r, rb_idx);
}

/* ---- provided-buffer ring (bufring mode) ------------------------------ */

/*
 * Arm the ONE multishot recv. It draws a fresh provided buffer per
 * completion from group BENCH_BGID; it stays live (each CQE carries
 * IORING_CQE_F_MORE) until buffers run out (-ENOBUFS) or the peer closes,
 * at which point run_uring re-arms it once at end-of-pass. Exactly one
 * multishot recv is ever outstanding, preserving stream byte order (§30.5).
 */
static void arm_multishot(struct run *r)
{
	struct io_uring_sqe *sqe = get_sqe(r);

	if (!sqe)
		return;
	io_uring_prep_recv_multishot(sqe, 0, NULL, 0, 0);
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
	sqe->buf_group = BENCH_BGID;
	io_uring_sqe_set_data64(sqe, UD(UD_KIND_RECV, 0));
	r->ms_armed = 1;
}

/*
 * Hand a drained provided buffer back to the kernel. Re-arming the
 * multishot recv is deliberately NOT done here: within one completion
 * pass the data CQEs (which recycle) are processed before the terminating
 * -ENOBUFS CQE (which clears ms_armed), so a recycle-time re-arm would
 * race and be skipped. The loop re-arms once at the end of the pass
 * (run_uring), when ms_armed is settled and br_avail is final.
 */
static void recycle_provided(struct run *r, uint32_t buf_id)
{
	io_uring_buf_ring_add(r->br, r->rbufs[buf_id].data, RECV_BUF_SZ, buf_id,
			      r->br_mask, 0);
	io_uring_buf_ring_advance(r->br, 1);
	r->br_avail++;
}

static void handle_recv_cqe_bufring(struct run *r, struct io_uring_cqe *cqe)
{
	uint32_t buf_id;
	struct msg_ctx mc = { .r = r };
	int ret;

	if (!(cqe->flags & IORING_CQE_F_MORE))
		r->ms_armed = 0;	/* multishot terminated; must re-arm */
	if (cqe->res == 0 || cqe->res == -ECONNRESET) {
		r->peer_closed = 1;
		return;
	}
	if (cqe->res == -ENOBUFS)
		return;			/* stalled: re-armed on next recycle */
	if (cqe->res < 0) {
		r->rc = cqe->res;
		return;
	}
	if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
		r->rc = -BENCH_EINVAL;	/* data CQE without a buffer */
		return;
	}
	buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
	r->br_avail--;

	mc.chunk = r->rbufs[buf_id].data;
	mc.chunk_len = (size_t)cqe->res;
	mc.recv_idx = (int)buf_id;
	ret = bench_deframe_feed(&r->df, r->rbufs[buf_id].data, (size_t)cqe->res,
				 on_msg, &mc);
	if (ret < 0) {
		r->rc = ret;
		return;
	}
	/* In-place echoes hold the buffer; recycle now only if none pend
	 * (held buffers are recycled in release_send when the echo lands).
	 * Re-arming is handled at end-of-pass in run_uring. */
	if (r->rbufs[buf_id].pending_echoes == 0)
		recycle_provided(r, buf_id);
}

static int run_done(const struct run *r)
{
	uint32_t i;

	if (r->cfg->pattern == BENCH_PATTERN_STREAM) {
		if (r->do_generate) {
			/* source: FIN sent, outbound flushed, no sends pending */
			if (!r->own_fin_sent || !r->src_carry_empty)
				return 0;
			for (i = 0; i < r->n_sends; i++)
				if (r->sends[i].in_use)
					return 0;
			return 1;
		}
		/* sink: drain until the source's FIN (or it closes). */
		return r->peer_fin_seen || r->peer_closed;
	}

	if (r->peer_closed)
		return 1;
	if (!(r->own_fin_echoed && r->peer_fin_seen))
		return 0;
	if (r->tracker.inflight_count > 0)
		return 0;
	for (i = 0; i < r->n_sends; i++)
		if (r->sends[i].in_use)
			return 0;
	return 1;
}

static void top_up(struct run *r)
{
	struct bench_batch b = { .window = r->cfg->batch };
	uint64_t remaining;
	uint32_t n;

	if (r->own_fin_sent || r->rc < 0)
		return;
	if (r->deadline_ns && now_ns() >= r->deadline_ns)
		remaining = 0;
	else
		remaining = r->goal > r->sent_originals ?
			    r->goal - r->sent_originals : 0;
	n = bench_batch_plan(&b, r->tracker.inflight_count, remaining);
	while (n-- && r->rc >= 0)
		if (queue_original_uring(r, 0))
			return;	/* slot backpressure: retry next loop */
	/* All originals sent (or deadline hit): append the FIN. */
	if (remaining == 0 &&
	    r->tracker.inflight_count < r->cfg->batch)
		queue_original_uring(r, 1);
}

static int run_uring(struct run *r)
{
	const struct bench_config *cfg = r->cfg;
	struct io_uring_params p = { 0 };
	uint64_t hard_deadline;
	uint32_t i;
	int ret;

	/* The one-way stream pattern is implemented on the blocking backend
	 * only for now; the io_uring source/sink split is a follow-up (§34.4). */
	if (cfg->pattern == BENCH_PATTERN_STREAM)
		skip(cfg, "stream_mode_todo");

	p.flags = 0;
	if (cfg->mode == BENCH_MODE_URING_SQPOLL) {
		p.flags |= IORING_SETUP_SQPOLL;
		p.sq_thread_idle = 2000;
	}
	if (cfg->defer_taskrun)
		p.flags |= IORING_SETUP_SINGLE_ISSUER |
			   IORING_SETUP_DEFER_TASKRUN |
			   IORING_SETUP_COOP_TASKRUN;

	ret = io_uring_queue_init_params(
		(unsigned)(4 * cfg->batch < 64 ? 64 : 4 * cfg->batch), &r->ring,
		&p);
	if (ret == -EPERM)
		skip(cfg, cfg->mode == BENCH_MODE_URING_SQPOLL ?
			  "sqpoll_eperm" : "no_io_uring");
	if (ret == -EINVAL && cfg->defer_taskrun)
		skip(cfg, "no_defer_taskrun");
	if (ret < 0)
		skip(cfg, "no_io_uring");

	if (cfg->mode == BENCH_MODE_URING_SENDZC) {
		struct io_uring_probe *probe = io_uring_get_probe_ring(&r->ring);

		if (!probe || !io_uring_opcode_supported(probe,
							 IORING_OP_SEND_ZC)) {
			io_uring_free_probe(probe);
			skip(cfg, "no_sendzc");
		}
		io_uring_free_probe(probe);
		r->use_sendzc = 1;
	}

	ret = io_uring_register_files(&r->ring, &r->fd, 1);
	if (ret < 0)
		fail(cfg, "register_files", ret);

	if (cfg->mode == BENCH_MODE_URING_FIXED) {
		/* recv [0..RB), send slots [RB..RB+batch), scratch pool */
		uint32_t n = r->n_rbufs + cfg->batch + r->n_scratch;
		struct iovec *iov = calloc(n, sizeof(*iov));

		if (!iov)
			fail(cfg, "oom", 0);
		for (i = 0; i < r->n_rbufs; i++) {
			iov[i].iov_base = r->rbufs[i].data;
			iov[i].iov_len = RECV_BUF_SZ;
		}
		for (i = 0; i < cfg->batch; i++) {
			iov[r->n_rbufs + i].iov_base =
				r->send_slots + (size_t)i * cfg->msg_size;
			iov[r->n_rbufs + i].iov_len = cfg->msg_size;
		}
		for (i = 0; i < r->n_scratch; i++) {
			iov[r->n_rbufs + cfg->batch + i].iov_base =
				r->scratch[i];
			iov[r->n_rbufs + cfg->batch + i].iov_len =
				r->scratch_sz;
		}
		ret = io_uring_register_buffers(&r->ring, iov, n);
		free(iov);
		if (ret < 0)
			skip(cfg, "no_fixed_buffers");
		r->use_fixed = 1;
	}

	if (cfg->mode == BENCH_MODE_URING_BUFRING) {
		int bret = 0;

		/* n_rbufs must be a power of two for the ring mask. */
		if (r->n_rbufs & (r->n_rbufs - 1))
			skip(cfg, "bufring_bad_geometry");
		r->br = io_uring_setup_buf_ring(&r->ring, r->n_rbufs, BENCH_BGID,
						0, &bret);
		if (!r->br)
			skip(cfg, "no_pbuf_ring");
		r->br_mask = io_uring_buf_ring_mask(r->n_rbufs);
		for (i = 0; i < r->n_rbufs; i++)
			io_uring_buf_ring_add(r->br, r->rbufs[i].data,
					      RECV_BUF_SZ, i, r->br_mask, (int)i);
		io_uring_buf_ring_advance(r->br, (int)r->n_rbufs);
		r->br_avail = r->n_rbufs;
		r->use_bufring = 1;
	}

	r->use_uring = 1;
	if (r->use_bufring)
		arm_multishot(r);	/* ONE multishot recv (stream ordering) */
	else
		prep_recv(r, 0);	/* ONE outstanding recv (stream ordering) */

	hard_deadline = now_ns() +
			(FIN_TIMEOUT_S +
			 (r->deadline_ns ? (uint64_t)cfg->duration_s :
					   cfg->count / 1000 + 30)) *
				1000000000ull;

	while (!run_done(r) && r->rc >= 0) {
		struct io_uring_cqe *cqe;
		unsigned head, seen = 0;

		top_up(r);
		r->syscalls++;
		ret = io_uring_submit_and_wait(&r->ring, 1);
		if (ret < 0 && ret != -EINTR) {
			r->rc = ret;
			break;
		}
		io_uring_for_each_cqe(&r->ring, head, cqe) {
			seen++;
			if (UD_KIND(cqe->user_data) != UD_KIND_RECV)
				handle_send_cqe(r, cqe);
			else if (r->use_bufring)
				handle_recv_cqe_bufring(r, cqe);
			else
				handle_recv_cqe(r, cqe);
		}
		io_uring_cq_advance(&r->ring, seen);
		/*
		 * bufring: the single multishot recv terminates (F_MORE clear /
		 * -ENOBUFS) when buffers momentarily run out. Re-arm it here —
		 * once per pass, after ms_armed and br_avail have settled — so a
		 * mid-pass recycle/terminate ordering can't leave it dead with
		 * buffers waiting (the §30.5 "receives before writes" invariant
		 * for the multishot path).
		 */
		if (r->use_bufring && !r->ms_armed && !r->peer_closed &&
		    r->br_avail > 0 && r->rc >= 0)
			arm_multishot(r);
		if (now_ns() > hard_deadline) {
			r->rc = -BENCH_EINVAL;
			fail(cfg, "timeout", 0);
		}
	}
	if (r->br)
		io_uring_free_buf_ring(&r->ring, r->br, r->n_rbufs, BENCH_BGID);
	io_uring_queue_exit(&r->ring);
	return r->rc < 0 ? (int)r->rc : 0;
}

/* ---- blocking (control) backend --------------------------------------- */

struct blk {
	uint8_t carry[RECV_BUF_SZ + ECHO_SCRATCH_SZ];
	size_t carry_len;	/* pending outbound bytes (partial write) */
};

static struct blk blk_state;

static int blk_flush(struct run *r)
{
	while (blk_state.carry_len) {
		ssize_t w;

		r->syscalls++;
		w = write(r->fd, blk_state.carry, blk_state.carry_len);
		if (w < 0) {
			if (errno == EAGAIN)
				return 0;	/* try again on POLLOUT */
			return -errno;
		}
		memmove(blk_state.carry, blk_state.carry + w,
			blk_state.carry_len - (size_t)w);
		blk_state.carry_len -= (size_t)w;
	}
	return 0;
}

static int blk_queue(struct run *r, const uint8_t *data, size_t len)
{
	size_t off = 0;

	/*
	 * A classic app writes straight from its message buffer; the carry
	 * buffer exists ONLY for partial-write remainders (and to preserve
	 * byte order behind them) so the control mode carries no extra
	 * steady-state memcpy.
	 */
	if (blk_state.carry_len == 0) {
		while (off < len) {
			ssize_t w;

			r->syscalls++;
			w = write(r->fd, data + off, len - off);
			if (w < 0) {
				if (errno == EAGAIN)
					break;
				return -errno;
			}
			off += (size_t)w;
		}
		if (off == len)
			return 0;
	}
	if (blk_state.carry_len + (len - off) > sizeof(blk_state.carry))
		return -BENCH_EFULL;
	memcpy(blk_state.carry + blk_state.carry_len, data + off, len - off);
	blk_state.carry_len += len - off;
	return 0;
}

static void queue_echo_blocking(struct run *r, struct msg_ctx *mc,
				const struct bench_hdr *hdr,
				const uint8_t *payload)
{
	int ret;

	if (payload_in_chunk(mc, hdr, payload)) {
		uint8_t *start = (uint8_t *)(payload - BENCH_HDR_SIZE);

		start[5] |= BENCH_FLAG_ECHO;
		ret = blk_queue(r, start, BENCH_HDR_SIZE + hdr->payload_len);
	} else {
		struct bench_hdr e = *hdr;
		uint8_t ehdr[BENCH_HDR_SIZE];

		e.flags |= BENCH_FLAG_ECHO;
		bench_hdr_encode(&e, ehdr);
		ret = blk_queue(r, ehdr, BENCH_HDR_SIZE);
		if (ret == 0 && hdr->payload_len)
			ret = blk_queue(r, payload, hdr->payload_len);
	}
	if (ret < 0)
		r->rc = ret;
}

/* Same 0/1 (success / slot-backpressure) contract as the uring twin. */
static int queue_original_blocking(struct run *r, int fin)
{
	uint8_t buf[BENCH_HDR_SIZE];
	struct bench_hdr h;
	uint64_t t = now_ns();
	int ret;

	/* In stream mode there are no echoes to clear the RTT window, so the
	 * tracker is not the pacing gate (the socket/carry backpressure is). */
	if (r->cfg->pattern == BENCH_PATTERN_ECHO &&
	    bench_track_sent(&r->tracker, (uint32_t)r->next_seq, t) < 0)
		return 1;	/* oldest window slot still in flight */

	h.magic = BENCH_MAGIC;
	h.version = BENCH_VERSION;
	h.flags = fin ? BENCH_FLAG_FIN : 0;
	h.origin_id = r->cfg->id;
	h.payload_len = fin ? 0 : r->cfg->msg_size - BENCH_HDR_SIZE;
	h.seq = (uint32_t)r->next_seq;
	h.t_send_ns = t;
	bench_hdr_encode(&h, buf);
	if (fin) {
		r->own_fin_sent = 1;
		r->own_fin_seq = h.seq;
	}
	r->next_seq++;
	r->sent_originals++;
	r->tx_wire_bytes += BENCH_HDR_SIZE + h.payload_len;

	ret = blk_queue(r, buf, BENCH_HDR_SIZE);
	if (ret == 0 && h.payload_len) {
		uint8_t *slot = r->send_slots;	/* one shared payload slot */

		if (r->cfg->verify == BENCH_VERIFY_FULL)
			bench_fill_payload(slot, h.payload_len, h.origin_id,
					   h.seq);
		ret = blk_queue(r, slot, h.payload_len);
	}
	if (ret < 0)
		r->rc = ret;
	return 0;
}

static int run_blocking(struct run *r)
{
	uint8_t *rbuf = r->rbufs[0].data;
	uint64_t hard_deadline =
		now_ns() + ((r->deadline_ns ? r->cfg->duration_s : 0) +
			    FIN_TIMEOUT_S + r->cfg->count / 1000) *
				   1000000000ull;

	while (!run_done(r) && r->rc >= 0) {
		struct pollfd pfd = { .fd = r->fd, .events = POLLIN };
		struct bench_batch b = { .window = r->cfg->batch };
		uint64_t remaining;
		uint32_t n;
		ssize_t got;
		int ret;

		/* top-up: the echo pattern and the stream *source* generate;
		 * the stream *sink* (do_generate == 0) never sources traffic. */
		if (r->do_generate && !r->own_fin_sent &&
		    r->cfg->pattern == BENCH_PATTERN_STREAM) {
			/* Blast while the socket accepts: carry empty means the
			 * last write fully drained, so keep feeding; a partial
			 * write leaves carry_len>0 and we fall through to poll
			 * POLLOUT. No RTT tracker window here (§34.4). */
			while (r->rc >= 0 && !r->own_fin_sent &&
			       blk_state.carry_len == 0) {
				if (r->deadline_ns && now_ns() >= r->deadline_ns)
					remaining = 0;
				else
					remaining = r->goal > r->sent_originals ?
						    r->goal - r->sent_originals : 0;
				queue_original_blocking(r, remaining == 0);
				if (remaining == 0)
					break;	/* FIN queued */
			}
		} else if (r->do_generate && !r->own_fin_sent) {
			if (r->deadline_ns && now_ns() >= r->deadline_ns)
				remaining = 0;
			else
				remaining = r->goal > r->sent_originals ?
					    r->goal - r->sent_originals : 0;
			n = bench_batch_plan(&b, r->tracker.inflight_count,
					     remaining);
			while (n-- && r->rc >= 0)
				if (queue_original_blocking(r, 0))
					break;	/* slot backpressure */
			if (remaining == 0 &&
			    r->tracker.inflight_count < r->cfg->batch)
				queue_original_blocking(r, 1);
		}

		if (blk_state.carry_len)
			pfd.events |= POLLOUT;
		r->syscalls++;
		ret = poll(&pfd, 1, 1000);
		if (ret < 0 && errno != EINTR) {
			r->rc = -errno;
			break;
		}
		if (pfd.revents & POLLOUT) {
			ret = blk_flush(r);
			if (ret < 0) {
				r->rc = ret;
				break;
			}
		}
		if (pfd.revents & (POLLIN | POLLHUP)) {
			struct msg_ctx mc = { .r = r, .recv_idx = 0 };

			r->syscalls++;
			got = read(r->fd, rbuf, RECV_BUF_SZ);
			if (got == 0) {
				r->peer_closed = 1;
				break;
			}
			if (got < 0 && errno != EAGAIN) {
				r->rc = -errno;
				break;
			}
			if (got > 0) {
				mc.chunk = rbuf;
				mc.chunk_len = (size_t)got;
				ret = bench_deframe_feed(&r->df, rbuf,
							 (size_t)got, on_msg,
							 &mc);
				if (ret < 0) {
					r->rc = ret;
					break;
				}
			}
		}
		/* stream source completion (run_done) keys off a fully-drained
		 * outbound path. */
		r->src_carry_empty = (blk_state.carry_len == 0);
		if (now_ns() > hard_deadline)
			fail(r->cfg, "timeout", 0);
	}
	return r->rc < 0 ? (int)r->rc : 0;
}

/* ---- fast (uring-cmd / zero-copy) backend ------------------------------ */

/*
 * The fast path (design 31) is not a socket transport: the app hands urp.ko a
 * pinned buffer pool over io_uring_cmd on /dev/urp and the NIC DMAs straight
 * into/out of those pages. urp-bench drives it by NESTING its own 24-byte frame
 * (bench_hdr + payload) inside the urp payload — the app writes the bench frame
 * at buf+URP_CMD_HEADER_RESV and the kernel prepends the 20-byte wire header, so
 * one SEND posts a single SGE with no payload copy. The pure core
 * (framing/deframe/tracker/stats) is reused unchanged. Roles (pattern x role):
 *
 *   stream connect  source     blast SENDs, then one bench FIN, drain
 *   stream listen   sink       arm RECVs, count delivered bytes to the FIN
 *   echo   connect  pinger     windowed SEND-original + RECV-echo -> RTT
 *   echo   listen   reflector  RECV + in-place zero-copy reflect (flip ECHO)
 *
 * The socket echo is a symmetric peer echo; the fast echo is an asymmetric
 * ping-pong (connect measures RTT, listen only reflects). The hw matrix scrapes
 * the connect side's BENCH_OK, so the reported RTT is identical in meaning.
 */

enum fast_bstate {
	FB_FREE,	/* app owns the buffer, idle */
	FB_SEND,	/* a SEND is in flight from this buffer */
	FB_RECV,	/* a RECV is armed on this buffer */
};

struct fast_ctx {
	struct run *r;
	uint8_t *pool;		/* mmap'd buffer pool */
	size_t pool_len;
	uint32_t buf_size;	/* per-buffer bytes (page multiple) */
	uint32_t count;		/* pool buffers */
	uint32_t msg_size;	/* urp payload == nested bench wire size */
	uint32_t send_lo;	/* send-pool is [send_lo, send_lo + send_n) */
	uint32_t send_n;
	uint16_t stream_id;	/* app-assigned, non-zero (0 is reserved) */
	uint8_t *bstate;	/* [count] enum fast_bstate */
	uint8_t *recv_pool;	/* [count] 1 == buffer is (re)armed as RECV */
};

/* First byte of buffer @idx's nested bench frame — past the 20-byte urp header
 * the kernel writes at +0. */
static uint8_t *fast_frame(struct fast_ctx *f, uint32_t idx)
{
	return f->pool + (size_t)idx * f->buf_size + URP_CMD_HEADER_RESV;
}

static uint32_t fast_state_count(struct fast_ctx *f, int state)
{
	uint32_t i, n = 0;

	for (i = 0; i < f->count; i++)
		if (f->bstate[i] == (uint8_t)state)
			n++;
	return n;
}

/* Post a SEND of @len urp-payload bytes (== nested bench wire bytes) from
 * buffer @idx; the bench frame must already be written at fast_frame(idx). */
static void fast_post_send(struct fast_ctx *f, uint32_t idx, uint32_t len)
{
	struct io_uring_sqe *sqe = get_sqe(f->r);
	struct urp_cmd_data d;

	if (!sqe)
		return;
	memset(&d, 0, sizeof(d));
	d.buf_index = idx;
	d.len = len;
	d.stream_id = f->stream_id;
	io_uring_prep_uring_cmd(sqe, URP_CMD_SEND, f->r->fd);
	memset(sqe->cmd, 0, 16);
	memcpy(sqe->cmd, &d, sizeof(d));
	io_uring_sqe_set_data64(sqe, UD(UD_KIND_SEND, idx));
	f->bstate[idx] = FB_SEND;
}

/* Arm buffer @idx as a zero-copy RDMA landing slot for one inbound frame. */
static void fast_post_recv(struct fast_ctx *f, uint32_t idx)
{
	struct io_uring_sqe *sqe = get_sqe(f->r);
	struct urp_cmd_data d;

	if (!sqe)
		return;
	memset(&d, 0, sizeof(d));
	d.buf_index = idx;
	d.len = f->msg_size;	/* donate the whole nested-frame region */
	io_uring_prep_uring_cmd(sqe, URP_CMD_RECV, f->r->fd);
	memset(sqe->cmd, 0, 16);
	memcpy(sqe->cmd, &d, sizeof(d));
	io_uring_sqe_set_data64(sqe, UD(UD_KIND_RECV, idx));
	f->bstate[idx] = FB_RECV;
}

/* Reflector's in-place zero-copy echo: flip the ECHO flag byte in the frame the
 * NIC just delivered and SEND the same bytes straight back — no copy. Mirrors
 * the socket in-place echo (queue_echo_blocking), buf_index-addressed. */
static void queue_echo_fast(struct run *r, struct msg_ctx *mc,
			    const struct bench_hdr *hdr,
			    const uint8_t *payload)
{
	struct fast_ctx *f = r->fast;
	uint32_t idx = (uint32_t)mc->recv_idx;
	uint8_t *frame = fast_frame(f, idx);

	(void)payload;
	frame[5] |= BENCH_FLAG_ECHO;	/* bench_hdr.flags is byte 5 of the frame */
	fast_post_send(f, idx, BENCH_HDR_SIZE + hdr->payload_len);
}

/* Synchronous REGISTER of the pool against the fast endpoint (cold path).
 * Returns 0 or the CQE's negative errno. */
static int fast_register(struct fast_ctx *f, const char *endpoint)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&f->r->ring);
	struct io_uring_cqe *cqe;
	struct urp_cmd_reg reg;
	struct urp_cmd_reg_sqe rs;
	int ret, res;

	if (!sqe)
		return -BENCH_EFULL;
	memset(&reg, 0, sizeof(reg));
	reg.base = (uint64_t)(uintptr_t)f->pool;
	reg.len = f->pool_len;
	reg.buf_size = f->buf_size;
	reg.count = f->count;
	strncpy(reg.endpoint, endpoint, URP_CMD_NAME_MAX - 1);
	memset(&rs, 0, sizeof(rs));
	rs.arg = (uint64_t)(uintptr_t)&reg;
	io_uring_prep_uring_cmd(sqe, URP_CMD_REGISTER, f->r->fd);
	memset(sqe->cmd, 0, 16);
	memcpy(sqe->cmd, &rs, sizeof(rs));
	io_uring_sqe_set_data64(sqe, 0);
	f->r->syscalls++;
	ret = io_uring_submit_and_wait(&f->r->ring, 1);
	if (ret < 0)
		return ret;
	ret = io_uring_wait_cqe(&f->r->ring, &cqe);
	if (ret < 0)
		return ret;
	res = cqe->res;
	io_uring_cqe_seen(&f->r->ring, cqe);
	return res;
}

/* True while the source/pinger should still originate data frames. */
static int fast_more_data(struct run *r, uint64_t now)
{
	if (r->cfg->count)
		return r->sent_originals < r->cfg->count;
	return now < r->deadline_ns;
}

/* Emit one data original from a free send-pool buffer. Returns 1 if sent. */
static int fast_gen_one(struct fast_ctx *f, uint32_t idx, uint64_t now,
			int pingpong)
{
	struct run *r = f->r;
	const struct bench_config *cfg = r->cfg;
	struct bench_hdr h;
	uint8_t *frame;
	uint32_t seq = (uint32_t)r->next_seq;

	memset(&h, 0, sizeof(h));
	h.magic = BENCH_MAGIC;
	h.version = BENCH_VERSION;
	h.origin_id = cfg->id;
	h.payload_len = f->msg_size - BENCH_HDR_SIZE;
	h.seq = seq;
	h.t_send_ns = now;
	/* pinger tracks each original so its returning echo yields an RTT; the
	 * tracker window == batch, so back off if it is momentarily full. */
	if (pingpong && bench_track_sent(&r->tracker, seq, now) < 0)
		return 0;
	r->next_seq++;
	frame = fast_frame(f, idx);
	bench_hdr_encode(&h, frame);
	if (cfg->verify != BENCH_VERIFY_NONE)
		bench_fill_payload(frame + BENCH_HDR_SIZE, h.payload_len,
				   cfg->id, seq);
	fast_post_send(f, idx, f->msg_size);
	r->sent_originals++;
	r->tx_wire_bytes += f->msg_size;
	return 1;
}

/* Emit the single terminating FIN frame from a free send-pool buffer. */
static void fast_gen_fin(struct fast_ctx *f, uint32_t idx, uint64_t now,
			 int pingpong)
{
	struct run *r = f->r;
	struct bench_hdr h;
	uint8_t *frame;
	uint32_t seq = (uint32_t)r->next_seq++;

	memset(&h, 0, sizeof(h));
	h.magic = BENCH_MAGIC;
	h.version = BENCH_VERSION;
	h.flags = BENCH_FLAG_FIN;
	h.origin_id = r->cfg->id;
	h.seq = seq;
	h.t_send_ns = now;
	frame = fast_frame(f, idx);
	bench_hdr_encode(&h, frame);
	if (pingpong)
		bench_track_sent(&r->tracker, seq, now);
	r->own_fin_sent = 1;
	r->own_fin_seq = seq;
	fast_post_send(f, idx, BENCH_HDR_SIZE);
}

/* Has the run reached its terminal state for this role? */
static int fast_done(struct fast_ctx *f, int gen, int pingpong, int reflect)
{
	struct run *r = f->r;

	if (reflect)
		return r->peer_fin_seen && fast_state_count(f, FB_SEND) == 0;
	if (!gen)			/* stream sink */
		return r->peer_fin_seen;
	if (pingpong)			/* echo pinger */
		return r->own_fin_sent && r->own_fin_echoed &&
		       fast_state_count(f, FB_SEND) == 0;
	/* stream source */
	return r->own_fin_sent && fast_state_count(f, FB_SEND) == 0;
}

static int run_fast(struct run *r)
{
	const struct bench_config *cfg = r->cfg;
	long pg = sysconf(_SC_PAGESIZE);
	struct fast_ctx f;
	uint32_t buf_size, count, W, i;
	unsigned entries;
	uint64_t hard_deadline;
	int gen, reflect, pingpong, res;

	if (pg <= 0)
		pg = 4096;
	/* Pool buffer holds the 20-byte urp header + the nested bench frame.
	 * Round up to a page so the pool length tiles to PAGE_SIZE for any
	 * msg_size — REGISTER requires len % buf_size == 0 && page-aligned. */
	buf_size = (uint32_t)(((uint64_t)URP_CMD_HEADER_RESV + cfg->msg_size +
			       (uint64_t)pg - 1) / (uint64_t)pg * (uint64_t)pg);
	if (buf_size > URP_CMD_BUF_SIZE_MAX)
		skip(cfg, "msg_too_big_for_fast");

	pingpong = cfg->pattern == BENCH_PATTERN_ECHO &&
		   cfg->role == BENCH_ROLE_CONNECT;
	reflect = cfg->pattern == BENCH_PATTERN_ECHO &&
		  cfg->role == BENCH_ROLE_LISTEN;
	gen = cfg->role == BENCH_ROLE_CONNECT;	/* source or pinger */

	/* Outstanding window == batch (== the RTT tracker window main sized). A
	 * pinger needs a send-pool AND a recv-pool; others need one pool. */
	W = cfg->batch;
	count = pingpong ? 2 * W : W;
	if (count < 4)
		count = 4;
	if (count > URP_CMD_POOL_COUNT_MAX)
		count = URP_CMD_POOL_COUNT_MAX;
	entries = 8;
	while (entries < count * 2u + 8u)
		entries <<= 1;

	memset(&f, 0, sizeof(f));
	f.r = r;
	f.buf_size = buf_size;
	f.count = count;
	f.msg_size = cfg->msg_size;
	f.stream_id = 1;	/* app-assigned, non-zero (stream 0 reserved) */
	if (pingpong) {
		f.send_lo = 0;
		f.send_n = W;
	} else if (gen) {	/* stream source: every buffer sources */
		f.send_lo = 0;
		f.send_n = count;
	}			/* sink / reflector: send_n stays 0 */
	f.bstate = calloc(count, 1);
	f.recv_pool = calloc(count, 1);
	if (!f.bstate || !f.recv_pool)
		fail(cfg, "oom", 0);
	r->fast = &f;
	r->use_fast = 1;

	r->fd = open(URP_CMD_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (r->fd < 0)
		fail(cfg, "open_urp", -errno);
	/* Fast RECV completes as a 32-byte CQE (payload len in res, buf_index |
	 * stream_id in res2). A CQE32 ring also carries SEND's 16-byte
	 * completion safely, so one ring width serves every role. */
	res = io_uring_queue_init(entries, &r->ring, IORING_SETUP_CQE32);
	if (res < 0)
		fail(cfg, "queue_init", res);

	f.pool_len = (size_t)buf_size * count;
	f.pool = mmap(NULL, f.pool_len, PROT_READ | PROT_WRITE,
		      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (f.pool == MAP_FAILED)
		fail(cfg, "mmap", -errno);

	/*
	 * REGISTER can transiently fail while the RC session is still settling:
	 * -ENOTCONN before the QP establishes, and -EIO if the QP is momentarily
	 * draining. The latter is the interop startup race (design 31 D1): a
	 * uds-initiator peer emits keepalive PINGs and re-dials on its own probe
	 * timeout until this fast acceptor has armed recvs to answer them, so the
	 * QP churns and REG_MR lands on a draining QP. Each fresh establishment
	 * gives a brief healthy window; retry on those two codes until REGISTER
	 * lands, after which the armed recvs below keep the peer's probe PONGed and
	 * the session stops churning. Other errors (e.g. -EINVAL misalign) are
	 * permanent -- fail immediately.
	 */
	{
		uint64_t reg_deadline = now_ns() + 8ULL * 1000000000ULL;

		for (;;) {
			res = fast_register(&f, r->fast_endpoint);
			if (res == 0 || (res != -EIO && res != -ENOTCONN))
				break;
			if (now_ns() >= reg_deadline)
				break;
			usleep(150000);
		}
	}
	if (res != 0)
		fail(cfg, "register", res);

	/* Recv-pool assignment + initial arming (flushed on the first submit). */
	if (pingpong) {
		for (i = W; i < count; i++) {
			f.recv_pool[i] = 1;
			fast_post_recv(&f, i);
		}
	} else if (!gen) {		/* sink / reflector: every buffer recvs */
		for (i = 0; i < count; i++) {
			f.recv_pool[i] = 1;
			fast_post_recv(&f, i);
		}
	}

	hard_deadline = now_ns() +
			(FIN_TIMEOUT_S +
			 (r->deadline_ns ? (uint64_t)cfg->duration_s :
					   cfg->count / 100000 + 30)) *
				1000000000ull;

	while (!fast_done(&f, gen, pingpong, reflect) && r->rc >= 0) {
		struct io_uring_cqe *cqe;
		unsigned head, seen = 0;
		uint64_t now = now_ns();

		/* --- originate (source / pinger) --- */
		if (gen) {
			for (i = f.send_lo; i < f.send_lo + f.send_n; i++) {
				if (f.bstate[i] != FB_FREE)
					continue;
				if (!fast_more_data(r, now))
					break;
				/* pinger: never exceed the RTT tracker window. */
				if (pingpong && r->tracker.inflight_count >= W)
					break;
				if (!fast_gen_one(&f, i, now, pingpong))
					break;	/* tracker momentarily full */
			}
			/* All data delivered (send completions drained — RC ACK
			 * means the peer's RQ already has every frame, so a FIN
			 * posted now cannot overtake data even across QPs) and,
			 * for a pinger, every echo home: emit the FIN. */
			if (!r->own_fin_sent && !fast_more_data(r, now) &&
			    fast_state_count(&f, FB_SEND) == 0 &&
			    (!pingpong || r->tracker.inflight_count == 0)) {
				for (i = f.send_lo; i < f.send_lo + f.send_n;
				     i++) {
					if (f.bstate[i] != FB_FREE)
						continue;
					fast_gen_fin(&f, i, now, pingpong);
					break;
				}
			}
		}

		r->syscalls++;
		res = io_uring_submit_and_wait(&r->ring, 1);
		if (res < 0 && res != -EINTR) {
			r->rc = res;
			break;
		}
		io_uring_for_each_cqe(&r->ring, head, cqe) {
			uint32_t idx = UD_IDX(cqe->user_data);
			int kind = UD_KIND(cqe->user_data);
			int32_t cres = cqe->res;

			seen++;
			if (idx >= count)
				continue;
			if (kind == UD_KIND_SEND) {
				if (cres < 0) {
					r->rc = cres;
					continue;
				}
				if (f.recv_pool[idx])
					fast_post_recv(&f, idx);  /* reflection */
				else
					f.bstate[idx] = FB_FREE;  /* original */
			} else {		/* UD_KIND_RECV */
				struct msg_ctx mc;
				uint8_t *frame;

				if (cres < 0) {
					/* RX starvation / RNR / drain surfaces
					 * here rather than silently dropping. */
					r->rc = cres;
					f.bstate[idx] = FB_FREE;
					continue;
				}
				frame = fast_frame(&f, idx);
				mc.r = r;
				mc.chunk = frame;
				mc.chunk_len = (size_t)cres;
				mc.recv_idx = (int)idx;
				/* tentatively free; on_msg's reflect flips to
				 * FB_SEND, so re-arm only if it did not. */
				f.bstate[idx] = FB_FREE;
				res = bench_deframe_feed(&r->df, frame,
							 (size_t)cres, on_msg,
							 &mc);
				if (res < 0) {
					r->rc = res;
					continue;
				}
				if (f.bstate[idx] == FB_FREE)
					fast_post_recv(&f, idx);
			}
		}
		io_uring_cq_advance(&r->ring, seen);

		if (now_ns() > hard_deadline)
			fail(cfg, "timeout", 0);
	}

	munmap(f.pool, f.pool_len);
	io_uring_queue_exit(&r->ring);	/* cancels in-flight recvs */
	close(r->fd);			/* release drains the RQ + unpins */
	free(f.bstate);
	free(f.recv_pool);
	r->fast = NULL;
	return r->rc < 0 ? (int)r->rc : 0;
}

/* ---- memcpy yardstick -------------------------------------------------- */

static void memcpy_baseline(uint32_t msg_size)
{
	uint8_t *a = malloc(msg_size), *b = malloc(msg_size);
	uint64_t start, elapsed, iters = 0;
	double mbps;

	if (!a || !b)
		exit(1);
	memset(a, 0xa5, msg_size);
	start = now_ns();
	do {
		memcpy(b, a, msg_size);
		iters++;
		elapsed = now_ns() - start;
	} while (elapsed < 300000000ull);
	mbps = (double)iters * msg_size / 1e6 / ((double)elapsed / 1e9);
	printf("BENCH_MEMCPY msg_size=%u mbps=%.1f\n", msg_size, mbps);
	free(a);
	free(b);
}

/* ---- main -------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr,
		"usage: urp-bench (--listen PATH | --connect PATH) --id N\n"
		"  --mode {blocking,uring-rw,uring-fixed,uring-bufring,\n"
		"          uring-sqpoll,uring-sendzc,uring-cmd}\n"
		"  --msg-size BYTES --batch N (--count N | --duration S)\n"
		"  [--verify {none,header,full}] [--pattern {echo,stream}]\n"
		"  [--defer-taskrun] [--memcpy-baseline] [--quiet-zc]\n"
		"  [--fast-endpoint NAME]  (mode uring-cmd: zero-copy fast path,\n"
		"    design 31; REGISTERs a pool against the `urp add --kind fast`\n"
		"    endpoint NAME on /dev/urp; --listen/--connect select role only)\n"
		"  (pattern stream: connect=source, listen=sink; blocking mode)\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct bench_config cfg = {
		.mode = BENCH_MODE_URING_RW,
		.verify = BENCH_VERIFY_HEADER,
		.msg_size = 4076,
		.batch = 32,
	};
	const char *path = NULL;
	const char *fast_endpoint = NULL;
	int do_memcpy = 0;
	struct run r = { 0 };
	struct rusage ru0, ru1;
	uint64_t t0, t1, cpu_ns;
	struct bench_stats_result rtt = { 0 };
	struct bench_report rep;
	char line[512];
	uint32_t i;
	int a, ret;

	/* A stream source keeps writing after the sink may have closed; take
	 * EPIPE as an error instead of a fatal signal. */
	signal(SIGPIPE, SIG_IGN);

	for (a = 1; a < argc; a++) {
		const char *s = argv[a];

		if (!strcmp(s, "--listen") && a + 1 < argc) {
			cfg.role = BENCH_ROLE_LISTEN;
			path = argv[++a];
		} else if (!strcmp(s, "--connect") && a + 1 < argc) {
			cfg.role = BENCH_ROLE_CONNECT;
			path = argv[++a];
		} else if (!strcmp(s, "--id") && a + 1 < argc) {
			cfg.id = (uint16_t)strtoul(argv[++a], NULL, 0);
		} else if (!strcmp(s, "--mode") && a + 1 < argc) {
			if (bench_mode_parse(argv[++a], &cfg.mode) < 0)
				usage();
		} else if (!strcmp(s, "--msg-size") && a + 1 < argc) {
			cfg.msg_size = (uint32_t)strtoul(argv[++a], NULL, 0);
		} else if (!strcmp(s, "--batch") && a + 1 < argc) {
			cfg.batch = (uint32_t)strtoul(argv[++a], NULL, 0);
		} else if (!strcmp(s, "--count") && a + 1 < argc) {
			cfg.count = strtoull(argv[++a], NULL, 0);
		} else if (!strcmp(s, "--duration") && a + 1 < argc) {
			cfg.duration_s = (uint32_t)strtoul(argv[++a], NULL, 0);
		} else if (!strcmp(s, "--verify") && a + 1 < argc) {
			if (bench_verify_parse(argv[++a], &cfg.verify) < 0)
				usage();
		} else if (!strcmp(s, "--pattern") && a + 1 < argc) {
			if (bench_pattern_parse(argv[++a], &cfg.pattern) < 0)
				usage();
		} else if (!strcmp(s, "--fast-endpoint") && a + 1 < argc) {
			fast_endpoint = argv[++a];
		} else if (!strcmp(s, "--defer-taskrun")) {
			cfg.defer_taskrun = 1;
		} else if (!strcmp(s, "--memcpy-baseline")) {
			do_memcpy = 1;
		} else {
			usage();
		}
	}

	if (do_memcpy) {
		memcpy_baseline(cfg.msg_size);
		return 0;
	}
	if (bench_config_validate(&cfg) < 0)
		usage();
	/* The fast path is addressed by endpoint name (--fast-endpoint), the
	 * socket paths by --listen/--connect; --listen/--connect still select
	 * the role for fast (their path arg is unused). */
	if (cfg.mode == BENCH_MODE_URING_CMD ? !fast_endpoint : !path)
		usage();

	r.cfg = &cfg;
	r.fast_endpoint = fast_endpoint;
	/* Role split (§34.4): echo sides both source+reflect; a stream source
	 * (connect) only sources, a stream sink (listen) only drains. */
	r.do_generate = cfg.pattern == BENCH_PATTERN_ECHO ||
			cfg.role == BENCH_ROLE_CONNECT;
	r.do_echo = cfg.pattern == BENCH_PATTERN_ECHO;
	/* The fast echo is an asymmetric ping-pong (design 31): the connect side
	 * pings and measures RTT, the listen side only reflects — unlike the
	 * socket's symmetric peer echo, where both sides generate and reflect. */
	if (cfg.mode == BENCH_MODE_URING_CMD) {
		r.do_generate = cfg.role == BENCH_ROLE_CONNECT;
		r.do_echo = cfg.pattern == BENCH_PATTERN_ECHO &&
			    cfg.role == BENCH_ROLE_LISTEN;
	}
	r.src_carry_empty = 1;
	r.goal = cfg.count ? cfg.count : ~0ull;
	r.n_rbufs = cfg.batch * 2 < 4 ? 4 : (cfg.batch * 2 > 64 ? 64 :
					     cfg.batch * 2);
	if (cfg.mode == BENCH_MODE_BLOCKING)
		r.n_rbufs = 1;
	r.n_scratch = cfg.batch + 4 > 64 ? 64 : cfg.batch + 4;
	r.scratch_sz = cfg.msg_size;
	r.n_sends = 2 * cfg.batch + r.n_rbufs + r.n_scratch + 8;

	r.rbufs = calloc(r.n_rbufs, sizeof(*r.rbufs));
	r.sends = calloc(r.n_sends, sizeof(*r.sends));
	r.send_slots = malloc((size_t)cfg.batch * cfg.msg_size);
	r.slot_free = malloc(cfg.batch);
	r.scratch = calloc(r.n_scratch, sizeof(*r.scratch));
	r.scratch_busy = calloc(r.n_scratch, 1);
	r.asm_buf = malloc(cfg.msg_size);
	if (!r.rbufs || !r.sends || !r.send_slots || !r.slot_free ||
	    !r.scratch || !r.scratch_busy || !r.asm_buf)
		fail(&cfg, "oom", 0);
	for (i = 0; i < r.n_rbufs; i++) {
		r.rbufs[i].data = malloc(RECV_BUF_SZ);
		if (!r.rbufs[i].data)
			fail(&cfg, "oom", 0);
	}
	for (i = 0; i < r.n_scratch; i++) {
		r.scratch[i] = malloc(r.scratch_sz);
		if (!r.scratch[i])
			fail(&cfg, "oom", 0);
	}
	memset(r.slot_free, 1, cfg.batch);
	memset(r.send_slots, 0x5a, (size_t)cfg.batch * cfg.msg_size);

	{
		uint64_t *sent_ns = calloc(cfg.batch, sizeof(uint64_t));
		uint32_t *seqs = calloc(cfg.batch, sizeof(uint32_t));
		uint8_t *infl = calloc(cfg.batch, 1);
		uint64_t *samples = calloc(SAMPLE_CAP, sizeof(uint64_t));

		if (!sent_ns || !seqs || !infl || !samples)
			fail(&cfg, "oom", 0);
		bench_tracker_init(&r.tracker, sent_ns, seqs, infl,
				   cfg.batch);
		bench_stats_init(&r.stats, samples, SAMPLE_CAP);
	}
	bench_deframer_init(&r.df, r.asm_buf, cfg.msg_size,
			    cfg.msg_size - BENCH_HDR_SIZE);

	if (cfg.mode == BENCH_MODE_URING_CMD) {
		/* run_fast opens /dev/urp and REGISTERs its own pool; there is
		 * no socket. The endpoint's RC connection is already up (fast
		 * initiator dials eagerly at activate). */
		if (cfg.duration_s)
			r.deadline_ns = now_ns() + (uint64_t)cfg.duration_s *
						   1000000000ull;
		getrusage(RUSAGE_SELF, &ru0);
		t0 = now_ns();
		ret = run_fast(&r);
		t1 = now_ns();
		getrusage(RUSAGE_SELF, &ru1);
	} else {
		r.fd = cfg.role == BENCH_ROLE_LISTEN ? uds_listen_accept(path) :
						       uds_connect(path);
		if (r.fd < 0)
			fail(&cfg, "socket", -errno);
		if (cfg.mode == BENCH_MODE_BLOCKING &&
		    fcntl(r.fd, F_SETFL, O_NONBLOCK) < 0)
			fail(&cfg, "nonblock", -errno);
		if (cfg.duration_s)
			r.deadline_ns = now_ns() + (uint64_t)cfg.duration_s *
						   1000000000ull;

		getrusage(RUSAGE_SELF, &ru0);
		t0 = now_ns();
		ret = cfg.mode == BENCH_MODE_BLOCKING ? run_blocking(&r) :
							run_uring(&r);
		t1 = now_ns();
		getrusage(RUSAGE_SELF, &ru1);
	}

	if (ret < 0)
		fail(&cfg, "run", ret);
	if (cfg.pattern == BENCH_PATTERN_ECHO) {
		if (r.peer_closed && !(r.own_fin_echoed && r.peer_fin_seen))
			fail(&cfg, "peer_closed_early", 0);
		if (bench_stats_finalize(&r.stats, &rtt) < 0 && cfg.count > 0)
			fail(&cfg, "no_samples", 0);
	} else if (cfg.role == BENCH_ROLE_LISTEN && r.peer_closed &&
		   !r.peer_fin_seen) {
		/* stream sink: peer closed before its FIN => truncated. */
		fail(&cfg, "peer_closed_early", 0);
	}

	cpu_ns = ((uint64_t)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) *
			  1000000ull +
		  (uint64_t)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) +
		  (uint64_t)(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) *
			  1000000ull +
		  (uint64_t)(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec)) *
		 1000ull;

	rep = (struct bench_report){
		.lang = "c",
		.cfg = &cfg,
		.rtt = rtt,
		.msgs = r.sent_originals,
		.bytes = r.bytes_echoed_back,
		.elapsed_ns = t1 - t0,
		.syscalls = r.syscalls,
		.cpu_ns = cpu_ns,
		.reassembled = r.df.msgs_reassembled,
		.msgs_rx_total = r.msgs_rx_total,
	};
	if (cfg.pattern == BENCH_PATTERN_STREAM) {
		if (cfg.role == BENCH_ROLE_LISTEN) {
			/* sink: authoritative goodput = bytes delivered over the
			 * first-frame..FIN window. */
			uint64_t win = r.t_stream_end_ns > r.t_first_rx_ns ?
				       r.t_stream_end_ns - r.t_first_rx_ns :
				       (t1 - t0);
			rep.msgs = r.rx_data_msgs;
			rep.bytes = r.rx_data_bytes;
			rep.elapsed_ns = win;
		} else {
			/* source: bytes handed to the socket (secondary). */
			rep.bytes = r.tx_wire_bytes;
		}
	}
	if (bench_format_result(&rep, line, sizeof(line)) < 0)
		fail(&cfg, "format", 0);
	puts(line);
	if (r.use_sendzc)
		printf("BENCH_ZC sends=%llu copied=%llu\n",
		       (unsigned long long)r.zc_sends,
		       (unsigned long long)r.zc_copied);
	return 0;
}
