/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure credit-routing decisions (design 32 hardware bring-up).
 *
 * Split out so the decision compiles both in the kernel and standalone in
 * userspace (nix/fuzz + the urp_credit_plan_test harness) -- a pure function of
 * the frame's stream_id with no sockets/kthreads/RDMA. Keep in lock-step with
 * the KUnit table in kernel/urp_test.c.
 *
 * Background: flow-control credit lives in two places. The legacy k0 path
 * (stream_id == 0) consumes and is granted per-QP credit (ep->qps[i].credit).
 * The multi-stream path (stream_id != 0) consumes per-stream credit
 * (stream->credit). The two pumps pick the right pool structurally, but the
 * *grant* side (URP_RX_CREDIT) must route an incoming CREDIT frame to the SAME
 * pool the sender drew from -- keyed on the frame's stream_id. Routing every
 * grant to the QP pool (the original bug) left per-stream credit seeded once and
 * never replenished: after the initial window it consumes to zero, every send
 * reports a credit stall, and the sender oversends into a drained SRQ -> RNR
 * retry storm -> throughput collapse. This predicate keeps consume and grant in
 * agreement.
 */
#ifndef _URP_CREDIT_PLAN_H
#define _URP_CREDIT_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
#endif

/*
 * Which credit pool does traffic on @stream_id draw from (and therefore which
 * pool an incoming CREDIT frame tagged with @stream_id must replenish)?
 *
 * stream_id == 0 -> per-QP pool (legacy k0 single connection).
 * stream_id != 0 -> per-stream pool (multi-stream data path).
 */
enum urp_credit_scope {
	URP_CREDIT_SCOPE_QP = 0,
	URP_CREDIT_SCOPE_STREAM = 1,
};

static inline enum urp_credit_scope
urp_credit_scope_for(u32 stream_id)
{
	return stream_id == 0 ? URP_CREDIT_SCOPE_QP : URP_CREDIT_SCOPE_STREAM;
}

#endif /* _URP_CREDIT_PLAN_H */
