/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure acceptor connection-plan decisions (design 32 hardware bring-up).
 *
 * Split out so the decision compiles both in the kernel and standalone in
 * userspace (nix/fuzz + the urp_conn_plan_test harness) -- a pure function of
 * (mode, config) with no sockets/kthreads/RDMA. Keep in lock-step with the
 * KUnit table in kernel/urp_test.c.
 *
 * Background: on the acceptor, the legacy k0 path opens a single ep->conn
 * backend UDS eagerly at CONNECT_REQUEST, while the multi-stream path opens a
 * backend per stream lazily on SYN. Running the eager connect in multi-stream
 * mode steals the single connection a backend accepts, so the real stream's
 * per-stream connect is refused (ECONNREFUSED) and the data path stalls. This
 * predicate gates the eager connect on the endpoint's mode.
 */
#ifndef _URP_CONN_PLAN_H
#define _URP_CONN_PLAN_H

#ifdef __KERNEL__
#include <linux/types.h>
#include "urp.h"		/* enum urp_ep_mode (via the bundled uapi header) */
#else
#include <stdbool.h>
#include <linux/urp.h>		/* enum urp_ep_mode (standalone) */
#endif

/*
 * Should the acceptor open the legacy single ep->conn backend connection
 * eagerly at CONNECT_REQUEST?
 *
 * Only in k0 mode, and only when a connect_path is configured. A multi-stream
 * acceptor defers to per-stream backend connects (urp_stream_open_backend) and
 * must NOT eager-connect.
 */
static inline bool
urp_acceptor_should_eager_connect(enum urp_ep_mode mode, bool have_connect_path)
{
	return have_connect_path && mode == URP_EP_MODE_K0;
}

/*
 * Should this CM teardown release the acceptor's ep->qps_accepted slot?
 *
 * Design 33 Bug 1: the acceptor claims a slot at CONNECT_REQUEST and must
 * return it on ANY teardown of that child -- including a half-open child that
 * was rejected or errored before RDMA_CM_EVENT_ESTABLISHED. The original code
 * released the slot only inside the `established` branch, so a never-established
 * child leaked its slot and the acceptor then refused every future connect
 * ("rejecting extra CONNECT_REQUEST 1 >= 1") until module reload.
 *
 * Release iff we are the acceptor (@is_initiator == false) and this slot still
 * holds a claim (@slot_held). The caller clears the slot flag after releasing
 * so a second teardown event for the same QP cannot double-decrement.
 */
static inline bool
urp_acceptor_should_release_slot(bool is_initiator, bool slot_held)
{
	return !is_initiator && slot_held;
}

/*
 * Should the initiator unlink its stale AF_UNIX listen-socket node before
 * binding (and again on cleanup)?
 *
 * Design 33 Bug 2: only the initiator binds a pathname listen socket, and
 * nothing ever unlinks it, so after drain+remove (or a crash) the next
 * `urp add` fails -98 EADDRINUSE on bind. Unlink iff we are the initiator and
 * a listen path is configured. The action itself (kern_path + vfs_unlink) is
 * filesystem I/O -- only this decision is pure/unit-testable.
 */
static inline bool
urp_should_unlink_listen_path(bool is_initiator, bool listen_path_set)
{
	return is_initiator && listen_path_set;
}

/*
 * gap #6 Phase 1: which ep->qps[] slot does an incoming CONNECT_REQUEST claim,
 * and must that slot first be torn down?
 *
 * The acceptor originally used a monotonic CONNECT_REQUEST counter as the slot
 * index. Under num_qps > 1 the initiator dials all QPs concurrently and each
 * per-QP retry creates a *fresh* cm_id / CONNECT_REQUEST; the counter then ran
 * past num_qps on surplus/reordered requests and rejected them -- a storm that
 * never let all N QPs reach ESTABLISHED, so ep->connected never latched and the
 * data path stayed dark.
 *
 * When @have_peer_index (the initiator advertised its qp_index in the wide
 * private_data trailer, urp_conn_priv_build_qp), the slot is chosen by peer
 * identity: a retry for QP k always resolves to slot k and tears down that
 * slot's stale cm_id, instead of consuming a new counter value. @counter_index
 * is the legacy fallback for a single-QP or old-build peer that sends no
 * qp_index (there num_qps is 1, so index 0 is the only valid slot and a genuine
 * surplus request is still correctly rejected).
 *
 * Pure: the caller supplies @slot_occupied (ep->qps[idx].cm_id != NULL) and,
 * on URP_SLOT_REUSE, runs urp_qp_hard_teardown(ep, *out_index) before reusing.
 * @out_index is written only when the decision is not URP_SLOT_REJECT.
 */
enum urp_slot_decision {
	URP_SLOT_REJECT = 0,	/* index out of range -> rdma_reject */
	URP_SLOT_FRESH,		/* slot empty -> create QP directly */
	URP_SLOT_REUSE,		/* slot holds a stale cm_id -> teardown, then reuse */
};

static inline enum urp_slot_decision
urp_acceptor_slot_decide(bool have_peer_index, unsigned int peer_qp_index,
			 unsigned int counter_index, unsigned int num_qps,
			 bool slot_occupied, unsigned int *out_index)
{
	unsigned int idx = have_peer_index ? peer_qp_index : counter_index;

	if (idx >= num_qps)
		return URP_SLOT_REJECT;
	*out_index = idx;
	return slot_occupied ? URP_SLOT_REUSE : URP_SLOT_FRESH;
}

#endif /* _URP_CONN_PLAN_H */
