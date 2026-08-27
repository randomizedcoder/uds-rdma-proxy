// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace table-driven tests for the default C reorder backend
 * (kernel/urp_reorder.c). Compiles the REAL backend against the kernel's own
 * userspace rbtree (tools/lib/rbtree.c, from the pinned kernel source) and
 * drives the SHARED op-script table in kernel/urp_reorder_cases.h -- the very
 * table the in-kernel KUnit suite (kernel/urp_test.c) runs. One data set, two
 * drivers; the fast sandbox gate here (nix check urp-reorder-units) and the
 * slow KUnit-in-VM pass must always agree.
 *
 * Coverage: positive / negative / boundary / corner scenarios from the shared
 * table, plus argument guards and the two -ENOMEM paths (insert node alloc,
 * drain-prefix alloc) that only a userspace allocator shim can reach.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/types.h>	/* tools: gfp_t, u8..u64 */
#include <linux/slab.h>		/* tools: declares kmalloc/kfree/kmalloc_array */

/*
 * Allocator shim with fault injection. kmalloc_fail_in counts successful
 * allocations before the one that fails: 0 => the next call fails, N => the
 * (N+1)-th call fails; -1 (default) => never fail. Self-disarms on the fail.
 * kmalloc MUST honour __GFP_ZERO -- kzalloc() is an inline over kmalloc().
 */
static int kmalloc_fail_in = -1;

void *kmalloc(size_t n, gfp_t f)
{
	void *p;

	if (kmalloc_fail_in == 0) {
		kmalloc_fail_in = -1;
		return NULL;
	}
	if (kmalloc_fail_in > 0)
		kmalloc_fail_in--;

	p = malloc(n);
	if (p && (f & __GFP_ZERO))
		memset(p, 0, n);
	return p;
}
void kfree(void *p) { free(p); }
void *kmalloc_array(size_t n, size_t s, gfp_t f)
{
	(void)f;
	return calloc(n, s);
}

#include "urp_reorder.h"
#include "urp_reorder_cases.h"

static int failures;
static int checks;

#define CHECK_EQ(got, want, ...)                                          \
	do {                                                              \
		long g_ = (long)(got), w_ = (long)(want);                 \
		checks++;                                                 \
		if (g_ != w_) {                                           \
			failures++;                                       \
			printf("FAIL: ");                                 \
			printf(__VA_ARGS__);                              \
			printf(" (got %ld want %ld)\n", g_, w_);          \
		}                                                         \
	} while (0)

static void run_scenario(const struct reorder_scenario *sc)
{
	struct urp_reorder *rb = urp_reorder_alloc(sc->initial, sc->max_buffered);
	unsigned char buf[64], pat[64];
	unsigned int j, i;

	if (!rb) {
		failures++;
		printf("FAIL: %s: alloc returned NULL\n", sc->name);
		return;
	}

	for (j = 0; sc->ops[j].kind != ROP_END; j++) {
		const struct rop *op = &sc->ops[j];
		u64 seq = 0;
		size_t len;
		int ret;

		switch (op->kind) {
		case ROP_INSERT:
			for (i = 0; i < op->len; i++)
				pat[i] = (unsigned char)(op->seq + i);
			ret = urp_reorder_insert(rb, op->seq,
						 op->len ? pat : NULL, op->len);
			CHECK_EQ(ret, op->want_ret, "%s op %u: insert seq=%llu",
				 sc->name, j, (unsigned long long)op->seq);
			break;
		case ROP_DRAIN:
			len = op->bufsz ? op->bufsz : sizeof(buf);
			ret = urp_reorder_drain_next(rb, &seq, buf, &len);
			CHECK_EQ(ret, op->want_ret, "%s op %u: drain", sc->name, j);
			if (op->want_ret == 0) {
				CHECK_EQ(seq, op->seq, "%s op %u: drained seq",
					 sc->name, j);
				CHECK_EQ(len, op->len, "%s op %u: drained len",
					 sc->name, j);
				for (i = 0; i < op->len; i++)
					CHECK_EQ(buf[i],
						 (unsigned char)(op->seq + i),
						 "%s op %u: byte %u",
						 sc->name, j, i);
			} else if (op->want_ret == -ENOBUFS) {
				CHECK_EQ(len, op->len, "%s op %u: required size",
					 sc->name, j);
			}
			break;
		case ROP_EXP_NEXT:
			CHECK_EQ(urp_reorder_next_expected(rb), op->seq,
				 "%s op %u: next_expected", sc->name, j);
			break;
		case ROP_EXP_GAP:
			CHECK_EQ(urp_reorder_gap_count(rb), op->seq,
				 "%s op %u: gap_count", sc->name, j);
			break;
		case ROP_EXP_DRAINPEND:
			CHECK_EQ(urp_reorder_drain_pending(rb), op->seq,
				 "%s op %u: drain_pending", sc->name, j);
			break;
		case ROP_ADVANCE:
			urp_reorder_advance(rb);
			break;
		default:
			break;
		}
	}

	urp_reorder_free(rb);
}

/* Argument / boundary guards (single calls, not op-scripts). */
static void test_guards(void)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 64);
	unsigned char b = 0xAB, buf[8];
	u64 seq;
	size_t len;

	if (!rb) {
		failures++;
		printf("FAIL: guards: alloc returned NULL\n");
		return;
	}

	CHECK_EQ(urp_reorder_insert(NULL, 0, &b, 1), -EINVAL, "insert null rb");
	CHECK_EQ(urp_reorder_insert(rb, 0, NULL, 1), -EINVAL, "insert null data");

	len = sizeof(buf);
	CHECK_EQ(urp_reorder_drain_next(NULL, &seq, buf, &len), -EINVAL,
		 "drain null rb");
	CHECK_EQ(urp_reorder_drain_next(rb, NULL, buf, &len), -EINVAL,
		 "drain null seq");
	CHECK_EQ(urp_reorder_drain_next(rb, &seq, buf, NULL), -EINVAL,
		 "drain null len");
	len = sizeof(buf);
	CHECK_EQ(urp_reorder_drain_next(rb, &seq, NULL, &len), -EINVAL,
		 "drain null data with len>0");
	len = sizeof(buf);
	CHECK_EQ(urp_reorder_drain_next(rb, &seq, buf, &len), -ENOENT,
		 "drain empty queue");

	urp_reorder_free(rb);

	CHECK_EQ(urp_reorder_alloc(0, 0) == NULL, 1, "alloc max_buffered=0 -> NULL");
	CHECK_EQ(urp_reorder_next_expected(NULL), 0, "next_expected(NULL)");
	CHECK_EQ(urp_reorder_gap_count(NULL), 0, "gap_count(NULL)");
	CHECK_EQ(urp_reorder_drain_pending(NULL), 0, "drain_pending(NULL)");
}

/* The two -ENOMEM paths, reachable only with the allocator shim. */
static void test_enomem(void)
{
	struct urp_reorder *rb = urp_reorder_alloc(0, 64);
	unsigned char b = 7, buf[8];
	u64 seq;
	size_t len;

	if (!rb) {
		failures++;
		printf("FAIL: enomem: alloc returned NULL\n");
		return;
	}

	/* insert's node allocation fails -> -ENOMEM. */
	kmalloc_fail_in = 0;
	CHECK_EQ(urp_reorder_insert(rb, 5, &b, 1), -ENOMEM, "insert node ENOMEM");
	kmalloc_fail_in = -1;

	/*
	 * drain-prefix allocation fails: the in-order frame is inserted (insert
	 * returns 0, best-effort drain) but left in pending, nothing drainable,
	 * next_expected unchanged. node kmalloc succeeds (count 1->0), the
	 * drained-node kmalloc is the one that fails.
	 */
	kmalloc_fail_in = 1;
	CHECK_EQ(urp_reorder_insert(rb, 0, &b, 1), 0, "insert ok, drain deferred");
	kmalloc_fail_in = -1;
	CHECK_EQ(urp_reorder_gap_count(rb), 1, "frame left pending on drain OOM");
	len = sizeof(buf);
	CHECK_EQ(urp_reorder_drain_next(rb, &seq, buf, &len), -ENOENT,
		 "nothing drained after drain OOM");
	CHECK_EQ(urp_reorder_next_expected(rb), 0, "next_expected unchanged");

	/* Inserting the next seq re-runs the prefix drain (alloc now works). */
	CHECK_EQ(urp_reorder_insert(rb, 1, &b, 1), 0, "insert seq 1 re-drains");
	CHECK_EQ(urp_reorder_next_expected(rb), 2, "0 and 1 now drained");

	urp_reorder_free(rb);
}

int main(void)
{
	unsigned int n = sizeof(urp_reorder_scenarios) /
			 sizeof(urp_reorder_scenarios[0]);
	unsigned int i;

	for (i = 0; i < n; i++)
		run_scenario(&urp_reorder_scenarios[i]);
	test_guards();
	test_enomem();

	printf("urp-reorder-units: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
