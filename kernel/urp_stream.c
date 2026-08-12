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
 * Stream-id allocation (design 09 section 9.5):
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
	atomic_set(&ep->pending_reap, 0);
	/* Initiator gets odd, acceptor gets even. Start past 0 (control). */
	atomic_set(&ep->next_stream_id, ep->is_initiator ? 1 : 2);
	return 0;
}

/*
 * Reap streams whose TX pump has exited (client closed its UDS).
 *
 * CURRENTLY UNCALLED -- deliberately. Eager reap-on-close (calling this
 * from urp_recv_done on tx_done) races an in-flight response:
 * `echo X | socat - UNIX-CONNECT` half-closes after sending, so the
 * stream would be destroyed before the reply is delivered back, losing
 * it. Correct reap-on-close needs the FIN handshake (reap when the PEER
 * closes), which is a follow-up; until then streams are reaped at
 * endpoint drain (urp_streams_destroy_all), so nothing leaks per
 * endpoint lifetime.
 *
 * The intended call site is urp_recv_done, whose recv-CQ completions are
 * serialized, so this never runs concurrently with the lock-free stream
 * lookups / post-rcu backend-connect there. That serialization is what
 * lets us destroy streams here without per-stream refcounting.
 *
 * tx_done is set at the end of urp_stream_tx_fn, which runs only after the
 * pump kthread has returned -- so urp_stream_destroy's kthread_stop is a
 * no-op join rather than a cancel.
 */
void urp_streams_reap(struct urp_endpoint *ep)
{
	struct rhashtable_iter iter;
	struct urp_stream *s;

	if (!ep->streams_inited)
		return;
	/* Fast path: nothing marked. Claim the pending count so we only
	 * walk the table when a pump has actually signalled a close.
	 */
	if (atomic_xchg(&ep->pending_reap, 0) == 0)
		return;

	rhashtable_walk_enter(&ep->streams, &iter);
	rhashtable_walk_start(&iter);
	while ((s = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(s))
			continue;
		if (!READ_ONCE(s->tx_done))
			continue;
		/* Drop the walk lock across the (sleeping) destroy, then
		 * restart the walk -- rhashtable_walk_next tolerates this.
		 */
		rhashtable_walk_stop(&iter);
		urp_stream_destroy(ep, s);
		rhashtable_walk_start(&iter);
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
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
	atomic64_set(&s->tx_bytes, 0);
	atomic64_set(&s->rx_bytes, 0);
	s->tx_done = false;
	mutex_init(&s->lock);
	urp_credit_init(&s->credit, URP_NUM_BUFS / 2);

	/* Reorder buffer is per-stream because each stream has its own
	 * sequence space (design 09 section 9.6). Capped at 256 -- well above
	 * any reasonable ECMP path-skew burst.
	 */
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

/*
 * Lookup by stream_id. Brackets the rhashtable traversal in its own
 * short rcu_read_lock() so callers do NOT need to hold one -- the RX
 * dispatch path (urp_stream_rx_dispatch) performs sleeping work
 * (mutex_lock, GFP_KERNEL alloc, kthread_stop) that must not run inside
 * an RCU read-side section. The returned pointer stays valid by the
 * caller's serialization invariant (the recv CQ is IB_POLL_WORKQUEUE
 * with serialized completions; endpoint drain runs only after the data
 * path quiesces), not by an outer RCU grace period.
 */
struct urp_stream *urp_stream_lookup(struct urp_endpoint *ep, u32 stream_id)
{
	struct urp_stream *s;

	if (!ep->streams_inited)
		return NULL;

	rcu_read_lock();
	s = rhashtable_lookup_fast(&ep->streams, &stream_id,
				   urp_stream_params);
	rcu_read_unlock();
	return s;
}

/* Remove from table and schedule deferred free via RCU. The stream's
 * uds_sock + reorder buffer are freed in the RCU callback.
 */
void urp_stream_destroy(struct urp_endpoint *ep, struct urp_stream *s)
{
	if (!s)
		return;

	rhashtable_remove_fast(&ep->streams, &s->ht_node, urp_stream_params);

	/* Ordering is critical: shut the UDS FIRST so a pump blocked in
	 * kernel_recvmsg() returns immediately, THEN stop the kthread --
	 * kthread_stop() before the shutdown would wait forever for a thread
	 * blocked in recvmsg. The pump may also have self-exited already
	 * (EOF/error paths); that is safe because urp_stream_pump_start()
	 * pinned the task_struct, making urp_stream_pump_stop()'s
	 * kthread_stop() a plain join with no use-after-free window.
	 */
	if (s->uds_sock)
		kernel_sock_shutdown(s->uds_sock, SHUT_RDWR);

	urp_stream_pump_stop(s);

	if (s->uds_sock) {
		sock_release(s->uds_sock);
		s->uds_sock = NULL;
	}

	mutex_destroy(&s->lock);
	call_rcu(&s->rcu, urp_stream_rcu_free);
}

/*
 * The pure stream state-machine core urp_stream_next_state() is extracted into
 * urp_stream_sm.c (so it can be fuzzed in userspace); the handlers below own
 * the locking + side effects and call it for the decision.
 */

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
		struct urp_stream_transition t;

		mutex_lock(&s->lock);
		t = urp_stream_next_state(s->state, URP_STREAM_EV_RX_SYN);
		if (!t.accepted) {
			mutex_unlock(&s->lock);
			return -EEXIST;
		}
		s->state = t.next;
		mutex_unlock(&s->lock);
		*out_stream = s;
		return 0;
	}

	ret = urp_stream_create(ep, stream_id, &s);
	if (ret)
		return ret;

	mutex_lock(&s->lock);
	s->state = URP_STREAM_STATE_SYN_RECEIVED;
	mutex_unlock(&s->lock);

	/*
	 * Acceptor-side backend UDS connect (Phase 3a Step 7d) is NOT done
	 * here: it is deferred to the caller (urp_recv_done) so the blocking
	 * kernel_connect() happens once, in one place, after dispatch returns.
	 * The fresh stream is left in SYN_RECEIVED with uds_sock == NULL;
	 * urp_stream_needs_backend() signals the caller to open it.
	 */
	*out_stream = s;
	return 0;
}

/*
 * True when @s is an acceptor-side stream that has been created (via an
 * incoming SYN) but does not yet have its backend UDS connection. The RX
 * path uses this to decide whether to run the (blocking) backend connect
 * outside rcu_read_lock. Cheap, lock-free field reads.
 */
bool urp_stream_needs_backend(struct urp_endpoint *ep, struct urp_stream *s)
{
	return s && !s->uds_sock && !ep->is_initiator && ep->connect_path[0];
}

/*
 * Acceptor-side: connect this stream's backend UDS and start its TX pump.
 * Called from urp_recv_done OUTSIDE rcu_read_lock (kernel_connect blocks).
 * Failure leaves uds_sock NULL; RX dispatch then drops frames for the
 * stream (counted in buffer_alloc_fails) until drain reaps it.
 */
void urp_stream_open_backend(struct urp_stream *s)
{
	if (!s || s->uds_sock)
		return;
	if (urp_stream_connect_uds(s, s->ep->connect_path) == 0)
		urp_stream_pump_start(s);
}

/*
 * Handle a FIN-flagged frame arriving from the peer. Implements the
 * half-close semantics in design 09 section 9.4: peer is done sending; we
 * shutdown(SHUT_WR) the UDS so the local app sees EOF on read, but
 * we keep reading from the UDS for any pending TX in this direction.
 *
 * When the local app eventually closes its side, the TX pump will
 * emit a FIN of our own, fully closing the stream.
 */
int urp_stream_rx_fin(struct urp_stream *s)
{
	struct urp_stream_transition t;

	if (!s)
		return -EINVAL;

	mutex_lock(&s->lock);
	t = urp_stream_next_state(s->state, URP_STREAM_EV_RX_FIN);
	if ((t.actions & URP_STREAM_ACT_SHUTDOWN_WR) && s->uds_sock)
		kernel_sock_shutdown(s->uds_sock, SHUT_WR);
	s->state = t.next;
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
	struct urp_stream_transition t;
	struct urp_endpoint *ep;

	if (!s)
		return -EINVAL;

	mutex_lock(&s->lock);
	t = urp_stream_next_state(s->state, URP_STREAM_EV_RX_RST);
	s->state = t.next;
	if ((t.actions & URP_STREAM_ACT_SHUTDOWN_RDWR) && s->uds_sock)
		kernel_sock_shutdown(s->uds_sock, SHUT_RDWR);
	ep = s->ep;
	mutex_unlock(&s->lock);

	if (t.actions & URP_STREAM_ACT_DESTROY)
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
	struct urp_stream_transition t;

	if (!s)
		return;

	mutex_lock(&s->lock);
	t = urp_stream_next_state(s->state, URP_STREAM_EV_TX_FIN);
	s->state = t.next;
	mutex_unlock(&s->lock);
}

/*
 * Frame-flag dispatcher used by the RX path (Step 7b will plumb the
 * call from urp_recv_done). Examines the DATA frame's flags byte and
 * fans out to syn/fin/rst handlers; pure-DATA frames (no flags)
 * proceed to ordinary in-order delivery via the reorder buffer.
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
 * endpoint drain time, after the data path has stopped.
 */
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
