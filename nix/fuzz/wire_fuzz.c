// SPDX-License-Identifier: GPL-2.0
/*
 * wire_fuzz -- a hostile-peer RDMA wire fuzzer for the "urp" kernel module
 * (design 27 F2, surfaces S1 + S2). This is the remotely-reachable attack
 * surface: a compromised RDMA peer that completes the CM handshake and then
 * injects arbitrary/malformed frames into the acceptor's RX path
 * (urp_recv_done -> urp_classify_frame -> urp_stream_rx_dispatch).
 *
 * Unlike the cooperative pair test -- where both ends are friendly and only
 * ever send well-formed DATA -- this peer is adversarial:
 *   - length-guard classes: frames < 20 B, payload_len > URP_MAX_PAYLOAD,
 *     payload_len > wire byte_len (the classifier's overrun guard), PROBE
 *     frames shorter than the 52-byte PING/PONG minimum;
 *   - frame-type fuzzing: unknown types 0x03..0xFF (fall through to DATA),
 *     CONTROL/CREDIT with peer-chosen credit counts, PROBE PING/PONG;
 *   - and, most importantly, the stream state machine: scripted
 *     SYN/FIN/RST sequences on a small stream_id space so streams get
 *     created, torn down, and their ids reused. A SYN on a fresh stream_id
 *     makes the acceptor open a backend UDS + spawn a pump kthread; a
 *     following RST drives urp_stream_destroy -> kthread_stop -> sock_release
 *     -> call_rcu. That is the "RST-under-RCU" / kthread-lifecycle path the
 *     friendly pair test structurally cannot reach.
 *
 * Runs INSIDE a VM whose peer VM has the urp acceptor loaded with KASAN /
 * KMEMLEAK / lockdep. Bugs surface as reports in the acceptor's dmesg, which
 * the microVM harness sanitizer phase already scrapes -- this binary is not
 * its own oracle, exactly like netlink_fuzz.
 *
 * Targets an endpoint with NO password (so no PSK gate); the S2 PSK path is
 * exercised separately by pointing it at a password-protected endpoint, where
 * it fuzzes the pre-auth private_data blob during connect.
 *
 * Deterministic: PRNG seeded from argv[4] (default 1) so a crashing run
 * reproduces. Resilient: reconnects if the RC QP errors so a single teardown
 * never ends the run early.
 *
 *   usage: wire_fuzz <server-ip> <port> <seconds> [seed]
 *
 * Self-contained: librdmacm + libibverbs + libc. Frame layout mirrors
 * kernel/urp_frame.h (kept in lock-step by hand; see the encoder below).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

/* Wire constants (mirror kernel/include/uapi/linux/urp.h + urp_frame.h). */
#define URP_FRAME_HEADER_SIZE	20
#define URP_BUF_SIZE		4096
#define URP_MAX_PAYLOAD		(URP_BUF_SIZE - URP_FRAME_HEADER_SIZE)	/* 4076 */

#define URP_FRAME_TYPE_DATA	0x00
#define URP_FRAME_TYPE_CONTROL	0x01
#define URP_FRAME_TYPE_PROBE	0x02

/* DATA flags */
#define URP_DATA_FLAG_SYN	(1u << 0)
#define URP_DATA_FLAG_FIN	(1u << 1)
#define URP_DATA_FLAG_RST	(1u << 2)
/* CONTROL flags */
#define URP_CTRL_FLAG_CREDIT	(1u << 0)
/* PROBE flags */
#define URP_PROBE_FLAG_PONG	(1u << 0)
#define URP_PROBE_PAYLOAD_SIZE	32	/* PING minimum; PONG is 48 */

/* QP sizing for the fuzzer side -- deeper than the test client so we can
 * keep frames in flight without stalling on completions.
 */
#define SQ_DEPTH		64
#define RQ_DEPTH		64
#define RECV_POOL		32

/* xorshift64 -- deterministic, reproducible from the seed. */
static uint64_t prng_state = 1;
static uint64_t xrand(void)
{
	uint64_t x = prng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	prng_state = x;
	return x;
}
static uint32_t xrand_below(uint32_t n)
{
	return n ? (uint32_t)(xrand() % n) : 0;
}

struct fuzz_ctx {
	struct rdma_event_channel *ec;
	struct rdma_cm_id	*id;
	struct ibv_pd		*pd;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	struct ibv_mr		*send_mr;
	struct ibv_mr		*recv_mr;
	/* One send buffer per in-flight WR (round-robin) + a recv pool. */
	char			send_buf[SQ_DEPTH][URP_BUF_SIZE];
	char			recv_buf[RECV_POOL][URP_BUF_SIZE];
	unsigned int		send_slot;
	int			connected;
};

/* Little-endian frame-header encoder. Byte-for-byte identical to
 * urp_frame_encode() in kernel/urp_frame.h -- the point of the fuzzer is to
 * feed the kernel decoder legal-looking headers with hostile field values,
 * so the encoding itself must match exactly.
 */
static void urp_frame_encode(void *buf, uint32_t stream_id, uint64_t seq,
			     uint8_t frame_type, uint8_t flags,
			     uint16_t credits, uint32_t payload_len)
{
	uint8_t *p = buf;

	p[0] = stream_id & 0xFF;
	p[1] = (stream_id >> 8) & 0xFF;
	p[2] = (stream_id >> 16) & 0xFF;
	p[3] = (stream_id >> 24) & 0xFF;

	p[4] = seq & 0xFF;
	p[5] = (seq >> 8) & 0xFF;
	p[6] = (seq >> 16) & 0xFF;
	p[7] = (seq >> 24) & 0xFF;
	p[8] = (seq >> 32) & 0xFF;
	p[9] = (seq >> 40) & 0xFF;
	p[10] = (seq >> 48) & 0xFF;
	p[11] = (seq >> 56) & 0xFF;

	p[12] = frame_type;
	p[13] = flags;

	p[14] = credits & 0xFF;
	p[15] = (credits >> 8) & 0xFF;

	p[16] = payload_len & 0xFF;
	p[17] = (payload_len >> 8) & 0xFF;
	p[18] = (payload_len >> 16) & 0xFF;
	p[19] = (payload_len >> 24) & 0xFF;
}

static int wait_for_event(struct fuzz_ctx *ctx, enum rdma_cm_event_type want)
{
	struct rdma_cm_event *event;

	if (rdma_get_cm_event(ctx->ec, &event)) {
		perror("rdma_get_cm_event");
		return -1;
	}
	if (event->event != want) {
		fprintf(stderr, "WIRE_FUZZ: expected CM event %d, got %d (%s)\n",
			want, event->event, rdma_event_str(event->event));
		rdma_ack_cm_event(event);
		return -1;
	}
	rdma_ack_cm_event(event);
	return 0;
}

static int post_recv(struct fuzz_ctx *ctx, int slot)
{
	struct ibv_recv_wr wr = {}, *bad;
	struct ibv_sge sge;

	sge.addr = (uintptr_t)ctx->recv_buf[slot];
	sge.length = URP_BUF_SIZE;
	sge.lkey = ctx->recv_mr->lkey;
	wr.wr_id = (uint64_t)slot;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	return ibv_post_recv(ctx->qp, &wr, &bad);
}

static int setup_qp(struct fuzz_ctx *ctx)
{
	struct ibv_qp_init_attr attr = {};
	int i;

	ctx->pd = ibv_alloc_pd(ctx->id->verbs);
	if (!ctx->pd)
		return -1;
	ctx->cq = ibv_create_cq(ctx->id->verbs, SQ_DEPTH + RQ_DEPTH,
				NULL, NULL, 0);
	if (!ctx->cq)
		return -1;
	ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, sizeof(ctx->send_buf),
				  IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->send_mr)
		return -1;
	ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, sizeof(ctx->recv_buf),
				  IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->recv_mr)
		return -1;

	attr.send_cq = ctx->cq;
	attr.recv_cq = ctx->cq;
	attr.cap.max_send_wr = SQ_DEPTH;
	attr.cap.max_recv_wr = RQ_DEPTH;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.qp_type = IBV_QPT_RC;
	attr.sq_sig_all = 1;
	if (rdma_create_qp(ctx->id, ctx->pd, &attr))
		return -1;
	ctx->qp = ctx->id->qp;

	for (i = 0; i < RECV_POOL; i++)
		if (post_recv(ctx, i))
			return -1;
	return 0;
}

static void teardown_qp(struct fuzz_ctx *ctx)
{
	if (ctx->id && ctx->qp)
		rdma_destroy_qp(ctx->id);
	if (ctx->recv_mr)
		ibv_dereg_mr(ctx->recv_mr);
	if (ctx->send_mr)
		ibv_dereg_mr(ctx->send_mr);
	if (ctx->cq)
		ibv_destroy_cq(ctx->cq);
	if (ctx->pd)
		ibv_dealloc_pd(ctx->pd);
	if (ctx->id)
		rdma_destroy_id(ctx->id);
	ctx->qp = NULL;
	ctx->recv_mr = ctx->send_mr = NULL;
	ctx->cq = NULL;
	ctx->pd = NULL;
	ctx->id = NULL;
	ctx->connected = 0;
}

/* Establish one RC connection to the acceptor. `psk` (may be NULL) is sent as
 * CM private_data -- for a no-password endpoint it is ignored; for a
 * password-protected one this is the S2 pre-auth fuzz surface.
 */
static int do_connect(struct fuzz_ctx *ctx, const char *server, int port,
		      const void *psk, uint8_t psk_len)
{
	struct sockaddr_in addr = {};
	struct rdma_conn_param param = {};

	if (rdma_create_id(ctx->ec, &ctx->id, NULL, RDMA_PS_TCP))
		return -1;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, server, &addr.sin_addr);

	if (rdma_resolve_addr(ctx->id, NULL, (struct sockaddr *)&addr, 2000) ||
	    wait_for_event(ctx, RDMA_CM_EVENT_ADDR_RESOLVED))
		return -1;
	if (rdma_resolve_route(ctx->id, 2000) ||
	    wait_for_event(ctx, RDMA_CM_EVENT_ROUTE_RESOLVED))
		return -1;
	if (setup_qp(ctx))
		return -1;

	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.retry_count = 7;
	param.rnr_retry_count = 7;
	if (psk && psk_len) {
		param.private_data = psk;
		param.private_data_len = psk_len;
	}
	if (rdma_connect(ctx->id, &param) ||
	    wait_for_event(ctx, RDMA_CM_EVENT_ESTABLISHED))
		return -1;

	ctx->connected = 1;
	return 0;
}

/* Drain the CQ. Returns -1 if any completion came back in error (the QP has
 * gone to error state -> caller reconnects); reposts recv buffers.
 */
static int drain_cq(struct fuzz_ctx *ctx)
{
	struct ibv_wc wc[16];
	int n, i, bad = 0;

	while ((n = ibv_poll_cq(ctx->cq, 16, wc)) > 0) {
		for (i = 0; i < n; i++) {
			if (wc[i].status != IBV_WC_SUCCESS)
				bad = 1;
			/* Recv completion: repost the buffer (discard data). */
			if (wc[i].opcode & IBV_WC_RECV)
				(void)post_recv(ctx, (int)wc[i].wr_id);
		}
	}
	if (n < 0)
		bad = 1;
	return bad ? -1 : 0;
}

/* Post one send WR of `len` bytes from the current round-robin send slot. */
static int post_frame(struct fuzz_ctx *ctx, char *buf, uint32_t len)
{
	struct ibv_send_wr wr = {}, *bad;
	struct ibv_sge sge;

	sge.addr = (uintptr_t)buf;
	sge.length = len;
	sge.lkey = ctx->send_mr->lkey;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	return ibv_post_send(ctx->qp, &wr, &bad);
}

/* Compose one hostile frame into `buf`; return the number of bytes to put on
 * the wire (the receiver's wc->byte_len). The header's payload_len field is
 * mutated INDEPENDENTLY of the wire length so the classifier's overrun and
 * oversize guards are exercised.
 *
 * `stream_id` and `flags` are chosen by the caller so multi-frame stream
 * scripts (SYN then RST, ...) can drive the state machine deterministically;
 * pass flags==0xFFFF to let this function pick.
 */
static uint32_t build_frame(char *buf, uint32_t stream_id, unsigned int flags_in)
{
	uint8_t type, flags;
	uint16_t credits;
	uint32_t payload_field, wire_payload, wire_len;
	uint32_t k;

	/* Pick a frame type. Bias toward DATA (the state machine) but reach
	 * CONTROL, PROBE and the unknown-type fall-through band.
	 */
	switch (xrand_below(6)) {
	case 0: case 1: case 2:
		type = URP_FRAME_TYPE_DATA;
		break;
	case 3:
		type = URP_FRAME_TYPE_CONTROL;
		break;
	case 4:
		type = URP_FRAME_TYPE_PROBE;
		break;
	default:
		/* Unknown 0x03..0xFF -> classifier falls through to DATA. */
		type = (uint8_t)(3 + xrand_below(253));
		break;
	}

	if (flags_in != 0xFFFF) {
		flags = (uint8_t)flags_in;
	} else {
		/* Random flag soup incl. garbage high bits. */
		flags = (uint8_t)xrand();
	}
	credits = (uint16_t)xrand();

	/* payload_len HEADER field: mostly plausible, sometimes hostile
	 * (> URP_MAX_PAYLOAD, or huge) to hit DROP_OVERSIZE / overrun.
	 */
	switch (xrand_below(8)) {
	case 0:
		payload_field = 0xFFFFFFFFu;
		break;
	case 1:
		payload_field = URP_MAX_PAYLOAD + 1 + xrand_below(4096);
		break;
	case 2:
		payload_field = xrand();		/* full 32-bit range */
		break;
	default:
		payload_field = xrand_below(URP_MAX_PAYLOAD + 1);
		break;
	}

	/* Actual bytes present after the header (bounded by the buffer). */
	wire_payload = xrand_below(URP_MAX_PAYLOAD + 1);

	urp_frame_encode(buf, stream_id, xrand(), type, flags, credits,
			 payload_field);
	for (k = 0; k < wire_payload; k++)
		buf[URP_FRAME_HEADER_SIZE + k] = (char)xrand();

	/* Wire length. Usually header + payload, but sometimes a runt frame
	 * (< 20 B, hits DROP_SHORT) or a bare header.
	 */
	switch (xrand_below(16)) {
	case 0:
		wire_len = xrand_below(URP_FRAME_HEADER_SIZE);	/* runt */
		break;
	case 1:
		wire_len = URP_FRAME_HEADER_SIZE;		/* header only */
		break;
	default:
		wire_len = URP_FRAME_HEADER_SIZE + wire_payload;
		break;
	}
	if (wire_len > URP_BUF_SIZE)
		wire_len = URP_BUF_SIZE;
	return wire_len;
}

/* Emit a scripted multi-frame stream sequence on one stream_id to drive the
 * SYN/FIN/RST state machine and the SYN->kthread->RST->destroy lifecycle.
 * Returns the number of frames posted (caller drains after).
 */
static int emit_stream_script(struct fuzz_ctx *ctx, uint32_t stream_id)
{
	static const unsigned int scripts[][3] = {
		{ URP_DATA_FLAG_SYN, URP_DATA_FLAG_RST, 0xFFFF },              /* SYN, RST */
		{ URP_DATA_FLAG_SYN, URP_DATA_FLAG_FIN, 0xFFFF },              /* SYN, FIN */
		{ URP_DATA_FLAG_SYN | URP_DATA_FLAG_RST, 0xFFFF, 0xFFFF },     /* SYN+RST one frame */
		{ URP_DATA_FLAG_RST, 0xFFFF, 0xFFFF },                        /* RST, no SYN */
		{ URP_DATA_FLAG_SYN, 0, URP_DATA_FLAG_RST },                  /* SYN, data, RST */
		{ URP_DATA_FLAG_RST, URP_DATA_FLAG_RST, 0xFFFF },             /* double RST */
		{ URP_DATA_FLAG_SYN, URP_DATA_FLAG_SYN, 0xFFFF },             /* SYN, SYN (reuse) */
		{ URP_DATA_FLAG_FIN, URP_DATA_FLAG_RST, 0xFFFF },             /* FIN, RST */
	};
	const unsigned int *seq = scripts[xrand_below(8)];
	int posted = 0, i;

	for (i = 0; i < 3; i++) {
		char *buf;
		uint32_t len;

		if (seq[i] == 0xFFFF && i > 0)
			break;
		buf = ctx->send_buf[ctx->send_slot % SQ_DEPTH];
		ctx->send_slot++;
		/* Force DATA type so the flags actually reach the state machine;
		 * build_frame's type roll would otherwise often pick non-DATA.
		 */
		len = build_frame(buf, stream_id, seq[i] == 0xFFFF ? 0 : seq[i]);
		buf[12] = URP_FRAME_TYPE_DATA;	/* frame_type offset */
		if (post_frame(ctx, buf, len))
			break;
		posted++;
	}
	return posted;
}

int main(int argc, char **argv)
{
	struct fuzz_ctx *ctx;
	const char *server;
	int port;
	long seconds;
	unsigned long frames = 0, reconnects = 0;
	time_t start;

	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <server-ip> <port> <seconds> [seed]\n",
			argv[0]);
		return 1;
	}
	server = argv[1];
	port = atoi(argv[2]);
	seconds = atol(argv[3]);
	if (argc > 4)
		prng_state = strtoull(argv[4], NULL, 0);
	if (prng_state == 0)
		prng_state = 1;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		perror("calloc");
		return 2;
	}
	ctx->ec = rdma_create_event_channel();
	if (!ctx->ec) {
		perror("rdma_create_event_channel");
		return 2;
	}

	if (do_connect(ctx, server, port, NULL, 0)) {
		fprintf(stderr,
			"WIRE_FUZZ: initial connect to %s:%d failed (acceptor up?)\n",
			server, port);
		teardown_qp(ctx);
		rdma_destroy_event_channel(ctx->ec);
		free(ctx);
		return 3;
	}
	printf("WIRE_FUZZ: connected to %s:%d, fuzzing %lds seed=%llu\n",
	       server, port, seconds, (unsigned long long)prng_state);
	fflush(stdout);

	start = time(NULL);
	while (time(NULL) - start < seconds) {
		int batch;

		for (batch = 0; batch < 256; batch++) {
			/* ~1 in 4 iterations: a scripted stream sequence on a
			 * small id space (collisions -> reuse/RST races).
			 * Otherwise: a single free-form hostile frame.
			 */
			if (xrand_below(4) == 0) {
				frames += emit_stream_script(ctx,
					1 + xrand_below(64));
			} else {
				char *buf = ctx->send_buf[ctx->send_slot % SQ_DEPTH];
				uint32_t sid = xrand_below(4) == 0
					? 0 : 1 + xrand_below(64);
				uint32_t len;

				ctx->send_slot++;
				len = build_frame(buf, sid, 0xFFFF);
				if (post_frame(ctx, buf, len) == 0)
					frames++;
			}

			/* Keep the SQ from overflowing and repost recvs. */
			if (drain_cq(ctx) < 0) {
				/* QP errored -> reconnect. */
				teardown_qp(ctx);
				if (do_connect(ctx, server, port, NULL, 0)) {
					/* Give the acceptor a moment, retry once. */
					usleep(200000);
					if (do_connect(ctx, server, port, NULL, 0))
						goto done;
				}
				reconnects++;
				break;
			}
		}
	}
done:
	/* Final drain. */
	if (ctx->connected)
		(void)drain_cq(ctx);
	printf("WIRE_FUZZ_DONE frames=%lu reconnects=%lu\n", frames, reconnects);
	fflush(stdout);

	if (ctx->connected)
		rdma_disconnect(ctx->id);
	teardown_qp(ctx);
	rdma_destroy_event_channel(ctx->ec);
	free(ctx);
	return 0;
}
