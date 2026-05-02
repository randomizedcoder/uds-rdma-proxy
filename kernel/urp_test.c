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
	{}
};

static struct kunit_suite urp_test_suite = {
	.name = "urp",
	.test_cases = urp_test_cases,
};

kunit_test_suite(urp_test_suite);
