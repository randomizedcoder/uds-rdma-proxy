// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- stream multiplexing core
 *
 * Phase 3a Step 6. Introduces struct urp_stream and the per-endpoint
 * rhashtable that holds them. Each accepted UDS connection (Phase 3a
 * Step 7) becomes a stream with its own monotonic tx/rx sequence
 * counters, per-stream credit state, and per-stream reorder buffer.
 *
 * This commit lands the data structure + lifecycle helpers only. The
 * data path still uses ep->conn (the k0-style single connection) so
 * existing tests are unaffected. Step 7 introduces SYN/FIN/RST flag
 * handling on the wire and starts allocating streams per UDS accept().
 *
 * Stream-id allocation (design 09 §9.5):
 *   - Initiator: odd IDs (1, 3, 5, ...)
 *   - Acceptor:  even IDs (2, 4, 6, ...)
 *   - stream_id = 0 reserved for the future control channel.
 *
 * Lifecycle:
 *   - urp_stream_create: insert into rhashtable; RCU-safe.
 *   - urp_stream_destroy: rhashtable_remove + kfree_rcu, so concurrent
 *     RCU lookups complete safely.
 *   - urp_streams_destroy_all: iterates the table and tears down each
 *     entry; called from urp_endpoint_drain.
 */

#include "urp.h"
#include <linux/slab.h>

/*
 * Fixed-key rhashtable on u32 stream_id. Same default-hashing trick as
 * the endpoint rhashtable (urp_endpoint.c) to avoid the BUG() on
 * obj_hashfn in rhashtable_lookup_insert_fast.
 */
static const struct rhashtable_params urp_stream_params = {
	.head_offset	= offsetof(struct urp_stream, ht_node),
	.key_offset	= offsetof(struct urp_stream, id),
	.key_len	= sizeof(u32),
	.automatic_shrinking = true,
};

int urp_streams_init(struct urp_endpoint *ep)
{
	int ret;

	ret = rhashtable_init(&ep->streams, &urp_stream_params);
	if (ret)
		return ret;

	ep->streams_inited = true;
	/* Initiator gets odd, acceptor gets even. Start past 0 (control). */
	atomic_set(&ep->next_stream_id, ep->is_initiator ? 1 : 2);
	return 0;
}

/*
 * Allocate the next stream_id following the initiator-odd / acceptor-even
 * convention. With atomic_add_return(2, ...) we step by 2 per allocation
 * and never collide with the peer's IDs.
 */
u32 urp_stream_next_id(struct urp_endpoint *ep)
{
	int next = atomic_fetch_add(2, &ep->next_stream_id);

	return (u32)next;
}

static void urp_stream_rcu_free(struct rcu_head *rcu)
{
	struct urp_stream *s = container_of(rcu, struct urp_stream, rcu);

	if (s->reorder)
		urp_reorder_free(s->reorder);
	kfree(s);
}

/*
 * Allocate and insert a new stream entry. The caller may already have
 * a stream_id (e.g. for the receiver side of a SYN), or pass 0 to ask
 * for the next allocated id (sender side). On success the new stream
 * is in URP_STREAM_STATE_SYN_SENT; Step 7 transitions through the rest
 * of the state machine on actual SYN/FIN/RST wire events.
 *
 * Returns 0 + *out_stream on success. -EEXIST if the requested id is
 * already in the table, -ENOMEM on allocation failure.
 */
int urp_stream_create(struct urp_endpoint *ep, u32 stream_id,
		      struct urp_stream **out_stream)
{
	struct urp_stream *s;
	int ret;

	if (!ep->streams_inited)
		return -EINVAL;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->ep = ep;
	s->id = stream_id ? stream_id : urp_stream_next_id(ep);
	s->state = URP_STREAM_STATE_SYN_SENT;
	s->tx_seq = 0;
	s->rx_next = 0;
	mutex_init(&s->lock);
	urp_credit_init(&s->credit, URP_NUM_BUFS / 2);

	/* Reorder buffer is per-stream because each stream has its own
	 * sequence space (design 09 §9.6). Capped at 256 -- well above
	 * any reasonable ECMP path-skew burst. */
	s->reorder = urp_reorder_alloc(0, 256);
	if (!s->reorder) {
		mutex_destroy(&s->lock);
		kfree(s);
		return -ENOMEM;
	}

	ret = rhashtable_lookup_insert_fast(&ep->streams, &s->ht_node,
					    urp_stream_params);
	if (ret) {
		urp_reorder_free(s->reorder);
		mutex_destroy(&s->lock);
		kfree(s);
		return ret;
	}

	*out_stream = s;
	return 0;
}

/* RCU-safe lookup by stream_id. Returned pointer is only valid inside
 * the current rcu_read_lock() critical section unless the caller
 * arranges other lifetime management. */
struct urp_stream *urp_stream_lookup(struct urp_endpoint *ep, u32 stream_id)
{
	if (!ep->streams_inited)
		return NULL;

	return rhashtable_lookup_fast(&ep->streams, &stream_id,
				      urp_stream_params);
}

/* Remove from table and schedule deferred free via RCU. The stream's
 * uds_sock + reorder buffer are freed in the RCU callback. */
void urp_stream_destroy(struct urp_endpoint *ep, struct urp_stream *s)
{
	if (!s)
		return;

	rhashtable_remove_fast(&ep->streams, &s->ht_node, urp_stream_params);

	if (s->uds_sock) {
		sock_release(s->uds_sock);
		s->uds_sock = NULL;
	}

	mutex_destroy(&s->lock);
	call_rcu(&s->rcu, urp_stream_rcu_free);
}

/* Walk the stream rhashtable and tear down every entry. Used at
 * endpoint drain time, after the data path has stopped. */
void urp_streams_destroy_all(struct urp_endpoint *ep)
{
	struct rhashtable_iter iter;
	struct urp_stream *s;

	if (!ep->streams_inited)
		return;

	for (;;) {
		rhashtable_walk_enter(&ep->streams, &iter);
		rhashtable_walk_start(&iter);

		s = NULL;
		while ((s = rhashtable_walk_next(&iter))) {
			if (IS_ERR(s))
				continue;
			break;
		}

		rhashtable_walk_stop(&iter);
		rhashtable_walk_exit(&iter);

		if (!s || IS_ERR(s))
			break;

		urp_stream_destroy(ep, s);
	}

	rcu_barrier();
	rhashtable_destroy(&ep->streams);
	ep->streams_inited = false;
}
