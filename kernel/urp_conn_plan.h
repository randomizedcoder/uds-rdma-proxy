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

#endif /* _URP_CONN_PLAN_H */
