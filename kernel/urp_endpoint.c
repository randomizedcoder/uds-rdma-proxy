// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- endpoint lifecycle and storage
 *
 * Endpoints are stored in a global rhashtable keyed by name. Each endpoint
 * encapsulates one RDMA CM connection and one UDS socket pair. State
 * transitions (CREATING -> ACTIVE -> DRAINING -> STOPPED) are serialized
 * by the per-endpoint mutex. Teardown uses kfree_rcu so any concurrent
 * dump walks see consistent state until their RCU read-side critical
 * section ends.
 */

#include "urp.h"
#include <linux/inet.h>
#include <linux/slab.h>
#include <crypto/sha2.h>
#include <net/ipv6.h>

struct rhashtable urp_endpoints;
bool urp_endpoints_inited;
static DEFINE_MUTEX(urp_endpoints_mutex);

/*
 * Fixed-key rhashtable: the name is a zero-padded URP_NAME_MAX byte field
 * (kzalloc'd struct, NUL-terminated). Using the default fixed-key hashing /
 * comparison keeps lookup_insert_fast safe -- that helper BUG()s when
 * obj_hashfn is set (see include/linux/rhashtable.h ~line 968).
 */
static const struct rhashtable_params urp_endpoints_params = {
	.head_offset	= offsetof(struct urp_endpoint, ht_node),
	.key_offset	= offsetof(struct urp_endpoint, name),
	.key_len	= URP_NAME_MAX,
	.automatic_shrinking = true,
};

int urp_endpoint_table_init(void)
{
	int ret;

	ret = rhashtable_init(&urp_endpoints, &urp_endpoints_params);
	if (ret)
		return ret;

	urp_endpoints_inited = true;
	return 0;
}

void urp_endpoint_table_destroy(void)
{
	if (!urp_endpoints_inited)
		return;

	rhashtable_destroy(&urp_endpoints);
	urp_endpoints_inited = false;
}

/*
 * Raw table lookup -- returns a bare pointer valid only within the caller's
 * RCU read-side critical section. Internal; external callers use
 * urp_endpoint_get() so they hold a reference.
 */
static struct urp_endpoint *urp_endpoint_lookup(const char *name)
{
	char key[URP_NAME_MAX];
	size_t len;

	if (!urp_endpoints_inited)
		return NULL;

	/*
	 * The hashtable hashes/compares the full URP_NAME_MAX bytes, so we
	 * have to zero-pad the (possibly short) caller-supplied name before
	 * lookup. Stored entries are kzalloc'd so they're always zero-padded.
	 */
	len = strnlen(name, URP_NAME_MAX);
	memset(key, 0, sizeof(key));
	memcpy(key, name, len);

	return rhashtable_lookup_fast(&urp_endpoints, key,
				      urp_endpoints_params);
}

/*
 * Look up an endpoint and take a reference so the returned pointer stays valid
 * after this returns (the netlink handlers dereference it outside any RCU
 * section). kref_get_unless_zero() fails if the endpoint's last reference has
 * already been dropped (it is mid-teardown), in which case we treat it as not
 * found. Caller must urp_endpoint_put().
 */
struct urp_endpoint *urp_endpoint_get(const char *name)
{
	struct urp_endpoint *ep;

	rcu_read_lock();
	ep = urp_endpoint_lookup(name);
	if (ep && !kref_get_unless_zero(&ep->refcount))
		ep = NULL;
	rcu_read_unlock();
	return ep;
}

/*
 * Convert the endpoint's stored sockaddr_in6 into the (string, port) tuple
 * urp_rdma_init currently expects. Phase 2 supports IPv4 only (encoded as
 * IPv4-mapped IPv6); native IPv6 lands in Phase 3 along with the rdma_init
 * refactor.
 */
int urp_endpoint_extract_v4(const struct sockaddr_in6 *addr,
			    char *out_ip, size_t out_len, int *out_port)
{
	__be32 v4;

	if (addr->sin6_family != AF_INET6)
		return -EAFNOSUPPORT;

	if (!ipv6_addr_v4mapped(&addr->sin6_addr))
		return -EAFNOSUPPORT;

	v4 = addr->sin6_addr.s6_addr32[3];
	snprintf(out_ip, out_len, "%pI4", &v4);
	*out_port = ntohs(addr->sin6_port);
	return 0;
}

/*
 * Allocate and validate an endpoint from a configuration template (which the
 * GENL handler fills in from netlink attributes). On success, ownership of
 * @cfg's contents transfers to the new endpoint and it is inserted into the
 * rhashtable in CREATING state.
 *
 * @cfg fields used: name, listen_path, connect_path, rdma_device, peer_addr,
 *                   bind_addr, has_peer_addr, has_bind_addr, num_qps,
 *                   buffer_count, buffer_size, password, has_password.
 */
int urp_endpoint_create(struct urp_endpoint *cfg, struct urp_endpoint **out)
{
	struct urp_endpoint *ep;
	bool initiator;
	int ret;

	if (!cfg->name[0])
		return -EINVAL;

	initiator = cfg->listen_path[0] != '\0';

	if (initiator && cfg->connect_path[0] != '\0')
		return -EINVAL;
	if (!initiator && cfg->connect_path[0] == '\0')
		return -EINVAL;
	if (initiator && !cfg->has_peer_addr)
		return -EINVAL;
	if (!initiator && !cfg->has_bind_addr)
		return -EINVAL;

	ep = kzalloc(sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	memcpy(ep->name, cfg->name, URP_NAME_MAX);
	memcpy(ep->listen_path, cfg->listen_path, URP_PATH_MAX_LEN);
	memcpy(ep->connect_path, cfg->connect_path, URP_PATH_MAX_LEN);
	memcpy(ep->rdma_device, cfg->rdma_device, URP_DEVICE_MAX);
	ep->peer_addr = cfg->peer_addr;
	ep->bind_addr = cfg->bind_addr;
	ep->has_peer_addr = cfg->has_peer_addr;
	ep->has_bind_addr = cfg->has_bind_addr;
	ep->num_qps      = cfg->num_qps      ? cfg->num_qps      : URP_NUM_QPS_DEFAULT;
	ep->buffer_count = cfg->buffer_count ? cfg->buffer_count : URP_BUFFER_COUNT_DEFAULT;
	ep->buffer_size  = cfg->buffer_size  ? cfg->buffer_size  : URP_BUFFER_SIZE_DEFAULT;
	ep->has_password = cfg->has_password;
	if (cfg->has_password) {
		/*
		 * Phase 3b Step 7: SHA-256 the raw 16-byte input from the
		 * netlink attr into a 32-byte hash, then zero the raw
		 * field so the kernel only retains the digest.
		 *
		 * Phase 3b Step 8: also pre-build the auth_priv buffer
		 * (1B method + 32B hash) we'll feed rdma_connect's
		 * private_data.
		 */
		sha256(cfg->password, URP_PASSWORD_MAX, ep->password_hash);
		memzero_explicit(ep->password, sizeof(ep->password));
		ep->auth_priv[0] = URP_PSK_AUTH_METHOD_SHA256;
		memcpy(ep->auth_priv + 1, ep->password_hash, URP_PSK_HASH_LEN);
	}

	ep->is_initiator = initiator;
	ep->state        = URP_STATE_CREATING;
	mutex_init(&ep->lock);
	init_completion(&ep->cm_done);
	/*
	 * design 33 Phase 2: lazy-connect latch + terminal flag. Set before the
	 * rhashtable publish below so a concurrent dumpit/drain walker never
	 * observes an uninitialized latch.
	 */
	atomic_set(&ep->connect_started, 0);
	ep->connect_failed = false;
	/* One reference for the table entry (dropped by urp_endpoint_remove). */
	kref_init(&ep->refcount);

	mutex_lock(&urp_endpoints_mutex);
	ret = rhashtable_lookup_insert_fast(&urp_endpoints, &ep->ht_node,
					    urp_endpoints_params);
	mutex_unlock(&urp_endpoints_mutex);
	if (ret) {
		/* Never published; no other reference exists -> free directly. */
		mutex_destroy(&ep->lock);
		kfree(ep);
		return ret == -EEXIST ? -EEXIST : ret;
	}

	/* A second reference for the caller (dropped once it finishes with ep). */
	kref_get(&ep->refcount);
	*out = ep;
	return 0;
}

/*
 * Bring an endpoint up: create the /proc subdir, start RDMA CM, create the
 * UDS socket. On any failure, the caller should call urp_endpoint_remove()
 * (which unpublishes + drains) and drop its reference. State moves
 * CREATING -> ACTIVE on success.
 */
int urp_endpoint_activate(struct urp_endpoint *ep)
{
	char ip_buf[INET_ADDRSTRLEN];
	int peer_port = 0;
	int bind_port = 0;
	const char *peer_ip = "";
	const char *uds_path;
	int ret;

	mutex_lock(&ep->lock);
	if (ep->state != URP_STATE_CREATING) {
		ret = -EINVAL;
		goto unlock;
	}

	/*
	 * Resolve the live pool geometry from the (mutable-while-inactive)
	 * buffer_count / buffer_size config. The netlink policy already bounds
	 * both to their ranges; the resolvers clamp defensively so num_bufs and
	 * buf_size are trustworthy for every downstream sizing (pool depth, CQ,
	 * SRQ, SQ depth, credit window; DMA slot bytes, sge lengths, wire
	 * max-payload) even if a value arrived by some other path.
	 */
	ep->num_bufs = urp_resolve_num_bufs(ep->buffer_count);
	ep->buf_size = urp_resolve_buf_size(ep->buffer_size);

	if (ep->is_initiator) {
		ret = urp_endpoint_extract_v4(&ep->peer_addr, ip_buf,
					      sizeof(ip_buf), &peer_port);
		if (ret)
			goto unlock;
		peer_ip = ip_buf;
		uds_path = ep->listen_path;
	} else {
		struct sockaddr_in6 *b = &ep->bind_addr;

		if (b->sin6_family != AF_INET6) {
			ret = -EAFNOSUPPORT;
			goto unlock;
		}
		bind_port = ntohs(b->sin6_port);
		uds_path  = ep->connect_path;
	}

	ret = urp_endpoint_proc_create(ep);
	if (ret)
		goto unlock;

	ret = urp_qps_init(ep);
	if (ret)
		goto err_proc;

	ret = urp_streams_init(ep);
	if (ret)
		goto err_qps;

	ret = urp_rdma_init(ep, peer_ip, peer_port, bind_port, ep->is_initiator);
	if (ret)
		goto err_streams;

	ret = urp_socket_init(ep, uds_path);
	if (ret)
		goto err_rdma;

	ep->state = URP_STATE_ACTIVE;
	urp_probe_work_start(ep);
	mutex_unlock(&ep->lock);
	urp_send_event(ep);
	return 0;

err_rdma:
	urp_rdma_cleanup(ep);
err_streams:
	urp_streams_destroy_all(ep);
err_qps:
	urp_qps_destroy(ep);
err_proc:
	urp_endpoint_proc_remove(ep);
unlock:
	mutex_unlock(&ep->lock);
	return ret;
}

/*
 * Quiesce an endpoint: stop accepting new work, tear down sockets and RDMA
 * resources. State transitions ACTIVE/CREATING -> DRAINING -> STOPPED.
 * Idempotent: safe to call on an already-STOPPED endpoint.
 */
void urp_endpoint_drain(struct urp_endpoint *ep)
{
	mutex_lock(&ep->lock);

	if (ep->state == URP_STATE_STOPPED) {
		mutex_unlock(&ep->lock);
		return;
	}

	ep->state = URP_STATE_DRAINING;
	mutex_unlock(&ep->lock);
	urp_send_event(ep);

	mutex_lock(&ep->lock);
	urp_probe_work_stop(ep);
	/*
	 * urp_socket_cleanup is responsible for both the listen socket
	 * and the per-connection state, and it shuts the relevant
	 * sockets down BEFORE kthread_stop'ing the corresponding
	 * kthreads (see comments there). Calling urp_pump_stop()
	 * standalone first would block waiting for the TX kthread to
	 * exit while the kthread is asleep in kernel_recvmsg with no
	 * shutdown -- previously this hung indefinitely on the acceptor
	 * side of the Phase 5 pair test until the harness's per-command
	 * expect timeout fired.
	 */
	urp_socket_cleanup(ep);
	/*
	 * Destroy the per-stream state (which joins the per-stream TX pump
	 * kthreads, urp_stream_tx_fn) BEFORE urp_rdma_cleanup frees the send
	 * buffer pool. Those kthreads call urp_buf_alloc_send/urp_buf_free_send
	 * against ep->bufs on every iteration and on their error/exit paths; if
	 * urp_rdma_cleanup ran first it would kfree(ep->bufs) (via
	 * urp_bufs_cleanup) out from under a still-running pump, a use-after-free
	 * KASAN caught during endpoint teardown. urp_stream_destroy shuts each
	 * stream's UDS socket before kthread_stop (so the join never hangs in
	 * kernel_recvmsg), and needs no RDMA resource itself -- the QP is still
	 * up here, so the pump's parting urp_stream_tx_fin FIN can still post.
	 * The legacy ep->conn pump is already joined above in urp_socket_cleanup.
	 */
	urp_streams_destroy_all(ep);
	urp_rdma_cleanup(ep);
	urp_qps_destroy(ep);
	urp_endpoint_proc_remove(ep);

	ep->state = URP_STATE_STOPPED;
	mutex_unlock(&ep->lock);
	urp_send_event(ep);
}

static void urp_endpoint_rcu_free(struct rcu_head *rcu)
{
	struct urp_endpoint *ep = container_of(rcu, struct urp_endpoint, rcu);

	mutex_destroy(&ep->lock);
	kfree(ep);
}

/*
 * kref release: the last reference is gone. Defer the free through RCU so any
 * concurrent rhashtable walk (dumpit, drain_all) that still holds a bare
 * pointer within its read-side critical section stays valid until it ends.
 */
static void urp_endpoint_release(struct kref *kref)
{
	struct urp_endpoint *ep = container_of(kref, struct urp_endpoint, refcount);

	call_rcu(&ep->rcu, urp_endpoint_rcu_free);
}

void urp_endpoint_put(struct urp_endpoint *ep)
{
	if (ep)
		kref_put(&ep->refcount, urp_endpoint_release);
}

/*
 * Unpublish from the table and tear down, exactly once. Removing from the
 * rhashtable is the serialization point: only the thread whose
 * rhashtable_remove_fast() succeeds drains the endpoint and drops the table
 * reference. Concurrent removers (two DELs, or DEL racing module exit) get
 * -ENOENT and return, so the endpoint is drained once and the table reference
 * is dropped once -- no double-drain, no double-free. Any references still
 * held by concurrent lookups keep the object alive until they are dropped.
 */
void urp_endpoint_remove(struct urp_endpoint *ep)
{
	int ret;

	mutex_lock(&urp_endpoints_mutex);
	ret = rhashtable_remove_fast(&urp_endpoints, &ep->ht_node,
				     urp_endpoints_params);
	mutex_unlock(&urp_endpoints_mutex);
	if (ret)
		return;			/* another remover won */

	urp_endpoint_drain(ep);
	urp_endpoint_put(ep);		/* drop the table reference */
}

/*
 * Drain and destroy all endpoints. Used at module exit. Must run after
 * urp_genl_unregister() so no new netlink-driven creates can race.
 */
void urp_endpoint_drain_all(void)
{
	struct rhashtable_iter iter;
	struct urp_endpoint *ep;

	if (!urp_endpoints_inited)
		return;

	for (;;) {
		rhashtable_walk_enter(&urp_endpoints, &iter);
		rhashtable_walk_start(&iter);

		/*
		 * Grab a reference on the first live entry while inside the walk
		 * (RCU) so it stays valid after we stop the walk. Skip entries
		 * already dropping their last reference.
		 */
		ep = NULL;
		while ((ep = rhashtable_walk_next(&iter))) {
			if (IS_ERR(ep))
				continue;
			if (kref_get_unless_zero(&ep->refcount))
				break;
		}

		rhashtable_walk_stop(&iter);
		rhashtable_walk_exit(&iter);

		if (!ep || IS_ERR(ep))
			break;

		urp_endpoint_remove(ep);	/* unpublish + drain + drop table ref */
		urp_endpoint_put(ep);		/* drop our walk reference */
	}

	/* Wait for any in-flight RCU callbacks (RCU-deferred frees) */
	rcu_barrier();
}
