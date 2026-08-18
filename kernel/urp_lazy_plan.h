/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure lazy-connect decision (design 33 Phase 2).
 *
 * Split out so the decision compiles both in the kernel and standalone in
 * userspace (nix/fuzz + the lazy_plan harness) -- a pure function of
 * (role, latch state) with no sockets/kthreads/RDMA. Keep in lock-step with
 * the KUnit table in kernel/urp_test.c.
 *
 * Background: the initiator used to dial RDMA-CM eagerly at `urp add`, so an
 * idle endpoint held a live RC QP + PD/CQ with no client (violating goal R4,
 * "no RDMA resources while idle") and the dial raced the acceptor's
 * rdma_listen at boot. Phase 2 restores TCP's "connect on first use": defer
 * the initiator's dial until the first local UDS client actually connects.
 * This predicate isolates the "should this accept fire the one-shot dial?"
 * decision from the RDMA plumbing that acts on it.
 */
#ifndef _URP_LAZY_PLAN_H
#define _URP_LAZY_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

/*
 * Should this accept fire the one-shot lazy RDMA dial?
 *
 * Only the initiator dials (the acceptor waits for CONNECT_REQUEST), so the
 * acceptor never lazy-connects -- @is_initiator gates that. The dial is a
 * lifetime one-shot: @already_started is the prior value of the endpoint's
 * connect_started latch (set via atomic_cmpxchg by the caller), so only the
 * accept that flips 0->1 gets a true here; every later client sees the latch
 * already set and returns false (the connection is being brought up, or is up,
 * or has terminally failed -- none of which want a second dial).
 */
static inline bool
urp_should_start_lazy_connect(bool is_initiator, bool already_started)
{
	return is_initiator && !already_started;
}

#endif /* _URP_LAZY_PLAN_H */
