// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace table-driven tests for the histogram bucketing in kernel/urp_hist.h
 * (design 40 §40.1). Drives the exact same pure classifier the RX inter-arrival
 * sampler (urp_interarrival_sample in kernel/urp_rdma.c) uses, and that the
 * in-kernel KUnit suite (kernel/urp_test.c) pins -- one set of decisions, two
 * drivers; this fast sandboxed gate (nix check urp-hist-units) and the
 * KUnit-in-VM pass must always agree.
 *
 * Coverage: le semantics (v lands in the smallest bucket whose edge >= v),
 * on-edge boundaries, the zero / sub-first-edge / above-last-edge corners, the
 * +Inf catch-all, edge-table monotonicity, and the sampling strides {1,10,100}.
 */

#include <stdio.h>
#include <stdbool.h>

#include "urp_hist.h"

static int failures;
static int checks;

#define CHECK_EQ(got, want, ...)                                          \
	do {                                                              \
		unsigned long long g_ = (unsigned long long)(got);        \
		unsigned long long w_ = (unsigned long long)(want);       \
		checks++;                                                 \
		if (g_ != w_) {                                           \
			failures++;                                       \
			printf("FAIL: ");                                 \
			printf(__VA_ARGS__);                              \
			printf(" (got %llu want %llu)\n", g_, w_);        \
		}                                                         \
	} while (0)

static void run_bucket(void)
{
	/* corner: zero and everything <= the first edge (250 ns) -> bucket 0. */
	CHECK_EQ(urp_hist_bucket(0), 0, "zero -> bucket 0");
	CHECK_EQ(urp_hist_bucket(1), 0, "1ns -> bucket 0");
	CHECK_EQ(urp_hist_bucket(250), 0, "on first edge 250 -> bucket 0");

	/* boundary: one past an edge crosses into the next bucket. */
	CHECK_EQ(urp_hist_bucket(251), 1, "251 -> bucket 1");
	CHECK_EQ(urp_hist_bucket(500), 1, "on edge 500 -> bucket 1");
	CHECK_EQ(urp_hist_bucket(501), 2, "501 -> bucket 2");

	/* positive: exact edges map to their own bucket (le semantics). */
	CHECK_EQ(urp_hist_bucket(1000), 2, "1us -> bucket 2");
	CHECK_EQ(urp_hist_bucket(10000), 5, "10us -> bucket 5");
	CHECK_EQ(urp_hist_bucket(100000), 8, "100us -> bucket 8");
	CHECK_EQ(urp_hist_bucket(1000000), 11, "1ms -> bucket 11");

	/* boundary: the last finite edge (25ms, index 13) and just past it. */
	CHECK_EQ(urp_hist_bucket(25000000), URP_HIST_NEDGES - 1,
		 "25ms -> last finite bucket");
	CHECK_EQ(urp_hist_bucket(25000001), URP_HIST_NBUCKETS - 1,
		 "25ms+1 -> +Inf bucket");

	/* corner: anything huge -> the +Inf catch-all. */
	CHECK_EQ(urp_hist_bucket(1000000000ULL), URP_HIST_NBUCKETS - 1,
		 "1s -> +Inf bucket");
	CHECK_EQ(urp_hist_bucket((u64)~0ULL), URP_HIST_NBUCKETS - 1,
		 "U64_MAX -> +Inf bucket");
}

static void run_edges_monotonic(void)
{
	u32 i;

	/* Every finite edge is strictly increasing; every value that lands in
	 * bucket i must be <= edge[i] and > edge[i-1] -- verify via the edges.
	 */
	for (i = 1; i < URP_HIST_NEDGES; i++)
		CHECK_EQ(urp_hist_edge_ns(i) > urp_hist_edge_ns(i - 1), true,
			 "edge[%u] > edge[%u]", i, i - 1);
	/* The +Inf sentinel is U64_MAX. */
	CHECK_EQ(urp_hist_edge_ns(URP_HIST_NEDGES), (u64)~0ULL,
		 "edge[+Inf] == U64_MAX");
}

static void run_strides(void)
{
	CHECK_EQ(urp_ia_stride(0), 1, "stride[0] == 1");
	CHECK_EQ(urp_ia_stride(1), 10, "stride[1] == 10");
	CHECK_EQ(urp_ia_stride(2), 100, "stride[2] == 100");
	CHECK_EQ(URP_IA_NSTRIDES, 3, "three strides");
}

/*
 * design 40 §40.2: OWD latency classifier -- same `le` semantics as the
 * inter-arrival one, latency-tuned edges (1 us .. 10 ms, 14 buckets).
 */
static void run_owd_bucket(void)
{
	u32 i;

	/* corner: zero / <= first edge (1 us) -> bucket 0. */
	CHECK_EQ(urp_owd_bucket(0), 0, "owd zero -> bucket 0");
	CHECK_EQ(urp_owd_bucket(1000), 0, "owd on first edge 1us -> bucket 0");
	/* boundary: one past an edge crosses into the next bucket. */
	CHECK_EQ(urp_owd_bucket(1001), 1, "owd 1us+1 -> bucket 1");
	CHECK_EQ(urp_owd_bucket(2000), 1, "owd on edge 2us -> bucket 1");
	CHECK_EQ(urp_owd_bucket(2001), 2, "owd 2us+1 -> bucket 2");
	/* positive: exact edges map to their own bucket. */
	CHECK_EQ(urp_owd_bucket(10000), 3, "owd 10us -> bucket 3");
	CHECK_EQ(urp_owd_bucket(100000), 6, "owd 100us -> bucket 6");
	CHECK_EQ(urp_owd_bucket(1000000), 9, "owd 1ms -> bucket 9");
	/* boundary: last finite edge (10 ms) and just past it -> +Inf. */
	CHECK_EQ(urp_owd_bucket(10000000), URP_OWD_NEDGES - 1,
		 "owd 10ms -> last finite bucket");
	CHECK_EQ(urp_owd_bucket(10000001), URP_OWD_NBUCKETS - 1,
		 "owd 10ms+1 -> +Inf bucket");
	/* corner: anything huge -> the +Inf catch-all. */
	CHECK_EQ(urp_owd_bucket((u64)~0ULL), URP_OWD_NBUCKETS - 1,
		 "owd U64_MAX -> +Inf bucket");

	/* edges strictly increasing; +Inf sentinel is U64_MAX; OWD buckets fit
	 * the shared urp_hist15 storage (<= the interarrival bucket count).
	 */
	for (i = 1; i < URP_OWD_NEDGES; i++)
		CHECK_EQ(urp_owd_edge_ns(i) > urp_owd_edge_ns(i - 1), true,
			 "owd edge[%u] > edge[%u]", i, i - 1);
	CHECK_EQ(urp_owd_edge_ns(URP_OWD_NEDGES), (u64)~0ULL,
		 "owd edge[+Inf] == U64_MAX");
	CHECK_EQ(URP_OWD_NBUCKETS <= URP_HIST_NBUCKETS, true,
		 "owd buckets fit urp_hist15");
}

int main(void)
{
	run_bucket();
	run_edges_monotonic();
	run_strides();
	run_owd_bucket();

	printf("urp-hist-units: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
