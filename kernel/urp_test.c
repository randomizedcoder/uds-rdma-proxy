// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for urp kernel module.
 *
 * Tests frame encode/decode and buffer free-list management.
 * Compiled into urp.ko only when CONFIG_KUNIT=y.
 */

#include <kunit/test.h>
#include "urp.h"

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

/* ---- Test suite registration ---- */

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
	{}
};

static struct kunit_suite urp_test_suite = {
	.name = "urp",
	.test_cases = urp_test_cases,
};

kunit_test_suite(urp_test_suite);
