// SPDX-License-Identifier: GPL-2.0
/*
 * reorder_fuzz -- coverage-guided harness for the DEFAULT C reorder backend
 * (kernel/urp_reorder.c) with a spec-model differential (design 27 F1.2 /
 * F0.3). Until now only the *optional* Rust reorder backend was fuzzed
 * (cargo-fuzz reorder_ops); the default C rbtree backend -- the one that
 * actually runs, fed by the remote S1 wire path -- was not.
 *
 * It compiles the real kernel C: urp_reorder.c plus the kernel's own
 * userspace rbtree (tools/lib/rbtree.c, pulled from the nixpkgs-pinned kernel
 * source -- not vendored), under -fsanitize=fuzzer,address,undefined. The
 * kernel slab is satisfied by the libc allocators below.
 *
 * Oracle -- a reference MODEL of the documented contract (urp_reorder.h),
 * which is exactly the contract the Rust ReorderBuffer mirrors, so this is a
 * differential against that shared spec:
 *   - delivery is in-order and contiguous from initial_expected (each drained
 *     seq == the running expected counter; the counter SATURATES at U64_MAX
 *     exactly as the backend does -- this is the overflow boundary the Rust
 *     fuzzer flagged in PR #11);
 *   - drained payload byte-matches the accepted insert for that seq;
 *   - gap_count() never exceeds max_buffered;
 *   - + ASAN/UBSan on the real rbtree usage and the kmalloc(sizeof+len) add.
 *
 * Input: [8B initial_expected][1B max_buffered] then a stream of ops:
 *   op byte & 1 == 0 -> INSERT (2B seq-offset, 1B len, len payload bytes)
 *   op byte & 1 == 1 -> DRAIN
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>	/* the harness's own -ENOENT/-ENOBUFS checks */
#include <linux/types.h>	/* tools: gfp_t, u8..u64 */
#include <linux/slab.h>		/* tools: declares kmalloc/kfree/kmalloc_array */

/* Satisfy the kernel slab decls with libc. kzalloc() in tools/slab.h is an
 * inline that calls kmalloc(size, gfp | __GFP_ZERO), so kmalloc MUST honour
 * __GFP_ZERO or urp_reorder's kzalloc'd handle comes back uninitialised. The
 * backend's kmalloc(sizeof(*node) + len) add runs under ASAN this way.
 */
void *kmalloc(size_t n, gfp_t f)
{
	void *p = malloc(n);

	if (p && (f & __GFP_ZERO))
		memset(p, 0, n);
	return p;
}
void kfree(void *p) { free(p); }
void *kmalloc_array(size_t n, size_t s, gfp_t f)
{
	void *p = calloc(n, s);

	(void)f;
	return p;
}

#include "urp_reorder.h"

#define WINDOW		512		/* seq offsets 0..WINDOW-1 */
#define MAXLEN		32
#define MAX_OPS		4096

struct slot {
	uint8_t	data[MAXLEN];
	uint32_t len;
	int	present;		/* an accepted insert recorded here */
};

int LLVMFuzzerTestOneInput(const uint8_t *in, size_t size)
{
	const uint8_t *p = in, *end = in + size;
	struct urp_reorder *rb;
	struct slot *win;
	uint64_t initial, expected;
	uint32_t max_buffered;
	int ops = 0;

	if (size < 9)
		return 0;

	memcpy(&initial, p, 8);
	p += 8;
	max_buffered = *p++;			/* 0..255 */

	rb = urp_reorder_alloc(initial, max_buffered);
	if (!rb)
		return 0;			/* invalid args -> nothing to test */

	win = calloc(WINDOW, sizeof(*win));
	if (!win) {
		urp_reorder_free(rb);
		return 0;
	}
	expected = initial;

	/* Drain everything currently ready, checking order + payload + counter. */
#define DRAIN_ALL()							\
	do {								\
		for (;;) {						\
			uint8_t obuf[MAXLEN];				\
			size_t olen = sizeof(obuf);			\
			uint64_t oseq = 0;				\
			int r = urp_reorder_drain_next(rb, &oseq,	\
						       obuf, &olen);	\
			uint64_t off;					\
			if (r == -ENOENT)				\
				break;					\
			if (r != 0)					\
				break;	/* -ENOBUFS/-EINVAL: not a delivery */ \
			/*						\
			 * Once next_expected saturates at U64_MAX the		\
			 * backend can no longer advance, so it may re-deliver	\
			 * seq==U64_MAX on each fresh insert -- a defined,	\
			 * unreachable-in-practice terminal corner (a stream	\
			 * would need 2^64 frames). Stop asserting there, as	\
			 * the Rust reorder harness does (PR #11); keep draining	\
			 * so we don't spin.				\
			 */						\
			if (expected == (uint64_t)-1)			\
				continue;				\
			/* In-order contiguous from initial. */		\
			if (oseq != expected)				\
				abort();				\
			off = oseq - initial;				\
			if (off < WINDOW) {				\
				if (!win[off].present)			\
					abort(); /* delivered a seq never accepted */ \
				if (olen != win[off].len ||		\
				    memcmp(obuf, win[off].data, olen))	\
					abort(); /* payload corrupted */ \
			}						\
			expected++;	/* not yet saturated */		\
		}							\
	} while (0)

	while (p < end && ops++ < MAX_OPS) {
		uint8_t op = *p++;

		if (op & 1) {
			DRAIN_ALL();
			continue;
		}
		/* INSERT: need 2B offset + 1B len (+ payload). */
		if (end - p < 3)
			break;
		{
			uint16_t rawoff = (uint16_t)(p[0] | (p[1] << 8));
			uint32_t off = rawoff % WINDOW;
			uint32_t len = p[2] % (MAXLEN + 1);
			uint64_t seq = initial + off;
			const uint8_t *payload;
			int ret;

			p += 3;
			if ((uint32_t)(end - p) < len)
				len = (uint32_t)(end - p);
			payload = p;
			p += len;

			ret = urp_reorder_insert(rb, seq, payload, len);

			/* Record the FIRST accepted insert for this seq: that is
			 * the payload the backend will deliver. Dups (-EEXIST)
			 * must not overwrite it.
			 */
			if (ret == 0 && !win[off].present) {
				win[off].present = 1;
				win[off].len = len;
				memcpy(win[off].data, payload, len);
			}

			/* Inserting the in-order seq makes frames drainable. */
			DRAIN_ALL();
		}

		/* Structural invariant: never buffer more than the cap. */
		if (urp_reorder_gap_count(rb) > max_buffered)
			abort();
	}

	free(win);
	urp_reorder_free(rb);
	return 0;
}
