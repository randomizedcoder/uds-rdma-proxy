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
#include <linux/file.h>
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

		/* Wait for RDMA to be ready before opening a stream. */
		if (!ep->connected) {
			pr_info("waiting for RDMA connection...\n");
			wait_for_completion_interruptible(&ep->cm_done);
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

	if (ep->accept_thread) {
		kthread_stop(ep->accept_thread);
		ep->accept_thread = NULL;
	}

	urp_socket_conn_cleanup(ep);

	if (ep->listen_sock) {
		sock_release(ep->listen_sock);
		ep->listen_sock = NULL;
	}
}
