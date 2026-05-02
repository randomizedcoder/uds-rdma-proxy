// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) — UDS socket endpoint
 *
 * Phase k0: Virtual UDS endpoint (design doc approach D).
 *
 * Initiator mode: creates a listening UDS socket at listen_path.
 *   Applications connect to it; accepted connections get pumped over RDMA.
 *
 * Acceptor mode: connects to a local UDS at connect_path.
 *   Data arriving from RDMA is forwarded to this connection.
 */

#include "urp.h"
#include <linux/file.h>
#include <net/sock.h>

/*
 * Accept loop kthread (initiator mode).
 *
 * Waits for a client to connect to the listening UDS socket,
 * then starts the TX/RX pump for that connection. k0 handles
 * only one connection at a time — subsequent accepts are rejected.
 */
static int urp_accept_thread_fn(void *data)
{
	struct urp_endpoint *ep = data;
	struct socket *new_sock = NULL;
	int ret;

	while (!kthread_should_stop()) {
		ret = kernel_accept(ep->listen_sock, &new_sock, 0);
		if (ret < 0) {
			if (ret == -EAGAIN || ret == -ERESTARTSYS)
				continue;
			pr_err("urp: accept failed: %d\n", ret);
			break;
		}

		if (ep->conn.active) {
			pr_warn("urp: k0 only supports one connection, rejecting\n");
			sock_release(new_sock);
			new_sock = NULL;
			continue;
		}

		pr_info("urp: UDS connection accepted\n");
		ep->conn.uds_sock = new_sock;
		ep->conn.seq = 0;
		ep->conn.active = true;
		atomic64_inc(&ep->stats.connections);

		/* Wait for RDMA to be ready before starting pump */
		if (!ep->connected) {
			pr_info("urp: waiting for RDMA connection...\n");
			wait_for_completion_interruptible(&ep->cm_done);
			if (!ep->connected) {
				pr_err("urp: RDMA connection failed\n");
				ep->conn.active = false;
				sock_release(new_sock);
				ep->conn.uds_sock = NULL;
				continue;
			}
		}

		ret = urp_pump_start(ep);
		if (ret) {
			pr_err("urp: pump start failed: %d\n", ret);
			ep->conn.active = false;
			sock_release(new_sock);
			ep->conn.uds_sock = NULL;
		}
	}

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
		pr_err("urp: sock_create_kern failed: %d\n", ret);
		return ret;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strscpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = kernel_connect(sock, (struct sockaddr *)&addr,
			     offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1,
			     0);
	if (ret) {
		pr_err("urp: connect to %s failed: %d\n", path, ret);
		sock_release(sock);
		return ret;
	}

	ep->conn.uds_sock = sock;
	ep->conn.seq = 0;
	ep->conn.active = true;
	atomic64_inc(&ep->stats.connections);

	pr_info("urp: connected to UDS %s\n", path);
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
		pr_err("urp: sock_create_kern failed: %d\n", ret);
		return ret;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strscpy(addr.sun_path, path, sizeof(addr.sun_path));

	ret = kernel_bind(sock, (struct sockaddr *)&addr,
			  offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
	if (ret) {
		pr_err("urp: bind to %s failed: %d\n", path, ret);
		sock_release(sock);
		return ret;
	}

	ret = kernel_listen(sock, 1);
	if (ret) {
		pr_err("urp: listen failed: %d\n", ret);
		sock_release(sock);
		return ret;
	}

	ep->listen_sock = sock;

	ep->accept_thread = kthread_run(urp_accept_thread_fn, ep, "urp-accept");
	if (IS_ERR(ep->accept_thread)) {
		ret = PTR_ERR(ep->accept_thread);
		ep->accept_thread = NULL;
		pr_err("urp: kthread_run failed: %d\n", ret);
		sock_release(sock);
		ep->listen_sock = NULL;
		return ret;
	}

	pr_info("urp: listening on UDS %s\n", path);
	return 0;
}

int urp_socket_init(struct urp_endpoint *ep, const char *path)
{
	if (strlen(path) >= URP_PATH_MAX) {
		pr_err("urp: path too long: %s\n", path);
		return -ENAMETOOLONG;
	}

	if (ep->is_initiator)
		return urp_listen_uds(ep, path);

	/* Acceptor: just store the path. UDS connect + pump start happen in
	 * the RDMA CM ESTABLISHED handler to avoid racing with recv completions. */
	strscpy(ep->uds_path, path, sizeof(ep->uds_path));
	pr_info("urp: acceptor waiting for RDMA connection (connect_path=%s)\n", path);
	return 0;
}

void urp_socket_cleanup(struct urp_endpoint *ep)
{
	if (ep->accept_thread) {
		kthread_stop(ep->accept_thread);
		ep->accept_thread = NULL;
	}

	if (ep->conn.uds_sock) {
		ep->conn.active = false;
		kernel_sock_shutdown(ep->conn.uds_sock, SHUT_RDWR);
		sock_release(ep->conn.uds_sock);
		ep->conn.uds_sock = NULL;
	}

	if (ep->listen_sock) {
		kernel_sock_shutdown(ep->listen_sock, SHUT_RDWR);
		sock_release(ep->listen_sock);
		ep->listen_sock = NULL;
	}
}
