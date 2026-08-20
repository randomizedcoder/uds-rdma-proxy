// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for urp kernel module.
 *
 * Tests frame encode/decode and buffer free-list management.
 * Compiled into urp.ko only when CONFIG_KUNIT=y.
 */

#include <kunit/test.h>
#include "urp.h"
#include "urp_cmd.h"
#include "urp_cmd_own.h"
#include "urp_conn_plan.h"
#include "urp_credit_plan.h"
#include "urp_retry_plan.h"
#include "urp_lazy_plan.h"
#include "include/uapi/linux/urp_cmd.h"

/* ---- Frame codec tests ---- */

static void test_frame_roundtrip_data(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];
	u32 stream_id = 42;
	u64 seq = 12345678;
	u8 type = URP_FRAME_TYPE_DATA;
	u8 flags = URP_DATA_FLAG_SYN;
	u16 credits = 100;
	u32 payload_len = 1024;

	urp_frame_encode(buf, stream_id, seq, type, flags, credits, payload_len);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_payload_len(buf), payload_len);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_seq(buf), seq);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_type(buf), type);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_flags(buf), flags);
}

static void test_frame_roundtrip_control(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];

	urp_frame_encode(buf, 0, 0, URP_FRAME_TYPE_CONTROL, 0, 500, 0);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_type(buf), (u8)URP_FRAME_TYPE_CONTROL);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_payload_len(buf), (u32)0);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_seq(buf), (u64)0);
}

static void test_frame_roundtrip_probe(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];

	urp_frame_encode(buf, 0, 99, URP_FRAME_TYPE_PROBE, 0, 0, 32);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_type(buf), (u8)URP_FRAME_TYPE_PROBE);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_seq(buf), (u64)99);
	KUNIT_EXPECT_EQ(test, urp_frame_decode_payload_len(buf), (u32)32);
}

static void test_frame_max_payload(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];
	u32 max = URP_MAX_PAYLOAD;

	urp_frame_encode(buf, 0, 0, URP_FRAME_TYPE_DATA, 0, 0, max);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_payload_len(buf), max);
}

static void test_frame_zero_payload(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];

	urp_frame_encode(buf, 0, 0, URP_FRAME_TYPE_DATA, 0, 0, 0);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_payload_len(buf), (u32)0);
}

static void test_frame_all_flags(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];
	u8 flags = URP_DATA_FLAG_SYN | URP_DATA_FLAG_FIN | URP_DATA_FLAG_RST;

	urp_frame_encode(buf, 0, 0, URP_FRAME_TYPE_DATA, flags, 0, 0);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_flags(buf), flags);
}

static void test_frame_max_stream_id(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];

	urp_frame_encode(buf, UINT_MAX, 0, URP_FRAME_TYPE_DATA, 0, 0, 0);

	/* Verify stream_id encoding via raw bytes (little-endian) */
	KUNIT_EXPECT_EQ(test, buf[0], (u8)0xFF);
	KUNIT_EXPECT_EQ(test, buf[1], (u8)0xFF);
	KUNIT_EXPECT_EQ(test, buf[2], (u8)0xFF);
	KUNIT_EXPECT_EQ(test, buf[3], (u8)0xFF);
}

static void test_frame_max_seq(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];

	urp_frame_encode(buf, 0, U64_MAX, URP_FRAME_TYPE_DATA, 0, 0, 0);

	KUNIT_EXPECT_EQ(test, urp_frame_decode_seq(buf), U64_MAX);
}

static void test_frame_endianness(struct kunit *test)
{
	u8 buf[URP_FRAME_HEADER_SIZE];

	/* Encode a known value and verify byte layout is little-endian */
	urp_frame_encode(buf, 0x04030201, 0, URP_FRAME_TYPE_DATA, 0, 0, 0);

	KUNIT_EXPECT_EQ(test, buf[0], (u8)0x01);
	KUNIT_EXPECT_EQ(test, buf[1], (u8)0x02);
	KUNIT_EXPECT_EQ(test, buf[2], (u8)0x03);
	KUNIT_EXPECT_EQ(test, buf[3], (u8)0x04);
}

/* ---- Buffer free-list tests ---- */

static void test_buf_list_init(struct kunit *test)
{
	struct list_head free_list;
	struct urp_buffer bufs[4];
	int i;

	INIT_LIST_HEAD(&free_list);

	for (i = 0; i < 4; i++) {
		bufs[i].index = i;
		list_add_tail(&bufs[i].list, &free_list);
	}

	KUNIT_EXPECT_FALSE(test, list_empty(&free_list));

	/* Verify FIFO order */
	struct urp_buffer *buf;

	i = 0;
	list_for_each_entry(buf, &free_list, list) {
		KUNIT_EXPECT_EQ(test, buf->index, (u32)i);
		i++;
	}
	KUNIT_EXPECT_EQ(test, i, 4);
}

static void test_buf_list_alloc_free(struct kunit *test)
{
	struct list_head free_list;
	struct urp_buffer bufs[4];
	struct urp_buffer *alloc;
	int i;

	INIT_LIST_HEAD(&free_list);

	for (i = 0; i < 4; i++) {
		bufs[i].index = i;
		list_add_tail(&bufs[i].list, &free_list);
	}

	/* Alloc first (FIFO: should be index 0) */
	alloc = list_first_entry(&free_list, struct urp_buffer, list);
	list_del(&alloc->list);
	KUNIT_EXPECT_EQ(test, alloc->index, (u32)0);

	/* Alloc second */
	alloc = list_first_entry(&free_list, struct urp_buffer, list);
	list_del(&alloc->list);
	KUNIT_EXPECT_EQ(test, alloc->index, (u32)1);

	/* Return first to tail */
	list_add_tail(&bufs[0].list, &free_list);

	/* Next alloc should be index 2 */
	alloc = list_first_entry(&free_list, struct urp_buffer, list);
	list_del(&alloc->list);
	KUNIT_EXPECT_EQ(test, alloc->index, (u32)2);
}

static void test_buf_list_exhaustion(struct kunit *test)
{
	struct list_head free_list;
	struct urp_buffer bufs[2];
	int i;

	INIT_LIST_HEAD(&free_list);

	for (i = 0; i < 2; i++) {
		bufs[i].index = i;
		list_add_tail(&bufs[i].list, &free_list);
	}

	/* Drain all */
	list_del(&bufs[0].list);
	list_del(&bufs[1].list);

	KUNIT_EXPECT_TRUE(test, list_empty(&free_list));
}

/* ---- Credit-state tests (Phase 3a Step 9; mirrors the 8 Rust
 *      uds_rdma_protocol::credit unit tests) ----
 */

static void test_credit_initial_state(struct kunit *test)
{
	struct urp_credit cs;

	urp_credit_init(&cs, 128);
	KUNIT_EXPECT_EQ(test, cs.send_credits, (u16)128);
	KUNIT_EXPECT_EQ(test, cs.initial_credits, (u16)128);
	KUNIT_EXPECT_EQ(test, cs.credits_to_grant, (u16)0);
	KUNIT_EXPECT_EQ(test, cs.threshold, (u16)32);	/* 128 / 4 */
	KUNIT_EXPECT_TRUE(test, urp_credit_can_send(&cs));
	KUNIT_EXPECT_FALSE(test, urp_credit_should_grant(&cs));
}

static void test_credit_consume_all(struct kunit *test)
{
	struct urp_credit cs;
	int i;

	urp_credit_init(&cs, 128);
	for (i = 0; i < 128; i++) {
		KUNIT_EXPECT_TRUE(test, urp_credit_can_send(&cs));
		KUNIT_EXPECT_EQ(test, urp_credit_consume(&cs), 0);
	}
	KUNIT_EXPECT_FALSE(test, urp_credit_can_send(&cs));
	KUNIT_EXPECT_EQ(test, cs.send_credits, (u16)0);
}

static void test_credit_consume_below_zero(struct kunit *test)
{
	struct urp_credit cs;

	urp_credit_init(&cs, 1);
	KUNIT_EXPECT_EQ(test, urp_credit_consume(&cs), 0);
	KUNIT_EXPECT_EQ(test, urp_credit_consume(&cs), -EAGAIN);
}

static void test_credit_grant_restores(struct kunit *test)
{
	struct urp_credit cs;
	int i;

	urp_credit_init(&cs, 10);
	for (i = 0; i < 10; i++)
		urp_credit_consume(&cs);
	KUNIT_EXPECT_FALSE(test, urp_credit_can_send(&cs));
	urp_credit_grant(&cs, 5);
	KUNIT_EXPECT_TRUE(test, urp_credit_can_send(&cs));
	KUNIT_EXPECT_EQ(test, cs.send_credits, (u16)5);
}

static void test_credit_record_recv_threshold(struct kunit *test)
{
	struct urp_credit cs;
	int i;

	urp_credit_init(&cs, 128);	/* threshold = 32 */
	for (i = 0; i < 31; i++)
		urp_credit_record_recv(&cs);
	KUNIT_EXPECT_FALSE(test, urp_credit_should_grant(&cs));
	KUNIT_EXPECT_EQ(test, urp_credit_pending_grants(&cs), (u16)31);

	urp_credit_record_recv(&cs);
	KUNIT_EXPECT_TRUE(test, urp_credit_should_grant(&cs));
	KUNIT_EXPECT_EQ(test, urp_credit_pending_grants(&cs), (u16)32);
}

static void test_credit_take_grants_resets(struct kunit *test)
{
	struct urp_credit cs;
	int i;

	urp_credit_init(&cs, 128);
	for (i = 0; i < 40; i++)
		urp_credit_record_recv(&cs);
	KUNIT_EXPECT_EQ(test, urp_credit_take_grants(&cs), (u16)40);
	KUNIT_EXPECT_EQ(test, urp_credit_pending_grants(&cs), (u16)0);
	KUNIT_EXPECT_FALSE(test, urp_credit_should_grant(&cs));
}

static void test_credit_initial_one(struct kunit *test)
{
	struct urp_credit cs;

	urp_credit_init(&cs, 1);
	KUNIT_EXPECT_EQ(test, cs.threshold, (u16)0);	/* 1 / 4 = 0 */
	KUNIT_EXPECT_TRUE(test, urp_credit_should_grant(&cs));
	urp_credit_consume(&cs);
	KUNIT_EXPECT_FALSE(test, urp_credit_can_send(&cs));
}

static void test_credit_initial_zero(struct kunit *test)
{
	struct urp_credit cs;

	urp_credit_init(&cs, 0);
	KUNIT_EXPECT_FALSE(test, urp_credit_can_send(&cs));
	KUNIT_EXPECT_TRUE(test, urp_credit_should_grant(&cs));
	KUNIT_EXPECT_EQ(test, cs.send_credits, (u16)0);
}

/* ---- Reorder buffer tests (Phase 3a Step 9; mirrors the 8 Rust
 *      uds_rdma_protocol::reorder unit tests against the C backend) ----
 */

static void test_reorder_in_order(struct kunit *test)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 64);
	u8 payload[8];
	u64 seq;
	size_t len;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, rb);

	for (i = 0; i < 3; i++) {
		u8 b = (u8)i;

		KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, i, &b, 1), 0);
		len = sizeof(payload);
		KUNIT_EXPECT_EQ(test, urp_reorder_drain_next(rb, &seq, payload, &len), 0);
		KUNIT_EXPECT_EQ(test, seq, (u64)i);
		KUNIT_EXPECT_EQ(test, len, (size_t)1);
		KUNIT_EXPECT_EQ(test, payload[0], (u8)i);
	}
	KUNIT_EXPECT_EQ(test, urp_reorder_next_expected(rb), (u64)3);

	urp_reorder_free(rb);
}

static void test_reorder_out_of_order(struct kunit *test)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 64);
	u8 payload[8];
	u8 b;
	u64 seq;
	size_t len = sizeof(payload);

	KUNIT_ASSERT_NOT_NULL(test, rb);

	/* Insert seq 2 first -- buffered, no drain */
	b = 2;
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 2, &b, 1), 0);
	KUNIT_EXPECT_EQ(test, urp_reorder_drain_next(rb, &seq, payload, &len), -ENOENT);
	KUNIT_EXPECT_EQ(test, urp_reorder_gap_count(rb), (size_t)1);

	/* Insert seq 0 -- drains immediately */
	b = 0;
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 0, &b, 1), 0);
	len = sizeof(payload);
	KUNIT_EXPECT_EQ(test, urp_reorder_drain_next(rb, &seq, payload, &len), 0);
	KUNIT_EXPECT_EQ(test, seq, (u64)0);

	/* Insert seq 1 -- drains 1 then 2 */
	b = 1;
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 1, &b, 1), 0);
	len = sizeof(payload);
	KUNIT_EXPECT_EQ(test, urp_reorder_drain_next(rb, &seq, payload, &len), 0);
	KUNIT_EXPECT_EQ(test, seq, (u64)1);
	len = sizeof(payload);
	KUNIT_EXPECT_EQ(test, urp_reorder_drain_next(rb, &seq, payload, &len), 0);
	KUNIT_EXPECT_EQ(test, seq, (u64)2);
	KUNIT_EXPECT_EQ(test, urp_reorder_next_expected(rb), (u64)3);

	urp_reorder_free(rb);
}

static void test_reorder_duplicate(struct kunit *test)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 64);
	u8 b = 2;

	KUNIT_ASSERT_NOT_NULL(test, rb);

	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 2, &b, 1), 0);
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 2, &b, 1), -EEXIST);

	urp_reorder_free(rb);
}

static void test_reorder_already_delivered(struct kunit *test)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 64);
	u8 b = 0;
	u8 payload[4];
	u64 seq;
	size_t len = sizeof(payload);

	KUNIT_ASSERT_NOT_NULL(test, rb);
	urp_reorder_insert(rb, 0, &b, 1);
	urp_reorder_drain_next(rb, &seq, payload, &len);
	/* Inserting seq 0 again should be rejected as duplicate */
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 0, &b, 1), -EEXIST);

	urp_reorder_free(rb);
}

static void test_reorder_buffer_full(struct kunit *test)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 2);
	u8 b;

	KUNIT_ASSERT_NOT_NULL(test, rb);
	b = 2; urp_reorder_insert(rb, 2, &b, 1);
	b = 3; urp_reorder_insert(rb, 3, &b, 1);
	b = 4;
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 4, &b, 1), -ENOBUFS);
	/* But the in-order seq still works (drains immediately) */
	b = 0;
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, 0, &b, 1), 0);

	urp_reorder_free(rb);
}

/*
 * Regression (C twin of the Rust reorder_ops fuzz find): delivering the
 * frame at the top of the sequence space must saturate next_expected at
 * U64_MAX rather than overflow. The Rust twin overflow-panics -> BUG();
 * the C twin must not wrap to 0.
 */
static void test_reorder_seq_saturates(struct kunit *test)
{
	struct urp_reorder *rb = urp_reorder_alloc(U64_MAX, 16);
	u8 b = 0xAB;

	KUNIT_ASSERT_NOT_NULL(test, rb);
	KUNIT_EXPECT_EQ(test, urp_reorder_insert(rb, U64_MAX, &b, 1), 0);
	KUNIT_EXPECT_EQ(test, urp_reorder_next_expected(rb), U64_MAX);

	urp_reorder_free(rb);
}

/* ---- QP round-robin selection tests (Phase 3a Step 9) ---- */

static void test_qp_select_round_robin_determinism(struct kunit *test)
{
	struct urp_endpoint ep = {};
	struct urp_qp qps[4] = {};
	struct urp_qp *picked;
	u32 i;

	ep.num_qps = 4;
	ep.qps = qps;
	for (i = 0; i < 4; i++) {
		qps[i].ep = &ep;
		qps[i].index = i;
		qps[i].established = true;
		qps[i].qp = (struct ib_qp *)(unsigned long)(i + 1);
	}
	atomic_set(&ep.rr_counter, 0);

	for (i = 0; i < 8; i++) {
		picked = urp_qp_select_round_robin(&ep);
		KUNIT_ASSERT_NOT_NULL(test, picked);
		KUNIT_EXPECT_EQ(test, picked->index, (i + 1) % 4);
	}
}

static void test_qp_select_skips_unestablished(struct kunit *test)
{
	struct urp_endpoint ep = {};
	struct urp_qp qps[3] = {};
	struct urp_qp *picked;
	u32 i;

	ep.num_qps = 3;
	ep.qps = qps;
	for (i = 0; i < 3; i++) {
		qps[i].ep = &ep;
		qps[i].index = i;
		qps[i].qp = (struct ib_qp *)(unsigned long)(i + 1);
	}
	/* Only QP 1 is established */
	qps[1].established = true;
	atomic_set(&ep.rr_counter, 0);

	for (i = 0; i < 5; i++) {
		picked = urp_qp_select_round_robin(&ep);
		KUNIT_ASSERT_NOT_NULL(test, picked);
		KUNIT_EXPECT_EQ(test, picked->index, (u32)1);
	}
}

static void test_qp_select_returns_null_when_none_ready(struct kunit *test)
{
	struct urp_endpoint ep = {};
	struct urp_qp qps[2] = {};
	struct urp_qp *picked;

	ep.num_qps = 2;
	ep.qps = qps;
	qps[0].ep = &ep;
	qps[1].ep = &ep;
	atomic_set(&ep.rr_counter, 0);

	picked = urp_qp_select_round_robin(&ep);
	KUNIT_EXPECT_NULL(test, picked);
}

/* ---- PROBE PING/PONG wire-format tests (Phase 3b Step 1) ---- */

static void test_probe_ping_roundtrip(struct kunit *test)
{
	u8 buf[URP_PING_PAYLOAD_SIZE];
	u32 probe_seq = 0x12345678;
	u16 qp_index = 3;
	u64 t_send_mono = 1000000000ULL;	/* 1s */
	u64 t_send_real = 2000000000ULL;	/* 2s */

	memset(buf, 0xAA, sizeof(buf));
	urp_ping_encode(buf, probe_seq, qp_index, t_send_mono, t_send_real);

	KUNIT_EXPECT_EQ(test, urp_ping_decode_seq(buf), probe_seq);
	KUNIT_EXPECT_EQ(test, urp_ping_decode_qp_index(buf), qp_index);
	KUNIT_EXPECT_EQ(test, urp_ping_decode_t_send_mono(buf), t_send_mono);
	KUNIT_EXPECT_EQ(test, urp_ping_decode_t_send_real(buf), t_send_real);
}

static void test_probe_pong_echoes_ping(struct kunit *test)
{
	u8 ping[URP_PING_PAYLOAD_SIZE];
	u8 pong[URP_PONG_PAYLOAD_SIZE];
	u32 probe_seq = 42;
	u16 qp_index = 7;
	u64 t_send_mono = 100;
	u64 t_send_real = 200;
	u64 t_recv_real = 300;
	u64 t_pong_mono = 400;
	u64 t_pong_real = 500;

	urp_ping_encode(ping, probe_seq, qp_index, t_send_mono, t_send_real);
	urp_pong_encode(pong, ping, t_recv_real, t_pong_mono, t_pong_real);

	/* Echoed fields */
	KUNIT_EXPECT_EQ(test, urp_ping_decode_seq(pong), probe_seq);
	KUNIT_EXPECT_EQ(test, urp_ping_decode_qp_index(pong), qp_index);
	KUNIT_EXPECT_EQ(test, urp_ping_decode_t_send_mono(pong), t_send_mono);
	KUNIT_EXPECT_EQ(test, urp_ping_decode_t_send_real(pong), t_send_real);

	/* Responder-added field */
	KUNIT_EXPECT_EQ(test, urp_pong_decode_t_pong_mono(pong), t_pong_mono);
}

static void test_probe_payload_sizes(struct kunit *test)
{
	/* Wire-format size constants must match the Rust reference impl
	 * (uds_rdma_protocol::constants::{PING,PONG}_PAYLOAD_SIZE).
	 */
	KUNIT_EXPECT_EQ(test, URP_PING_PAYLOAD_SIZE, 32);
	KUNIT_EXPECT_EQ(test, URP_PONG_PAYLOAD_SIZE, 48);
}

/*
 * Stream state machine (design 28 E2). Table-drives the full
 * (state, event) -> (next, actions, accepted) matrix over the pure
 * urp_stream_next_state(). This is the C half of the differential with
 * crates/uds-rdma-protocol/src/stream.rs -- the two tables must match
 * row-for-row. Enumerates all 6 states x 5 events = 30 transitions.
 */
static void test_stream_next_state(struct kunit *test)
{
#define S(x)   URP_STREAM_STATE_##x
#define EV(x)  URP_STREAM_EV_##x
#define A_WR   URP_STREAM_ACT_SHUTDOWN_WR
#define A_RDWR URP_STREAM_ACT_SHUTDOWN_RDWR
#define A_DEST URP_STREAM_ACT_DESTROY
	static const struct {
		enum urp_stream_state cur;
		enum urp_stream_event ev;
		enum urp_stream_state next;
		u32 actions;
		bool accepted;
	} cases[] = {
		/* RX_SYN: idempotent handshake advance, else reject */
		{ S(SYN_SENT),     EV(RX_SYN), S(ESTABLISHED), 0, true  },
		{ S(SYN_RECEIVED), EV(RX_SYN), S(ESTABLISHED), 0, true  },
		{ S(ESTABLISHED),  EV(RX_SYN), S(ESTABLISHED), 0, true  },
		{ S(FIN_WAIT),     EV(RX_SYN), S(FIN_WAIT),    0, false },
		{ S(CLOSE_WAIT),   EV(RX_SYN), S(CLOSE_WAIT),  0, false },
		{ S(CLOSED),       EV(RX_SYN), S(CLOSED),      0, false },
		/* RX_FIN: always SHUT_WR; advance EST/FIN_WAIT */
		{ S(SYN_SENT),     EV(RX_FIN), S(SYN_SENT),     A_WR, true },
		{ S(SYN_RECEIVED), EV(RX_FIN), S(SYN_RECEIVED), A_WR, true },
		{ S(ESTABLISHED),  EV(RX_FIN), S(CLOSE_WAIT),   A_WR, true },
		{ S(FIN_WAIT),     EV(RX_FIN), S(CLOSED),       A_WR, true },
		{ S(CLOSE_WAIT),   EV(RX_FIN), S(CLOSE_WAIT),   A_WR, true },
		{ S(CLOSED),       EV(RX_FIN), S(CLOSED),       A_WR, true },
		/* RX_RST: any -> CLOSED, shutdown RDWR + destroy */
		{ S(SYN_SENT),     EV(RX_RST), S(CLOSED), A_RDWR | A_DEST, true },
		{ S(SYN_RECEIVED), EV(RX_RST), S(CLOSED), A_RDWR | A_DEST, true },
		{ S(ESTABLISHED),  EV(RX_RST), S(CLOSED), A_RDWR | A_DEST, true },
		{ S(FIN_WAIT),     EV(RX_RST), S(CLOSED), A_RDWR | A_DEST, true },
		{ S(CLOSE_WAIT),   EV(RX_RST), S(CLOSED), A_RDWR | A_DEST, true },
		{ S(CLOSED),       EV(RX_RST), S(CLOSED), A_RDWR | A_DEST, true },
		/* TX_FIN: advance EST->FIN_WAIT, CLOSE_WAIT->CLOSED */
		{ S(SYN_SENT),     EV(TX_FIN), S(SYN_SENT),     0, true },
		{ S(SYN_RECEIVED), EV(TX_FIN), S(SYN_RECEIVED), 0, true },
		{ S(ESTABLISHED),  EV(TX_FIN), S(FIN_WAIT),     0, true },
		{ S(FIN_WAIT),     EV(TX_FIN), S(FIN_WAIT),     0, true },
		{ S(CLOSE_WAIT),   EV(TX_FIN), S(CLOSED),       0, true },
		{ S(CLOSED),       EV(TX_FIN), S(CLOSED),       0, true },
		/* TX_RST: any -> CLOSED, shutdown RDWR (no destroy) */
		{ S(SYN_SENT),     EV(TX_RST), S(CLOSED), A_RDWR, true },
		{ S(SYN_RECEIVED), EV(TX_RST), S(CLOSED), A_RDWR, true },
		{ S(ESTABLISHED),  EV(TX_RST), S(CLOSED), A_RDWR, true },
		{ S(FIN_WAIT),     EV(TX_RST), S(CLOSED), A_RDWR, true },
		{ S(CLOSE_WAIT),   EV(TX_RST), S(CLOSED), A_RDWR, true },
		{ S(CLOSED),       EV(TX_RST), S(CLOSED), A_RDWR, true },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct urp_stream_transition t =
			urp_stream_next_state(cases[i].cur, cases[i].ev);

		KUNIT_EXPECT_EQ_MSG(test, t.next, cases[i].next,
				    "case %d: next", i);
		KUNIT_EXPECT_EQ_MSG(test, t.actions, cases[i].actions,
				    "case %d: actions", i);
		KUNIT_EXPECT_EQ_MSG(test, (int)t.accepted, (int)cases[i].accepted,
				    "case %d: accepted", i);
	}
#undef S
#undef EV
#undef A_WR
#undef A_RDWR
#undef A_DEST
}

/*
 * RX frame classifier (design 28 E1). Table-drives urp_classify_frame over
 * the length guards + type/flag routing. The design 27 27.8 #1 info-leak
 * (a header-only frame declaring a full payload) and the short-PROBE guard
 * are explicit regression rows.
 */
static void test_classify_frame(struct kunit *test)
{
#define FT_D   URP_FRAME_TYPE_DATA
#define FT_C   URP_FRAME_TYPE_CONTROL
#define FT_P   URP_FRAME_TYPE_PROBE
#define HDRSZ  URP_FRAME_HEADER_SIZE
#define PINGSZ URP_PING_PAYLOAD_SIZE
#define PONGSZ URP_PONG_PAYLOAD_SIZE
	static const struct {
		u32 stream_id;
		u8  type;
		u8  flags;
		u16 credits;
		u32 payload_len;
		u32 byte_len;
		enum urp_rx_action want;
	} cases[] = {
		/* Normal delivery */
		{ 0, FT_D, 0, 0, 10, 30,    URP_RX_DELIVER_LEGACY },
		{ 5, FT_D, 0, 0, 10, 30,    URP_RX_DELIVER_STREAM },
		{ 5, FT_D, 0, 0,  0, HDRSZ, URP_RX_DELIVER_STREAM },  /* zero payload */
		{ 5, FT_D, 0, 0, 10, 30,    URP_RX_DELIVER_STREAM },  /* fits exactly */
		/* Short frame: header itself would be stale */
		{ 0, FT_D, 0, 0, 0, 10,        URP_RX_DROP_SHORT },
		{ 0, FT_D, 0, 0, 0, HDRSZ - 1, URP_RX_DROP_SHORT },
		/* Oversize payload_len */
		{ 0, FT_D, 0, 0, URP_MAX_PAYLOAD + 1, URP_BUF_SIZE, URP_RX_DROP_OVERSIZE },
		/* 27.8 #1: header-only frame declaring a full payload */
		{ 0, FT_D, 0, 0, URP_MAX_PAYLOAD, HDRSZ, URP_RX_DROP_PAYLOAD_OVERRUN },
		{ 5, FT_D, 0, 0, 11, 30, URP_RX_DROP_PAYLOAD_OVERRUN },  /* overrun by 1 */
		/* CONTROL -> credit */
		{ 0, FT_C, URP_CTRL_FLAG_CREDIT, 64, 0, HDRSZ, URP_RX_CREDIT },
		/* PROBE ping / pong */
		{ 0, FT_P, 0, 0, PINGSZ, HDRSZ + PINGSZ, URP_RX_PROBE_PING },
		{ 0, FT_P, URP_PROBE_FLAG_PONG, 0, PONGSZ, HDRSZ + PONGSZ, URP_RX_PROBE_PONG },
		/* PROBE too short to read the fixed payload offsets */
		{ 0, FT_P, 0, 0, 0, HDRSZ + PINGSZ - 1, URP_RX_DROP_SHORT_PROBE },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		u8 hdr[URP_FRAME_HEADER_SIZE];
		struct urp_rx_decoded dec;
		enum urp_rx_action got;

		urp_frame_encode(hdr, cases[i].stream_id, 0, cases[i].type,
				 cases[i].flags, cases[i].credits,
				 cases[i].payload_len);
		got = urp_classify_frame(cases[i].byte_len, hdr, &dec);
		KUNIT_EXPECT_EQ_MSG(test, got, cases[i].want, "case %d", i);
	}
#undef FT_D
#undef FT_C
#undef FT_P
#undef HDRSZ
#undef PINGSZ
#undef PONGSZ
}

/* ---- Buffer-geometry resolver tests (design 29 Gap 2) ---- */

/*
 * urp_resolve_num_bufs / urp_resolve_buf_size clamp the (admin-supplied,
 * mutable-while-inactive) buffer_count / buffer_size into their valid ranges,
 * and urp_ep_max_payload derives the wire payload ceiling from the slot. These
 * feed every downstream sizing (pool depth, CQ/SRQ/SQ depth, DMA slot bytes,
 * recv sge.length, UDS read cap), so pin every boundary here -- a clamp that
 * silently lets a 0 or an over-max value through would size the pool wrong or,
 * for buf_size, underflow the payload subtraction.
 */
static void test_resolve_num_bufs(struct kunit *test)
{
	const struct { u32 in; u32 want; } cases[] = {
		{ 0,				URP_BUFFER_COUNT_MIN },	/* unset -> MIN */
		{ 1,				URP_BUFFER_COUNT_MIN },	/* below MIN */
		{ URP_BUFFER_COUNT_MIN - 1,	URP_BUFFER_COUNT_MIN },
		{ URP_BUFFER_COUNT_MIN,		URP_BUFFER_COUNT_MIN },	/* at MIN */
		{ 64,				64 },			/* mid (old fixed size) */
		{ URP_BUFFER_COUNT_DEFAULT,	URP_BUFFER_COUNT_DEFAULT },
		{ URP_BUFFER_COUNT_MAX,		URP_BUFFER_COUNT_MAX },	/* at MAX */
		{ URP_BUFFER_COUNT_MAX + 1,	URP_BUFFER_COUNT_MAX },	/* above MAX */
		{ 0xffffffffu,			URP_BUFFER_COUNT_MAX },	/* saturated */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test, urp_resolve_num_bufs(cases[i].in),
				    cases[i].want, "num_bufs case %d (in=%u)",
				    i, cases[i].in);
}

static void test_resolve_buf_size(struct kunit *test)
{
	const struct { u32 in; u32 want; } cases[] = {
		{ 0,				URP_BUFFER_SIZE_MIN },	/* unset -> MIN */
		{ 1,				URP_BUFFER_SIZE_MIN },	/* below MIN */
		{ URP_BUFFER_SIZE_MIN - 1,	URP_BUFFER_SIZE_MIN },
		{ URP_BUFFER_SIZE_MIN,		URP_BUFFER_SIZE_MIN },	/* at MIN (= header) */
		{ URP_BUFFER_SIZE_DEFAULT,	URP_BUFFER_SIZE_DEFAULT },
		{ URP_BUF_SIZE,			URP_BUF_SIZE },		/* default slot */
		{ URP_BUFFER_SIZE_MAX,		URP_BUFFER_SIZE_MAX },	/* at MAX */
		{ URP_BUFFER_SIZE_MAX + 1,	URP_BUFFER_SIZE_MAX },	/* above MAX */
		{ 0xffffffffu,			URP_BUFFER_SIZE_MAX },	/* saturated */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test, urp_resolve_buf_size(cases[i].in),
				    cases[i].want, "buf_size case %d (in=%u)",
				    i, cases[i].in);
}

static void test_ep_max_payload(struct kunit *test)
{
	/* payload = slot - header; the resolver guarantees slot >= header so
	 * this never underflows. Pair each slot with its expected payload.
	 */
	const struct { u32 slot; u32 want; } cases[] = {
		{ URP_BUFFER_SIZE_MIN, 0 },				/* header-only: 0 payload */
		{ URP_BUFFER_SIZE_MIN + 1, 1 },
		{ URP_BUF_SIZE, URP_BUF_SIZE - URP_FRAME_HEADER_SIZE },	/* 4096 -> 4076 */
		{ URP_BUFFER_SIZE_DEFAULT, 4076 },
		{ URP_BUFFER_SIZE_MAX, URP_MAX_PAYLOAD },		/* MAX slot -> absolute ceiling */
	};
	int i;

	/* The decoder's absolute ceiling must equal the max slot's payload. */
	KUNIT_EXPECT_EQ(test, URP_MAX_PAYLOAD,
			URP_BUFFER_SIZE_MAX - URP_FRAME_HEADER_SIZE);

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test, urp_ep_max_payload(cases[i].slot),
				    cases[i].want, "payload case %d (slot=%u)",
				    i, cases[i].slot);
}

/* ---- Test suite registration ---- */

/*
 * urp-fast uring_cmd SEND/RECV validator (design 31 section 31.10, the
 * app->kernel trust boundary). Table-drives urp_cmd_validate_data over the
 * opcode gate, reserved-field and flag checks, the "no pool registered"
 * case, and every buf_index / len boundary. Rows that expect success also
 * assert the decoded fields are copied out verbatim.
 */
static void test_cmd_validate_data(struct kunit *test)
{
#define GOOD_COUNT 4u
#define GOOD_BSZ   4096u
	static const struct {
		u32 cmd_op;
		u32 buf_index;
		u32 len;
		u16 stream_id;
		u16 flags;
		u32 resv;
		u32 count;	/* pool geometry */
		u32 buf_size;
		int want;
	} cases[] = {
		/* --- valid --- */
		{ URP_CMD_SEND, 0, 100,  7, 0,            0, GOOD_COUNT, GOOD_BSZ, 0 },
		{ URP_CMD_SEND, 0, 100,  7, URP_CMD_F_FIN, 0, GOOD_COUNT, GOOD_BSZ, 0 },
		{ URP_CMD_RECV, 3, GOOD_BSZ - URP_FRAME_HEADER_SIZE, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, 0 },
		/* boundaries: last index, max payload (buf_size - header), len == 1 */
		{ URP_CMD_SEND, GOOD_COUNT - 1, GOOD_BSZ - URP_FRAME_HEADER_SIZE, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, 0 },
		{ URP_CMD_RECV, 0, 1, 0, 0,               0, GOOD_COUNT, GOOD_BSZ, 0 },
		/* --- opcode gate --- */
		{ URP_CMD_REGISTER,   0, 100, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -EOPNOTSUPP },
		{ URP_CMD_UNREGISTER, 0, 100, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -EOPNOTSUPP },
		{ 99,                 0, 100, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -EOPNOTSUPP },
		/* --- reserved / flags --- */
		{ URP_CMD_SEND, 0, 100, 0, 0,        1, GOOD_COUNT, GOOD_BSZ, -EINVAL },
		{ URP_CMD_RECV, 0, 100, 0, URP_CMD_F_FIN, 0, GOOD_COUNT, GOOD_BSZ, -EINVAL },
		{ URP_CMD_SEND, 0, 100, 0, (1 << 5), 0, GOOD_COUNT, GOOD_BSZ, -EINVAL },
		/* --- length: 0 rejected; payload > buf_size - header rejected --- */
		{ URP_CMD_SEND, 0, 0,          0, 0, 0, GOOD_COUNT, GOOD_BSZ, -EINVAL },
		{ URP_CMD_SEND, 0, GOOD_BSZ - URP_FRAME_HEADER_SIZE + 1, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -EMSGSIZE },
		{ URP_CMD_SEND, 0, GOOD_BSZ+1, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -EMSGSIZE },
		/* --- pool / index --- */
		{ URP_CMD_SEND, 0, 100, 0, 0, 0, 0,          GOOD_BSZ, -ENXIO },
		{ URP_CMD_SEND, GOOD_COUNT, 100, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -ERANGE },
		{ URP_CMD_RECV, 0xffffffffu, 100, 0, 0, 0, GOOD_COUNT, GOOD_BSZ, -ERANGE },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct urp_cmd_data in = {
			.buf_index = cases[i].buf_index,
			.len	   = cases[i].len,
			.stream_id = cases[i].stream_id,
			.flags	   = cases[i].flags,
			.__resv	   = cases[i].resv,
		};
		struct urp_cmd_pool_geom geom = {
			.count	  = cases[i].count,
			.buf_size = cases[i].buf_size,
		};
		struct urp_cmd_req out = {};
		int ret = urp_cmd_validate_data(cases[i].cmd_op, &in, &geom, &out);

		KUNIT_EXPECT_EQ_MSG(test, ret, cases[i].want, "case %d: ret", i);
		if (ret == 0) {
			KUNIT_EXPECT_EQ_MSG(test, out.op, cases[i].cmd_op,
					    "case %d: op", i);
			KUNIT_EXPECT_EQ_MSG(test, out.buf_index,
					    cases[i].buf_index, "case %d: idx", i);
			KUNIT_EXPECT_EQ_MSG(test, out.len, cases[i].len,
					    "case %d: len", i);
			KUNIT_EXPECT_EQ_MSG(test, out.stream_id,
					    cases[i].stream_id, "case %d: sid", i);
			KUNIT_EXPECT_EQ_MSG(test, out.flags, cases[i].flags,
					    "case %d: flags", i);
		}
	}
#undef GOOD_COUNT
#undef GOOD_BSZ
}

/*
 * urp-fast REGISTER descriptor validator. Table-drives urp_cmd_validate_reg
 * over base alignment, buf_size clamps, the len-is-a-positive-page-and-buffer-
 * multiple rule, and the count == len/buf_size cross-check. Uses PAGE_SIZE so
 * the rows stay correct on any arch KUnit runs on.
 */
static void test_cmd_validate_reg(struct kunit *test)
{
	static const struct {
		const char *name;
		u64 base;
		u64 len;
		u32 buf_size;
		u32 count;
		u32 flags;
		u32 resv;
		int want;
	} cases[] = {
		/* valid: page-sized buffers, then sub-page buffers in one page */
		{ "page-bufs",  4096, 4 * 4096, 4096, 4,  0, 0, 0 },
		{ "subpage",    4096, 4096,     64,   64, 0, 0, 0 },
		/* flags / resv must be zero */
		{ "flags-set",  4096, 4 * 4096, 4096, 4, 1, 0, -EINVAL },
		{ "resv-set",   4096, 4 * 4096, 4096, 4, 0, 1, -EINVAL },
		/* base: zero, and misaligned */
		{ "base-zero",     0, 4 * 4096, 4096, 4, 0, 0, -EINVAL },
		{ "base-misalign", 4097, 4 * 4096, 4096, 4, 0, 0, -EINVAL },
		/* buf_size clamps */
		{ "bsz-small",  4096, 4 * 4096, URP_CMD_BUF_SIZE_MIN - 1, 4, 0, 0, -EINVAL },
		{ "bsz-big",    4096, 4 * 4096, URP_CMD_BUF_SIZE_MAX + 1, 4, 0, 0, -EINVAL },
		/* len == 0 */
		{ "len-zero",   4096, 0, 4096, 4, 0, 0, -EINVAL },
		/* len not a multiple of buf_size (buf_size not a page divisor) */
		{ "len-notmult", 4096, 4096, 3072, 4, 0, 0, -EINVAL },
		/* len a multiple of buf_size but not of a page */
		{ "len-notpage", 4096, 64 * 63, 64, 63, 0, 0, -EINVAL },
		/* count zero, over the cap, and != len / buf_size */
		{ "count-zero",  4096, 4096, 4096, 0, 0, 0, -EINVAL },
		{ "count-toobig", 4096, (u64)(URP_CMD_POOL_COUNT_MAX + 1) * 4096,
		  4096, URP_CMD_POOL_COUNT_MAX + 1, 0, 0, -E2BIG },
		{ "count-mismatch", 4096, 4 * 4096, 4096, 3, 0, 0, -EINVAL },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct urp_cmd_reg reg = {
			.base		= cases[i].base,
			.len		= cases[i].len,
			.buf_size	= cases[i].buf_size,
			.count		= cases[i].count,
			.flags		= cases[i].flags,
			.__resv		= cases[i].resv,
		};
		int ret = urp_cmd_validate_reg(&reg);

		KUNIT_EXPECT_EQ_MSG(test, ret, cases[i].want, "%s", cases[i].name);
	}
}

/*
 * urp-fast per-buffer ownership state machine (urp_cmd_own.h, design 31 §31.2).
 * Same pure transitions the ->uring_cmd SEND path serialises under a spinlock;
 * the userspace units (tools/urp-fast-validate-test.c) re-check them out of VM.
 */
static void test_cmd_own(struct kunit *test)
{
	enum { COUNT = 130 };	/* spans 3 x 64-bit words on the word math */
	unsigned long own[3] = { 0, 0, 0 };

	KUNIT_EXPECT_EQ(test, urp_own_words(COUNT), 3u);
	KUNIT_EXPECT_FALSE(test, urp_own_any_kernel(own, COUNT));

	/* APP -> KERNEL across word boundaries */
	KUNIT_EXPECT_EQ(test, urp_own_claim(own, COUNT, 0), 0);
	KUNIT_EXPECT_EQ(test, urp_own_claim(own, COUNT, 65), 0);
	KUNIT_EXPECT_EQ(test, urp_own_claim(own, COUNT, 129), 0);
	KUNIT_EXPECT_TRUE(test, urp_own_any_kernel(own, COUNT));

	/* double submit + out of range */
	KUNIT_EXPECT_EQ(test, urp_own_claim(own, COUNT, 65), -EBUSY);
	KUNIT_EXPECT_EQ(test, urp_own_claim(own, COUNT, COUNT), -ERANGE);
	KUNIT_EXPECT_EQ(test, urp_own_release(own, COUNT, COUNT), -ERANGE);

	/* KERNEL -> APP, and the double-release / spurious-completion guard */
	KUNIT_EXPECT_EQ(test, urp_own_release(own, COUNT, 0), 0);
	KUNIT_EXPECT_EQ(test, urp_own_release(own, COUNT, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, urp_own_release(own, COUNT, 42), -EINVAL);

	KUNIT_EXPECT_TRUE(test, urp_own_any_kernel(own, COUNT));	/* 65,129 left */
	KUNIT_EXPECT_EQ(test, urp_own_release(own, COUNT, 65), 0);
	KUNIT_EXPECT_EQ(test, urp_own_release(own, COUNT, 129), 0);
	KUNIT_EXPECT_FALSE(test, urp_own_any_kernel(own, COUNT));

	/* buffer cycles: re-claim after release is allowed */
	KUNIT_EXPECT_EQ(test, urp_own_claim(own, COUNT, 0), 0);
}

/*
 * design 32 hardware bring-up: the acceptor connection plan.
 *
 * Regression for the real-HW stall where a multi-stream acceptor eagerly
 * opened the legacy ep->conn backend at CONNECT_REQUEST, stealing the single
 * connection the backend accepts so the real stream's per-stream connect was
 * refused (ECONNREFUSED). urp_acceptor_should_eager_connect() must eager-connect
 * only in k0 mode with a connect_path set.
 */
static void test_acceptor_eager_connect(struct kunit *test)
{
	static const struct {
		enum urp_ep_mode mode;
		bool have_connect_path;
		bool expect;
	} cases[] = {
		/* multi-stream must NOT eager-connect (the bug) */
		{ URP_EP_MODE_MULTISTREAM, true,  false },
		{ URP_EP_MODE_MULTISTREAM, false, false },
		/* k0 eager-connects only when a connect_path is configured */
		{ URP_EP_MODE_K0,          true,  true  },
		{ URP_EP_MODE_K0,          false, false },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_acceptor_should_eager_connect(cases[i].mode,
							     cases[i].have_connect_path);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: mode=%d path=%d", i,
				    (int)cases[i].mode,
				    (int)cases[i].have_connect_path);
	}
}

/*
 * design 32: credit-grant routing.
 *
 * Regression for the real-HW throughput collapse where the multi-stream TX
 * pump consumes per-stream credit (stream->credit) but incoming CREDIT frames
 * were granted to the per-QP pool -- so the stream pool drained after its
 * initial window and never refilled, every send reported a stall, and the
 * sender oversent into a drained SRQ (RNR retry storm). urp_credit_scope_for()
 * must route a grant to the SAME pool the sender drew from, keyed on stream_id.
 *
 * Uses the real urp_credit primitives so the round-trip (drain -> grant ->
 * send again) is exercised end to end, not just the routing predicate.
 */
static void test_credit_grant_routing(struct kunit *test)
{
	static const struct {
		u32 stream_id;
		enum urp_credit_scope expect;
	} scope_cases[] = {
		{ 0,     URP_CREDIT_SCOPE_QP     }, /* legacy k0 */
		{ 1,     URP_CREDIT_SCOPE_STREAM }, /* first multi-stream id */
		{ 3,     URP_CREDIT_SCOPE_STREAM },
		{ 65535, URP_CREDIT_SCOPE_STREAM },
	};
	const u16 w = 8; /* initial window */
	struct urp_credit qp, strm;
	int i;

	for (i = 0; i < ARRAY_SIZE(scope_cases); i++)
		KUNIT_EXPECT_EQ_MSG(test,
			(int)urp_credit_scope_for(scope_cases[i].stream_id),
			(int)scope_cases[i].expect,
			"scope for stream_id=%u", scope_cases[i].stream_id);

	/* Multi-stream (stream_id 1): drain the stream pool, then a grant
	 * routed by scope must land on the stream pool so it can send again.
	 */
	urp_credit_init(&qp, w);
	urp_credit_init(&strm, w);
	for (i = 0; i < w; i++)
		KUNIT_EXPECT_EQ(test, urp_credit_consume(&strm), 0);
	KUNIT_EXPECT_EQ(test, urp_credit_consume(&strm), -EAGAIN);

	/* Route a grant for stream 1 the way urp_recv_done does. */
	KUNIT_ASSERT_EQ(test, (int)urp_credit_scope_for(1),
			(int)URP_CREDIT_SCOPE_STREAM);
	urp_credit_grant(&strm, w);
	KUNIT_EXPECT_EQ_MSG(test, urp_credit_consume(&strm), 0,
			    "stream pool must refill from its own grant");
	/* The QP pool was never touched by the stream's grant. */
	KUNIT_EXPECT_EQ(test, qp.send_credits, w);

	/* Legacy k0 (stream_id 0): the grant routes to the QP pool. */
	urp_credit_init(&qp, w);
	urp_credit_init(&strm, w);
	for (i = 0; i < w; i++)
		KUNIT_EXPECT_EQ(test, urp_credit_consume(&qp), 0);
	KUNIT_ASSERT_EQ(test, (int)urp_credit_scope_for(0),
			(int)URP_CREDIT_SCOPE_QP);
	urp_credit_grant(&qp, w);
	KUNIT_EXPECT_EQ_MSG(test, urp_credit_consume(&qp), 0,
			    "k0 QP pool must refill from its grant");
	KUNIT_EXPECT_EQ(test, strm.send_credits, w);
}

/*
 * design 33 Bug 1: acceptor QP-slot release.
 *
 * The acceptor claims an ep->qps_accepted slot at CONNECT_REQUEST and must
 * return it on ANY teardown -- including a half-open child rejected before
 * ESTABLISHED. Releasing only inside the `established` branch leaked the slot
 * and the acceptor then refused every future connect. Release iff acceptor and
 * the slot is still held; the caller clears the flag to avoid a double release.
 */
static void test_acceptor_should_release_slot(struct kunit *test)
{
	static const struct {
		bool is_initiator;
		bool slot_held;
		bool expect;
	} cases[] = {
		/* acceptor holding a slot (established OR half-open) -> release */
		{ false, true,  true  },
		/* acceptor, slot already released -> no double-decrement */
		{ false, false, false },
		/* initiator never owns an acceptor slot */
		{ true,  true,  false },
		{ true,  false, false },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_acceptor_should_release_slot(cases[i].is_initiator,
							    cases[i].slot_held);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: is_initiator=%d slot_held=%d", i,
				    (int)cases[i].is_initiator,
				    (int)cases[i].slot_held);
	}
}

/*
 * design 33 Bug 2: stale UDS listen-socket unlink decision.
 *
 * Only the initiator binds a pathname listen socket, and the node must be
 * unlinked (before bind and on cleanup) so a re-`urp add` doesn't fail -98
 * EADDRINUSE. Unlink iff initiator and a listen path is configured. (Only the
 * decision is pure; the vfs_unlink action is integration-tested.)
 */
static void test_should_unlink_listen_path(struct kunit *test)
{
	static const struct {
		bool is_initiator;
		bool path_set;
		bool expect;
	} cases[] = {
		{ true,  true,  true  }, /* initiator with a listen path */
		{ true,  false, false }, /* initiator, no path (shouldn't happen) */
		{ false, true,  false }, /* acceptor has no listen socket */
		{ false, false, false },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_should_unlink_listen_path(cases[i].is_initiator,
							 cases[i].path_set);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: is_initiator=%d path_set=%d", i,
				    (int)cases[i].is_initiator,
				    (int)cases[i].path_set);
	}
}

/*
 * design 33 Phase 1: initiator connect-retry eligibility.
 *
 * Only the initiator dials, so the acceptor never retries. The initiator
 * retries while under the attempt budget; max_attempts == 0 is the runtime
 * "off switch" (connect_max_attempts sysctl). Same predicate covers first
 * bring-up and reconnect-on-drop (counter resets to 0 on ESTABLISHED).
 */
static void test_should_retry_connect(struct kunit *test)
{
	static const struct {
		bool is_initiator;
		unsigned int attempts;
		unsigned int max_attempts;
		bool expect;
	} cases[] = {
		{ false, 0,   300, false }, /* acceptor never retries */
		{ false, 5,   300, false },
		{ true,  0,   300, true  }, /* initiator, budget remaining */
		{ true,  299, 300, true  }, /* last allowed attempt */
		{ true,  300, 300, false }, /* budget exhausted */
		{ true,  301, 300, false },
		{ true,  0,   0,   false }, /* max_attempts=0 disables retry */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_should_retry_connect(cases[i].is_initiator,
						    cases[i].attempts,
						    cases[i].max_attempts);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: is_initiator=%d attempts=%u max=%u",
				    i, (int)cases[i].is_initiator,
				    cases[i].attempts, cases[i].max_attempts);
	}
}

/*
 * design 33 Phase 1: capped-exponential connect backoff (base << attempt,
 * clamped to ceil). base=100 ceil=2000: 100,200,400,800,1600, then saturate
 * at 2000. Must never invoke shift UB / overflow at large attempts, and a
 * mis-ordered (ceil < base) pair degrades to constant ceil.
 */
static void test_connect_backoff_ms(struct kunit *test)
{
	static const struct {
		unsigned int attempt;
		unsigned int base_ms;
		unsigned int ceil_ms;
		unsigned int expect;
	} cases[] = {
		{ 0,       100,  2000, 100  },
		{ 1,       100,  2000, 200  },
		{ 2,       100,  2000, 400  },
		{ 3,       100,  2000, 800  },
		{ 4,       100,  2000, 1600 },
		{ 5,       100,  2000, 2000 }, /* clamps */
		{ 50,      100,  2000, 2000 },
		{ 1000000, 100,  2000, 2000 }, /* no shift UB / overflow */
		{ 0,       1000, 500,  500  }, /* ceil < base -> constant ceil */
		{ 3,       1000, 500,  500  },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int got = urp_connect_backoff_ms(cases[i].attempt,
							  cases[i].base_ms,
							  cases[i].ceil_ms);

		KUNIT_EXPECT_EQ_MSG(test, got, cases[i].expect,
				    "case %d: attempt=%u base=%u ceil=%u", i,
				    cases[i].attempt, cases[i].base_ms,
				    cases[i].ceil_ms);
	}
}

/*
 * design 33 Phase 1.5: liveness probing runs on all multi-QP endpoints (health
 * / load balancing) and additionally on a single-QP *initiator* (silent-drop
 * keepalive). A single-QP acceptor stays quiet.
 */
static void test_should_emit_probes(struct kunit *test)
{
	static const struct {
		bool is_initiator;
		unsigned int num_qps;
		bool expect;
	} cases[] = {
		{ false, 1, false }, /* single-QP acceptor: quiet baseline */
		{ true,  1, true  }, /* single-QP initiator: silent-drop keepalive */
		{ false, 4, true  }, /* multi-QP: health/LB probing both roles */
		{ true,  4, true  },
		{ false, 0, false }, /* no QPs: nothing to probe */
		{ true,  0, true  }, /* initiator predicate keys on role, not count */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_should_emit_probes(cases[i].is_initiator,
						  cases[i].num_qps);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: is_initiator=%d num_qps=%u",
				    i, (int)cases[i].is_initiator,
				    cases[i].num_qps);
	}
}

/*
 * design 33 Phase 1.5: a probe-detected silent drop converts to a reconnect
 * only on the initiator, and only for a QP that actually carried a session
 * (was_established) -- a never-established bring-up failure is the CM-event
 * retry path's job, not the liveness path's.
 */
static void test_silent_drop_should_reconnect(struct kunit *test)
{
	static const struct {
		bool is_initiator;
		bool was_established;
		bool expect;
	} cases[] = {
		{ true,  true,  true  }, /* initiator, live session dropped */
		{ true,  false, false }, /* initiator, never established */
		{ false, true,  false }, /* acceptor never reconnects */
		{ false, false, false },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_silent_drop_should_reconnect(cases[i].is_initiator,
							    cases[i].was_established);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: is_initiator=%d was_established=%d",
				    i, (int)cases[i].is_initiator,
				    (int)cases[i].was_established);
	}
}

/*
 * design 33 Phase 2: the first UDS client connect fires a lifetime one-shot
 * lazy RDMA dial -- initiator only (the acceptor waits for CONNECT_REQUEST),
 * and only on the accept that flips the connect_started latch 0->1.
 */
static void test_should_start_lazy_connect(struct kunit *test)
{
	static const struct {
		bool is_initiator;
		bool already_started;
		bool expect;
	} cases[] = {
		{ true,  false, true  }, /* initiator, first accept: dial */
		{ true,  true,  false }, /* initiator, latch set: no re-dial */
		{ false, false, false }, /* acceptor never lazy-dials */
		{ false, true,  false },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bool got = urp_should_start_lazy_connect(cases[i].is_initiator,
							 cases[i].already_started);

		KUNIT_EXPECT_EQ_MSG(test, (int)got, (int)cases[i].expect,
				    "case %d: is_initiator=%d already_started=%d",
				    i, (int)cases[i].is_initiator,
				    (int)cases[i].already_started);
	}
}

static struct kunit_case urp_test_cases[] = {
	KUNIT_CASE(test_frame_roundtrip_data),
	KUNIT_CASE(test_frame_roundtrip_control),
	KUNIT_CASE(test_frame_roundtrip_probe),
	KUNIT_CASE(test_frame_max_payload),
	KUNIT_CASE(test_frame_zero_payload),
	KUNIT_CASE(test_frame_all_flags),
	KUNIT_CASE(test_frame_max_stream_id),
	KUNIT_CASE(test_frame_max_seq),
	KUNIT_CASE(test_frame_endianness),
	KUNIT_CASE(test_buf_list_init),
	KUNIT_CASE(test_buf_list_alloc_free),
	KUNIT_CASE(test_buf_list_exhaustion),
	/* Phase 3a Step 9 additions */
	KUNIT_CASE(test_credit_initial_state),
	KUNIT_CASE(test_credit_consume_all),
	KUNIT_CASE(test_credit_consume_below_zero),
	KUNIT_CASE(test_credit_grant_restores),
	KUNIT_CASE(test_credit_record_recv_threshold),
	KUNIT_CASE(test_credit_take_grants_resets),
	KUNIT_CASE(test_credit_initial_one),
	KUNIT_CASE(test_credit_initial_zero),
	KUNIT_CASE(test_reorder_in_order),
	KUNIT_CASE(test_reorder_out_of_order),
	KUNIT_CASE(test_reorder_duplicate),
	KUNIT_CASE(test_reorder_already_delivered),
	KUNIT_CASE(test_reorder_buffer_full),
	KUNIT_CASE(test_reorder_seq_saturates),
	KUNIT_CASE(test_qp_select_round_robin_determinism),
	KUNIT_CASE(test_qp_select_skips_unestablished),
	KUNIT_CASE(test_qp_select_returns_null_when_none_ready),
	/* Phase 3b Step 1 additions */
	KUNIT_CASE(test_probe_ping_roundtrip),
	KUNIT_CASE(test_probe_pong_echoes_ping),
	KUNIT_CASE(test_probe_payload_sizes),
	/* design 28 E2: stream state machine */
	KUNIT_CASE(test_stream_next_state),
	/* design 28 E1: RX frame classifier */
	KUNIT_CASE(test_classify_frame),
	/* design 32: acceptor connection plan (eager-connect gate) */
	KUNIT_CASE(test_acceptor_eager_connect),
	/* design 32: credit-grant routing (per-stream vs per-QP pool) */
	KUNIT_CASE(test_credit_grant_routing),
	/* design 33 Bug 1: acceptor QP-slot release on any teardown */
	KUNIT_CASE(test_acceptor_should_release_slot),
	/* design 33 Bug 2: stale UDS listen-socket unlink decision */
	KUNIT_CASE(test_should_unlink_listen_path),
	/* design 33 Phase 1: initiator connect-retry eligibility + backoff */
	KUNIT_CASE(test_should_retry_connect),
	KUNIT_CASE(test_connect_backoff_ms),
	KUNIT_CASE(test_should_emit_probes),
	KUNIT_CASE(test_silent_drop_should_reconnect),
	/* design 33 Phase 2: lazy-connect on first UDS accept */
	KUNIT_CASE(test_should_start_lazy_connect),
	KUNIT_CASE(test_resolve_num_bufs),
	KUNIT_CASE(test_resolve_buf_size),
	KUNIT_CASE(test_ep_max_payload),
	/* design 31 section 31.10: urp-fast uring_cmd trust boundary */
	KUNIT_CASE(test_cmd_validate_data),
	KUNIT_CASE(test_cmd_validate_reg),
	KUNIT_CASE(test_cmd_own),
	{}
};

static struct kunit_suite urp_test_suite = {
	.name = "urp",
	.test_cases = urp_test_cases,
};

kunit_test_suite(urp_test_suite);
