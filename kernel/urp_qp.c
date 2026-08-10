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
		/*
		 * Step 4: initial credits. SRQ pool size is the natural
		 * upper bound (peer can grant up to its SRQ population),
		 * so use that. Step 4b wires the gate.
		 */
		urp_credit_init(&ep->qps[i].credit, URP_NUM_BUFS / 2);
		/* Step 5: start in QUALIFYING; RDMA_CM_EVENT_ESTABLISHED
		 * promotes to ACTIVE (probe-driven Qualifying lands later).
		 */
		ep->qps[i].health = URP_QP_STATE_QUALIFYING;
		/*
		 * Phase 5 Step 3: ROUTE_RESOLVED schedules this to call
		 * rdma_connect() outside the CM handler so we don't
		 * recursively take id->qp_mutex.
		 */
		INIT_WORK(&ep->qps[i].connect_work, urp_connect_work_fn);
	}

	atomic_set(&ep->qps_connected, 0);
	atomic_set(&ep->rr_counter, 0);
	return 0;
}

void urp_qps_destroy(struct urp_endpoint *ep)
{
	u32 i;

	if (ep->qps) {
		for (i = 0; i < ep->num_qps; i++)
			cancel_work_sync(&ep->qps[i].connect_work);
	}
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
 * Hash-affinity and adaptive-EWMA selection (design 08 section 8.5) depend on
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

		if (!qps->established || !qps->qp)
			continue;
		/* Step 5: skip QPs that probes have demoted out of the
		 * working set (DRAINING / REMOVED). QUALIFYING and ACTIVE
		 * both carry data -- QUALIFYING is the just-established
		 * pre-promotion state, ACTIVE is the steady-state.
		 */
		if (qps->health == URP_QP_STATE_DRAINING ||
		    qps->health == URP_QP_STATE_REMOVED)
			continue;
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
