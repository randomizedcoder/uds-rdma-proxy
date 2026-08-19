// SPDX-License-Identifier: GPL-2.0
/*
 * urp-bench pure core — see urp-bench-core.h and design 30 §30.9.
 *
 * No liburing, no syscalls, no allocation: every function is a pure
 * transformation over caller-owned memory, so the table tests and the
 * libFuzzer harness cover the exact code the benchmark runs.
 *
 * LE encode/decode uses explicit byte operations (no pointer casts), so
 * the code is alignment-safe and identical under UBSan and on big-endian
 * hosts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urp-bench-core.h"

/* ---- LE byte codecs --------------------------------------------------- */

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)v);
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
	return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

/* ---- header codec ----------------------------------------------------- */

void bench_hdr_encode(const struct bench_hdr *h, uint8_t out[BENCH_HDR_SIZE])
{
	put_le32(out, h->magic);
	out[4] = h->version;
	out[5] = h->flags;
	put_le16(out + 6, h->origin_id);
	put_le32(out + 8, h->payload_len);
	put_le32(out + 12, h->seq);
	put_le64(out + 16, h->t_send_ns);
}

int bench_hdr_decode(const uint8_t *buf, size_t len, uint32_t max_payload,
		     struct bench_hdr *out)
{
	uint32_t cap = BENCH_PAYLOAD_MAX;

	if (len < BENCH_HDR_SIZE)
		return -BENCH_ESHORT;

	out->magic = get_le32(buf);
	out->version = buf[4];
	out->flags = buf[5];
	out->origin_id = get_le16(buf + 6);
	out->payload_len = get_le32(buf + 8);
	out->seq = get_le32(buf + 12);
	out->t_send_ns = get_le64(buf + 16);

	if (out->magic != BENCH_MAGIC)
		return -BENCH_EMAGIC;
	if (out->version != BENCH_VERSION)
		return -BENCH_EVERSION;
	if (out->flags & (uint8_t)~BENCH_FLAG_MASK)
		return -BENCH_EFLAGS;
	if (max_payload && max_payload < cap)
		cap = max_payload;
	if (out->payload_len > cap)
		return -BENCH_ECAP;
	return 0;
}

/* ---- deterministic payload -------------------------------------------- */

static uint32_t bench_seed(uint16_t origin_id, uint32_t seq)
{
	uint32_t s = ((uint32_t)origin_id << 16) ^ seq;

	return s ? s : 0x9e3779b9u;	/* xorshift32 state must be nonzero */
}

static uint32_t xorshift32(uint32_t x)
{
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

void bench_fill_payload(uint8_t *dst, size_t len, uint16_t origin_id,
			uint32_t seq)
{
	uint32_t x = bench_seed(origin_id, seq);
	size_t i;

	for (i = 0; i < len; i++) {
		if ((i & 3) == 0)
			x = xorshift32(x);
		dst[i] = (uint8_t)(x >> ((i & 3) * 8));
	}
}

int bench_verify_payload(const uint8_t *p, size_t len, uint16_t origin_id,
			 uint32_t seq)
{
	uint32_t x = bench_seed(origin_id, seq);
	size_t i;

	for (i = 0; i < len; i++) {
		if ((i & 3) == 0)
			x = xorshift32(x);
		if (p[i] != (uint8_t)(x >> ((i & 3) * 8)))
			return -BENCH_ECORRUPT;
	}
	return 0;
}

/* ---- incremental deframer --------------------------------------------- */

void bench_deframer_init(struct bench_deframer *d, uint8_t *asm_buf,
			 size_t asm_cap, uint32_t max_payload)
{
	memset(d, 0, sizeof(*d));
	d->state = BENCH_DF_WANT_HDR;
	d->asm_buf = asm_buf;
	d->asm_cap = asm_cap;
	d->max_payload = max_payload;
}

static int deframe_deliver(struct bench_deframer *d, const uint8_t *payload,
			   int reassembled, bench_msg_cb cb, void *ctx)
{
	int ret;

	d->msgs_total++;
	if (reassembled)
		d->msgs_reassembled++;
	ret = cb(ctx, &d->hdr, payload);
	d->state = BENCH_DF_WANT_HDR;
	d->hdr_have = 0;
	d->asm_have = 0;
	return ret;
}

int bench_deframe_feed(struct bench_deframer *d, const uint8_t *chunk,
		       size_t len, bench_msg_cb cb, void *ctx)
{
	size_t off = 0;

	while (off < len) {
		if (d->state == BENCH_DF_WANT_HDR) {
			size_t avail = len - off;
			int ret;

			/*
			 * Fast path: complete header AND payload lie inside
			 * this chunk with nothing staged — deliver in place,
			 * zero copies (this is the buffer the echo goes back
			 * out of, §30.5).
			 */
			if (d->hdr_have == 0 && avail >= BENCH_HDR_SIZE) {
				ret = bench_hdr_decode(chunk + off, avail,
						       d->max_payload,
						       &d->hdr);
				if (ret < 0 && ret != -BENCH_ESHORT)
					return ret;
				if (ret == 0 &&
				    avail - BENCH_HDR_SIZE >=
					    d->hdr.payload_len) {
					const uint8_t *payload =
						chunk + off + BENCH_HDR_SIZE;

					off += BENCH_HDR_SIZE +
					       d->hdr.payload_len;
					ret = deframe_deliver(d, payload, 0,
							      cb, ctx);
					if (ret < 0)
						return ret;
					continue;
				}
			}

			/* Slow path: stage header bytes. */
			{
				size_t want = BENCH_HDR_SIZE - d->hdr_have;
				size_t take = avail < want ? avail : want;

				memcpy(d->hdr_buf + d->hdr_have, chunk + off,
				       take);
				d->hdr_have += (uint32_t)take;
				off += take;
				if (d->hdr_have < BENCH_HDR_SIZE)
					return 0;	/* need more bytes */

				ret = bench_hdr_decode(d->hdr_buf,
						       BENCH_HDR_SIZE,
						       d->max_payload,
						       &d->hdr);
				if (ret < 0)
					return ret;
				if (d->hdr.payload_len > d->asm_cap)
					return -BENCH_ECAP;
				d->state = BENCH_DF_WANT_PAYLOAD;
				d->asm_have = 0;
				if (d->hdr.payload_len == 0) {
					ret = deframe_deliver(d, d->asm_buf,
							      1, cb, ctx);
					if (ret < 0)
						return ret;
				}
			}
		} else {	/* BENCH_DF_WANT_PAYLOAD */
			size_t want = d->hdr.payload_len - d->asm_have;
			size_t avail = len - off;
			size_t take = avail < want ? avail : want;
			int ret;

			memcpy(d->asm_buf + d->asm_have, chunk + off, take);
			d->asm_have += (uint32_t)take;
			off += take;
			if (d->asm_have < d->hdr.payload_len)
				return 0;	/* need more bytes */

			ret = deframe_deliver(d, d->asm_buf, 1, cb, ctx);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

/* ---- echo RTT tracker ------------------------------------------------- */

void bench_tracker_init(struct bench_tracker *t, uint64_t *sent_ns,
			uint32_t *seqs, uint8_t *in_flight, uint32_t window)
{
	memset(t, 0, sizeof(*t));
	t->sent_ns = sent_ns;
	t->seqs = seqs;
	t->in_flight = in_flight;
	t->window = window;
	memset(in_flight, 0, window);
	memset(seqs, 0, window * sizeof(*seqs));
	memset(sent_ns, 0, window * sizeof(*sent_ns));
}

int bench_track_sent(struct bench_tracker *t, uint32_t seq,
		     uint64_t t_send_ns)
{
	uint32_t slot = seq % t->window;

	if (t->in_flight[slot])
		return -BENCH_EFULL;
	t->in_flight[slot] = 1;
	t->seqs[slot] = seq;
	t->sent_ns[slot] = t_send_ns;
	t->inflight_count++;
	return 0;
}

int64_t bench_track_echo(struct bench_tracker *t, uint32_t seq,
			 uint64_t now_ns)
{
	uint32_t slot = seq % t->window;

	if (!t->in_flight[slot]) {
		/* Slot free: either never sent, or already echoed (dup). */
		if (t->seqs[slot] == seq && t->sent_ns[slot] != 0) {
			t->dups++;
			return -BENCH_EDUP;
		}
		t->unknowns++;
		return -BENCH_EUNKNOWN;
	}
	if (t->seqs[slot] != seq) {
		t->unknowns++;
		return -BENCH_EUNKNOWN;
	}
	t->in_flight[slot] = 0;
	t->inflight_count--;
	if (now_ns < t->sent_ns[slot])
		return 0;	/* clock oddity: clamp, never negative */
	return (int64_t)(now_ns - t->sent_ns[slot]);
}

/* ---- batch / window accounting ---------------------------------------- */

uint32_t bench_batch_plan(const struct bench_batch *b, uint32_t inflight,
			  uint64_t remaining)
{
	uint32_t room;

	if (inflight >= b->window)
		return 0;
	room = b->window - inflight;
	return remaining < room ? (uint32_t)remaining : room;
}

/* ---- provided-buffer-ring bookkeeping --------------------------------- */

void bench_bufring_init(struct bench_bufring *r, uint16_t *free_idx,
			uint8_t *in_use, uint32_t cap)
{
	uint32_t i;

	r->free_idx = free_idx;
	r->in_use = in_use;
	r->cap = cap;
	r->free_count = cap;
	for (i = 0; i < cap; i++) {
		free_idx[i] = (uint16_t)i;
		in_use[i] = 0;
	}
}

int bench_bufring_take(struct bench_bufring *r)
{
	uint16_t idx;

	if (r->free_count == 0)
		return -BENCH_EEMPTY;
	idx = r->free_idx[--r->free_count];
	r->in_use[idx] = 1;
	return idx;
}

int bench_bufring_recycle(struct bench_bufring *r, uint32_t idx)
{
	if (idx >= r->cap)
		return -BENCH_ERANGE;
	if (!r->in_use[idx])
		return -BENCH_EDUP;
	r->in_use[idx] = 0;
	r->free_idx[r->free_count++] = (uint16_t)idx;
	return 0;
}

/* ---- RTT statistics ---------------------------------------------------- */

void bench_stats_init(struct bench_stats *s, uint64_t *samples, size_t cap)
{
	s->samples = samples;
	s->cap = cap;
	s->count = 0;
	s->dropped = 0;
}

void bench_stats_add(struct bench_stats *s, uint64_t rtt_ns)
{
	if (s->count < s->cap)
		s->samples[s->count++] = rtt_ns;
	else
		s->dropped++;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	if (x < y)
		return -1;
	return x > y ? 1 : 0;
}

int bench_stats_finalize(struct bench_stats *s,
			 struct bench_stats_result *out)
{
	if (s->count == 0)
		return -BENCH_EEMPTY;
	qsort(s->samples, s->count, sizeof(uint64_t), cmp_u64);
	out->count = s->count;
	out->min_ns = s->samples[0];
	out->max_ns = s->samples[s->count - 1];
	out->p50_ns = s->samples[s->count / 2];
	out->p99_ns = s->samples[(s->count * 99) / 100];
	return 0;
}

/* ---- configuration ----------------------------------------------------- */

static const struct {
	const char *name;
	enum bench_mode mode;
} mode_names[] = {
	{ "blocking", BENCH_MODE_BLOCKING },
	{ "uring-rw", BENCH_MODE_URING_RW },
	{ "uring-fixed", BENCH_MODE_URING_FIXED },
	{ "uring-bufring", BENCH_MODE_URING_BUFRING },
	{ "uring-sqpoll", BENCH_MODE_URING_SQPOLL },
	{ "uring-sendzc", BENCH_MODE_URING_SENDZC },
};

static const struct {
	const char *name;
	enum bench_verify verify;
} verify_names[] = {
	{ "none", BENCH_VERIFY_NONE },
	{ "header", BENCH_VERIFY_HEADER },
	{ "full", BENCH_VERIFY_FULL },
};

int bench_mode_parse(const char *s, enum bench_mode *out)
{
	size_t i;

	for (i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
		if (strcmp(s, mode_names[i].name) == 0) {
			*out = mode_names[i].mode;
			return 0;
		}
	}
	return -BENCH_EINVAL;
}

const char *bench_mode_str(enum bench_mode m)
{
	size_t i;

	for (i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
		if (mode_names[i].mode == m)
			return mode_names[i].name;
	}
	return "?";
}

int bench_verify_parse(const char *s, enum bench_verify *out)
{
	size_t i;

	for (i = 0; i < sizeof(verify_names) / sizeof(verify_names[0]); i++) {
		if (strcmp(s, verify_names[i].name) == 0) {
			*out = verify_names[i].verify;
			return 0;
		}
	}
	return -BENCH_EINVAL;
}

static const struct {
	const char *name;
	enum bench_pattern pattern;
} pattern_names[] = {
	{ "echo", BENCH_PATTERN_ECHO },
	{ "stream", BENCH_PATTERN_STREAM },
};

int bench_pattern_parse(const char *s, enum bench_pattern *out)
{
	size_t i;

	for (i = 0; i < sizeof(pattern_names) / sizeof(pattern_names[0]); i++) {
		if (strcmp(s, pattern_names[i].name) == 0) {
			*out = pattern_names[i].pattern;
			return 0;
		}
	}
	return -BENCH_EINVAL;
}

const char *bench_pattern_str(enum bench_pattern p)
{
	size_t i;

	for (i = 0; i < sizeof(pattern_names) / sizeof(pattern_names[0]); i++) {
		if (pattern_names[i].pattern == p)
			return pattern_names[i].name;
	}
	return "?";
}

const char *bench_verify_str(enum bench_verify v)
{
	size_t i;

	for (i = 0; i < sizeof(verify_names) / sizeof(verify_names[0]); i++) {
		if (verify_names[i].verify == v)
			return verify_names[i].name;
	}
	return "?";
}

int bench_config_validate(const struct bench_config *c)
{
	if (c->role != BENCH_ROLE_LISTEN && c->role != BENCH_ROLE_CONNECT)
		return -BENCH_EINVAL;
	if (c->msg_size < BENCH_HDR_SIZE || c->msg_size > BENCH_MSG_MAX)
		return -BENCH_EINVAL;
	if (c->batch < 1 || c->batch > BENCH_BATCH_MAX)
		return -BENCH_EINVAL;
	if (c->count == 0 && c->duration_s == 0)
		return -BENCH_EINVAL;
	if (c->count != 0 && c->duration_s != 0)
		return -BENCH_EINVAL;
	return 0;
}

/* ---- result line ------------------------------------------------------- */

int bench_format_result(const struct bench_report *r, char *buf, size_t n)
{
	double secs = (double)r->elapsed_ns / 1e9;
	double mbps = 0.0, msgs_per_s = 0.0, syscalls_per_msg = 0.0;
	double cpu_us_per_msg = 0.0, reassembled_pct = 0.0;
	int len;

	if (secs > 0.0) {
		mbps = (double)r->bytes / 1e6 / secs;
		msgs_per_s = (double)r->msgs / secs;
	}
	if (r->msgs + r->msgs_rx_total > 0) {
		syscalls_per_msg = (double)r->syscalls /
				   (double)(r->msgs + r->msgs_rx_total);
		cpu_us_per_msg = (double)r->cpu_ns / 1e3 /
				 (double)(r->msgs + r->msgs_rx_total);
	}
	if (r->msgs_rx_total > 0)
		reassembled_pct = 100.0 * (double)r->reassembled /
				  (double)r->msgs_rx_total;

	len = snprintf(buf, n,
		       "BENCH_OK lang=%s mode=%s msg_size=%u batch=%u"
		       " msgs=%llu mbps=%.1f msgs_per_s=%.0f"
		       " p50_us=%.1f p99_us=%.1f min_us=%.1f max_us=%.1f"
		       " syscalls_per_msg=%.2f cpu_us_per_msg=%.2f"
		       " reassembled_pct=%.1f verify=%s",
		       r->lang, bench_mode_str(r->cfg->mode),
		       r->cfg->msg_size, r->cfg->batch,
		       (unsigned long long)r->msgs, mbps, msgs_per_s,
		       (double)r->rtt.p50_ns / 1e3,
		       (double)r->rtt.p99_ns / 1e3,
		       (double)r->rtt.min_ns / 1e3,
		       (double)r->rtt.max_ns / 1e3,
		       syscalls_per_msg, cpu_us_per_msg, reassembled_pct,
		       bench_verify_str(r->cfg->verify));
	if (len < 0 || (size_t)len >= n)
		return -BENCH_ENOSPC;
	return len;
}
