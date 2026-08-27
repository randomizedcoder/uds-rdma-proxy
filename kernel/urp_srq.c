// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- Shared Receive Queue
 *
 * Phase 3a Step 3. One SRQ per endpoint, shared by all of its QPs. The
 * per-QP RQ is collapsed (max_recv_wr = 0 in qp_init_attr.cap) and
 * every recv buffer comes off the shared pool via ib_post_srq_recv.
 *
 * Rationale (design 05 section 5.2):
 *   - Memory efficiency -- one pool of N buffers instead of N*num_qps
 *     buffers (one set per QP) sitting unused on idle QPs.
 *   - Prevents per-QP RQ starvation when a single QP is hammered.
 *   - Smaller NIC-side QP context footprint.
 *
 * The credit-based flow control in Step 4 still tracks credits
 * per-QP, but the buffers themselves are drawn from this shared pool.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"

int urp_post_srq_recv(struct urp_endpoint *ep, struct urp_buffer *buf)
{
	struct ib_recv_wr wr = {};
	const struct ib_recv_wr *bad_wr;
	u32 j, remaining;

	/*
	 * A frame lands across the buffer's chunks (design 37 path Y). Arm the
	 * SGE lengths to sum to exactly ep->buf_size (the last chunk short if
	 * buf_size isn't a chunk multiple), so total recv capacity stays the
	 * logical slot -- a completion's byte_len can never exceed buf_size, the
	 * invariant urp_classify_frame's overrun guard relies on. At
	 * num_chunks == 1 this is one SGE of ep->buf_size -- unchanged.
	 */
	remaining = ep->buf_size;
	for (j = 0; j < buf->num_chunks; j++) {
		u32 clen = min(remaining, ep->chunk_size);

		buf->sges[j].length = clen;
		remaining -= clen;
	}
	buf->cqe.done = urp_recv_done_for_srq;

	wr.wr_cqe = &buf->cqe;
	wr.sg_list = buf->sges;
	wr.num_sge = buf->num_chunks;

	return ib_post_srq_recv(ep->srq, &wr, &bad_wr);
}

/*
 * Post the initial set of recv buffers from the pool to the SRQ.
 * Returns 0 on success or -ENOBUFS if no buffers were available.
 */
int urp_srq_post_initial(struct urp_endpoint *ep)
{
	u32 posted = 0;

	while (posted < ep->srq_pool_target) {
		struct urp_buffer *buf = urp_buf_alloc_recv(ep);
		int ret;

		if (!buf)
			break;	/* pool exhausted -- not fatal */

		ret = urp_post_srq_recv(ep, buf);
		if (ret) {
			urp_buf_free_recv(ep, buf);
			pr_err("ib_post_srq_recv failed: %d\n", ret);
			return ret;
		}
		posted++;
	}

	pr_info("posted %u initial recv buffers to SRQ\n", posted);
	return posted ? 0 : -ENOBUFS;
}

int urp_srq_create(struct urp_endpoint *ep)
{
	struct ib_srq_init_attr attr = {};
	u32 max_wr;
	int ret;

	if (ep->srq)
		return 0;

	/*
	 * Cap the SRQ at the size of the recv half of the buffer pool -- we
	 * never need more outstanding posts than the pool can supply -- and
	 * never past the device's max_srq_wr.
	 */
	max_wr = min_t(u32, ep->num_bufs / 2,
		       (u32)ep->ib_dev->attrs.max_srq_wr);
	ep->srq_pool_target = max_wr;

	attr.event_handler = NULL;
	attr.attr.max_wr = max_wr;
	/*
	 * A recv frame is scattered across ep->num_chunks chunks (design 37
	 * path Y), so the SRQ must allow that many SGEs per recv. Clamp to the
	 * device's SRQ SGE limit. PR 3a: num_chunks == 1.
	 */
	attr.attr.max_sge = min_t(u32, ep->num_chunks,
				  (u32)ep->ib_dev->attrs.max_srq_sge);
	attr.attr.srq_limit = 0;

	ep->srq = ib_create_srq(ep->pd, &attr);
	if (IS_ERR(ep->srq)) {
		ret = PTR_ERR(ep->srq);
		ep->srq = NULL;
		pr_err("ib_create_srq failed: %d\n", ret);
		return ret;
	}

	ret = urp_srq_post_initial(ep);
	if (ret) {
		ib_destroy_srq(ep->srq);
		ep->srq = NULL;
		return ret;
	}

	return 0;
}

void urp_srq_destroy(struct urp_endpoint *ep)
{
	if (!ep->srq)
		return;

	/*
	 * ib_destroy_srq drains posted WRs and fires completions with
	 * IB_WC_WR_FLUSH_ERR -- urp_recv_done_for_srq returns the buffers
	 * to the pool so this is clean.
	 */
	ib_destroy_srq(ep->srq);
	ep->srq = NULL;
}
