// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- per-QP state and selection
 *
 * Phase 3a Step 2: introduces the multi-QP abstraction. The endpoint owns
 * an array of urp_qp entries (one per QP). TX dispatch picks a QP
 * via urp_qp_select_round_robin; RX completion uses urp_qp_index_of to
 * route a completed work request back to the QP that owns it.
 *
 * In this commit num_qps is still capped at 1: urp_rdma_init rejects
 * num_qps > 1 with -EOPNOTSUPP. The scaffold is in place so Step 2b can
 * wire up N parallel rdma_cm_ids and actually allocate N QPs without
 * further restructuring of the data path.
 */

#include "urp.h"
#include <linux/slab.h>

int urp_qps_init(struct urp_endpoint *ep)
{
	u32 i;

	if (!ep->num_qps)
		return -EINVAL;

	ep->qps = kcalloc(ep->num_qps, sizeof(*ep->qps), GFP_KERNEL);
	if (!ep->qps)
		return -ENOMEM;

	for (i = 0; i < ep->num_qps; i++) {
		ep->qps[i].ep = ep;
		ep->qps[i].index = i;
	}

	atomic_set(&ep->qps_connected, 0);
	atomic_set(&ep->rr_counter, 0);
	return 0;
}

void urp_qps_destroy(struct urp_endpoint *ep)
{
	kfree(ep->qps);
	ep->qps = NULL;
}

/*
 * Pick the next QP for a TX frame. Returns NULL if no QPs are connected.
 *
 * For Step 2 the selection is round-robin across all configured QPs and
 * filters non-connected entries by retrying up to num_qps times. With
 * num_qps == 1 this simplifies to returning ep->qps[0] when established.
 *
 * Hash-affinity and adaptive-EWMA selection (design 08 §8.5) depend on
 * probe RTT data and ship in Phase 3b.
 */
struct urp_qp *urp_qp_select_round_robin(struct urp_endpoint *ep)
{
	u32 i;

	if (!ep->qps || !ep->num_qps)
		return NULL;

	for (i = 0; i < ep->num_qps; i++) {
		u32 idx = (u32)atomic_inc_return(&ep->rr_counter) % ep->num_qps;
		struct urp_qp *qps = &ep->qps[idx];

		if (qps->established && qps->qp)
			return qps;
	}

	return NULL;
}

/*
 * Linear lookup of the QP index for a given ib_qp. Used by RX completion
 * handling when a recv buffer needs to be reposted to the same QP it
 * arrived on (SRQ in Step 3 removes this need entirely).
 *
 * Returns -1 if the QP is not part of the endpoint.
 */
int urp_qp_index_of(struct urp_endpoint *ep, struct ib_qp *qp)
{
	u32 i;

	if (!ep->qps)
		return -1;

	for (i = 0; i < ep->num_qps; i++) {
		if (ep->qps[i].qp == qp)
			return (int)i;
	}

	return -1;
}
