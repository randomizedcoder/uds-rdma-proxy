// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- reorder buffer (C rbtree backend)
 *
 * Phase 3a Step 5. Default backend for the per-stream multi-QP reorder
 * buffer. Step 5b will add an opt-in Rust backend behind
 * CONFIG_URP_REORDER_RUST that shims to liburp_protocol_ffi.a; this
 * .c file is the *default* compile target and the wire-compatible
 * partner of the Rust crate's `reorder` module.
 *
 * Data structure: linux/rbtree.h indexed by sequence number, with a
 * separate `drained` list of frames that have already become deliverable
 * but haven't been popped via urp_reorder_drain_next yet. Insert moves
 * any newly drainable frames from the rbtree into the drained list.
 *
 * Concurrency: caller holds the per-stream mutex (or similar) -- this
 * file is not internally synchronized. The Rust reference impl also
 * requires external serialization (&mut self everywhere), so this
 * matches semantics 1:1.
 */

#include <linux/errno.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "urp_reorder.h"

struct urp_reorder_node {
	struct rb_node	rb;
	u64		seq;
	size_t		len;
	/* payload appended after the struct: kmalloc(sizeof(*n) + len) */
};

struct urp_drained_node {
	struct list_head	list;
	u64			seq;
	size_t			len;
	/* payload appended after the struct */
};

struct urp_reorder {
	struct rb_root		pending;
	struct list_head	drained;
	u64			next_expected;
	u32			pending_count;
	u32			drain_count;
	u32			max_buffered;
};

static inline u8 *node_payload(struct urp_reorder_node *n)
{
	return (u8 *)(n + 1);
}

static inline u8 *drained_payload(struct urp_drained_node *d)
{
	return (u8 *)(d + 1);
}

struct urp_reorder *urp_reorder_alloc(u64 initial_expected, u32 max_buffered)
{
	struct urp_reorder *rb;

	if (max_buffered == 0)
		return NULL;

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb)
		return NULL;

	rb->pending = RB_ROOT;
	INIT_LIST_HEAD(&rb->drained);
	rb->next_expected = initial_expected;
	rb->max_buffered = max_buffered;
	return rb;
}

static void urp_reorder_clear(struct urp_reorder *rb)
{
	struct urp_reorder_node *n, *next;
	struct urp_drained_node *d, *dn;

	rbtree_postorder_for_each_entry_safe(n, next, &rb->pending, rb)
		kfree(n);
	rb->pending = RB_ROOT;
	rb->pending_count = 0;

	list_for_each_entry_safe(d, dn, &rb->drained, list) {
		list_del(&d->list);
		kfree(d);
	}
	rb->drain_count = 0;
}

void urp_reorder_free(struct urp_reorder *rb)
{
	if (!rb)
		return;
	urp_reorder_clear(rb);
	kfree(rb);
}

/*
 * Look up @seq in the rbtree. Returns the node if present, else NULL.
 * If @parent_out is non-NULL, also returns the would-be-parent + link
 * slot suitable for rb_link_node when inserting.
 */
static struct urp_reorder_node *
urp_reorder_lookup(struct urp_reorder *rb, u64 seq,
		   struct rb_node **parent_out, struct rb_node ***link_out)
{
	struct rb_node **link = &rb->pending.rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		struct urp_reorder_node *n =
			rb_entry(*link, struct urp_reorder_node, rb);

		parent = *link;
		if (seq < n->seq)
			link = &(*link)->rb_left;
		else if (seq > n->seq)
			link = &(*link)->rb_right;
		else
			return n;
	}

	if (parent_out)
		*parent_out = parent;
	if (link_out)
		*link_out = link;
	return NULL;
}

/*
 * Drain any prefix of the pending tree where keys are consecutive
 * starting at next_expected -- move them to the drained list and bump
 * next_expected.
 */
static void urp_reorder_drain_prefix(struct urp_reorder *rb)
{
	struct rb_node *first;

	while ((first = rb_first(&rb->pending))) {
		struct urp_reorder_node *n =
			rb_entry(first, struct urp_reorder_node, rb);
		struct urp_drained_node *d;

		if (n->seq != rb->next_expected)
			break;

		d = kmalloc(sizeof(*d) + n->len, GFP_KERNEL);
		if (!d)
			break;	/* leave it in pending; caller can retry */

		d->seq = n->seq;
		d->len = n->len;
		memcpy(drained_payload(d), node_payload(n), n->len);
		list_add_tail(&d->list, &rb->drained);
		rb->drain_count++;

		rb_erase(first, &rb->pending);
		rb->pending_count--;
		/* Saturate at the top of the sequence space to match the Rust
		 * twin (reorder.rs), where an unchecked increment overflow-
		 * panics -> BUG(). Unreachable in practice (needs 2^64 in-order
		 * deliveries); bounded for defense in depth.
		 */
		if (rb->next_expected != U64_MAX)
			rb->next_expected++;
		kfree(n);
	}
}

int urp_reorder_insert(struct urp_reorder *rb, u64 seq,
		       const u8 *data, size_t data_len)
{
	struct urp_reorder_node *node;
	struct rb_node *parent = NULL;
	struct rb_node **link = NULL;

	if (!rb)
		return -EINVAL;
	if (data_len && !data)
		return -EINVAL;

	if (seq < rb->next_expected)
		return -EEXIST;

	if (urp_reorder_lookup(rb, seq, &parent, &link))
		return -EEXIST;

	if (rb->pending_count >= rb->max_buffered && seq != rb->next_expected)
		return -ENOBUFS;

	node = kmalloc(sizeof(*node) + data_len, GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->seq = seq;
	node->len = data_len;
	if (data_len)
		memcpy(node_payload(node), data, data_len);

	rb_link_node(&node->rb, parent, link);
	rb_insert_color(&node->rb, &rb->pending);
	rb->pending_count++;

	urp_reorder_drain_prefix(rb);
	return 0;
}

int urp_reorder_drain_next(struct urp_reorder *rb, u64 *out_seq,
			   u8 *out_data, size_t *inout_len)
{
	struct urp_drained_node *d;

	if (!rb || !out_seq || !inout_len)
		return -EINVAL;
	if (*inout_len && !out_data)
		return -EINVAL;

	if (list_empty(&rb->drained))
		return -ENOENT;

	d = list_first_entry(&rb->drained, struct urp_drained_node, list);

	if (*inout_len < d->len) {
		*inout_len = d->len;
		return -ENOBUFS;	/* leave frame queued */
	}

	*out_seq = d->seq;
	if (d->len)
		memcpy(out_data, drained_payload(d), d->len);
	*inout_len = d->len;

	list_del(&d->list);
	rb->drain_count--;
	kfree(d);
	return 0;
}

u64 urp_reorder_next_expected(const struct urp_reorder *rb)
{
	return rb ? rb->next_expected : 0;
}

size_t urp_reorder_gap_count(const struct urp_reorder *rb)
{
	return rb ? rb->pending_count : 0;
}

size_t urp_reorder_drain_pending(const struct urp_reorder *rb)
{
	return rb ? rb->drain_count : 0;
}
