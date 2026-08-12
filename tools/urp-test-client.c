// SPDX-License-Identifier: GPL-2.0
/*
 * urp-test-client — RDMA CM client for testing the urp kernel module.
 *
 * Connects to the module's RDMA listener, sends URP DATA frames,
 * receives echoed frames, and verifies the payload matches.
 *
 * Modes:
 *   echo       Send/recv N messages, verify payload match (default)
 *   throughput Send SIZE_MB of 4KB data, measure MB/s
 *   latency    N 64-byte roundtrips, measure p50/p99 RTT
 *
 * Usage:
 *   urp-test-client <server-ip> <port> echo [message] [count]
 *   urp-test-client <server-ip> <port> throughput [size_mb]
 *   urp-test-client <server-ip> <port> latency [count]
 *
 * The module must be loaded in acceptor mode with an echo server
 * behind the connect_path UDS socket.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

/* URP frame header (matches kernel/include/uapi/linux/urp.h) */
#define URP_FRAME_HEADER_SIZE	20
#define URP_FRAME_TYPE_DATA	0x00
#define URP_DEFAULT_PORT	4791
#define BUF_SIZE		4096
#define MAX_PAYLOAD		(BUF_SIZE - URP_FRAME_HEADER_SIZE)

struct test_context {
	struct rdma_cm_id	*id;
	struct ibv_pd		*pd;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	struct ibv_mr		*send_mr;
	struct ibv_mr		*recv_mr;
	char			send_buf[BUF_SIZE];
	char			recv_buf[BUF_SIZE];
	struct rdma_event_channel *ec;
};

static void urp_frame_encode(void *buf, uint32_t stream_id, uint64_t seq,
			     uint8_t frame_type, uint8_t flags,
			     uint16_t credits, uint32_t payload_len)
{
	uint8_t *p = buf;

	/* Little-endian encoding matching the kernel module */
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

static uint32_t urp_frame_decode_payload_len(const void *buf)
{
	const uint8_t *p = buf;

	return (uint32_t)p[16] |
	       ((uint32_t)p[17] << 8) |
	       ((uint32_t)p[18] << 16) |
	       ((uint32_t)p[19] << 24);
}

static int wait_for_event(struct test_context *ctx, enum rdma_cm_event_type expected)
{
	struct rdma_cm_event *event;
	int ret;

	ret = rdma_get_cm_event(ctx->ec, &event);
	if (ret) {
		perror("rdma_get_cm_event");
		return -1;
	}

	if (event->event != expected) {
		fprintf(stderr, "expected event %d, got %d (%s)\n",
			expected, event->event, rdma_event_str(event->event));
		rdma_ack_cm_event(event);
		return -1;
	}

	rdma_ack_cm_event(event);
	return 0;
}

static int setup_qp(struct test_context *ctx)
{
	struct ibv_qp_init_attr attr = {};

	ctx->pd = ibv_alloc_pd(ctx->id->verbs);
	if (!ctx->pd) {
		perror("ibv_alloc_pd");
		return -1;
	}

	ctx->cq = ibv_create_cq(ctx->id->verbs, 16, NULL, NULL, 0);
	if (!ctx->cq) {
		perror("ibv_create_cq");
		return -1;
	}

	ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, BUF_SIZE,
				  IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->send_mr) {
		perror("ibv_reg_mr (send)");
		return -1;
	}

	ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, BUF_SIZE,
				  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->recv_mr) {
		perror("ibv_reg_mr (recv)");
		return -1;
	}

	attr.send_cq = ctx->cq;
	attr.recv_cq = ctx->cq;
	attr.cap.max_send_wr = 4;
	attr.cap.max_recv_wr = 4;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.qp_type = IBV_QPT_RC;
	attr.sq_sig_all = 1;

	if (rdma_create_qp(ctx->id, ctx->pd, &attr)) {
		perror("rdma_create_qp");
		return -1;
	}
	ctx->qp = ctx->id->qp;

	return 0;
}

static int post_recv(struct test_context *ctx)
{
	struct ibv_recv_wr wr = {}, *bad;
	struct ibv_sge sge;

	sge.addr = (uintptr_t)ctx->recv_buf;
	sge.length = BUF_SIZE;
	sge.lkey = ctx->recv_mr->lkey;

	wr.sg_list = &sge;
	wr.num_sge = 1;

	return ibv_post_recv(ctx->qp, &wr, &bad);
}

static int post_send(struct test_context *ctx, size_t len)
{
	struct ibv_send_wr wr = {}, *bad;
	struct ibv_sge sge;

	sge.addr = (uintptr_t)ctx->send_buf;
	sge.length = len;
	sge.lkey = ctx->send_mr->lkey;

	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	return ibv_post_send(ctx->qp, &wr, &bad);
}

static int poll_cq(struct test_context *ctx, struct ibv_wc *wc)
{
	int ret;
	int timeout_ms = 5000;

	while (timeout_ms > 0) {
		ret = ibv_poll_cq(ctx->cq, 1, wc);
		if (ret > 0)
			return 0;
		if (ret < 0) {
			perror("ibv_poll_cq");
			return -1;
		}
		usleep(1000);
		timeout_ms--;
	}

	fprintf(stderr, "poll_cq: timeout\n");
	return -1;
}

static void cleanup(struct test_context *ctx)
{
	if (ctx->qp) rdma_destroy_qp(ctx->id);
	if (ctx->recv_mr) ibv_dereg_mr(ctx->recv_mr);
	if (ctx->send_mr) ibv_dereg_mr(ctx->send_mr);
	if (ctx->cq) ibv_destroy_cq(ctx->cq);
	if (ctx->pd) ibv_dealloc_pd(ctx->pd);
	if (ctx->id) rdma_destroy_id(ctx->id);
	if (ctx->ec) rdma_destroy_event_channel(ctx->ec);
}

/* Send one echo roundtrip. Returns 0 on success. */
static int do_echo(struct test_context *ctx, uint64_t seq,
		   const void *payload, size_t payload_len)
{
	struct ibv_wc wc;
	size_t frame_len = URP_FRAME_HEADER_SIZE + payload_len;

	urp_frame_encode(ctx->send_buf, 0, seq,
			 URP_FRAME_TYPE_DATA, 0, 0, payload_len);
	memcpy(ctx->send_buf + URP_FRAME_HEADER_SIZE, payload, payload_len);

	if (post_send(ctx, frame_len)) {
		perror("post_send");
		return -1;
	}
	if (poll_cq(ctx, &wc) || wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "send failed: %s\n",
			ibv_wc_status_str(wc.status));
		return -1;
	}
	if (poll_cq(ctx, &wc) || wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "recv failed: %s\n",
			ibv_wc_status_str(wc.status));
		return -1;
	}
	return 0;
}

static int compare_sort(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;

	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

static uint64_t timespec_diff_ns(struct timespec *start, struct timespec *end)
{
	return (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000ULL +
	       (uint64_t)(end->tv_nsec - start->tv_nsec);
}

/* ---- Mode: echo ---- */

static int mode_echo(struct test_context *ctx, const char *message, int count)
{
	int i, passed = 0, failed = 0;
	size_t msg_len = strlen(message);

	for (i = 0; i < count; i++) {
		if (post_recv(ctx)) {
			perror("post_recv");
			failed++;
			break;
		}

		if (do_echo(ctx, i, message, msg_len)) {
			failed++;
			continue;
		}

		uint32_t recv_payload_len = urp_frame_decode_payload_len(ctx->recv_buf);
		if (recv_payload_len == msg_len &&
		    memcmp(ctx->recv_buf + URP_FRAME_HEADER_SIZE, message,
			   msg_len) == 0) {
			passed++;
		} else {
			fprintf(stderr, "echo mismatch at iteration %d: "
				"sent %zu bytes, got %u bytes\n",
				i, msg_len, recv_payload_len);
			failed++;
		}
	}

	printf("Results: %d/%d passed\n", passed, count);
	return failed > 0 ? 1 : 0;
}

/* ---- Mode: throughput ---- */

static int mode_throughput(struct test_context *ctx, int size_mb)
{
	size_t total = (size_t)size_mb * 1024 * 1024;
	size_t chunk = MAX_PAYLOAD;
	size_t sent = 0;
	uint64_t seq = 0;
	struct timespec start, end;
	double elapsed, mbps;

	/* Fill payload with pattern */
	memset(ctx->send_buf + URP_FRAME_HEADER_SIZE, 'T', chunk);

	printf("Throughput test: %d MB in %zu-byte chunks\n", size_mb, chunk);

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (sent < total) {
		size_t this_chunk = chunk;
		if (sent + this_chunk > total)
			this_chunk = total - sent;

		if (post_recv(ctx)) {
			perror("post_recv");
			return 1;
		}

		if (do_echo(ctx, seq++, ctx->send_buf + URP_FRAME_HEADER_SIZE,
			    this_chunk)) {
			fprintf(stderr, "echo failed at %zu/%zu bytes\n",
				sent, total);
			return 1;
		}

		sent += this_chunk;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	elapsed = (double)timespec_diff_ns(&start, &end) / 1e9;
	mbps = (double)sent / (1024.0 * 1024.0) / elapsed;

	printf("Transferred: %zu bytes (%.1f MB)\n", sent,
	       (double)sent / (1024.0 * 1024.0));
	printf("Time: %.3f s\n", elapsed);
	printf("Throughput: %.1f MB/s\n", mbps);
	printf("Frames: %lu\n", (unsigned long)seq);

	return 0;
}

/* ---- Mode: latency ---- */

static int mode_latency(struct test_context *ctx, int count)
{
	uint64_t *samples;
	struct timespec start, end;
	int i;
	char payload[64];

	samples = calloc(count, sizeof(uint64_t));
	if (!samples) {
		perror("calloc");
		return 1;
	}

	memset(payload, 'L', sizeof(payload));

	printf("Latency test: %d x %zu-byte roundtrips\n", count, sizeof(payload));

	/* Warmup */
	for (i = 0; i < 10 && i < count; i++) {
		if (post_recv(ctx)) break;
		do_echo(ctx, i, payload, sizeof(payload));
	}

	/* Measured runs */
	for (i = 0; i < count; i++) {
		if (post_recv(ctx)) {
			perror("post_recv");
			free(samples);
			return 1;
		}

		clock_gettime(CLOCK_MONOTONIC, &start);

		if (do_echo(ctx, 1000 + i, payload, sizeof(payload))) {
			fprintf(stderr, "echo failed at iteration %d\n", i);
			free(samples);
			return 1;
		}

		clock_gettime(CLOCK_MONOTONIC, &end);
		samples[i] = timespec_diff_ns(&start, &end);
	}

	qsort(samples, count, sizeof(uint64_t), compare_sort);

	uint64_t min = samples[0];
	uint64_t p50 = samples[count / 2];
	uint64_t p99 = samples[(int)(count * 0.99)];
	uint64_t max = samples[count - 1];

	uint64_t sum = 0;
	for (i = 0; i < count; i++)
		sum += samples[i];
	uint64_t avg = sum / count;

	printf("RTT (ns): min=%lu avg=%lu p50=%lu p99=%lu max=%lu\n",
	       (unsigned long)min, (unsigned long)avg,
	       (unsigned long)p50, (unsigned long)p99,
	       (unsigned long)max);

	free(samples);
	return 0;
}

/* ---- Mode: reorder (design 29 Gap 1 / design 28 §28.8.3) ---- */

#ifndef URP_DATA_FLAG_SYN
#define URP_DATA_FLAG_SYN	(1 << 0)
#endif

/*
 * Send one framed stream message and reap its send completion. A stray
 * early echo (RECV) can also land on the shared CQ; skip it and re-post a
 * recv -- echoes are drained politely later. Bounded so a stuck QP fails
 * rather than spins forever.
 */
static int send_frame(struct test_context *ctx, uint32_t stream_id,
		      uint64_t seq, uint8_t flags, size_t payload_len)
{
	struct ibv_wc wc;
	int spins = 5000;	/* ~5 s at 1 ms */

	urp_frame_encode(ctx->send_buf, stream_id, seq, URP_FRAME_TYPE_DATA,
			 flags, 0, payload_len);
	if (post_send(ctx, URP_FRAME_HEADER_SIZE + payload_len)) {
		perror("post_send");
		return -1;
	}

	while (spins-- > 0) {
		int n = ibv_poll_cq(ctx->cq, 1, &wc);

		if (n < 0) {
			perror("ibv_poll_cq");
			return -1;
		}
		if (n == 0) {
			usleep(1000);
			continue;
		}
		if (wc.opcode == IBV_WC_SEND) {
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "send failed: %s\n",
					ibv_wc_status_str(wc.status));
				return -1;
			}
			return 0;
		}
		/* RECV (early echo): re-post and keep waiting for the send. */
		post_recv(ctx);
	}
	fprintf(stderr, "send_frame: no completion for seq %lu\n",
		(unsigned long)seq);
	return -1;
}

/*
 * Open a stream and deliberately send frames OUT OF SEQUENCE so the
 * acceptor's per-stream reorder buffer must restore order before it
 * delivers to the backend UDS socket. Sends SYN (seq 0) first so the
 * stream exists, then swaps adjacent pairs (k+1 before k) -- every
 * higher-of-pair arrives before its predecessor and gets buffered.
 *
 * This proves the WIRING (design 29 Gap 1: that urp_recv_done actually
 * feeds urp_reorder_insert), verified by the acceptor's
 * reorder_insertions / rx_frames / reorder_drops counters in the pair
 * test. The byte-exact in-order delivery guarantee itself is the reorder
 * buffer's own contract, exhaustively covered by fuzz-reorder + KUnit +
 * the Rust twin -- not re-proven here.
 */
static int mode_reorder(struct test_context *ctx, int nframes)
{
	const uint32_t stream_id = 1;	/* odd: initiator-side id */
	const size_t payln = 32;
	int base, i;

	if (nframes < 2)
		nframes = 8;
	if (nframes > 64)
		nframes = 64;

	printf("Reorder test: stream %u, %d frames, adjacent pairs swapped\n",
	       stream_id, nframes);

	/* Payload for frame i is @payln bytes all equal to (uint8_t)i. */
	memset(ctx->send_buf + URP_FRAME_HEADER_SIZE, 0, payln);
	if (send_frame(ctx, stream_id, 0, URP_DATA_FLAG_SYN, payln))
		return 1;

	for (base = 1; base < nframes; base += 2) {
		if (base + 1 < nframes) {
			memset(ctx->send_buf + URP_FRAME_HEADER_SIZE,
			       base + 1, payln);
			if (send_frame(ctx, stream_id, base + 1, 0, payln))
				return 1;
			memset(ctx->send_buf + URP_FRAME_HEADER_SIZE,
			       base, payln);
			if (send_frame(ctx, stream_id, base, 0, payln))
				return 1;
		} else {
			memset(ctx->send_buf + URP_FRAME_HEADER_SIZE,
			       base, payln);
			if (send_frame(ctx, stream_id, base, 0, payln))
				return 1;
		}
	}

	/* Politely soak up a few echoes so the acceptor's stream TX toward
	 * us doesn't RNR-storm; contents are not verified here.
	 */
	for (i = 0; i < nframes; i++) {
		struct ibv_wc wc;
		int spins = 200;

		if (post_recv(ctx))
			break;
		while (spins-- > 0) {
			if (ibv_poll_cq(ctx->cq, 1, &wc) > 0)
				break;
			usleep(1000);
		}
	}

	printf("REORDER_SENT frames=%d payload=%zu\n", nframes, payln);
	return 0;
}

/* ---- Connection setup + dispatch ---- */

static int connect_to_server(struct test_context *ctx, const char *server, int port)
{
	struct sockaddr_in addr = {};
	struct rdma_conn_param param = {};

	ctx->ec = rdma_create_event_channel();
	if (!ctx->ec) {
		perror("rdma_create_event_channel");
		return -1;
	}

	if (rdma_create_id(ctx->ec, &ctx->id, NULL, RDMA_PS_TCP)) {
		perror("rdma_create_id");
		return -1;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, server, &addr.sin_addr);

	if (rdma_resolve_addr(ctx->id, NULL, (struct sockaddr *)&addr, 2000)) {
		perror("rdma_resolve_addr");
		return -1;
	}
	if (wait_for_event(ctx, RDMA_CM_EVENT_ADDR_RESOLVED))
		return -1;

	if (rdma_resolve_route(ctx->id, 2000)) {
		perror("rdma_resolve_route");
		return -1;
	}
	if (wait_for_event(ctx, RDMA_CM_EVENT_ROUTE_RESOLVED))
		return -1;

	if (setup_qp(ctx))
		return -1;

	/* Pre-post a receive before connecting */
	if (post_recv(ctx)) {
		perror("post_recv");
		return -1;
	}

	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.retry_count = 7;
	param.rnr_retry_count = 7;

	if (rdma_connect(ctx->id, &param)) {
		perror("rdma_connect");
		return -1;
	}
	if (wait_for_event(ctx, RDMA_CM_EVENT_ESTABLISHED))
		return -1;

	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s <ip> <port> echo [message] [count]\n", prog);
	fprintf(stderr, "  %s <ip> <port> throughput [size_mb]\n", prog);
	fprintf(stderr, "  %s <ip> <port> latency [count]\n", prog);
	fprintf(stderr, "  %s <ip> <port> reorder [nframes]\n", prog);
	fprintf(stderr, "\nLegacy (no mode keyword):\n");
	fprintf(stderr, "  %s <ip> [port] [message] [count]\n", prog);
}

int main(int argc, char *argv[])
{
	struct test_context ctx = {};
	const char *server;
	int port;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	server = argv[1];

	/* Detect mode: if argv[3] is a known keyword, use new syntax */
	if (argc >= 4 && strcmp(argv[3], "echo") == 0) {
		port = atoi(argv[2]);
		if (connect_to_server(&ctx, server, port)) {
			cleanup(&ctx);
			return 1;
		}
		printf("RDMA connected to %s:%d\n", server, port);
		const char *msg = argc > 4 ? argv[4] : "hello RDMA kernel";
		int count = argc > 5 ? atoi(argv[5]) : 1;
		ret = mode_echo(&ctx, msg, count);

	} else if (argc >= 4 && strcmp(argv[3], "throughput") == 0) {
		port = atoi(argv[2]);
		if (connect_to_server(&ctx, server, port)) {
			cleanup(&ctx);
			return 1;
		}
		printf("RDMA connected to %s:%d\n", server, port);
		int size_mb = argc > 4 ? atoi(argv[4]) : 100;
		ret = mode_throughput(&ctx, size_mb);

	} else if (argc >= 4 && strcmp(argv[3], "latency") == 0) {
		port = atoi(argv[2]);
		if (connect_to_server(&ctx, server, port)) {
			cleanup(&ctx);
			return 1;
		}
		printf("RDMA connected to %s:%d\n", server, port);
		int count = argc > 4 ? atoi(argv[4]) : 1000;
		ret = mode_latency(&ctx, count);

	} else if (argc >= 4 && strcmp(argv[3], "reorder") == 0) {
		port = atoi(argv[2]);
		if (connect_to_server(&ctx, server, port)) {
			cleanup(&ctx);
			return 1;
		}
		printf("RDMA connected to %s:%d\n", server, port);
		int nframes = argc > 4 ? atoi(argv[4]) : 8;
		ret = mode_reorder(&ctx, nframes);

	} else {
		/* Legacy syntax: urp-test-client <ip> [port] [message] [count] */
		port = argc > 2 ? atoi(argv[2]) : URP_DEFAULT_PORT;
		if (connect_to_server(&ctx, server, port)) {
			cleanup(&ctx);
			return 1;
		}
		printf("RDMA connected to %s:%d\n", server, port);
		const char *msg = argc > 3 ? argv[3] : "hello RDMA kernel";
		int count = argc > 4 ? atoi(argv[4]) : 1;
		ret = mode_echo(&ctx, msg, count);
	}

	rdma_disconnect(ctx.id);
	cleanup(&ctx);
	return ret;
}
