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
	i = 0;
	struct urp_buffer *buf;
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
 *      uds_rdma_protocol::credit unit tests) ---- */

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
 *      uds_rdma_protocol::reorder unit tests against the C backend) ---- */

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
	qps[0].ep = qps[1].ep = &ep;
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
	 * (uds_rdma_protocol::constants::{PING,PONG}_PAYLOAD_SIZE). */
	KUNIT_EXPECT_EQ(test, URP_PING_PAYLOAD_SIZE, 32);
	KUNIT_EXPECT_EQ(test, URP_PONG_PAYLOAD_SIZE, 48);
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
	KUNIT_CASE(test_qp_select_round_robin_determinism),
	KUNIT_CASE(test_qp_select_skips_unestablished),
	KUNIT_CASE(test_qp_select_returns_null_when_none_ready),
	/* Phase 3b Step 1 additions */
	KUNIT_CASE(test_probe_ping_roundtrip),
	KUNIT_CASE(test_probe_pong_echoes_ping),
	KUNIT_CASE(test_probe_payload_sizes),
	{}
};

static struct kunit_suite urp_test_suite = {
	.name = "urp",
	.test_cases = urp_test_cases,
};

kunit_test_suite(urp_test_suite);
