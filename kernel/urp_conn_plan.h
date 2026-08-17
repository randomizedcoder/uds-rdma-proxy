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

#endif /* _URP_CONN_PLAN_H */
