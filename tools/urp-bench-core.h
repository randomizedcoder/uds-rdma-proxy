/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp-bench pure core — framing, deframing, echo tracking, batch/window
 * accounting, stats, config validation, result formatting.
 *
 * Design: docs/design/30-urp-bench-io-uring.md §30.5/§30.9 (work item B1).
 *
 * This header and urp-bench-core.c compile WITHOUT liburing (or any other
 * dependency beyond libc), so the unit-test binary (tools/urp-bench-test.c)
 * and the fuzz harness stay hermetic and nix-sandbox-safe. The io_uring
 * shell (tools/urp-bench.c) is the only liburing consumer.
 *
 * The Rust twin (crates/urp-bench) mirrors this API 1:1; the shared hex
 * test vectors and the differential fuzzer keep the two honest.
 */
#ifndef URP_BENCH_CORE_H
#define URP_BENCH_CORE_H

#include <stddef.h>
#include <stdint.h>

/* ---- wire format (§30.5) ---------------------------------------------- */

#define BENCH_HDR_SIZE		24
#define BENCH_MAGIC		0x42505255u	/* "URPB" when LE-encoded */
#define BENCH_VERSION		1
#define BENCH_PAYLOAD_MAX	(1u << 20)	/* absolute payload cap, 1 MiB */
#define BENCH_MSG_MAX		(BENCH_HDR_SIZE + BENCH_PAYLOAD_MAX)

#define BENCH_FLAG_ECHO		(1u << 0)
#define BENCH_FLAG_FIN		(1u << 1)
#define BENCH_FLAG_MASK		(BENCH_FLAG_ECHO | BENCH_FLAG_FIN)

/* Error codes: returned negated (-BENCH_ESHORT etc.), 0 = success. */
#define BENCH_ESHORT		1	/* buffer shorter than a header */
#define BENCH_EMAGIC		2	/* bad magic */
#define BENCH_EVERSION		3	/* unknown version */
#define BENCH_EFLAGS		4	/* reserved flag bit set */
#define BENCH_ECAP		5	/* payload_len above cap */
#define BENCH_ECORRUPT		6	/* payload byte mismatch */
#define BENCH_EUNKNOWN		7	/* echo for a seq never sent */
#define BENCH_EDUP		8	/* duplicate echo / double recycle */
#define BENCH_EFULL		9	/* tracker window / ring full */
#define BENCH_EEMPTY		10	/* ring empty */
#define BENCH_ERANGE		11	/* index out of range */
#define BENCH_EINVAL		12	/* invalid configuration */
#define BENCH_ENOSPC		13	/* output buffer too small */

struct bench_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t flags;
	uint16_t origin_id;
	uint32_t payload_len;
	uint32_t seq;
	uint64_t t_send_ns;
};

void bench_hdr_encode(const struct bench_hdr *h, uint8_t out[BENCH_HDR_SIZE]);

/*
 * Decode and validate a header from buf. max_payload caps payload_len on
 * top of BENCH_PAYLOAD_MAX; pass 0 for the absolute cap only.
 */
int bench_hdr_decode(const uint8_t *buf, size_t len, uint32_t max_payload,
		     struct bench_hdr *out);

/* ---- deterministic payload (§30.5) ------------------------------------ */

void bench_fill_payload(uint8_t *dst, size_t len, uint16_t origin_id,
			uint32_t seq);
int bench_verify_payload(const uint8_t *p, size_t len, uint16_t origin_id,
			 uint32_t seq);

/* ---- incremental deframer (§30.5) ------------------------------------- */

/*
 * Callback per complete message. payload points either into the fed chunk
 * (fast path — valid only for the duration of the callback) or into the
 * deframer's assembly buffer (reassembly path). Return 0 to continue,
 * negative to abort the feed (propagated).
 */
typedef int (*bench_msg_cb)(void *ctx, const struct bench_hdr *hdr,
			    const uint8_t *payload);

enum bench_deframe_state {
	BENCH_DF_WANT_HDR,
	BENCH_DF_WANT_PAYLOAD,
};

struct bench_deframer {
	enum bench_deframe_state state;
	uint8_t hdr_buf[BENCH_HDR_SIZE];
	uint32_t hdr_have;
	struct bench_hdr hdr;	/* valid in WANT_PAYLOAD */
	uint8_t *asm_buf;	/* caller-owned assembly buffer */
	size_t asm_cap;
	uint32_t asm_have;
	uint32_t max_payload;	/* 0 = absolute cap only */
	/* counters */
	uint64_t msgs_total;
	uint64_t msgs_reassembled;	/* delivered via asm_buf */
};

/* asm_cap must be >= the largest payload the peer may send. */
void bench_deframer_init(struct bench_deframer *d, uint8_t *asm_buf,
			 size_t asm_cap, uint32_t max_payload);

/*
 * Feed one received chunk. Invokes cb once per completed message, in
 * order. Returns 0, a negative decode error (stream is then poisoned —
 * hard error, no resync), -BENCH_ECAP if a payload exceeds asm_cap on the
 * reassembly path, or the callback's negative return.
 */
int bench_deframe_feed(struct bench_deframer *d, const uint8_t *chunk,
		       size_t len, bench_msg_cb cb, void *ctx);

/* ---- echo RTT tracker (§30.5) ----------------------------------------- */

struct bench_tracker {
	uint64_t *sent_ns;	/* [window] send timestamps */
	uint32_t *seqs;		/* [window] seq occupying the slot */
	uint8_t *in_flight;	/* [window] slot occupied? */
	uint32_t window;
	uint32_t inflight_count;
	uint64_t dups;
	uint64_t unknowns;
};

/* All three arrays are caller-owned, length = window (>= 1). */
void bench_tracker_init(struct bench_tracker *t, uint64_t *sent_ns,
			uint32_t *seqs, uint8_t *in_flight, uint32_t window);

/* Record an original as sent. -BENCH_EFULL if the window slot is busy. */
int bench_track_sent(struct bench_tracker *t, uint32_t seq,
		     uint64_t t_send_ns);

/*
 * Record an echo. Returns RTT in ns (>= 0), or -BENCH_EUNKNOWN /
 * -BENCH_EDUP. u32 seq wraparound is handled (slot = seq % window).
 */
int64_t bench_track_echo(struct bench_tracker *t, uint32_t seq,
			 uint64_t now_ns);

/* ---- batch / window accounting (§30.6) -------------------------------- */

struct bench_batch {
	uint32_t window;	/* max outstanding originals (= batch) */
};

/*
 * How many new originals to queue this iteration: never exceeds the free
 * window, never exceeds what remains to be sent.
 */
uint32_t bench_batch_plan(const struct bench_batch *b, uint32_t inflight,
			  uint64_t remaining);

/* ---- provided-buffer-ring bookkeeping (§30.9) ------------------------- */

struct bench_bufring {
	uint16_t *free_idx;	/* [cap] stack of free buffer indices */
	uint8_t *in_use;	/* [cap] occupancy, double-recycle guard */
	uint32_t cap;
	uint32_t free_count;
};

/* Arrays caller-owned, length = cap; starts with all buffers free. */
void bench_bufring_init(struct bench_bufring *r, uint16_t *free_idx,
			uint8_t *in_use, uint32_t cap);

/* Take a free buffer index, or -BENCH_EEMPTY. */
int bench_bufring_take(struct bench_bufring *r);

/* Return a buffer. -BENCH_ERANGE / -BENCH_EDUP on misuse. */
int bench_bufring_recycle(struct bench_bufring *r, uint32_t idx);

/* ---- RTT statistics (§30.8) ------------------------------------------- */

struct bench_stats {
	uint64_t *samples;	/* caller-owned, [cap] */
	size_t cap;
	size_t count;
	uint64_t dropped;	/* samples beyond cap (saturating) */
};

struct bench_stats_result {
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t p50_ns;
	uint64_t p99_ns;
	size_t count;
};

void bench_stats_init(struct bench_stats *s, uint64_t *samples, size_t cap);
void bench_stats_add(struct bench_stats *s, uint64_t rtt_ns);
/* Sorts in place. -BENCH_EEMPTY if no samples. */
int bench_stats_finalize(struct bench_stats *s,
			 struct bench_stats_result *out);

/* ---- configuration (§30.6) -------------------------------------------- */

enum bench_mode {
	BENCH_MODE_BLOCKING,
	BENCH_MODE_URING_RW,
	BENCH_MODE_URING_FIXED,
	BENCH_MODE_URING_BUFRING,
	BENCH_MODE_URING_SQPOLL,
	BENCH_MODE_URING_SENDZC,
	/*
	 * Zero-copy fast path (design 31): io_uring_cmd on /dev/urp against a
	 * `urp add --kind fast` endpoint. Not a socket transport — the shell
	 * (urp-bench.c run_fast) drives REGISTER/SEND/RECV and nests the bench
	 * frame inside the urp payload; the pure core here is untouched.
	 */
	BENCH_MODE_URING_CMD,
};

enum bench_verify {
	BENCH_VERIFY_NONE,
	BENCH_VERIFY_HEADER,
	BENCH_VERIFY_FULL,
};

/*
 * App protocol, orthogonal to the io_uring mode (§34.4). ECHO is the
 * symmetric RTT reflector (default, unchanged). STREAM is one-way bulk:
 * the --connect side is the source (blast, never echo, no RTT), the
 * --listen side is the sink (drain + count, never echo); goodput is
 * measured at the sink.
 */
enum bench_pattern {
	BENCH_PATTERN_ECHO,
	BENCH_PATTERN_STREAM,
};

#define BENCH_BATCH_MAX		1024
#define BENCH_ROLE_NONE		0
#define BENCH_ROLE_LISTEN	1
#define BENCH_ROLE_CONNECT	2

struct bench_config {
	int role;		/* BENCH_ROLE_* */
	uint16_t id;
	enum bench_mode mode;
	enum bench_verify verify;
	enum bench_pattern pattern;	/* ECHO (default) or STREAM (§34.4) */
	uint32_t msg_size;	/* total wire bytes incl. header */
	uint32_t batch;
	uint64_t count;		/* 0 = use duration */
	uint32_t duration_s;	/* 0 = use count */
	int defer_taskrun;
};

/* -BENCH_EINVAL on unknown string. */
int bench_mode_parse(const char *s, enum bench_mode *out);
int bench_verify_parse(const char *s, enum bench_verify *out);
int bench_pattern_parse(const char *s, enum bench_pattern *out);
const char *bench_mode_str(enum bench_mode m);
const char *bench_verify_str(enum bench_verify v);
const char *bench_pattern_str(enum bench_pattern p);

int bench_config_validate(const struct bench_config *c);

/* ---- result line (§30.8) ---------------------------------------------- */

struct bench_report {
	const char *lang;	/* "c" / "rust" */
	const struct bench_config *cfg;
	struct bench_stats_result rtt;
	uint64_t msgs;		/* own originals completed */
	uint64_t bytes;		/* own original wire bytes echoed back */
	uint64_t elapsed_ns;
	uint64_t syscalls;
	uint64_t cpu_ns;
	uint64_t reassembled;	/* messages delivered via assembly buffer */
	uint64_t msgs_rx_total;	/* all messages received (originals + echoes) */
};

/*
 * Writes the single-line "BENCH_OK lang=… mode=…" result (§30.8 grammar),
 * NUL-terminated, no trailing newline. Returns the length, or
 * -BENCH_ENOSPC if buf is too small.
 */
int bench_format_result(const struct bench_report *r, char *buf, size_t n);

#endif /* URP_BENCH_CORE_H */
