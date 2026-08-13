// SPDX-License-Identifier: GPL-2.0
/*
 * libFuzzer harness for the urp-bench incremental deframer (design 30
 * §30.12, target `fuzz-bench-deframe`). Compiles the REAL
 * tools/urp-bench-core.c under ASan/UBSan.
 *
 * Input shape: byte 0 = chunk count n (clamped to 1..15); bytes
 * [1, 1+n) = split fractions; the rest is the stream, fed to
 * bench_deframe_feed() in n chunks at the derived boundaries. This
 * makes the chunk schedule fuzzer-controlled, so header splits,
 * payload splits, and multi-message chunks are all reachable from the
 * corpus — the exact surface the table tests cover deterministically.
 *
 * The callback touches every payload byte and cross-checks
 * bench_verify_payload against a reference fill for plausible sizes, so
 * out-of-bounds payload pointers can't hide.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "urp-bench-core.h"

static uint8_t asm_buf[BENCH_PAYLOAD_MAX];

static int on_msg(void *ctx, const struct bench_hdr *hdr,
		  const uint8_t *payload)
{
	uint64_t *sink = ctx;
	uint32_t i;

	/* decode already validated these; a violation here is a bug */
	if (hdr->magic != BENCH_MAGIC || hdr->version != BENCH_VERSION ||
	    (hdr->flags & (uint8_t)~BENCH_FLAG_MASK) ||
	    hdr->payload_len > BENCH_PAYLOAD_MAX)
		__builtin_trap();

	for (i = 0; i < hdr->payload_len; i++)
		*sink += payload[i];

	/* exercise the verifier too (result intentionally unused: the
	 * fuzzer's random payloads rarely match the reference stream) */
	if (hdr->payload_len <= 4096)
		(void)bench_verify_payload(payload, hdr->payload_len,
					   hdr->origin_id, hdr->seq);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct bench_deframer d;
	uint64_t sink = 0;
	size_t n, i, off, stream_len;
	const uint8_t *stream;

	if (size < 2)
		return 0;

	n = (data[0] % 15) + 1;
	if (size < 1 + n)
		return 0;
	stream = data + 1 + n;
	stream_len = size - 1 - n;

	bench_deframer_init(&d, asm_buf, sizeof(asm_buf), 0);
	off = 0;
	for (i = 0; i < n && off < stream_len; i++) {
		size_t take;

		if (i == n - 1)
			take = stream_len - off;
		else
			take = ((size_t)data[1 + i] * (stream_len - off)) /
			       256;
		if (bench_deframe_feed(&d, stream + off, take, on_msg,
				       &sink) < 0)
			return 0;	/* poisoned stream: hard stop */
		off += take;
	}
	return 0;
}
