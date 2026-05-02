// SPDX-License-Identifier: GPL-2.0
/*
 * urp-test-client — minimal RDMA CM client for testing the urp kernel module.
 *
 * Connects to the module's RDMA listener, sends URP DATA frames,
 * receives echoed frames, and verifies the payload matches.
 *
 * Usage:
 *   urp-test-client <server-ip> [port] [message] [count]
 *
 * The module must be loaded in acceptor mode with an echo server
 * behind the connect_path UDS socket.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

/* URP frame header (matches kernel/include/uapi/linux/urp.h) */
#define URP_FRAME_HEADER_SIZE	20
#define URP_FRAME_TYPE_DATA	0x00
#define URP_DEFAULT_PORT	4791
#define BUF_SIZE		4096

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

int main(int argc, char *argv[])
{
	struct test_context ctx = {};
	struct sockaddr_in addr = {};
	struct rdma_conn_param param = {};
	struct ibv_wc wc;
	const char *server;
	const char *message;
	int port, count, i;
	int passed = 0, failed = 0;

	server = argc > 1 ? argv[1] : "127.0.0.1";
	port = argc > 2 ? atoi(argv[2]) : URP_DEFAULT_PORT;
	message = argc > 3 ? argv[3] : "hello RDMA kernel";
	count = argc > 4 ? atoi(argv[4]) : 1;

	ctx.ec = rdma_create_event_channel();
	if (!ctx.ec) {
		perror("rdma_create_event_channel");
		return 1;
	}

	if (rdma_create_id(ctx.ec, &ctx.id, NULL, RDMA_PS_TCP)) {
		perror("rdma_create_id");
		return 1;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, server, &addr.sin_addr);

	if (rdma_resolve_addr(ctx.id, NULL, (struct sockaddr *)&addr, 2000)) {
		perror("rdma_resolve_addr");
		cleanup(&ctx);
		return 1;
	}
	if (wait_for_event(&ctx, RDMA_CM_EVENT_ADDR_RESOLVED)) {
		cleanup(&ctx);
		return 1;
	}

	if (rdma_resolve_route(ctx.id, 2000)) {
		perror("rdma_resolve_route");
		cleanup(&ctx);
		return 1;
	}
	if (wait_for_event(&ctx, RDMA_CM_EVENT_ROUTE_RESOLVED)) {
		cleanup(&ctx);
		return 1;
	}

	if (setup_qp(&ctx)) {
		cleanup(&ctx);
		return 1;
	}

	/* Pre-post a receive before connecting */
	if (post_recv(&ctx)) {
		perror("post_recv");
		cleanup(&ctx);
		return 1;
	}

	param.responder_resources = 1;
	param.initiator_depth = 1;
	param.retry_count = 7;
	param.rnr_retry_count = 7;

	if (rdma_connect(ctx.id, &param)) {
		perror("rdma_connect");
		cleanup(&ctx);
		return 1;
	}
	if (wait_for_event(&ctx, RDMA_CM_EVENT_ESTABLISHED)) {
		cleanup(&ctx);
		return 1;
	}

	printf("RDMA connected to %s:%d\n", server, port);

	for (i = 0; i < count; i++) {
		size_t msg_len = strlen(message);
		size_t frame_len = URP_FRAME_HEADER_SIZE + msg_len;
		uint32_t recv_payload_len;

		/* Encode URP DATA frame */
		urp_frame_encode(ctx.send_buf, 0, i,
				 URP_FRAME_TYPE_DATA, 0, 0, msg_len);
		memcpy(ctx.send_buf + URP_FRAME_HEADER_SIZE, message, msg_len);

		/* Send frame */
		if (post_send(&ctx, frame_len)) {
			perror("post_send");
			failed++;
			continue;
		}

		/* Wait for send completion */
		if (poll_cq(&ctx, &wc) || wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "send failed: %s\n",
				ibv_wc_status_str(wc.status));
			failed++;
			continue;
		}

		/* Wait for receive completion */
		if (poll_cq(&ctx, &wc) || wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "recv failed: %s\n",
				ibv_wc_status_str(wc.status));
			failed++;
			continue;
		}

		/* Verify the echo: module extracts payload, sends to echo server,
		 * echo server sends it back, module re-encodes as URP frame */
		recv_payload_len = urp_frame_decode_payload_len(ctx.recv_buf);
		if (recv_payload_len == msg_len &&
		    memcmp(ctx.recv_buf + URP_FRAME_HEADER_SIZE, message,
			   msg_len) == 0) {
			passed++;
		} else {
			fprintf(stderr, "echo mismatch at iteration %d: "
				"sent %zu bytes, got %u bytes\n",
				i, msg_len, recv_payload_len);
			failed++;
		}

		/* Repost recv for next iteration */
		if (i + 1 < count) {
			if (post_recv(&ctx)) {
				perror("post_recv");
				break;
			}
		}
	}

	rdma_disconnect(ctx.id);
	cleanup(&ctx);

	printf("Results: %d/%d passed\n", passed, count);
	return failed > 0 ? 1 : 0;
}
