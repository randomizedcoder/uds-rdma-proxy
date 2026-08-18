// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- UDS socket endpoint
 *
 * Phase k0: Virtual UDS endpoint (design doc approach D).
 *
 * Initiator mode: creates a listening UDS socket at listen_path.
 *   Applications connect to it; accepted connections get pumped over RDMA.
 *
 * Acceptor mode: connects to a local UDS at connect_path.
 *   Data arriving from RDMA is forwarded to this connection.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"
#include "urp_conn_plan.h"	/* urp_should_unlink_listen_path (design 33) */
#include "urp_lazy_plan.h"	/* urp_should_start_lazy_connect (design 33 P2) */
#include <linux/dcache.h>	/* d_is_socket, d_really_is_negative */
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/sched/task.h>
#include <net/sock.h>

/*
 * Accept loop kthread (initiator mode).
 *
 * Waits for clients to connect to the listening UDS socket. Phase 3a
 * Step 7d: each accepted connection becomes its own stream (multiplexed
 * over the shared QP set) rather than the single k0 ep->conn. This is
 * what lets a real client -- e.g. franz-go, which opens a separate
 * connection per broker beyond the metadata bootstrap -- work end to end.
 *
 * urp_stream_create allocates the next odd stream_id; the per-stream TX
 * pump (urp_stream_tx_fn) emits a SYN-flagged first frame, which makes
 * the acceptor open a matching backend UDS for this stream.
 */
static int urp_accept_thread_fn(void *data)
{
	struct urp_endpoint *ep = data;
	struct socket *new_sock = NULL;
	int ret;

	while (!kthread_should_stop()) {
		struct urp_stream *s = NULL;

		ret = kernel_accept(ep->listen_sock, &new_sock, 0);
		if (ret < 0) {
			if (ret == -EAGAIN || ret == -ERESTARTSYS)
				continue;
			pr_err_ratelimited("accept failed: %d\n", ret);
			break;
		}

		pr_debug("UDS connection accepted\n");

		/*
		 * design 33 Phase 2: the first client connect fires the
		 * one-shot lazy RDMA dial. atomic_cmpxchg flips the latch 0->1
		 * exactly once; urp_should_start_lazy_connect gates on role +
		 * that prior latch value. The state == ACTIVE guard rejects the
		 * sliver where the accept thread is already live but state is
		 * still pre-ACTIVE (urp_socket_init starts the thread before
		 * urp_endpoint_activate sets state = ACTIVE).
		 */
		if (ep->state == URP_STATE_ACTIVE &&
		    urp_should_start_lazy_connect(ep->is_initiator,
			atomic_cmpxchg(&ep->connect_started, 0, 1) != 0))
			urp_lazy_connect_start(ep);

		/* Wait for RDMA to be ready before opening a stream. */
		if (!ep->connected) {
			/*
			 * design 33 Phase 2 fail-fast: after retry exhaustion
			 * the initiator's terminal paths set connect_failed and
			 * consume cm_done. Don't park a late client on an
			 * already-consumed completion -- reject it and let the
			 * endpoint stay dead until `urp remove`/`add`.
			 */
			if (READ_ONCE(ep->connect_failed)) {
				pr_err("RDMA connect terminally failed (%d); rejecting client\n",
				       ep->cm_status);
				sock_release(new_sock);
				new_sock = NULL;
				continue;
			}
			pr_info("waiting for RDMA connection...\n");
			wait_for_completion_interruptible(&ep->cm_done);
			/*
			 * Drain releases the waiter via complete(&cm_done) in
			 * urp_socket_cleanup before kthread_stop; a woken thread
			 * whose endpoint is no longer ACTIVE must exit the loop
			 * (release the held sock first -- it was accepted above)
			 * so kthread_stop joins cleanly.
			 */
			if (ep->state != URP_STATE_ACTIVE) {
				sock_release(new_sock);
				new_sock = NULL;
				break;
			}
			if (!ep->connected) {
				pr_err("RDMA connection failed\n");
				sock_release(new_sock);
				new_sock = NULL;
				continue;
			}
		}

		ret = urp_stream_create(ep, 0, &s);
		if (ret) {
			pr_err("stream create failed: %d\n", ret);
			sock_release(new_sock);
			new_sock = NULL;
			continue;
		}

		/* Ownership of new_sock transfers to the stream; it is
		 * released by urp_stream_destroy (drain / RST).
		 */
		s->uds_sock = new_sock;
		new_sock = NULL;
		atomic64_inc(&ep->stats.connections);

		ret = urp_stream_pump_start(s);
		if (ret) {
			pr_err("stream %u pump start failed: %d\n",
			       s->id, ret);
			urp_stream_destroy(ep, s);
			continue;
		}
	}

	return 0;
}

/*
 * Open a fresh UDS connection to @path and attach it to @stream
 * (Phase 3a Step 7c). Used by the acceptor side to give each
 * multi-stream entry its own backend UDS connection.
 *
 * On success the kernel takes ownership of the socket via
 * stream->uds_sock; urp_stream_destroy will sock_release it.
 */
int urp_stream_connect_uds(struct urp_stream *stream, const char *path)
{
	struct socket *sock;
	struct sockaddr_un addr;
	int ret;

	ret = sock_create_kern(&init_net, AF_UNIX, SOCK_STREAM, 0, &sock);
	if (ret) {
		pr_err("stream %u sock_create_kern failed: %d\n",
		       stream->id, ret);
		return ret;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	urp_strscpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = kernel_connect(sock, (urp_sockaddr_t *)&addr,
			     offsetof(struct sockaddr_un, sun_path) +
				     strlen(path) + 1,
			     0);
	if (ret) {
		pr_err("stream %u connect to %s failed: %d\n",
		       stream->id, path, ret);
		sock_release(sock);
		return ret;
	}

	stream->uds_sock = sock;
	pr_debug("stream %u connected to UDS %s\n", stream->id, path);
	return 0;
}

/*
 * Connect to local UDS (acceptor mode).
 *
 * The acceptor side connects to a local application's UDS socket
 * (e.g., the actual service like Redpanda or PostgreSQL).
 *
 * Called from the RDMA CM ESTABLISHED handler (workqueue context)
 * so that the data path is ready before any recv completions fire.
 */
int urp_connect_uds(struct urp_endpoint *ep, const char *path)
{
	struct socket *sock;
	struct sockaddr_un addr;
	int ret;

	ret = sock_create_kern(&init_net, AF_UNIX, SOCK_STREAM, 0, &sock);
	if (ret) {
		pr_err("sock_create_kern failed: %d\n", ret);
		return ret;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* urp_strscpy(): sized_strscpy() on 6.8+, strscpy() before -- see urp.h. */
	urp_strscpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = kernel_connect(sock, (urp_sockaddr_t *)&addr,
			     offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1,
			     0);
	if (ret) {
		pr_err("connect to %s failed: %d\n", path, ret);
		sock_release(sock);
		return ret;
	}

	ep->conn.uds_sock = sock;
	ep->conn.seq = 0;
	ep->conn.active = true;
	atomic64_inc(&ep->stats.connections);

	pr_info("connected to UDS %s\n", path);
	return 0;
}

/*
 * Design 33 Bug 2: unlink a stale pathname AF_UNIX socket node.
 *
 * Only the initiator binds a pathname listen socket, and nothing ever removed
 * the filesystem node. After drain+remove -- or a crash/OOM where cleanup never
 * ran -- the node survives, so the next `urp add` fails -98 EADDRINUSE on bind.
 * Unlink it here (both before bind and on cleanup). We refuse to remove anything
 * that is not a socket, so a mis-configured path pointing at a real file can't
 * destroy data. -ENOENT (nothing there) is the normal case and is silent.
 *
 * NB: kern_path_locked() returns with the parent inode locked and a reference
 * on the target dentry; the exact idmap/signature is verified against the hp
 * kernel at build time (see design 33 plan).
 */
static void urp_unlink_stale_socket(const char *path)
{
	struct path target;
	struct dentry *dentry, *parent;
	struct inode *dir, *inode;
	int ret;

	/*
	 * kern_path_locked() / lookup_one_len() would be the natural primitives
	 * but they drift across the kernels we build against (kern_path_locked
	 * is not exported to modules on 6.1; lookup_one_len was removed by 7.x).
	 * Compose the unlink from symbols that are exported and stable across
	 * 6.1..7.x: resolve the node (kern_path), grab its parent (dget_parent),
	 * then vfs_unlink under the parent i_rwsem. Only urp listen paths reach
	 * here (absolute, e.g. /run/urp.sock).
	 */
	ret = kern_path(path, 0, &target);
	if (ret)
		return;			/* -ENOENT etc: nothing to remove */

	dentry = target.dentry;
	parent = dget_parent(dentry);
	dir = d_inode(parent);

	inode_lock_nested(dir, I_MUTEX_PARENT);
	inode = d_inode(dentry);
	if (!inode || dentry->d_parent != parent) {
		/* Raced with a concurrent rename/unlink -- leave it alone. */
	} else if (!S_ISSOCK(inode->i_mode)) {
		/* Never remove a real file/dir that shares the name. */
		pr_warn("refusing to unlink non-socket %s\n", path);
	} else {
		/*
		 * vfs_unlink() took a struct user_namespace * (via mnt_user_ns)
		 * until v6.3 switched it to struct mnt_idmap * (via mnt_idmap).
		 * We build down to 6.1, so gate the idmap argument on the
		 * version (same idiom as the >=6.8 fast path in urp_cmd.c).
		 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		ret = vfs_unlink(mnt_idmap(target.mnt), dir, dentry, NULL);
#else
		ret = vfs_unlink(mnt_user_ns(target.mnt), dir, dentry, NULL);
#endif
		if (ret)
			pr_warn("unlink stale socket %s failed: %d\n",
				path, ret);
		else
			pr_info("removed stale socket %s\n", path);
	}
	inode_unlock(dir);
	dput(parent);
	path_put(&target);
}

/*
 * Create a listening UDS socket (initiator mode).
 */
static int urp_listen_uds(struct urp_endpoint *ep, const char *path)
{
	struct socket *sock;
	struct sockaddr_un addr;
	int ret;

	ret = sock_create_kern(&init_net, AF_UNIX, SOCK_STREAM, 0, &sock);
	if (ret) {
		pr_err("sock_create_kern failed: %d\n", ret);
		return ret;
	}

	/*
	 * Design 33 Bug 2: clear any stale node left by a prior instance so
	 * bind() below doesn't fail -98 EADDRINUSE. Gated by the pure predicate
	 * (initiator + a path is set) to keep the decision unit-testable.
	 */
	if (urp_should_unlink_listen_path(ep->is_initiator, path[0] != '\0'))
		urp_unlink_stale_socket(path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	urp_strscpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = kernel_bind(sock, (urp_sockaddr_t *)&addr,
			  offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
	if (ret) {
		pr_err("bind to %s failed: %d\n", path, ret);
		sock_release(sock);
		return ret;
	}

	ret = kernel_listen(sock, 1);
	if (ret) {
		pr_err("listen failed: %d\n", ret);
		sock_release(sock);
		return ret;
	}

	ep->listen_sock = sock;

	ep->accept_thread = kthread_run(urp_accept_thread_fn, ep, "urp-accept");
	if (IS_ERR(ep->accept_thread)) {
		ret = PTR_ERR(ep->accept_thread);
		ep->accept_thread = NULL;
		pr_err("kthread_run failed: %d\n", ret);
		sock_release(sock);
		ep->listen_sock = NULL;
		return ret;
	}
	/*
	 * Pin the task: the accept thread self-exits on fatal accept errors
	 * (including the -ECONNABORTED our own teardown shutdown provokes),
	 * and urp_socket_cleanup()'s kthread_stop() must never race a
	 * reaped task_struct. Dropped in urp_socket_cleanup().
	 */
	get_task_struct(ep->accept_thread);

	pr_info("listening on UDS %s\n", path);
	return 0;
}

int urp_socket_init(struct urp_endpoint *ep, const char *path)
{
	if (strlen(path) >= URP_PATH_MAX_LEN) {
		pr_err("path too long: %s\n", path);
		return -ENAMETOOLONG;
	}

	if (ep->is_initiator)
		return urp_listen_uds(ep, path);

	/*
	 * Acceptor: nothing to do here. The path is already stored on the
	 * endpoint as ep->connect_path; the UDS connect + pump start happen in
	 * the RDMA CM ESTABLISHED handler (urp_rdma.c) to avoid racing with
	 * recv completions.
	 */
	pr_info("acceptor waiting for RDMA connection (connect_path=%s)\n", path);
	return 0;
}

/*
 * Tear down the legacy single-connection state on the acceptor when
 * the peer disconnects (or pre-emptively on a fresh CONNECT_REQUEST
 * if the previous teardown didn't fire). Phase 4 Step 5.
 *
 * Order matters:
 *   1. set conn.active = false so the pump loop's outer while
 *      condition will exit on its next iteration.
 *   2. kernel_sock_shutdown(SHUT_RDWR) wakes any pending
 *      kernel_recvmsg in the pump kthread with -ENOTCONN, so
 *      kthread_stop below doesn't block indefinitely.
 *   3. urp_pump_stop kthread_stop's the pump and waits for it to
 *      exit. Without step 2 this would deadlock when the pump is
 *      asleep in recvmsg.
 *   4. sock_release the UDS socket.
 *
 * Idempotent: safe to call when conn is already clean (callers may
 * defensively invoke it without checking).
 */
void urp_socket_conn_cleanup(struct urp_endpoint *ep)
{
	struct socket *sock = ep->conn.uds_sock;

	if (!ep->conn.active && !sock && !ep->conn.tx_thread)
		return;

	ep->conn.active = false;
	if (sock)
		kernel_sock_shutdown(sock, SHUT_RDWR);

	urp_pump_stop(ep);	/* joins tx_thread (now unblocked by shutdown) */

	if (sock) {
		sock_release(sock);
		ep->conn.uds_sock = NULL;
	}
}

void urp_socket_cleanup(struct urp_endpoint *ep)
{
	/*
	 * Order matters (same principle as urp_socket_conn_cleanup):
	 *   1. kernel_sock_shutdown(listen_sock) wakes the accept_thread
	 *      blocked in kernel_accept with -EINTR / -ECONNABORTED.
	 *      Without this, step 2's kthread_stop blocks for 45s+ while
	 *      the accept syscall sits in TASK_INTERRUPTIBLE -- expect's
	 *      command timeout fires before kthread_stop returns, the
	 *      shell goes back to the caller, and `urp drain` looks
	 *      hung. Driver-level teardown still finishes, just very
	 *      slowly. This was the root cause of the 76s teardown in
	 *      the Phase 5 pair test.
	 *   2. kthread_stop joins the now-woken accept_thread.
	 *   3. urp_socket_conn_cleanup handles the per-connection
	 *      kthread + uds_sock the same way.
	 *   4. sock_release the listen_sock.
	 */
	if (ep->listen_sock)
		kernel_sock_shutdown(ep->listen_sock, SHUT_RDWR);

	/*
	 * design 33 Phase 2: release an accept-thread waiter parked on cm_done
	 * (a client accepted but still waiting for the lazy dial to establish)
	 * BEFORE kthread_stop -- kthread_stop sends no signal, so a thread in
	 * wait_for_completion_interruptible would never wake and the join would
	 * hang. state is already DRAINING here (set in urp_endpoint_drain), so
	 * the woken thread takes the state != ACTIVE break and exits the loop.
	 * Single accept thread, backlog 1 -> one possible waiter; a stray count
	 * is harmless on an endpoint being torn down. This also closes a latent
	 * pre-existing hang: a client parked here during the eager path's drain.
	 */
	complete(&ep->cm_done);

	if (ep->accept_thread) {
		kthread_stop(ep->accept_thread);
		put_task_struct(ep->accept_thread);
		ep->accept_thread = NULL;
	}

	urp_socket_conn_cleanup(ep);

	if (ep->listen_sock) {
		sock_release(ep->listen_sock);
		ep->listen_sock = NULL;
	}

	/*
	 * Design 33 Bug 2: remove the pathname node so a later `urp add`
	 * (re-create of this endpoint) binds cleanly instead of failing -98
	 * EADDRINUSE. sock_release() above drops the socket but leaves the
	 * filesystem node behind. Gated on the pure predicate.
	 */
	if (urp_should_unlink_listen_path(ep->is_initiator,
					  ep->listen_path[0] != '\0'))
		urp_unlink_stale_socket(ep->listen_path);
}
