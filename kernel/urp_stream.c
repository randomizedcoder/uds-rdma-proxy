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
#include <linux/net.h>

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

	/* Stop the per-stream TX kthread before we release its uds_sock,
	 * so the kthread never reads from a half-freed socket. (Step 7c) */
	urp_stream_pump_stop(s);

	if (s->uds_sock) {
		kernel_sock_shutdown(s->uds_sock, SHUT_RDWR);
		sock_release(s->uds_sock);
		s->uds_sock = NULL;
	}

	mutex_destroy(&s->lock);
	call_rcu(&s->rcu, urp_stream_rcu_free);
}

/*
 * Handle a SYN-flagged frame arriving from the peer. Creates a stream
 * for the given peer-assigned id if absent, transitions to
 * SYN_RECEIVED, and returns the stream (locked-free; caller doesn't
 * need to hold any lock for read of the returned pointer because RCU
 * + table lookup keep it alive across the call).
 *
 * Idempotent: a duplicate SYN on an already-known stream is a no-op
 * (returns the existing stream).
 *
 * Returns 0 on success and writes *out_stream. -ENOMEM on alloc
 * failure. -EEXIST if the stream id was already in the table with a
 * non-handshake state (sign of a peer protocol bug).
 */
int urp_stream_rx_syn(struct urp_endpoint *ep, u32 stream_id,
		      struct urp_stream **out_stream)
{
	struct urp_stream *s;
	int ret;

	s = urp_stream_lookup(ep, stream_id);
	if (s) {
		mutex_lock(&s->lock);
		if (s->state == URP_STREAM_STATE_SYN_SENT ||
		    s->state == URP_STREAM_STATE_SYN_RECEIVED ||
		    s->state == URP_STREAM_STATE_ESTABLISHED) {
			s->state = URP_STREAM_STATE_ESTABLISHED;
			mutex_unlock(&s->lock);
			*out_stream = s;
			return 0;
		}
		mutex_unlock(&s->lock);
		return -EEXIST;
	}

	ret = urp_stream_create(ep, stream_id, &s);
	if (ret)
		return ret;

	mutex_lock(&s->lock);
	s->state = URP_STREAM_STATE_SYN_RECEIVED;
	mutex_unlock(&s->lock);

	/*
	 * Acceptor-side multi-stream wiring (Phase 3a Step 7c). When the
	 * endpoint has a connect_path (we're the URP acceptor of an external
	 * peer's RDMA connection), open a fresh UDS to the backend for this
	 * stream and spin up its TX pump. The initiator side instead
	 * populates uds_sock from its UDS accept loop and starts the pump
	 * there.
	 *
	 * Failure here doesn't tear the stream down -- we leave it in
	 * SYN_RECEIVED with no UDS, RX dispatch will drop subsequent frames
	 * (counted in buffer_alloc_fails), and the drain path cleans up.
	 */
	if (!ep->is_initiator && ep->connect_path[0]) {
		if (urp_stream_connect_uds(s, ep->connect_path) == 0)
			urp_stream_pump_start(s);
	}

	*out_stream = s;
	return 0;
}

/*
 * Handle a FIN-flagged frame arriving from the peer. Implements the
 * half-close semantics in design 09 §9.4: peer is done sending; we
 * shutdown(SHUT_WR) the UDS so the local app sees EOF on read, but
 * we keep reading from the UDS for any pending TX in this direction.
 *
 * When the local app eventually closes its side, the TX pump will
 * emit a FIN of our own, fully closing the stream.
 */
int urp_stream_rx_fin(struct urp_stream *s)
{
	if (!s)
		return -EINVAL;

	mutex_lock(&s->lock);

	if (s->uds_sock)
		kernel_sock_shutdown(s->uds_sock, SHUT_WR);

	if (s->state == URP_STREAM_STATE_ESTABLISHED)
		s->state = URP_STREAM_STATE_CLOSE_WAIT;
	else if (s->state == URP_STREAM_STATE_FIN_WAIT)
		s->state = URP_STREAM_STATE_CLOSED;

	mutex_unlock(&s->lock);
	return 0;
}

/*
 * Handle a RST-flagged frame arriving from the peer. Abrupt close --
 * drop in-flight data and tear down the stream. The destroy itself is
 * deferred via call_rcu so concurrent rhashtable walks still see the
 * entry until their critical section ends.
 */
int urp_stream_rx_rst(struct urp_stream *s)
{
	struct urp_endpoint *ep;

	if (!s)
		return -EINVAL;

	mutex_lock(&s->lock);
	s->state = URP_STREAM_STATE_CLOSED;
	if (s->uds_sock) {
		kernel_sock_shutdown(s->uds_sock, SHUT_RDWR);
	}
	ep = s->ep;
	mutex_unlock(&s->lock);

	urp_stream_destroy(ep, s);
	return 0;
}

/*
 * Local-side send of FIN -- graceful close from our direction. The
 * caller will emit a DATA frame with URP_DATA_FLAG_FIN; this helper
 * just updates the stream-state machine. ESTABLISHED -> FIN_WAIT (we
 * stop reading from UDS in this direction); CLOSE_WAIT -> CLOSED
 * (peer already FIN'd, this is the second half).
 */
void urp_stream_tx_fin(struct urp_stream *s)
{
	if (!s)
		return;

	mutex_lock(&s->lock);
	if (s->state == URP_STREAM_STATE_ESTABLISHED)
		s->state = URP_STREAM_STATE_FIN_WAIT;
	else if (s->state == URP_STREAM_STATE_CLOSE_WAIT)
		s->state = URP_STREAM_STATE_CLOSED;
	mutex_unlock(&s->lock);
}

/*
 * Local-side send of RST -- abrupt close from our direction. The
 * caller will emit a DATA frame with URP_DATA_FLAG_RST; this helper
 * updates state. Subsequent calls to urp_stream_destroy reap the
 * entry from the rhashtable.
 */
void urp_stream_tx_rst(struct urp_stream *s)
{
	if (!s)
		return;

	mutex_lock(&s->lock);
	s->state = URP_STREAM_STATE_CLOSED;
	if (s->uds_sock)
		kernel_sock_shutdown(s->uds_sock, SHUT_RDWR);
	mutex_unlock(&s->lock);
}

/*
 * Frame-flag dispatcher used by the RX path (Step 7b will plumb the
 * call from urp_recv_done). Examines the DATA frame's flags byte and
 * fans out to syn/fin/rst handlers; pure-DATA frames (no flags)
 * fall through to ordinary in-order delivery via the reorder buffer.
 *
 * For Step 7 this is exercised by KUnit only (Step 9). The wire-path
 * call site lands in a follow-up commit alongside the multi-stream
 * test in test-kmod-k0.
 */
int urp_stream_rx_dispatch(struct urp_endpoint *ep, u32 stream_id, u8 flags,
			   struct urp_stream **out_stream)
{
	struct urp_stream *s = NULL;
	int ret = 0;

	if (flags & URP_DATA_FLAG_SYN) {
		ret = urp_stream_rx_syn(ep, stream_id, &s);
		if (ret)
			return ret;
	} else {
		s = urp_stream_lookup(ep, stream_id);
		if (!s)
			return -ENOENT;
	}

	if (flags & URP_DATA_FLAG_RST) {
		urp_stream_rx_rst(s);
		*out_stream = NULL;
		return 0;
	}

	if (flags & URP_DATA_FLAG_FIN)
		urp_stream_rx_fin(s);

	*out_stream = s;
	return 0;
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
