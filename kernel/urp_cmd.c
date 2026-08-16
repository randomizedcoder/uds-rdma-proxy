// SPDX-License-Identifier: GPL-2.0
/*
 * urp-fast: io_uring uring_cmd char device (design 31, PR1).
 *
 * Opens a /dev/urp misc device whose only interesting file op is
 * ->uring_cmd. An aware application drives the zero-copy fast path over
 * IORING_OP_URING_CMD submissions on this fd:
 *
 *   URP_CMD_REGISTER    pin an app buffer pool once (pin_user_pages,
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
 * <linux/io_uring/cmd.h> header (6.7) and the 4-argument pin_user_pages (the
 * vmas parameter was dropped in 6.5). Gate the whole device on >= 6.8 -- the
 * same boundary urp.h already uses -- and stub the register/unregister hooks
 * on older LTS kernels so those compile-only CI gates stay green. The pure
 * validators in urp_cmd_validate.c compile on every supported kernel, so
 * KUnit and the userspace check exercise the trust boundary regardless.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/io_uring/cmd.h>

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

struct urp_cmd_ctx {
	struct mutex		lock;		/* serialises REGISTER/UNREGISTER */
	bool			registered;
	u64			base;		/* app pool base address        */
	u64			pool_len;	/* total bytes                  */
	unsigned long		nr_pages;	/* pinned pages                 */
	struct page		**pages;	/* FOLL_LONGTERM pin            */
	dma_addr_t		*dma;		/* per-page DMA address (nr_pages) */
	struct urp_cmd_pool_geom geom;		/* count + buf_size             */

	/*
	 * The endpoint the pool is registered against. We hold a kref for the
	 * registration's lifetime; @ib_dev is cached from ep->ib_dev at REGISTER
	 * so UNREGISTER can unmap even if the endpoint has since cleared it, and
	 * @lkey is ep->pd->local_dma_lkey -- what the data path (PR3) posts with.
	 */
	struct urp_endpoint	*ep;
	struct ib_device	*ib_dev;
	u32			lkey;
};

static void urp_cmd_pool_release(struct urp_cmd_ctx *ctx)
{
	unsigned long i;

	lockdep_assert_held(&ctx->lock);

	if (!ctx->registered)
		return;

	if (ctx->dma) {
		for (i = 0; i < ctx->nr_pages; i++)
			ib_dma_unmap_page(ctx->ib_dev, ctx->dma[i], PAGE_SIZE,
					  DMA_BIDIRECTIONAL);
		kvfree(ctx->dma);
		ctx->dma = NULL;
	}
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
 * Bind the pinned pool to the named endpoint and DMA-map every page against
 * that endpoint's RDMA device, so the pool shares the endpoint's protection
 * domain (the data path in a later PR posts on its QP with @lkey). On success
 * fills ctx->{ep,ib_dev,dma,lkey} and holds an endpoint kref; on failure
 * returns a negative errno having undone any partial mapping and dropped the
 * ref. The caller still owns the pin on failure.
 *
 * NOTE (hardened in the PR that adds the data path): ep->pd / ep->ib_dev are
 * read without ep->lock. The kref keeps the endpoint struct alive, but a
 * concurrent endpoint teardown could still race the mapping. Safe for the
 * current milestone (the pool is registered against a stable, established
 * endpoint); the ownership/quiesce protocol closes the race.
 */
static int urp_cmd_map_pool(struct urp_cmd_ctx *ctx, struct page **pages,
			    unsigned long nr_pages, const char *ep_name)
{
	struct urp_endpoint *ep;
	dma_addr_t *dma;
	unsigned long i;
	int ret;

	ep = urp_endpoint_get(ep_name);
	if (!ep)
		return -ENODEV;

	/* Need an established RDMA back-end (PD + device) to map against. */
	if (!ep->pd || !ep->ib_dev) {
		ret = -ENOTCONN;
		goto err_put;
	}

	dma = kvmalloc_array(nr_pages, sizeof(*dma), GFP_KERNEL);
	if (!dma) {
		ret = -ENOMEM;
		goto err_put;
	}

	for (i = 0; i < nr_pages; i++) {
		dma[i] = ib_dma_map_page(ep->ib_dev, pages[i], 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
		if (ib_dma_mapping_error(ep->ib_dev, dma[i])) {
			ret = -ENOMEM;
			goto err_unmap;
		}
	}

	ctx->ep		= ep;
	ctx->ib_dev	= ep->ib_dev;
	ctx->dma	= dma;
	ctx->lkey	= ep->pd->local_dma_lkey;
	return 0;

err_unmap:
	while (i-- > 0)
		ib_dma_unmap_page(ep->ib_dev, dma[i], PAGE_SIZE,
				  DMA_BIDIRECTIONAL);
	kvfree(dma);
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
	 */
	pinned = pin_user_pages(reg.base, nr_pages,
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
	} else {
		urp_cmd_pool_release(ctx);
		ret = 0;
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

static int urp_cmd_do_data(struct urp_cmd_ctx *ctx, u32 cmd_op,
			   const struct io_uring_sqe *sqe)
{
	const struct urp_cmd_data *in =
		urp_sqe_cmd(sqe, struct urp_cmd_data);
	struct urp_cmd_pool_geom geom;
	struct urp_cmd_req op;
	int ret;

	mutex_lock(&ctx->lock);
	geom = ctx->geom;
	mutex_unlock(&ctx->lock);

	ret = urp_cmd_validate_data(cmd_op, in, &geom, &op);
	if (ret)
		return ret;

	/* PR1 validates the op; the RDMA data path arrives in a later PR. */
	return -ENOSYS;
}

static int urp_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct urp_cmd_ctx *ctx = ioucmd->file->private_data;
	const struct io_uring_sqe *sqe = ioucmd->sqe;

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
	case URP_CMD_RECV:
		return urp_cmd_do_data(ctx, ioucmd->cmd_op, sqe);
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

#else /* LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0) */

/*
 * Older LTS kernels lack the modern uring_cmd / pin_user_pages plumbing. The
 * fast path is simply absent there; the rest of the module is unaffected.
 */
int urp_cmd_dev_register(void)
{
	pr_info("fast-path char device needs kernel >= 6.8; disabled\n");
	return 0;
}

void urp_cmd_dev_unregister(void)
{
}

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0) */
