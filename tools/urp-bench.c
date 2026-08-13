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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "urp-bench-core.h"

#define RECV_BUF_SZ	65536u
#define ECHO_SCRATCH_SZ	(BENCH_HDR_SIZE + BENCH_PAYLOAD_MAX)
#define SAMPLE_CAP	200000
#define FIN_TIMEOUT_S	10

/* user_data encoding: kind in the top byte, index below. */
#define UD_KIND_RECV	1
#define UD_KIND_SEND	2
#define UD(kind, idx)	(((uint64_t)(kind) << 56) | (uint64_t)(idx))
#define UD_KIND(ud)	((int)((ud) >> 56))
#define UD_IDX(ud)	((uint32_t)((ud) & 0xffffffffu))

struct recv_buf {
	uint8_t *data;
	int pending_echoes;	/* in-place echo sends referencing us */
	int in_recv;		/* a recv SQE is outstanding */
	int want_rearm;		/* processed, waiting for echoes to drain */
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

	/* a peer original: echo it back */
	if (hdr->flags & BENCH_FLAG_FIN)
		r->peer_fin_seen = 1;
	if (r->cfg->verify == BENCH_VERIFY_FULL &&
	    bench_verify_payload(payload, hdr->payload_len, hdr->origin_id,
				 hdr->seq) < 0)
		return -BENCH_ECORRUPT;
	if (r->use_uring)
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
	rb->want_rearm = 0;
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

static void queue_original_uring(struct run *r, int fin)
{
	uint32_t slot;
	int op_idx = alloc_send_op(r);
	struct send_op *op;
	struct bench_hdr h;
	uint8_t *buf;
	uint64_t t = now_ns();

	if (op_idx < 0) {
		r->rc = -BENCH_EFULL;
		return;
	}
	for (slot = 0; slot < r->cfg->batch; slot++)
		if (r->slot_free[slot])
			break;
	if (slot == r->cfg->batch) {
		r->rc = -BENCH_EFULL;	/* planner said there was room */
		return;
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

	if (bench_track_sent(&r->tracker, h.seq, t) < 0) {
		r->rc = -BENCH_EFULL;
		return;
	}
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
}

static void release_send(struct run *r, struct send_op *op)
{
	if (op->recv_idx >= 0) {
		struct recv_buf *rb = &r->rbufs[op->recv_idx];

		rb->pending_echoes--;
		if (rb->pending_echoes == 0 && rb->want_rearm)
			prep_recv(r, (uint32_t)op->recv_idx);
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
	if (rb->pending_echoes == 0)
		prep_recv(r, rb_idx);
	else
		rb->want_rearm = 1;
}

static int run_done(const struct run *r)
{
	uint32_t i;

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
		queue_original_uring(r, 0);
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

	r->use_uring = 1;
	for (i = 0; i < r->n_rbufs; i++)
		prep_recv(r, i);

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
			if (UD_KIND(cqe->user_data) == UD_KIND_RECV)
				handle_recv_cqe(r, cqe);
			else
				handle_send_cqe(r, cqe);
		}
		io_uring_cq_advance(&r->ring, seen);
		if (now_ns() > hard_deadline) {
			r->rc = -BENCH_EINVAL;
			fail(cfg, "timeout", 0);
		}
	}
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

static void queue_original_blocking(struct run *r, int fin)
{
	uint8_t buf[BENCH_HDR_SIZE];
	struct bench_hdr h;
	uint64_t t = now_ns();
	int ret;

	h.magic = BENCH_MAGIC;
	h.version = BENCH_VERSION;
	h.flags = fin ? BENCH_FLAG_FIN : 0;
	h.origin_id = r->cfg->id;
	h.payload_len = fin ? 0 : r->cfg->msg_size - BENCH_HDR_SIZE;
	h.seq = (uint32_t)r->next_seq;
	h.t_send_ns = t;
	bench_hdr_encode(&h, buf);
	if (bench_track_sent(&r->tracker, h.seq, t) < 0) {
		r->rc = -BENCH_EFULL;
		return;
	}
	if (fin) {
		r->own_fin_sent = 1;
		r->own_fin_seq = h.seq;
	}
	r->next_seq++;
	r->sent_originals++;

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

		/* top-up (writes drain into the carry buffer + socket) */
		if (!r->own_fin_sent) {
			if (r->deadline_ns && now_ns() >= r->deadline_ns)
				remaining = 0;
			else
				remaining = r->goal > r->sent_originals ?
					    r->goal - r->sent_originals : 0;
			n = bench_batch_plan(&b, r->tracker.inflight_count,
					     remaining);
			while (n-- && r->rc >= 0)
				queue_original_blocking(r, 0);
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
		if (now_ns() > hard_deadline)
			fail(r->cfg, "timeout", 0);
	}
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
		"          uring-sqpoll,uring-sendzc}\n"
		"  --msg-size BYTES --batch N (--count N | --duration S)\n"
		"  [--verify {none,header,full}] [--defer-taskrun]\n"
		"  [--memcpy-baseline] [--quiet-zc]\n");
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
	int do_memcpy = 0;
	struct run r = { 0 };
	struct rusage ru0, ru1;
	uint64_t t0, t1, cpu_ns;
	struct bench_stats_result rtt = { 0 };
	struct bench_report rep;
	char line[512];
	uint32_t i;
	int a, ret;

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
	if (bench_config_validate(&cfg) < 0 || !path)
		usage();
	/* bufring mode is a stub until the PBUF_RING wiring lands (B4+) */
	if (cfg.mode == BENCH_MODE_URING_BUFRING)
		skip(&cfg, "bufring_not_implemented");

	r.cfg = &cfg;
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

	if (ret < 0)
		fail(&cfg, "run", ret);
	if (r.peer_closed && !(r.own_fin_echoed && r.peer_fin_seen))
		fail(&cfg, "peer_closed_early", 0);

	if (bench_stats_finalize(&r.stats, &rtt) < 0 && cfg.count > 0)
		fail(&cfg, "no_samples", 0);

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
	if (bench_format_result(&rep, line, sizeof(line)) < 0)
		fail(&cfg, "format", 0);
	puts(line);
	if (r.use_sendzc)
		printf("BENCH_ZC sends=%llu copied=%llu\n",
		       (unsigned long long)r.zc_sends,
		       (unsigned long long)r.zc_copied);
	return 0;
}
