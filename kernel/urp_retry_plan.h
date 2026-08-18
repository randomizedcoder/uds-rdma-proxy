/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure initiator connect-retry decisions (design 33 Phase 1).
 *
 * Split out so the decisions compile both in the kernel and standalone in
 * userspace (nix/fuzz + the retry_plan harness) -- pure functions of
 * (role, attempt count, tunables) with no sockets/kthreads/RDMA. Keep in
 * lock-step with the KUnit table in kernel/urp_test.c.
 *
 * Background: the initiator dials RDMA-CM at bring-up and, before Phase 1,
 * gave up on the first failure -- a sub-second boot skew past the acceptor's
 * rdma_listen, or a mid-session peer reboot, left the session permanently
 * down. Phase 1 re-arms the connect with capped exponential backoff over a
 * bounded window (the TCP-RTO idiom), tunable at runtime via /proc/sys/urp/.
 * These predicates isolate the "should we retry, and how long do we wait?"
 * decisions from the RDMA plumbing that acts on them.
 */
#ifndef _URP_RETRY_PLAN_H
#define _URP_RETRY_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

/*
 * Should the initiator schedule another connect attempt?
 *
 * Only the initiator dials (the acceptor waits for CONNECT_REQUEST), so the
 * acceptor never retries -- @is_initiator gates that. Retry while we are still
 * under the attempt budget. @max_attempts == 0 disables retry entirely
 * (0 < 0 is false), which is the runtime "off switch" exposed via the
 * connect_max_attempts sysctl. The same predicate covers both first bring-up
 * (never established) and reconnect-on-drop (an established session that
 * disconnected): the attempt counter is reset to 0 on each ESTABLISHED, so a
 * post-establish drop starts a fresh budget.
 */
static inline bool
urp_should_retry_connect(bool is_initiator, unsigned int attempts,
			 unsigned int max_attempts)
{
	return is_initiator && attempts < max_attempts;
}

/*
 * Backoff (milliseconds) before the @attempt-th retry: capped exponential,
 * base_ms << attempt, clamped to ceil_ms -- i.e. base, 2*base, 4*base, ...
 * until it saturates at the ceiling, the standard network backoff shape
 * (à la TCP RTO doubling to a maximum). base_ms/ceil_ms are runtime tunables;
 * the caller passes the live sysctl values so an operator can pick a
 * more/less aggressive stance without a reload.
 *
 * The shift is capped at 20 (and the intermediate computed in unsigned long)
 * so a large attempt count can never invoke shift UB or overflow before the
 * clamp. ceil_ms is returned as-is when it is below base_ms, so a mis-ordered
 * (ceil < base) pair degrades to a constant ceil rather than inverting.
 */
static inline unsigned int
urp_connect_backoff_ms(unsigned int attempt, unsigned int base_ms,
		       unsigned int ceil_ms)
{
	unsigned int shift = attempt < 20 ? attempt : 20;
	unsigned long ms = (unsigned long)base_ms << shift;

	return ms > ceil_ms ? ceil_ms : (unsigned int)ms;
}

/*
 * Should this endpoint emit liveness PINGs on its QP(s)?
 *
 * A hard peer reboot is a SILENT drop: the RC connection dies without RDMA-CM
 * delivering any event to the initiator, so the CM-driven retry above never
 * fires. Periodic PINGs (and their missing PONGs) are the only signal that a
 * silent drop happened. Multi-QP endpoints already probe for per-QP health
 * (load balancing, DRAINING demotion). Single-QP endpoints gained no benefit
 * from probing -- until Phase 1.5, where the *initiator* needs the PING
 * keepalive to notice a silent drop and re-dial. The acceptor never retries,
 * so a single-QP acceptor stays quiet (unchanged data-path baseline); it still
 * answers PINGs with PONGs unconditionally in the recv path.
 */
static inline bool
urp_should_emit_probes(bool is_initiator, unsigned int num_qps)
{
	return num_qps > 1 || is_initiator;
}

/*
 * On a probe-detected silent drop (>= miss-threshold consecutive missing
 * PONGs), should we convert the dead QP into a reconnect?
 *
 * Only the initiator dials, so only it reconnects; the acceptor merely demotes
 * the QP out of its dispatch set (urp_qp_select_round_robin skips DRAINING) and
 * waits for a fresh CONNECT_REQUEST. @was_established gates out a QP that never
 * carried a session (a bring-up failure is already handled by the CM-event
 * retry path, not the liveness path -- there are no PONGs to miss before
 * ESTABLISHED). When true, the caller runs the same bounded connect-retry the
 * CM error handler uses, so a silent drop self-heals identically to a
 * CM-visible one.
 */
static inline bool
urp_silent_drop_should_reconnect(bool is_initiator, bool was_established)
{
	return is_initiator && was_established;
}

#endif /* _URP_RETRY_PLAN_H */
