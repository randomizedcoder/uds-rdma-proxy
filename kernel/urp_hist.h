/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp_hist.h -- pure histogram bucketing for the RX inter-arrival distribution
 * (design 40 §40.1). Split out -- like urp_window.h / urp_reorder cases -- so
 * the bucket classifier compiles both in the kernel and standalone in userspace
 * (nix check urp-hist-units) and is table-tested in KUnit, with no atomics /
 * ktime / sockets. Keep the edge table in lock-step with the Rust `le` label
 * table in crates/urp-netlink/src/format.rs (asserted by a parity test there)
 * and with the KUnit cases in kernel/urp_test.c.
 *
 * Model: at RX we time-stamp each delivered DATA frame and bucket the delta
 * since the previous sample into a fixed, log-spaced classic histogram (the
 * Prometheus `le` contract: an observation v lands in the smallest bucket whose
 * upper bound edge >= v; the last bucket is +Inf). The kernel stores per-bucket
 * counts; the exporter renders the cumulative _bucket/_sum/_count triplet.
 *
 * Only counts cross the netlink wire -- the edges are compile-time constants
 * known to both the kernel encoder and the Rust decoder, so they are never
 * serialized.
 */
#ifndef _URP_HIST_H
#define _URP_HIST_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
#endif

/*
 * 15 buckets = 14 finite le upper-bound edges (nanoseconds) + a trailing +Inf.
 * Spans sub-us (line-rate back-to-back frames on a 25-800 GbE fabric) to the
 * tens-of-ms credit-stall tail. Do NOT reorder or renumber without updating the
 * Rust le table and the KUnit/units cases in lock-step.
 */
#define URP_HIST_NBUCKETS	15u
#define URP_HIST_NEDGES		(URP_HIST_NBUCKETS - 1u)

/*
 * The i-th le upper bound in ns. i == URP_HIST_NEDGES is the +Inf bucket, which
 * has no finite edge (returns U64_MAX). Kept behind an accessor so the array has
 * exactly one definition and no unused-variable warning under -Werror when a
 * translation unit includes the header but only calls urp_hist_bucket().
 */
static inline u64 urp_hist_edge_ns(u32 i)
{
	static const u64 edges[URP_HIST_NEDGES] = {
		250u,		500u,		1000u,		2000u,
		5000u,		10000u,		25000u,		50000u,
		100000u,	250000u,	500000u,	1000000u,
		5000000u,	25000000u,
	};

	return i < URP_HIST_NEDGES ? edges[i] : (u64)~0ULL;
}

/*
 * Classify a delta (ns) into a bucket index [0, URP_HIST_NBUCKETS). Prometheus
 * `le` semantics: the smallest bucket whose edge is >= @ns; the +Inf bucket
 * (URP_HIST_NBUCKETS - 1) catches everything larger than the last finite edge.
 * Pure: the sole thing KUnit + urp-hist-units pin.
 */
static inline u32 urp_hist_bucket(u64 ns)
{
	u32 i;

	for (i = 0; i < URP_HIST_NEDGES; i++)
		if (ns <= urp_hist_edge_ns(i))
			return i;
	return URP_HIST_NBUCKETS - 1u;
}

/*
 * Inter-arrival sampling strides (design 40 §40.1): stride 1 = every delivered
 * frame (instantaneous jitter), stride 10 / 100 = every 10th / 100th (short and
 * sustained rate, noise-smoothed). Counter-based (frames delivered, modulo the
 * stride) rather than seq-arithmetic, so out-of-order arrival never confuses the
 * sampling. Accessor for the same one-definition/no-warning reason as above.
 */
#define URP_IA_NSTRIDES		3u

static inline u32 urp_ia_stride(u32 i)
{
	static const u32 strides[URP_IA_NSTRIDES] = { 1u, 10u, 100u };

	return i < URP_IA_NSTRIDES ? strides[i] : 1u;
}

#endif /* _URP_HIST_H */
