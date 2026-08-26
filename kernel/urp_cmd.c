// SPDX-License-Identifier: GPL-2.0
/*
 * urp-fast: io_uring uring_cmd char device (design 31, PR1).
 *
 * Opens a /dev/urp misc device whose only interesting file op is
 * ->uring_cmd. An aware application drives the zero-copy fast path over
 * IORING_OP_URING_CMD submissions on this fd:
 *
 *   URP_CMD_REGISTER    pin an app buffer pool once (pin_user_pages_fast,
 *                       FOLL_LONGTERM) -- the "malloc at startup, then flat
 *                       memory pressure" phase the design centers on.
 *   URP_CMD_UNREGISTER  unpin it.
 *   URP_CMD_SEND/RECV   hand a buffer to / take a buffer from the NIC.
 *                       Validated here; the RDMA data path lands in a later
 *                       PR (returns -ENOSYS for now).
 *
 * This PR1 does the pin/unpin and the full argument validation -- the
 * app->kernel trust boundary of design 31 section 31.10 -- but not yet the
 * MR registration (needs a fast endpoint's PD) or the ib_post_send/recv.
 *
 * Ownership discipline (section 31.2): a buffer is APP_OWNED or KERNEL_OWNED,
 * and the SQE/CQE pair is the handoff. PR1 only owns the *pool*, not
 * individual buffers, so the per-buffer state machine arrives with the data
 * path.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"
#include "urp_cmd.h"
#include "include/uapi/linux/urp_cmd.h"

/*
 * The fast-path char device leans on modern io_uring plumbing: the split-out
 * <linux/io_uring/cmd.h> header (6.7) and the 4-argument pin_user_pages_fast.
 * Gate the whole device on URP_FAST_ENABLED
 * (urp.h) -- CONFIG_URP_FAST && >= 6.8 -- and stub the register/unregister hooks
 * otherwise (older LTS, or a deliberate CONFIG_URP_FAST=n build) so those
 * compile-only CI gates stay green and the zero-copy path can be excluded
 * wholesale. The pure validators in urp_cmd_validate.c compile on every
 * supported kernel, so KUnit and the userspace check exercise the trust
 * boundary regardless.
 */
#if URP_FAST_ENABLED

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>	/* kmap_local_page */
#include <linux/uaccess.h>
#include <linux/io_uring/cmd.h>

#include "urp_cmd_own.h"	/* pure per-buffer ownership state machine */

/*
 * The in-place header reservation (design 31 D3) is the wire header size. The
 * ABI mirrors it as URP_CMD_HEADER_RESV so the dual-compiled validator stays
 * self-contained; assert the two never drift.
 */
static_assert(URP_CMD_HEADER_RESV == URP_FRAME_HEADER_SIZE,
	      "fast-path header reservation must equal the wire header size");

/*
 * Typed accessor for the 16-byte inline SQE command area.
 *
 * Upstream's io_uring_sqe_cmd() only grew its 2-arg typed form (returning a
 * (const type *) with a compile-time size assertion) in the 7.x series; the
 * 6.8-era kernels this fast path also targets expose just the 1-arg
 * io_uring_sqe_cmd(sqe) that returns a bare (const void *). Rather than gate on
 * the exact version the macro changed, replicate the typed form locally so the
 * fast path compiles cleanly across the whole >= 6.8 range. Both kernels back
 * the accessor with the same sqe->cmd bytes.
 */
#define urp_sqe_cmd(sqe, type)						\
({									\
	BUILD_BUG_ON(sizeof(type) > (sizeof(struct io_uring_sqe) -	\
				     offsetof(struct io_uring_sqe, cmd)));\
	(const type *)(sqe)->cmd;					\
})

/* Refuse pools larger than this so a bad REGISTER can't try to pin the world. */
#define URP_CMD_POOL_BYTES_MAX	(1UL << 30)	/* 1 GiB */

/*
 * The pure command validators (urp_cmd_validate_data / urp_cmd_validate_reg)
 * live in urp_cmd_validate.c so the same source compiles into both the module
 * and the userspace validator check. See that file's header.
 */

/* ----------------------------------------------------------------------- */
/* Per-open context: one registered pool per fd (PR1).                     */
/* ----------------------------------------------------------------------- */

struct urp_fast_op;	/* completion op, defined after the ctx it points back at */

struct urp_cmd_ctx {
	struct mutex		lock;		/* serialises REGISTER/UNREGISTER */
	bool			registered;
	u64			base;		/* app pool base address        */
	u64			pool_len;	/* total bytes                  */
	unsigned long		nr_pages;	/* pinned pages                 */
	struct page		**pages;	/* FOLL_LONGTERM pin            */
	struct urp_cmd_pool_geom geom;		/* count + buf_size             */

	/*
	 * The endpoint the pool is registered against. We hold a kref for the
	 * registration's lifetime; @ib_dev is cached from ep->ib_dev at REGISTER
	 * so UNREGISTER can unmap even if the endpoint has since cleared it.
	 *
	 * The pool is registered as ONE memory region (design 31 PR3b, Option C):
	 * @mr addresses the whole pinned pool by virtual offset, so a SEND posts a
	 * single SGE regardless of frame size (@lkey == mr->lkey, base == mr->iova).
	 * @sgt is the scatter table over the pinned pages, DMA-mapped to @sg_nents
	 * entries. This replaces PR2's per-page @dma[]/local_dma_lkey, which could
	 * only name one page per SGE and so capped a frame at a single page.
	 */
	struct urp_endpoint	*ep;
	struct ib_device	*ib_dev;
	struct ib_mr		*mr;
	u64			mr_iova;	/* == mr->iova; frame @ +buf_index*buf_size */
	u32			lkey;		/* == mr->lkey                            */
	struct sg_table		sgt;
	int			sg_nents;	/* ib_dma_map_sg result (for unmap)       */

	/*
	 * Per-buffer ownership (design 31 section 31.2): bit set == KERNEL_OWNED
	 * (in flight to/from the NIC). @own_lock serialises the pure transitions in
	 * urp_cmd_own.h between the submit path and the completion callback.
	 */
	spinlock_t		own_lock;
	unsigned long		*own;		/* bitmap[urp_own_words(geom.count)]      */

	/*
	 * One preallocated completion op per pool buffer, indexed by buf_index.
	 * The ownership SM guarantees at most one op in flight per buffer, so no
	 * per-op allocation is needed on the hot path (the flat-pressure invariant).
	 */
	struct urp_fast_op	*ops;		/* [geom.count]                           */
};

/*
 * One in-flight SEND/RECV op. Drawn from ctx->ops[buf_index] (no per-op alloc).
 * @cqe is the RDMA completion hook: the send CQ (IB_POLL_WORKQUEUE) dispatches
 * to urp_fast_send_done via container_of(wc->wr_cqe). @res is filled there and
 * read by the task-work callback that posts the io_uring CQE.
 */
struct urp_fast_op {
	struct ib_cqe		cqe;
	struct io_uring_cmd	*ioucmd;
	struct urp_cmd_ctx	*ctx;
	u32			buf_index;
	u32			len;		/* payload bytes (the SEND res on success) */
	u16			stream_id;
	s32			res;		/* set by the CQ handler, read by task-work */
};

/*
 * Completion ABI shims. io_uring changed both the cmd-completion call and the
 * task-work callback signature between the 6.x LTS line and 7.0:
 *
 *   - 6.x: io_uring_cmd_done(cmd, ret, res2, issue_flags); the CQE size (16 vs
 *     32) follows the ring, so one call serves both.
 *   - 7.x: io_uring_cmd_done(cmd, ret, issue_flags) posts a 16-byte CQE and
 *     io_uring_cmd_done32(cmd, ret, res2, issue_flags) a 32-byte one.
 *
 *   - 6.x task-work: void cb(struct io_uring_cmd *cmd, unsigned issue_flags).
 *   - 7.x task-work: void cb(struct io_tw_req, io_tw_token_t); recover the cmd
 *     with io_uring_cmd_from_tw() and complete with the fixed defer flags.
 *
 * urp_cmd_complete() hides the first difference (SEND passes cqe32=false,res2=0;
 * PR4's RECV will pass cqe32=true to carry buf_index/stream_id in res2). The
 * task-work callbacks below hide the second so their body is version-common.
 */
static inline void urp_cmd_complete(struct io_uring_cmd *ioucmd, s32 res,
				    u64 res2, bool cqe32, unsigned int issue_flags)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	if (cqe32)
		io_uring_cmd_done32(ioucmd, res, res2, issue_flags);
	else
		io_uring_cmd_done(ioucmd, res, issue_flags);
#else
	io_uring_cmd_done(ioucmd, res, res2, issue_flags);
#endif
}

/* Common task-work body: pull the op stashed in the cmd's inline pdu and post
 * the io_uring CQE with the result the CQ handler recorded. SEND uses a 16-byte
 * CQE (no res2); RECV will switch to cqe32 in PR4.
 */
static void urp_fast_send_complete(struct io_uring_cmd *ioucmd,
				   unsigned int issue_flags)
{
	struct urp_fast_op *op = *io_uring_cmd_to_pdu(ioucmd, struct urp_fast_op *);

	urp_cmd_complete(ioucmd, op->res, 0, false, issue_flags);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static void urp_fast_send_tw(struct io_tw_req tw_req, io_tw_token_t tw)
{
	urp_fast_send_complete(io_uring_cmd_from_tw(tw_req),
			       IO_URING_CMD_TASK_WORK_ISSUE_FLAGS);
}
#else
static void urp_fast_send_tw(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	urp_fast_send_complete(ioucmd, issue_flags);
}
#endif

/*
 * RECV task-work body: post a 32-byte CQE carrying the payload length in res and
 * the buffer index + demuxed stream_id in res2, so the app finds which donated
 * buffer the NIC filled and which stream it belongs to (design 31 section 31.6,
 * open question Q3) -- all without a copy, the bytes are already in the app page.
 */
static void urp_fast_recv_complete(struct io_uring_cmd *ioucmd,
				   unsigned int issue_flags)
{
	struct urp_fast_op *op = *io_uring_cmd_to_pdu(ioucmd, struct urp_fast_op *);
	u64 res2 = (u64)op->buf_index | ((u64)op->stream_id << 32);

	urp_cmd_complete(ioucmd, op->res, res2, true, issue_flags);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static void urp_fast_recv_tw(struct io_tw_req tw_req, io_tw_token_t tw)
{
	urp_fast_recv_complete(io_uring_cmd_from_tw(tw_req),
			       IO_URING_CMD_TASK_WORK_ISSUE_FLAGS);
}
#else
static void urp_fast_recv_tw(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	urp_fast_recv_complete(ioucmd, issue_flags);
}
#endif

/*
 * Flush the endpoint's receive queues so no posted fast-RECV WR still references
 * the pinned pool. A donated RX buffer that never received a frame -- or one
 * whose io_uring RECV was cancelled on ring teardown -- leaves its recv WR armed
 * on the QP with the buffer KERNEL_OWNED; draining the RQ transitions the QP to
 * error and completes every armed WR with IB_WC_WR_FLUSH_ERR, which runs
 * urp_fast_recv_done to release each buffer. After this returns the NIC will not
 * DMA into the pool, so the caller may safely deregister the MR and unpin. This
 * ends the endpoint's RDMA connection, which is correct: it is only reached when
 * the app's control fd is closing (design 31 D4 teardown quiesce).
 */
static void urp_fast_recv_drain(struct urp_cmd_ctx *ctx)
{
	struct urp_endpoint *ep = ctx->ep;
	u32 i;

	if (!ep || !ep->qps)
		return;

	for (i = 0; i < ep->num_qps; i++)
		if (ep->qps[i].qp)
			ib_drain_rq(ep->qps[i].qp);
}

static void urp_cmd_pool_release(struct urp_cmd_ctx *ctx)
{
	bool inflight;

	lockdep_assert_held(&ctx->lock);

	if (!ctx->registered)
		return;

	/*
	 * Teardown quiesce (design 31 D4): if any buffer is still KERNEL_OWNED a
	 * recv WR may still reference the pinned pages, so drain the endpoint's
	 * RQs before touching the MR or the pin -- otherwise the NIC could DMA
	 * into freed memory. Only reachable on fd close: UNREGISTER refuses with
	 * -EBUSY while any buffer is in flight, and by the time io_uring calls
	 * ->release every -EIOCBQUEUED RECV has already been cancel-completed
	 * (its buffer stays KERNEL_OWNED with a NULL ioucmd until this drain
	 * flushes it). SEND completions always flip ownership back on their own.
	 */
	spin_lock(&ctx->own_lock);
	inflight = urp_own_any_kernel(ctx->own, ctx->geom.count);
	spin_unlock(&ctx->own_lock);
	if (inflight)
		urp_fast_recv_drain(ctx);

	/*
	 * Order: drop the per-op slab and ownership bitmap first (no NIC access
	 * once the MR is gone and the RQ is drained), then dereg the MR, unmap the
	 * scatter table, and finally unpin.
	 */
	kvfree(ctx->ops);
	ctx->ops = NULL;
	kvfree(ctx->own);
	ctx->own = NULL;

	if (ctx->mr) {
		ib_dereg_mr(ctx->mr);
		ctx->mr = NULL;
	}
	if (ctx->sgt.sgl) {
		ib_dma_unmap_sg(ctx->ib_dev, ctx->sgt.sgl, ctx->sgt.orig_nents,
				DMA_BIDIRECTIONAL);
		sg_free_table(&ctx->sgt);
	}
	ctx->sg_nents	= 0;
	ctx->mr_iova	= 0;
	if (ctx->ep) {
		urp_endpoint_put(ctx->ep);
		ctx->ep = NULL;
	}
	ctx->ib_dev	= NULL;
	ctx->lkey	= 0;

	unpin_user_pages(ctx->pages, ctx->nr_pages);
	kvfree(ctx->pages);
	ctx->pages	= NULL;
	ctx->nr_pages	= 0;
	ctx->registered	= false;
	ctx->geom.count	= 0;
	ctx->geom.buf_size = 0;
	ctx->base	= 0;
	ctx->pool_len	= 0;
}

/* ----------------------------------------------------------------------- */
/* uring_cmd handlers.                                                      */
/* ----------------------------------------------------------------------- */

/*
 * Upper bound on how long a REGISTER waits for its fast-registration WR to
 * complete before giving up. A healthy QP completes REG_MR in microseconds;
 * the timeout only bites when REGISTER is issued before the RC connection is
 * up, so the WR sits un-drained in the send queue (the classic wedge). Paired
 * with a killable wait so a fatal signal (the app being killed mid-REGISTER)
 * unblocks the io-wq worker immediately rather than parking it uninterruptibly.
 */
#define URP_REG_MR_TIMEOUT_MS 5000

/*
 * Completion cookie for the REG_MR work request below. Heap-allocated and
 * refcounted (not stack) so the waiter can bail out early -- on a fatal signal
 * or timeout -- while the WR is still in flight: the CQ callback may fire long
 * afterwards (when the QP finally connects, or as IB_WC_WR_FLUSH_ERR on QP
 * teardown) and must find live memory to complete()/free. Two refs: one held by
 * the waiter, one by the CQ callback; the last put frees, and dereg's an
 * abandoned MR only once its WR has drained (see urp_cmd_reg_mr_sync).
 */
struct urp_reg_wait {
	struct ib_cqe		cqe;
	struct completion	done;
	int			status;
	struct kref		ref;
	struct ib_mr		*mr;	/* non-NULL only if abandoned: dereg here */
};

static void urp_reg_wait_release(struct kref *ref)
{
	struct urp_reg_wait *rw = container_of(ref, struct urp_reg_wait, ref);

	/* Set only on the abandon path, where the caller was told NOT to dereg.
	 * The last put runs after the CQ callback fired (WR drained), so the MR
	 * is no longer referenced by an in-flight WR and is safe to release. */
	if (rw->mr)
		ib_dereg_mr(rw->mr);
	kfree(rw);
}

static void urp_cmd_reg_mr_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_reg_wait *rw = container_of(wc->wr_cqe, struct urp_reg_wait, cqe);

	rw->status = (wc->status == IB_WC_SUCCESS) ? 0 : -EIO;
	complete(&rw->done);
	kref_put(&rw->ref, urp_reg_wait_release);
}

/*
 * Register @mr with the HCA via a fast-registration work request on @qp and
 * wait for its completion.
 *
 * Returns 0 once the MR is live (caller keeps @mr), or a negative errno:
 *   - a hard failure (ib_post_send error, or WC error -EIO) means the WR is
 *     drained and the caller still owns @mr (must dereg it);
 *   - -ETIMEDOUT / -ERESTARTSYS means the WR is STILL IN FLIGHT and this helper
 *     has taken ownership of @mr via the deferred releaser -- the caller MUST
 *     NOT dereg @mr in that case (see urp_cmd_map_pool).
 */
static int urp_cmd_reg_mr_sync(struct ib_qp *qp, struct ib_mr *mr)
{
	struct urp_reg_wait *rw;
	struct ib_reg_wr rwr = {};
	long tmo;
	int ret;

	rw = kzalloc(sizeof(*rw), GFP_KERNEL);
	if (!rw)
		return -ENOMEM;
	init_completion(&rw->done);
	kref_init(&rw->ref);		/* ref 1: the waiter (this function) */
	kref_get(&rw->ref);		/* ref 2: the CQ callback */
	rw->cqe.done = urp_cmd_reg_mr_done;

	rwr.wr.opcode	  = IB_WR_REG_MR;
	rwr.wr.wr_cqe	  = &rw->cqe;
	rwr.wr.send_flags = IB_SEND_SIGNALED;
	rwr.mr		  = mr;
	rwr.key		  = mr->lkey;
	rwr.access	  = IB_ACCESS_LOCAL_WRITE;

	ret = ib_post_send(qp, &rwr.wr, NULL);
	if (ret) {
		/* WR never posted: the CQ callback will never run, so drop its
		 * ref here too. rw->mr stays NULL -- the caller owns @mr. */
		kref_put(&rw->ref, urp_reg_wait_release);
		kref_put(&rw->ref, urp_reg_wait_release);
		return ret;
	}

	tmo = wait_for_completion_killable_timeout(&rw->done,
						   msecs_to_jiffies(URP_REG_MR_TIMEOUT_MS));
	if (tmo > 0) {
		/* Completed: WR drained. @mr is live (status 0) or errored; the
		 * caller owns @mr either way. */
		ret = rw->status;
		kref_put(&rw->ref, urp_reg_wait_release);
		return ret;
	}

	/* Abandoned: killed (tmo < 0) or timed out (tmo == 0) with the WR still
	 * in flight. Hand @mr to the deferred releaser so it is dereg'd only
	 * after the WR finally drains, then drop the waiter's ref. The waiter's
	 * ref kept rw alive across this store, so this is UAF-free. */
	rw->mr = mr;
	kref_put(&rw->ref, urp_reg_wait_release);
	return (tmo == 0) ? -ETIMEDOUT : -ERESTARTSYS;
}

/*
 * Bind the pinned pool to the named endpoint and register it as ONE memory
 * region against that endpoint's RDMA device (design 31 PR3b, Option C): build
 * a scatter table over the pinned pages, DMA-map it, allocate an FRWR MR big
 * enough for every page, program it with the page list, and register it on the
 * endpoint's QP. The pool then shares the endpoint's PD and is addressable by
 * virtual offset through mr->lkey, so a SEND of any frame size posts a single
 * SGE. On success fills ctx->{ep,ib_dev,mr,mr_iova,lkey,sgt,sg_nents} and holds
 * an endpoint kref; on failure returns a negative errno having undone every
 * partial step and dropped the ref. The caller still owns the pin on failure.
 *
 * NOTE: ep->pd / ep->ib_dev / qp[0] are read without ep->lock. The kref keeps
 * the endpoint struct alive; a concurrent endpoint teardown racing the mapping
 * is closed by the ownership/quiesce protocol (design 31 D4) landing in PR4.
 */
static int urp_cmd_map_pool(struct urp_cmd_ctx *ctx, struct page **pages,
			    unsigned long nr_pages, const char *ep_name)
{
	struct urp_endpoint *ep;
	struct ib_qp *qp;
	struct ib_mr *mr;
	int nents, mapped, ret;

	ep = urp_endpoint_get(ep_name);
	if (!ep)
		return -ENODEV;

	/* Need an established RDMA back-end (PD + device + a QP to register on). */
	if (!ep->pd || !ep->ib_dev || !ep->qps || !ep->qps[0].qp) {
		ret = -ENOTCONN;
		goto err_put;
	}
	qp = ep->qps[0].qp;

	ret = sg_alloc_table_from_pages(&ctx->sgt, pages, nr_pages, 0,
					(size_t)nr_pages << PAGE_SHIFT, GFP_KERNEL);
	if (ret)
		goto err_put;

	nents = ib_dma_map_sg(ep->ib_dev, ctx->sgt.sgl, ctx->sgt.orig_nents,
			      DMA_BIDIRECTIONAL);
	if (nents == 0) {
		ret = -ENOMEM;
		goto err_sgt;
	}
	ctx->sg_nents = nents;

	mr = ib_alloc_mr(ep->pd, IB_MR_TYPE_MEM_REG, nr_pages);
	if (IS_ERR(mr)) {
		ret = PTR_ERR(mr);
		goto err_unmap;
	}

	/*
	 * Program the MR's page table from the DMA-mapped scatter list. The IOMMU
	 * may coalesce adjacent pages, so the mapped count is compared against the
	 * post-map @nents (sg elements), NOT nr_pages -- ib_map_mr_sg returns the
	 * number of sg elements it consumed. A short map means the whole pool did
	 * not fit the MR page list; verify mr->length covers every pinned page too,
	 * so the app can never address a byte the MR does not map.
	 */
	mapped = ib_map_mr_sg(mr, ctx->sgt.sgl, nents, NULL, PAGE_SIZE);
	if (mapped < 0) {
		ret = mapped;
		goto err_mr;
	}
	if (mapped != nents || mr->length < ((u64)nr_pages << PAGE_SHIFT)) {
		ret = -EINVAL;
		goto err_mr;
	}

	/* Bump the key so a stale rkey/lkey from a prior MR generation is invalid,
	 * then register the MR with the HCA and block until it is live.
	 */
	ib_update_fast_reg_key(mr, ib_inc_rkey(mr->lkey) & 0xff);
	ret = urp_cmd_reg_mr_sync(qp, mr);
	if (ret) {
		/* On abandonment (killed/timed out) the REG_MR WR may still be in
		 * flight and urp_cmd_reg_mr_sync has taken ownership of @mr for a
		 * deferred dereg; do NOT dereg it here. Hard errors leave @mr with
		 * us to release via err_mr. */
		if (ret == -ETIMEDOUT || ret == -ERESTARTSYS)
			goto err_unmap;
		goto err_mr;
	}

	ctx->ep		= ep;
	ctx->ib_dev	= ep->ib_dev;
	ctx->mr		= mr;
	ctx->mr_iova	= mr->iova;
	ctx->lkey	= mr->lkey;
	return 0;

err_mr:
	ib_dereg_mr(mr);
err_unmap:
	ib_dma_unmap_sg(ep->ib_dev, ctx->sgt.sgl, ctx->sgt.orig_nents,
			DMA_BIDIRECTIONAL);
	ctx->sg_nents = 0;
err_sgt:
	sg_free_table(&ctx->sgt);
err_put:
	urp_endpoint_put(ep);
	return ret;
}

static int urp_cmd_do_register(struct urp_cmd_ctx *ctx,
			       const struct io_uring_sqe *sqe)
{
	const struct urp_cmd_reg_sqe *inl =
		urp_sqe_cmd(sqe, struct urp_cmd_reg_sqe);
	struct urp_cmd_reg reg;
	unsigned long nr_pages;
	struct page **pages;
	long pinned;
	int ret;

	if (inl->__resv != 0)
		return -EINVAL;

	if (copy_from_user(&reg, u64_to_user_ptr(inl->arg), sizeof(reg)))
		return -EFAULT;

	reg.endpoint[URP_CMD_NAME_MAX - 1] = '\0';	/* trust nothing */

	ret = urp_cmd_validate_reg(&reg);
	if (ret)
		return ret;

	if (reg.len > URP_CMD_POOL_BYTES_MAX)
		return -E2BIG;

	nr_pages = reg.len >> PAGE_SHIFT;

	mutex_lock(&ctx->lock);
	if (ctx->registered) {
		ret = -EEXIST;			/* one pool per fd in PR1 */
		goto out;
	}

	pages = kvmalloc_array(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Pin the whole pool once, for the lifetime of the registration
	 * (FOLL_LONGTERM). FOLL_WRITE because RX buffers are DMA *write*
	 * targets. Runs in the submitting task's mm; the io-wq punt below
	 * keeps that true even when io_uring offloads this blocking op.
	 *
	 * pin_user_pages_fast(), not the bare pin_user_pages(): the latter
	 * requires the caller to already hold mmap_read_lock -- it does
	 * mmap_assert_locked() and walks the VMA tree (find_vma) without taking
	 * the lock itself. We hold no such lock in this io-wq issue context, so
	 * the plain variant trips a rwsem "lock not held" WARN in find_vma().
	 * The _fast variant manages mmap_lock internally (lockless fast path,
	 * slow-path fallback takes the lock) and is the same API io_uring uses
	 * to pin its own fixed buffers. nr_pages <= 1 GiB/PAGE_SIZE fits int.
	 */
	pinned = pin_user_pages_fast(reg.base, (int)nr_pages,
				     FOLL_LONGTERM | FOLL_WRITE, pages);
	if (pinned < 0) {
		ret = (int)pinned;
		kvfree(pages);
		goto out;
	}
	if ((unsigned long)pinned != nr_pages) {
		unpin_user_pages(pages, pinned);
		kvfree(pages);
		ret = -EFAULT;
		goto out;
	}

	/*
	 * Register the pinned pool with the endpoint's RDMA device (DMA-map +
	 * local lkey). nr_pages is set first so a failure here unwinds cleanly.
	 */
	ctx->nr_pages = nr_pages;
	ret = urp_cmd_map_pool(ctx, pages, nr_pages, reg.endpoint);
	if (ret) {
		unpin_user_pages(pages, nr_pages);
		kvfree(pages);
		ctx->nr_pages = 0;
		goto out;
	}

	ctx->base		= reg.base;
	ctx->pool_len		= reg.len;
	ctx->pages		= pages;
	ctx->geom.count		= reg.count;
	ctx->geom.buf_size	= reg.buf_size;
	ctx->registered		= true;

	/*
	 * Ownership bitmap (all APP_OWNED) + the per-buffer completion slab. On
	 * failure urp_cmd_pool_release undoes the MR/map/pin fully -- ctx is now
	 * marked registered so it runs the whole teardown.
	 */
	ctx->own = kvcalloc(urp_own_words(reg.count), sizeof(unsigned long),
			    GFP_KERNEL);
	ctx->ops = kvcalloc(reg.count, sizeof(*ctx->ops), GFP_KERNEL);
	if (!ctx->own || !ctx->ops) {
		urp_cmd_pool_release(ctx);
		ret = -ENOMEM;
		goto out;
	}
	ret = 0;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int urp_cmd_do_unregister(struct urp_cmd_ctx *ctx)
{
	int ret;

	mutex_lock(&ctx->lock);
	if (!ctx->registered) {
		ret = -ENXIO;
		goto out;
	}
	/*
	 * Refuse while any buffer is still in flight (KERNEL_OWNED): unpinning /
	 * deregistering under the NIC would be a use-after-free. The app must reap
	 * all SEND completions first. The full teardown-quiesce that also drains
	 * outstanding WRs (design 31 D4) lands with PR4; explicit fd close is
	 * already safe because io_uring holds the file until every SEND completes.
	 */
	spin_lock(&ctx->own_lock);
	if (urp_own_any_kernel(ctx->own, ctx->geom.count)) {
		spin_unlock(&ctx->own_lock);
		ret = -EBUSY;
		goto out;
	}
	spin_unlock(&ctx->own_lock);

	urp_cmd_pool_release(ctx);
	ret = 0;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

/*
 * Per-stream TX state for a fast SEND, created on first use (no pump kthread --
 * the app drives the QP). Reuses struct urp_stream so tx_seq/SYN, credits, and
 * the PR4 RX reorder come from the existing machinery. Two concurrent first
 * frames race on create; the loser gets -EEXIST and re-looks-up.
 */
static int urp_fast_stream_get(struct urp_endpoint *ep, u16 stream_id,
			       struct urp_stream **out)
{
	struct urp_stream *s;
	int ret;

	s = urp_stream_lookup(ep, stream_id);
	if (s) {
		*out = s;
		return 0;
	}
	ret = urp_stream_create(ep, stream_id, &s);
	if (ret == -EEXIST) {
		s = urp_stream_lookup(ep, stream_id);
		if (!s)
			return -EEXIST;
	} else if (ret) {
		return ret;
	}
	*out = s;
	return 0;
}

/*
 * Write the 20-byte in-place frame header (design 31 D3) into the app's pinned
 * pages at the start of buffer @buf_index. buf_size need not divide the page
 * size, so the header can straddle a page boundary -- split the copy in that
 * case. The payload was written by the app in userspace and is untouched.
 *
 * DMA coherence: header (kernel) and payload (userspace) reach the NIC through
 * the shared MR mapping. On the DMA-coherent targets (x86 ConnectX; the rxe/siw
 * software providers copy from the pages directly) no explicit sync is needed;
 * a non-coherent arch would additionally dma_sync_sg_for_device the frame range
 * before the post -- deferred with real non-x86 HW support.
 */
static void urp_fast_write_header(struct urp_cmd_ctx *ctx, u32 buf_index,
				  const u8 *hdr)
{
	u64 off = (u64)buf_index * ctx->geom.buf_size;
	unsigned long pg = off >> PAGE_SHIFT;
	unsigned int poff = offset_in_page(off);
	unsigned int n0 = min_t(unsigned int, URP_FRAME_HEADER_SIZE,
				PAGE_SIZE - poff);
	void *k;

	k = kmap_local_page(ctx->pages[pg]);
	memcpy(k + poff, hdr, n0);
	kunmap_local(k);
	if (n0 < URP_FRAME_HEADER_SIZE) {
		k = kmap_local_page(ctx->pages[pg + 1]);
		memcpy(k, hdr + n0, URP_FRAME_HEADER_SIZE - n0);
		kunmap_local(k);
	}
}

/*
 * Read the 20-byte in-place frame header the NIC DMA'd into the app's pinned
 * pages at the start of buffer @buf_index (the RECV mirror of
 * urp_fast_write_header). Copies the header bytes out to @hdr so the pure
 * classifier can decode/validate them; the payload stays untouched in the app
 * page (zero copy). Handles a header that straddles a page boundary.
 */
static void urp_fast_read_header(struct urp_cmd_ctx *ctx, u32 buf_index, u8 *hdr)
{
	u64 off = (u64)buf_index * ctx->geom.buf_size;
	unsigned long pg = off >> PAGE_SHIFT;
	unsigned int poff = offset_in_page(off);
	unsigned int n0 = min_t(unsigned int, URP_FRAME_HEADER_SIZE,
				PAGE_SIZE - poff);
	void *k;

	k = kmap_local_page(ctx->pages[pg]);
	memcpy(hdr, k + poff, n0);
	kunmap_local(k);
	if (n0 < URP_FRAME_HEADER_SIZE) {
		k = kmap_local_page(ctx->pages[pg + 1]);
		memcpy(hdr + n0, k, URP_FRAME_HEADER_SIZE - n0);
		kunmap_local(k);
	}
}

/*
 * Copy @len bytes at in-buffer offset @off out of the app's pinned pages for
 * buffer @buf_index -- the payload analogue of urp_fast_read_header, used to
 * lift a small control-frame payload (e.g. a PROBE PING) out for in-kernel
 * handling without disturbing the zero-copy data path. The buffer base is
 * page-aligned and control payloads are tiny, so this stays within the first
 * page in practice; the straddle path mirrors urp_fast_read_header for safety.
 */
static void urp_fast_read_at(struct urp_cmd_ctx *ctx, u32 buf_index,
			     unsigned int off, void *dst, unsigned int len)
{
	u64 boff = (u64)buf_index * ctx->geom.buf_size + off;
	unsigned long pg = boff >> PAGE_SHIFT;
	unsigned int poff = offset_in_page(boff);
	unsigned int n0 = min_t(unsigned int, len, PAGE_SIZE - poff);
	void *k;

	k = kmap_local_page(ctx->pages[pg]);
	memcpy(dst, k + poff, n0);
	kunmap_local(k);
	if (n0 < len) {
		k = kmap_local_page(ctx->pages[pg + 1]);
		memcpy((u8 *)dst + n0, k, len - n0);
		kunmap_local(k);
	}
}

/*
 * Send completion (send CQ, IB_POLL_WORKQUEUE). Flip the buffer back to
 * APP_OWNED -- always, including the IB_WC_WR_FLUSH_ERR drain path, so a QP
 * teardown never strands a buffer or an io_uring request (mirrors
 * urp_send_done). Record the result and bounce to the owning task to post the
 * CQE (io_uring_cmd_done must run in the submitter's task, not here).
 */
static void urp_fast_send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_fast_op *op = container_of(wc->wr_cqe, struct urp_fast_op, cqe);
	struct urp_cmd_ctx *ctx = op->ctx;

	if (wc->status != IB_WC_SUCCESS && wc->status != IB_WC_WR_FLUSH_ERR)
		pr_err_ratelimited("fast send completion error: %s (%d)\n",
				   ib_wc_status_msg(wc->status), wc->status);

	op->res = (wc->status == IB_WC_SUCCESS) ? (s32)op->len : -EIO;

	spin_lock(&ctx->own_lock);
	urp_own_release(ctx->own, ctx->geom.count, op->buf_index);
	spin_unlock(&ctx->own_lock);

	io_uring_cmd_complete_in_task(op->ioucmd, urp_fast_send_tw);
}

/*
 * URP_CMD_SEND: zero-copy transmit of buffer @buf_index's payload. Claims the
 * buffer (APP->KERNEL), encodes the in-place header, and posts a single-SGE
 * IB_WR_SEND addressing the pool MR by offset (any frame size, no payload copy).
 * Returns -EIOCBQUEUED: the CQE is posted from urp_fast_send_done via task-work.
 * A synchronous failure returns a negative errno (no deferred completion) after
 * releasing ownership. Runs in a blocking context (see the fop dispatch) since
 * first-use stream creation may allocate.
 */
static int urp_cmd_do_send(struct urp_cmd_ctx *ctx, struct io_uring_cmd *ioucmd,
			   const struct io_uring_sqe *sqe)
{
	const struct urp_cmd_data *in = urp_sqe_cmd(sqe, struct urp_cmd_data);
	struct urp_cmd_pool_geom geom;
	struct urp_cmd_req req;
	struct urp_endpoint *ep;
	struct urp_stream *stream;
	struct urp_fast_op *op;
	struct urp_qp *qp;
	u8 hdr[URP_FRAME_HEADER_SIZE];
	u8 flags = 0;
	u64 seq;
	int ret;

	mutex_lock(&ctx->lock);
	if (!ctx->registered) {
		mutex_unlock(&ctx->lock);
		return -ENXIO;
	}
	geom = ctx->geom;
	ep = ctx->ep;

	ret = urp_cmd_validate_data(URP_CMD_SEND, in, &geom, &req);
	if (ret)
		goto out_unlock;

	/*
	 * Fast streams are app-assigned and must be non-zero: urp_stream_create
	 * treats stream_id 0 as "pick the next id", which would desync the id the
	 * app stamps in the header from the stream object. Reject it up front.
	 */
	if (req.stream_id == 0) {
		ret = -EINVAL;
		goto out_unlock;
	}

	spin_lock(&ctx->own_lock);
	ret = urp_own_claim(ctx->own, geom.count, req.buf_index);
	spin_unlock(&ctx->own_lock);
	if (ret)
		goto out_unlock;			/* -EBUSY: double submit */

	ret = urp_fast_stream_get(ep, req.stream_id, &stream);
	if (ret)
		goto err_release;

	qp = urp_qp_select_round_robin(ep);
	if (!qp || !qp->qp) {
		ret = -ENOTCONN;
		goto err_release;
	}

	mutex_lock(&stream->lock);
	seq = stream->tx_seq++;
	mutex_unlock(&stream->lock);

	if (seq == 0)
		flags |= URP_DATA_FLAG_SYN;		/* first frame opens the stream */
	if (req.flags & URP_CMD_F_FIN)
		flags |= URP_DATA_FLAG_FIN;

	urp_frame_encode(hdr, req.stream_id, seq, URP_FRAME_TYPE_DATA, flags, 0,
			 req.len);
	urp_fast_write_header(ctx, req.buf_index, hdr);

	op = &ctx->ops[req.buf_index];
	op->cqe.done	= urp_fast_send_done;
	op->ioucmd	= ioucmd;
	op->ctx		= ctx;
	op->buf_index	= req.buf_index;
	op->len		= req.len;
	op->stream_id	= req.stream_id;
	op->res		= 0;
	*io_uring_cmd_to_pdu(ioucmd, struct urp_fast_op *) = op;

	ret = urp_post_frame_raw(qp->qp,
				 ctx->mr_iova + (u64)req.buf_index * geom.buf_size,
				 URP_FRAME_HEADER_SIZE + req.len, ctx->lkey,
				 &op->cqe);
	if (ret)
		goto err_release;

	mutex_unlock(&ctx->lock);
	return -EIOCBQUEUED;

err_release:
	spin_lock(&ctx->own_lock);
	urp_own_release(ctx->own, geom.count, req.buf_index);
	spin_unlock(&ctx->own_lock);
out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

/*
 * Recv completion (recv CQ, IB_POLL_WORKQUEUE). The NIC DMA'd an inbound frame
 * straight into the app's donated page; decode the in-place header to find the
 * payload length and stream, then hand the buffer back to the app by posting its
 * CQE. Flip the buffer back to APP_OWNED unconditionally -- including the
 * IB_WC_WR_FLUSH_ERR drain path -- so a QP teardown never strands a buffer. If
 * the RECV was cancelled on ring teardown the io_uring cmd was already completed
 * (op->ioucmd cleared under own_lock by the canceller); we then only release the
 * buffer and post no CQE, so there is exactly one completion per op.
 */
static void urp_fast_recv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct urp_fast_op *op = container_of(wc->wr_cqe, struct urp_fast_op, cqe);
	struct urp_cmd_ctx *ctx = op->ctx;
	struct io_uring_cmd *ioucmd;

	if (wc->status == IB_WC_SUCCESS) {
		u8 hdr[URP_FRAME_HEADER_SIZE];
		struct urp_rx_decoded dec;
		enum urp_rx_action action;

		/*
		 * Decode + validate the header the peer wrote into the app page.
		 * urp_classify_frame carries every length guard (design 27 27.8
		 * #1). Only DATA frames deliver a payload to the app.
		 */
		urp_fast_read_header(ctx, op->buf_index, hdr);
		action = urp_classify_frame(wc->byte_len, hdr, &dec);
		switch (urp_fast_rx_disposition(action)) {
		case URP_FAST_RX_DELIVER:
			op->res = (s32)dec.payload_len;
			op->stream_id = (u16)dec.stream_id;
			break;
		case URP_FAST_RX_PONG: {
			/*
			 * A fast endpoint has no pump, so the recv path itself
			 * must keep the peer's liveness protocol alive: answer
			 * the PROBE PING with a PONG. Design 33 requires an
			 * acceptor to "answer PINGs with PONGs in the recv path"
			 * regardless of kind -- without this a UDS-initiator peer
			 * never sees a PONG, trips the missed-probe silent-drop
			 * detector (URP_QP_MISS_THRESHOLD * URP_PROBE_INTERVAL_MS)
			 * and re-dials in a loop, so REGISTER/SEND/RECV against a
			 * fast acceptor never stabilizes. The kernel send pool is
			 * allocated for fast endpoints too (only the SRQ is
			 * gated), so urp_emit_pong_on works verbatim; lift the
			 * 32-byte ping payload out of the app page to echo it.
			 *
			 * Hand the buffer back as a benign zero-length completion
			 * (res = 0, below), NOT -EBADMSG: the app re-donates the
			 * buffer, and a hard error would (correctly) abort a
			 * well-behaved zero-copy consumer on a mere keepalive.
			 */
			u8 ping[URP_PING_PAYLOAD_SIZE];

			urp_fast_read_at(ctx, op->buf_index,
					 URP_FRAME_HEADER_SIZE, ping,
					 sizeof(ping));
			urp_emit_pong_on(ctx->ep, wc->qp, ping);
			op->res = 0;
			op->stream_id = 0;
			break;
		}
		case URP_FAST_RX_ABSORB:
			/*
			 * Peer liveness/flow-control the fast app does not
			 * consume (fast endpoints do not probe and use RC's own
			 * backpressure, not pump credits). Absorb silently and
			 * hand the buffer back empty so the app re-donates.
			 */
			op->res = 0;
			op->stream_id = 0;
			break;
		case URP_FAST_RX_REJECT:
		default:
			/*
			 * Genuinely malformed (DROP_SHORT / OVERSIZE /
			 * PAYLOAD_OVERRUN / SHORT_PROBE): surface the error.
			 */
			op->res = -EBADMSG;
			op->stream_id = 0;
			break;
		}
	} else {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			pr_err_ratelimited("fast recv completion error: %s (%d)\n",
					   ib_wc_status_msg(wc->status),
					   wc->status);
		op->res = -ECONNRESET;
		op->stream_id = 0;
	}

	spin_lock(&ctx->own_lock);
	ioucmd = op->ioucmd;		/* NULL once the canceller has claimed it */
	urp_own_release(ctx->own, ctx->geom.count, op->buf_index);
	spin_unlock(&ctx->own_lock);

	if (ioucmd)
		io_uring_cmd_complete_in_task(ioucmd, urp_fast_recv_tw);
}

/*
 * URP_CMD_RECV: donate buffer @buf_index as zero-copy RDMA landing space. Claims
 * the buffer (APP->KERNEL), marks the op cancelable so ring teardown can reclaim
 * a buffer that never received a frame, and arms it as a recv WR directly on the
 * endpoint QP (no SRQ -- see urp_qp_create_on_cm_id). Returns -EIOCBQUEUED: the
 * CQE32 (payload len in res, buf_index|stream_id in res2) is posted from
 * urp_fast_recv_done once the NIC fills the buffer. A synchronous failure returns
 * a negative errno after releasing ownership. The app-supplied stream_id is
 * ignored: the delivered stream is decoded from the received frame's header.
 */
static int urp_cmd_do_recv(struct urp_cmd_ctx *ctx, struct io_uring_cmd *ioucmd,
			   const struct io_uring_sqe *sqe, unsigned int issue_flags)
{
	const struct urp_cmd_data *in = urp_sqe_cmd(sqe, struct urp_cmd_data);
	struct urp_cmd_pool_geom geom;
	struct urp_cmd_req req;
	struct urp_endpoint *ep;
	struct urp_fast_op *op;
	struct urp_qp *qp;
	u32 sink_len;
	int ret;

	mutex_lock(&ctx->lock);
	if (!ctx->registered) {
		mutex_unlock(&ctx->lock);
		return -ENXIO;
	}
	geom = ctx->geom;
	ep = ctx->ep;

	ret = urp_cmd_validate_data(URP_CMD_RECV, in, &geom, &req);
	if (ret)
		goto out_unlock;

	spin_lock(&ctx->own_lock);
	ret = urp_own_claim(ctx->own, geom.count, req.buf_index);
	spin_unlock(&ctx->own_lock);
	if (ret)
		goto out_unlock;			/* -EBUSY: double donate */

	qp = urp_qp_select_round_robin(ep);
	if (!qp || !qp->qp) {
		ret = -ENOTCONN;
		goto err_release;
	}

	op = &ctx->ops[req.buf_index];
	op->cqe.done	= urp_fast_recv_done;
	op->ioucmd	= ioucmd;
	op->ctx		= ctx;
	op->buf_index	= req.buf_index;
	op->len		= req.len;
	op->stream_id	= 0;
	op->res		= 0;
	*io_uring_cmd_to_pdu(ioucmd, struct urp_fast_op *) = op;

	/*
	 * Landing space is the whole frame (header + payload). Post up to the
	 * app's requested payload cap plus the header, never past the buffer.
	 */
	sink_len = min_t(u32, geom.buf_size, URP_FRAME_HEADER_SIZE + req.len);

	/*
	 * Mark cancelable BEFORE posting so the op is reclaimable the instant it
	 * can complete: a fast peer could DMA a frame into this buffer immediately
	 * after the post, and io_uring must already know the op is cancelable.
	 */
	io_uring_cmd_mark_cancelable(ioucmd, issue_flags);

	ret = urp_post_recv_raw(qp->qp,
				ctx->mr_iova + (u64)req.buf_index * geom.buf_size,
				sink_len, ctx->lkey, &op->cqe);
	if (ret) {
		/*
		 * The WR never armed, so no async completion will ever come --
		 * we must finish the op here. It is already cancelable, so a
		 * concurrent ring-exit cancel may be completing it in parallel;
		 * coordinate through the ioucmd sentinel under own_lock so the
		 * cmd is completed exactly once (mirrors urp_fast_recv_done).
		 * Complete via io_uring_cmd_done (not the return value) to
		 * un-mark the cancelable state, then return -EIOCBQUEUED.
		 */
		struct io_uring_cmd *ic;

		spin_lock(&ctx->own_lock);
		ic = op->ioucmd;
		op->ioucmd = NULL;
		urp_own_release(ctx->own, geom.count, req.buf_index);
		spin_unlock(&ctx->own_lock);
		mutex_unlock(&ctx->lock);
		if (ic)
			urp_cmd_complete(ic, ret, 0, true, issue_flags);
		return -EIOCBQUEUED;
	}

	mutex_unlock(&ctx->lock);
	return -EIOCBQUEUED;

err_release:
	/* Reached only before the op is marked cancelable: a plain synchronous
	 * errno, with ownership handed back to the app.
	 */
	spin_lock(&ctx->own_lock);
	urp_own_release(ctx->own, geom.count, req.buf_index);
	spin_unlock(&ctx->own_lock);
out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

/*
 * io_uring is tearing down a cancelable in-flight RECV (ring exit). The recv WR
 * is still armed on the QP; we cannot un-post a single WR, so complete the
 * io_uring cmd here and let urp_cmd_pool_release drain the RQ (which flushes the
 * WR and releases the buffer). Under own_lock, claim the completion only if the
 * buffer is still KERNEL_OWNED: if urp_fast_recv_done already ran it owns the
 * completion, so we must not double-complete. Clearing op->ioucmd tells a later
 * flush completion to skip the (now freed) cmd.
 */
static int urp_cmd_recv_cancel(struct urp_cmd_ctx *ctx,
			       struct io_uring_cmd *ioucmd,
			       unsigned int issue_flags)
{
	struct urp_fast_op *op = *io_uring_cmd_to_pdu(ioucmd, struct urp_fast_op *);
	bool complete_now;

	spin_lock(&ctx->own_lock);
	complete_now = urp_own_is_kernel(ctx->own, ctx->geom.count, op->buf_index);
	if (complete_now)
		op->ioucmd = NULL;
	spin_unlock(&ctx->own_lock);

	if (complete_now)
		urp_cmd_complete(ioucmd, -ECANCELED, 0, true, issue_flags);
	return 0;
}

static int urp_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct urp_cmd_ctx *ctx = ioucmd->file->private_data;
	const struct io_uring_sqe *sqe = ioucmd->sqe;

	/*
	 * io_uring is cancelling a cancelable in-flight op (only RECV marks
	 * itself so). Reclaim it regardless of opcode; the RQ drain on pool
	 * release finishes freeing the buffer.
	 */
	if (issue_flags & IO_URING_F_CANCEL)
		return urp_cmd_recv_cancel(ctx, ioucmd, issue_flags);

	switch (ioucmd->cmd_op) {
	case URP_CMD_REGISTER:
	case URP_CMD_UNREGISTER:
		/*
		 * These sleep (pin_user_pages / unpin). If io_uring issued us
		 * on the inline non-blocking path, punt to io-wq where the
		 * submitting task's mm is installed and blocking is allowed.
		 */
		if (issue_flags & IO_URING_F_NONBLOCK)
			return -EAGAIN;
		if (ioucmd->cmd_op == URP_CMD_REGISTER)
			return urp_cmd_do_register(ctx, sqe);
		return urp_cmd_do_unregister(ctx);
	case URP_CMD_SEND:
		/*
		 * SEND may sleep on first-use stream creation (GFP_KERNEL), so
		 * take the same io-wq punt as REGISTER. A future optimisation can
		 * attempt an inline non-sleeping fast path and punt only on the
		 * cold first frame. Completion is asynchronous (-EIOCBQUEUED).
		 */
		if (issue_flags & IO_URING_F_NONBLOCK)
			return -EAGAIN;
		return urp_cmd_do_send(ctx, ioucmd, sqe);
	case URP_CMD_RECV:
		/*
		 * RECV takes ctx->lock (may sleep) and marks the op cancelable,
		 * so it needs the io-wq context like SEND. Completion is
		 * asynchronous (-EIOCBQUEUED) once the NIC fills the buffer.
		 */
		if (issue_flags & IO_URING_F_NONBLOCK)
			return -EAGAIN;
		return urp_cmd_do_recv(ctx, ioucmd, sqe, issue_flags);
	default:
		return -EOPNOTSUPP;
	}
}

/* ----------------------------------------------------------------------- */
/* Char-device plumbing.                                                    */
/* ----------------------------------------------------------------------- */

static int urp_cmd_open(struct inode *inode, struct file *file)
{
	struct urp_cmd_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	spin_lock_init(&ctx->own_lock);
	file->private_data = ctx;
	return 0;
}

static int urp_cmd_release(struct inode *inode, struct file *file)
{
	struct urp_cmd_ctx *ctx = file->private_data;

	mutex_lock(&ctx->lock);
	urp_cmd_pool_release(ctx);
	mutex_unlock(&ctx->lock);

	mutex_destroy(&ctx->lock);
	kfree(ctx);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations urp_cmd_fops = {
	.owner		= THIS_MODULE,
	.open		= urp_cmd_open,
	.release	= urp_cmd_release,
	.uring_cmd	= urp_uring_cmd,
};

static struct miscdevice urp_cmd_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= URP_CMD_DEVICE_NAME,
	.fops	= &urp_cmd_fops,
	.mode	= 0600,
};

int urp_cmd_dev_register(void)
{
	int ret = misc_register(&urp_cmd_misc);

	if (ret)
		pr_err("misc_register(/dev/%s) failed: %d\n",
		       URP_CMD_DEVICE_NAME, ret);
	else
		pr_info("fast-path char device /dev/%s ready\n",
			URP_CMD_DEVICE_NAME);
	return ret;
}

void urp_cmd_dev_unregister(void)
{
	misc_deregister(&urp_cmd_misc);
}

#else /* !URP_FAST_ENABLED */

/*
 * The fast path is absent: either the kernel predates the modern uring_cmd /
 * pin_user_pages plumbing (< 6.8) or the module was built CONFIG_URP_FAST=n to
 * exclude the zero-copy path. Either way /dev/urp is not registered and the rest
 * of the module is unaffected; a `fast` endpoint is refused at the control plane.
 */
int urp_cmd_dev_register(void)
{
	pr_info("fast-path char device disabled (needs CONFIG_URP_FAST and kernel >= 6.8)\n");
	return 0;
}

void urp_cmd_dev_unregister(void)
{
}

#endif /* URP_FAST_ENABLED */
