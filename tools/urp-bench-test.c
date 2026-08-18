// SPDX-License-Identifier: GPL-2.0
/*
 * Table-driven unit tests for the urp-bench pure core (design 30 §30.11,
 * work item B5, C half). Standalone binary, no framework, no liburing:
 * built and run by the sandboxed `urp-bench-units` nix check.
 *
 * Idiom follows kernel/urp_test.c (case arrays + loop, design 28). The
 * BENCH_VECTORS hex table below is duplicated verbatim in the Rust twin
 * (crates/urp-bench) as the cross-language oracle — keep them identical.
 */
#include <stdio.h>
#include <string.h>

#include "urp-bench-core.h"

static int failures;
static int checks;

#define CHECK(cond, ...)						\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			failures++;					\
			printf("FAIL %s:%d: ", __func__, __LINE__);	\
			printf(__VA_ARGS__);				\
			printf("\n");					\
		}							\
	} while (0)

/* ---- shared cross-language test vectors (also in Rust frame.rs) ------- */

static const struct {
	const char *name;
	struct bench_hdr hdr;
	uint8_t bytes[BENCH_HDR_SIZE];
} vectors[] = {
	{
		.name = "minimal",
		.hdr = { BENCH_MAGIC, 1, 0, 0x0001, 0, 0, 0 },
		.bytes = { 0x55, 0x52, 0x50, 0x42, 0x01, 0x00, 0x01, 0x00,
			   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	},
	{
		.name = "echo-1k",
		.hdr = { BENCH_MAGIC, 1, BENCH_FLAG_ECHO, 0xbeef, 1024,
			 0x12345678, 0x0102030405060708ull },
		.bytes = { 0x55, 0x52, 0x50, 0x42, 0x01, 0x01, 0xef, 0xbe,
			   0x00, 0x04, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
			   0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 },
	},
	{
		.name = "fin-extremes",
		.hdr = { BENCH_MAGIC, 1, BENCH_FLAG_FIN, 0xffff, 0,
			 0xffffffffu, 0xffffffffffffffffull },
		.bytes = { 0x55, 0x52, 0x50, 0x42, 0x01, 0x02, 0xff, 0xff,
			   0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
			   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
	},
	{
		.name = "echo-fin-maxpayload",
		.hdr = { BENCH_MAGIC, 1, BENCH_FLAG_ECHO | BENCH_FLAG_FIN,
			 0x0042, BENCH_PAYLOAD_MAX, 7, 1 },
		.bytes = { 0x55, 0x52, 0x50, 0x42, 0x01, 0x03, 0x42, 0x00,
			   0x00, 0x00, 0x10, 0x00, 0x07, 0x00, 0x00, 0x00,
			   0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	},
};

static void test_hdr_vectors(void)
{
	size_t i;

	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		uint8_t out[BENCH_HDR_SIZE];
		struct bench_hdr dec;
		int ret;

		bench_hdr_encode(&vectors[i].hdr, out);
		CHECK(memcmp(out, vectors[i].bytes, BENCH_HDR_SIZE) == 0,
		      "vector %s: encode mismatch", vectors[i].name);

		ret = bench_hdr_decode(vectors[i].bytes, BENCH_HDR_SIZE, 0,
				       &dec);
		CHECK(ret == 0, "vector %s: decode ret %d", vectors[i].name,
		      ret);
		CHECK(memcmp(&dec, &vectors[i].hdr, sizeof(dec)) == 0,
		      "vector %s: decode fields mismatch", vectors[i].name);
	}
}

static void test_hdr_decode(void)
{
	/* A valid template mutated per case. */
	const struct bench_hdr tmpl = { BENCH_MAGIC, 1, 0, 7, 16, 3, 99 };
	static const struct {
		const char *name;
		size_t len;		/* fed length */
		int mut_off;		/* byte to overwrite, -1 = none */
		uint8_t mut_val;
		uint32_t max_payload;	/* decoder cap param */
		uint32_t set_payload;	/* payload_len override, 0 = keep */
		int want;
	} cases[] = {
		{ "empty", 0, -1, 0, 0, 0, -BENCH_ESHORT },
		{ "one-byte", 1, -1, 0, 0, 0, -BENCH_ESHORT },
		{ "short-23", 23, -1, 0, 0, 0, -BENCH_ESHORT },
		{ "exact-24", 24, -1, 0, 0, 0, 0 },
		{ "bad-magic", 24, 0, 0xAA, 0, 0, -BENCH_EMAGIC },
		{ "bad-version", 24, 4, 2, 0, 0, -BENCH_EVERSION },
		{ "reserved-bit2", 24, 5, 0x04, 0, 0, -BENCH_EFLAGS },
		{ "reserved-bit7", 24, 5, 0x80, 0, 0, -BENCH_EFLAGS },
		{ "flags-valid-both", 24, 5, 0x03, 0, 0, 0 },
		{ "payload-at-abs-cap", 24, -1, 0, 0, BENCH_PAYLOAD_MAX, 0 },
		{ "payload-over-abs-cap", 24, -1, 0, 0,
		  BENCH_PAYLOAD_MAX + 1, -BENCH_ECAP },
		{ "payload-at-cfg-cap", 24, -1, 0, 16, 16, 0 },
		{ "payload-over-cfg-cap", 24, -1, 0, 16, 17, -BENCH_ECAP },
		{ "payload-u32-max", 24, -1, 0, 0, 0xffffffffu,
		  -BENCH_ECAP },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t buf[BENCH_HDR_SIZE];
		struct bench_hdr h = tmpl, dec;
		int ret;

		if (cases[i].set_payload)
			h.payload_len = cases[i].set_payload;
		bench_hdr_encode(&h, buf);
		if (cases[i].mut_off >= 0)
			buf[cases[i].mut_off] = cases[i].mut_val;
		ret = bench_hdr_decode(buf, cases[i].len,
				       cases[i].max_payload, &dec);
		CHECK(ret == cases[i].want, "case %s: ret %d want %d",
		      cases[i].name, ret, cases[i].want);
	}
}

static void test_payload_fill_verify(void)
{
	uint8_t a[257], b[257];

	bench_fill_payload(a, sizeof(a), 1, 42);
	bench_fill_payload(b, sizeof(b), 1, 42);
	CHECK(memcmp(a, b, sizeof(a)) == 0, "fill not deterministic");
	CHECK(bench_verify_payload(a, sizeof(a), 1, 42) == 0,
	      "verify of own fill failed");

	bench_fill_payload(b, sizeof(b), 1, 43);
	CHECK(memcmp(a, b, sizeof(a)) != 0, "seq not mixed into stream");
	bench_fill_payload(b, sizeof(b), 2, 42);
	CHECK(memcmp(a, b, sizeof(a)) != 0, "origin not mixed into stream");

	a[100] ^= 1;
	CHECK(bench_verify_payload(a, sizeof(a), 1, 42) == -BENCH_ECORRUPT,
	      "flipped byte not detected");

	/* zero-seed corner: origin 0, seq 0 must still produce a stream */
	bench_fill_payload(a, 8, 0, 0);
	CHECK(bench_verify_payload(a, 8, 0, 0) == 0, "zero-seed roundtrip");

	/* zero-length is trivially valid */
	CHECK(bench_verify_payload(a, 0, 9, 9) == 0, "empty payload");
}

/* ---- deframer --------------------------------------------------------- */

struct df_ctx {
	int msgs;
	int fail_on;		/* callback returns error on msg N (1-based) */
	struct bench_hdr last;
	uint8_t last_payload[64];
};

static int df_cb(void *ctx, const struct bench_hdr *hdr,
		 const uint8_t *payload)
{
	struct df_ctx *c = ctx;

	c->msgs++;
	c->last = *hdr;
	if (hdr->payload_len && hdr->payload_len <= sizeof(c->last_payload))
		memcpy(c->last_payload, payload, hdr->payload_len);
	if (c->fail_on && c->msgs == c->fail_on)
		return -BENCH_ECORRUPT;
	return 0;
}

/* Build one wire message into buf, returns total length. */
static size_t mk_msg(uint8_t *buf, uint16_t origin, uint32_t seq,
		     uint32_t payload_len, uint8_t flags)
{
	struct bench_hdr h = { BENCH_MAGIC, 1, flags, origin, payload_len,
			       seq, 1000 + seq };

	bench_hdr_encode(&h, buf);
	bench_fill_payload(buf + BENCH_HDR_SIZE, payload_len, origin, seq);
	return BENCH_HDR_SIZE + payload_len;
}

static void test_deframer_chunking(void)
{
	/* Two back-to-back messages, fed at every possible split point. */
	uint8_t wire[2 * (BENCH_HDR_SIZE + 32)];
	size_t n1 = mk_msg(wire, 1, 10, 32, 0);
	size_t n2 = mk_msg(wire + n1, 1, 11, 32, 0);
	size_t total = n1 + n2;
	size_t split;

	for (split = 0; split <= total; split++) {
		uint8_t asm_buf[64];
		struct bench_deframer d;
		struct df_ctx ctx = { 0 };
		int ret;

		bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 0);
		ret = bench_deframe_feed(&d, wire, split, df_cb, &ctx);
		CHECK(ret == 0, "split %zu: first feed ret %d", split, ret);
		ret = bench_deframe_feed(&d, wire + split, total - split,
					 df_cb, &ctx);
		CHECK(ret == 0, "split %zu: second feed ret %d", split, ret);
		CHECK(ctx.msgs == 2, "split %zu: %d msgs, want 2", split,
		      ctx.msgs);
		CHECK(ctx.last.seq == 11, "split %zu: last seq %u", split,
		      ctx.last.seq);
		CHECK(bench_verify_payload(ctx.last_payload, 32, 1, 11) == 0,
		      "split %zu: last payload corrupt", split);
		/* Only splits inside a message force reassembly. */
		if (split == 0 || split == n1 || split == total)
			CHECK(d.msgs_reassembled == 0,
			      "split %zu: clean split reassembled %llu",
			      split,
			      (unsigned long long)d.msgs_reassembled);
		else
			CHECK(d.msgs_reassembled >= 1,
			      "split %zu: mid-msg split not counted", split);
	}
}

static void test_deframer_drip(void)
{
	/* 1-byte drip feed of hdr-only + payload message. */
	uint8_t wire[BENCH_HDR_SIZE + BENCH_HDR_SIZE + 16];
	size_t n1 = mk_msg(wire, 2, 0, 0, BENCH_FLAG_FIN);
	size_t n2 = mk_msg(wire + n1, 2, 1, 16, 0);
	size_t total = n1 + n2, i;
	uint8_t asm_buf[32];
	struct bench_deframer d;
	struct df_ctx ctx = { 0 };

	bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 0);
	for (i = 0; i < total; i++) {
		int ret = bench_deframe_feed(&d, wire + i, 1, df_cb, &ctx);

		CHECK(ret == 0, "drip byte %zu: ret %d", i, ret);
	}
	CHECK(ctx.msgs == 2, "drip: %d msgs, want 2", ctx.msgs);
	CHECK(d.msgs_total == 2, "drip: msgs_total %llu",
	      (unsigned long long)d.msgs_total);
	CHECK(d.msgs_reassembled == 2, "drip: everything reassembled");
}

static void test_deframer_errors(void)
{
	uint8_t asm_buf[64];
	uint8_t wire[BENCH_HDR_SIZE + 32];
	struct bench_deframer d;
	struct df_ctx ctx = { 0 };
	int ret;

	/* garbage first byte: hard error, no resync */
	mk_msg(wire, 1, 0, 0, 0);
	wire[0] = 0xAA;
	bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 0);
	ret = bench_deframe_feed(&d, wire, BENCH_HDR_SIZE, df_cb, &ctx);
	CHECK(ret == -BENCH_EMAGIC, "garbage magic: ret %d", ret);

	/* config-cap violation detected at decode time */
	mk_msg(wire, 1, 0, 32, 0);
	bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 16);
	ret = bench_deframe_feed(&d, wire, sizeof(wire), df_cb, &ctx);
	CHECK(ret == -BENCH_ECAP, "cfg cap: ret %d", ret);

	/* payload larger than assembly buffer, forced onto the slow path */
	mk_msg(wire, 1, 0, 32, 0);
	bench_deframer_init(&d, asm_buf, 16, 0);
	ret = bench_deframe_feed(&d, wire, 1, df_cb, &ctx);	/* stage.. */
	CHECK(ret == 0, "asm-cap stage: ret %d", ret);
	ret = bench_deframe_feed(&d, wire + 1, sizeof(wire) - 1, df_cb,
				 &ctx);
	CHECK(ret == -BENCH_ECAP, "asm cap: ret %d", ret);

	/* callback error propagates */
	mk_msg(wire, 1, 5, 8, 0);
	bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 0);
	ctx.msgs = 0;
	ctx.fail_on = 1;
	ret = bench_deframe_feed(&d, wire, BENCH_HDR_SIZE + 8, df_cb, &ctx);
	CHECK(ret == -BENCH_ECORRUPT, "cb error: ret %d", ret);

	/* empty chunk is a no-op */
	bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 0);
	ret = bench_deframe_feed(&d, wire, 0, df_cb, &ctx);
	CHECK(ret == 0, "empty chunk: ret %d", ret);
}

/* ---- tracker ---------------------------------------------------------- */

static void test_tracker(void)
{
	enum { W = 4 };
	uint64_t sent[W];
	uint32_t seqs[W];
	uint8_t inflight[W];
	struct bench_tracker t;
	int64_t rtt;
	int ret;

	bench_tracker_init(&t, sent, seqs, inflight, W);

	/* in-order */
	CHECK(bench_track_sent(&t, 1, 100) == 0, "sent 1");
	rtt = bench_track_echo(&t, 1, 150);
	CHECK(rtt == 50, "in-order rtt %lld", (long long)rtt);

	/* out-of-order */
	CHECK(bench_track_sent(&t, 2, 200) == 0, "sent 2");
	CHECK(bench_track_sent(&t, 3, 210) == 0, "sent 3");
	rtt = bench_track_echo(&t, 3, 240);
	CHECK(rtt == 30, "ooo rtt(3) %lld", (long long)rtt);
	rtt = bench_track_echo(&t, 2, 260);
	CHECK(rtt == 60, "ooo rtt(2) %lld", (long long)rtt);

	/* duplicate echo */
	rtt = bench_track_echo(&t, 2, 270);
	CHECK(rtt == -BENCH_EDUP, "dup ret %lld", (long long)rtt);
	CHECK(t.dups == 1, "dup counter %llu", (unsigned long long)t.dups);

	/* unknown seq (never sent) */
	rtt = bench_track_echo(&t, 77, 280);
	CHECK(rtt == -BENCH_EUNKNOWN, "unknown ret %lld", (long long)rtt);

	/* window full: slot collision at seq % W */
	CHECK(bench_track_sent(&t, 8, 300) == 0, "sent 8");
	ret = bench_track_sent(&t, 12, 310);	/* 12 % 4 == 8 % 4 */
	CHECK(ret == -BENCH_EFULL, "window-full ret %d", ret);
	rtt = bench_track_echo(&t, 8, 350);
	CHECK(rtt == 50, "post-full rtt(8) %lld", (long long)rtt);

	/* u32 wraparound */
	CHECK(bench_track_sent(&t, 0xffffffffu, 400) == 0, "sent wrap-1");
	CHECK(bench_track_sent(&t, 0, 410) == 0, "sent wrap-0");
	rtt = bench_track_echo(&t, 0xffffffffu, 450);
	CHECK(rtt == 50, "wrap rtt(max) %lld", (long long)rtt);
	rtt = bench_track_echo(&t, 0, 460);
	CHECK(rtt == 50, "wrap rtt(0) %lld", (long long)rtt);
	CHECK(t.inflight_count == 0, "inflight drained: %u",
	      t.inflight_count);

	/* clock going backwards clamps to 0, never negative */
	CHECK(bench_track_sent(&t, 20, 1000) == 0, "sent 20");
	rtt = bench_track_echo(&t, 20, 999);
	CHECK(rtt == 0, "clock-backwards rtt %lld", (long long)rtt);
}

/* ---- batch planner ---------------------------------------------------- */

static void test_batch_plan(void)
{
	static const struct {
		const char *name;
		uint32_t window;
		uint32_t inflight;
		uint64_t remaining;
		uint32_t want;
	} cases[] = {
		{ "empty-window-free", 8, 0, 100, 8 },
		{ "partial-inflight", 8, 5, 100, 3 },
		{ "window-full", 8, 8, 100, 0 },
		{ "remaining-below-room", 8, 2, 3, 3 },
		{ "remaining-zero", 8, 0, 0, 0 },
		{ "remaining-one", 8, 7, 1, 1 },
		{ "window-one", 1, 0, 100, 1 },
		{ "window-one-busy", 1, 1, 100, 0 },
		{ "huge-remaining", 256, 0, ~0ull, 256 },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct bench_batch b = { cases[i].window };
		uint32_t got = bench_batch_plan(&b, cases[i].inflight,
						cases[i].remaining);

		CHECK(got == cases[i].want, "case %s: got %u want %u",
		      cases[i].name, got, cases[i].want);
	}
}

/* ---- bufring ---------------------------------------------------------- */

static void test_bufring(void)
{
	enum { N = 3 };
	uint16_t free_idx[N];
	uint8_t in_use[N];
	struct bench_bufring r;
	int a, b, c, ret;

	bench_bufring_init(&r, free_idx, in_use, N);

	a = bench_bufring_take(&r);
	b = bench_bufring_take(&r);
	c = bench_bufring_take(&r);
	CHECK(a >= 0 && b >= 0 && c >= 0, "takes: %d %d %d", a, b, c);
	CHECK(a != b && b != c && a != c, "takes distinct: %d %d %d",
	      a, b, c);

	ret = bench_bufring_take(&r);
	CHECK(ret == -BENCH_EEMPTY, "exhausted ret %d", ret);

	CHECK(bench_bufring_recycle(&r, (uint32_t)b) == 0, "recycle b");
	ret = bench_bufring_recycle(&r, (uint32_t)b);
	CHECK(ret == -BENCH_EDUP, "double recycle ret %d", ret);
	ret = bench_bufring_recycle(&r, N);
	CHECK(ret == -BENCH_ERANGE, "out-of-range ret %d", ret);

	ret = bench_bufring_take(&r);
	CHECK(ret == b, "reuse recycled idx: got %d want %d", ret, b);

	/* corner: recycle a never-taken index (in_use == 0) → EDUP */
	{
		uint16_t fi[2];
		uint8_t iu[2];
		struct bench_bufring r2;

		bench_bufring_init(&r2, fi, iu, 2);
		CHECK(bench_bufring_recycle(&r2, 0) == -BENCH_EDUP,
		      "recycle never-taken idx → EDUP");
	}

	/* corner: N == 1 (single buffer, take/exhaust/recycle/reuse) */
	{
		uint16_t fi[1];
		uint8_t iu[1];
		struct bench_bufring r1;
		int x;

		bench_bufring_init(&r1, fi, iu, 1);
		x = bench_bufring_take(&r1);
		CHECK(x == 0, "N=1 take → 0 (got %d)", x);
		CHECK(bench_bufring_take(&r1) == -BENCH_EEMPTY, "N=1 exhausted");
		CHECK(bench_bufring_recycle(&r1, 0) == 0, "N=1 recycle");
		CHECK(bench_bufring_take(&r1) == 0, "N=1 reuse → 0");
	}

	/* conservation (the steady-state "no leak" invariant that the bufring
	 * event loop depends on): take all, recycle all, repeat — free_count
	 * must return to full and every index be reusable, forever. */
	{
		enum { M = 8, ROUNDS = 4 };
		uint16_t fi[M];
		uint8_t iu[M], seen[M];
		struct bench_bufring rc;
		int round, i, idx, all;

		bench_bufring_init(&rc, fi, iu, M);
		for (round = 0; round < ROUNDS; round++) {
			memset(seen, 0, sizeof(seen));
			for (i = 0; i < M; i++) {
				idx = bench_bufring_take(&rc);
				CHECK(idx >= 0 && idx < M && !seen[idx],
				      "round %d take %d distinct (idx %d)",
				      round, i, idx);
				seen[idx] = 1;
			}
			CHECK(bench_bufring_take(&rc) == -BENCH_EEMPTY,
			      "round %d drained", round);
			CHECK(rc.free_count == 0, "round %d free_count 0", round);
			all = 1;
			for (i = 0; i < M; i++)
				all &= (bench_bufring_recycle(&rc, (uint32_t)i) == 0);
			CHECK(all, "round %d recycle all", round);
			CHECK(rc.free_count == M, "round %d free_count restored", round);
		}
	}
}

/* ---- stats ------------------------------------------------------------ */

static void test_stats(void)
{
	uint64_t samples[100];
	struct bench_stats s;
	struct bench_stats_result res;
	uint64_t i;
	int ret;

	/* known distribution 1..100 -> exact percentiles */
	bench_stats_init(&s, samples, 100);
	for (i = 100; i >= 1; i--)	/* reversed: finalize must sort */
		bench_stats_add(&s, i * 1000);
	ret = bench_stats_finalize(&s, &res);
	CHECK(ret == 0, "finalize ret %d", ret);
	CHECK(res.min_ns == 1000, "min %llu",
	      (unsigned long long)res.min_ns);
	CHECK(res.max_ns == 100000, "max %llu",
	      (unsigned long long)res.max_ns);
	CHECK(res.p50_ns == 51000, "p50 %llu",
	      (unsigned long long)res.p50_ns);
	CHECK(res.p99_ns == 100000, "p99 %llu",
	      (unsigned long long)res.p99_ns);

	/* single sample */
	bench_stats_init(&s, samples, 100);
	bench_stats_add(&s, 42);
	ret = bench_stats_finalize(&s, &res);
	CHECK(ret == 0 && res.min_ns == 42 && res.max_ns == 42 &&
	      res.p50_ns == 42 && res.p99_ns == 42, "single sample");

	/* saturation drops beyond cap */
	bench_stats_init(&s, samples, 2);
	bench_stats_add(&s, 1);
	bench_stats_add(&s, 2);
	bench_stats_add(&s, 3);
	CHECK(s.count == 2 && s.dropped == 1, "saturation: count %zu drop %llu",
	      s.count, (unsigned long long)s.dropped);

	/* empty */
	bench_stats_init(&s, samples, 100);
	ret = bench_stats_finalize(&s, &res);
	CHECK(ret == -BENCH_EEMPTY, "empty ret %d", ret);
}

/* ---- config ----------------------------------------------------------- */

static void test_config(void)
{
	const struct bench_config good = {
		.role = BENCH_ROLE_CONNECT, .id = 1,
		.mode = BENCH_MODE_URING_RW, .verify = BENCH_VERIFY_HEADER,
		.msg_size = 4076, .batch = 32, .count = 1000,
	};
	static const struct {
		const char *name;
		/* deltas applied to `good` */
		int role;		/* -1 = keep */
		int64_t msg_size;	/* -1 = keep */
		int64_t batch;
		int64_t count;
		int64_t duration_s;
		int want;
	} cases[] = {
		{ "good", -1, -1, -1, -1, -1, 0 },
		{ "msg-below-hdr", -1, 23, -1, -1, -1, -BENCH_EINVAL },
		{ "msg-exact-hdr", -1, 24, -1, -1, -1, 0 },
		{ "msg-at-cap", -1, BENCH_MSG_MAX, -1, -1, -1, 0 },
		{ "msg-over-cap", -1, BENCH_MSG_MAX + 1, -1, -1, -1,
		  -BENCH_EINVAL },
		{ "batch-zero", -1, -1, 0, -1, -1, -BENCH_EINVAL },
		{ "batch-one", -1, -1, 1, -1, -1, 0 },
		{ "batch-max", -1, -1, BENCH_BATCH_MAX, -1, -1, 0 },
		{ "batch-over", -1, -1, BENCH_BATCH_MAX + 1, -1, -1,
		  -BENCH_EINVAL },
		{ "role-none", BENCH_ROLE_NONE, -1, -1, -1, -1,
		  -BENCH_EINVAL },
		{ "role-listen", BENCH_ROLE_LISTEN, -1, -1, -1, -1, 0 },
		{ "neither-count-nor-dur", -1, -1, -1, 0, 0, -BENCH_EINVAL },
		{ "both-count-and-dur", -1, -1, -1, 5, 5, -BENCH_EINVAL },
		{ "duration-only", -1, -1, -1, 0, 5, 0 },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct bench_config c = good;
		int ret;

		if (cases[i].role != -1)
			c.role = cases[i].role;
		if (cases[i].msg_size != -1)
			c.msg_size = (uint32_t)cases[i].msg_size;
		if (cases[i].batch != -1)
			c.batch = (uint32_t)cases[i].batch;
		if (cases[i].count != -1)
			c.count = (uint64_t)cases[i].count;
		if (cases[i].duration_s != -1)
			c.duration_s = (uint32_t)cases[i].duration_s;
		ret = bench_config_validate(&c);
		CHECK(ret == cases[i].want, "case %s: ret %d want %d",
		      cases[i].name, ret, cases[i].want);
	}
}

static void test_mode_verify_parse(void)
{
	static const struct {
		const char *s;
		int want_ret;
		enum bench_mode want_mode;
	} cases[] = {
		{ "blocking", 0, BENCH_MODE_BLOCKING },
		{ "uring-rw", 0, BENCH_MODE_URING_RW },
		{ "uring-fixed", 0, BENCH_MODE_URING_FIXED },
		{ "uring-bufring", 0, BENCH_MODE_URING_BUFRING },
		{ "uring-sqpoll", 0, BENCH_MODE_URING_SQPOLL },
		{ "uring-sendzc", 0, BENCH_MODE_URING_SENDZC },
		{ "bogus", -BENCH_EINVAL, 0 },
		{ "", -BENCH_EINVAL, 0 },
		{ "URING-RW", -BENCH_EINVAL, 0 },	/* case-sensitive */
	};
	size_t i;
	enum bench_verify v;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		enum bench_mode m = BENCH_MODE_BLOCKING;
		int ret = bench_mode_parse(cases[i].s, &m);

		CHECK(ret == cases[i].want_ret, "mode '%s': ret %d",
		      cases[i].s, ret);
		if (ret == 0) {
			CHECK(m == cases[i].want_mode, "mode '%s': got %d",
			      cases[i].s, m);
			CHECK(strcmp(bench_mode_str(m), cases[i].s) == 0,
			      "mode '%s': str roundtrip", cases[i].s);
		}
	}

	CHECK(bench_verify_parse("none", &v) == 0 && v == BENCH_VERIFY_NONE,
	      "verify none");
	CHECK(bench_verify_parse("header", &v) == 0 &&
	      v == BENCH_VERIFY_HEADER, "verify header");
	CHECK(bench_verify_parse("full", &v) == 0 && v == BENCH_VERIFY_FULL,
	      "verify full");
	CHECK(bench_verify_parse("nope", &v) == -BENCH_EINVAL,
	      "verify bogus");
}

static void test_pattern_parse(void)
{
	static const struct {
		const char *s;
		int want_ret;
		enum bench_pattern want;
	} cases[] = {
		{ "echo", 0, BENCH_PATTERN_ECHO },	/* P: default */
		{ "stream", 0, BENCH_PATTERN_STREAM },	/* P */
		{ "bogus", -BENCH_EINVAL, 0 },		/* N */
		{ "", -BENCH_EINVAL, 0 },		/* B: empty */
		{ "STREAM", -BENCH_EINVAL, 0 },		/* C: case-sensitive */
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		enum bench_pattern p = BENCH_PATTERN_ECHO;
		int ret = bench_pattern_parse(cases[i].s, &p);

		CHECK(ret == cases[i].want_ret, "pattern '%s': ret %d",
		      cases[i].s, ret);
		if (ret == 0) {
			CHECK(p == cases[i].want, "pattern '%s': got %d",
			      cases[i].s, p);
			CHECK(strcmp(bench_pattern_str(p), cases[i].s) == 0,
			      "pattern '%s': str roundtrip", cases[i].s);
		}
	}
}

/* ---- result formatting ------------------------------------------------ */

static void test_format_result(void)
{
	const struct bench_config cfg = {
		.role = BENCH_ROLE_CONNECT, .id = 1,
		.mode = BENCH_MODE_URING_FIXED,
		.verify = BENCH_VERIFY_HEADER,
		.msg_size = 4076, .batch = 32, .count = 100000,
	};
	const struct bench_report r = {
		.lang = "c",
		.cfg = &cfg,
		.rtt = { .min_ns = 7900, .max_ns = 310000, .p50_ns = 9800,
			 .p99_ns = 22100, .count = 100000 },
		.msgs = 100000,
		.bytes = 407600000ull,
		.elapsed_ns = 500000000ull,	/* 0.5 s */
		.syscalls = 26000,
		.cpu_ns = 380000000ull,
		.reassembled = 800,
		.msgs_rx_total = 200000,
	};
	char buf[512];
	char small[32];
	int len = bench_format_result(&r, buf, sizeof(buf));

	CHECK(len > 0, "format ret %d", len);
	CHECK(strncmp(buf, "BENCH_OK lang=c mode=uring-fixed msg_size=4076"
		      " batch=32 msgs=100000 ", 68) == 0,
	      "prefix mismatch: %.80s", buf);
	CHECK(strstr(buf, "mbps=815.2") != NULL, "mbps: %s", buf);
	CHECK(strstr(buf, "msgs_per_s=200000") != NULL, "msgs_per_s: %s",
	      buf);
	CHECK(strstr(buf, "p50_us=9.8") != NULL, "p50: %s", buf);
	CHECK(strstr(buf, "p99_us=22.1") != NULL, "p99: %s", buf);
	CHECK(strstr(buf, "syscalls_per_msg=0.09") != NULL, "syscalls: %s",
	      buf);
	CHECK(strstr(buf, "reassembled_pct=0.4") != NULL, "reassembled: %s",
	      buf);
	CHECK(strstr(buf, "verify=header") != NULL, "verify: %s", buf);
	CHECK((size_t)len == strlen(buf), "len %d vs strlen %zu", len,
	      strlen(buf));

	len = bench_format_result(&r, small, sizeof(small));
	CHECK(len == -BENCH_ENOSPC, "truncation ret %d", len);
}

int main(void)
{
	test_hdr_vectors();
	test_hdr_decode();
	test_payload_fill_verify();
	test_deframer_chunking();
	test_deframer_drip();
	test_deframer_errors();
	test_tracker();
	test_batch_plan();
	test_bufring();
	test_stats();
	test_config();
	test_mode_verify_parse();
	test_pattern_parse();
	test_format_result();

	printf("urp-bench-test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
