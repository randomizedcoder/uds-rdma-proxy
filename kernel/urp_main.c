// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) kernel module — main entry point
 *
 * Phase k0: Single endpoint configured via module_param.
 * Loads the module, creates a UDS listening socket at listen_path (initiator)
 * or connect_path (acceptor), establishes RDMA connection to peer, and pumps
 * data between UDS and RDMA.
 */

#include "urp.h"

/* Global endpoint (k0: single instance) */
struct urp_endpoint *urp_ep;

/*
 * Module parameters (k0 only — replaced by GENL in Phase k1/Phase 2)
 *
 * Initiator mode: listen_path + peer_address + peer_port
 *   Applications connect to listen_path; module forwards to peer via RDMA.
 *
 * Acceptor mode: connect_path + bind_port
 *   Module accepts RDMA connections and forwards to a local UDS at connect_path.
 */
static char *listen_path = "";
module_param(listen_path, charp, 0444);
MODULE_PARM_DESC(listen_path, "UDS path to listen on (initiator mode)");

static char *connect_path = "";
module_param(connect_path, charp, 0444);
MODULE_PARM_DESC(connect_path, "UDS path to connect to (acceptor mode)");

static char *peer_address = "";
module_param(peer_address, charp, 0444);
MODULE_PARM_DESC(peer_address, "Remote peer IP address (initiator mode)");

static int peer_port = URP_DEFAULT_PORT;
module_param(peer_port, int, 0444);
MODULE_PARM_DESC(peer_port, "Remote peer RDMA port (initiator mode)");

static int bind_port = URP_DEFAULT_PORT;
module_param(bind_port, int, 0444);
MODULE_PARM_DESC(bind_port, "Local RDMA port to bind (acceptor mode)");

static bool is_initiator(void)
{
	return listen_path[0] != '\0';
}

static const char *uds_path(void)
{
	return is_initiator() ? listen_path : connect_path;
}

static int __init urp_init(void)
{
	int ret;

	if (listen_path[0] == '\0' && connect_path[0] == '\0') {
		pr_err("urp: must specify listen_path (initiator) or connect_path (acceptor)\n");
		return -EINVAL;
	}

	if (listen_path[0] != '\0' && connect_path[0] != '\0') {
		pr_err("urp: specify only one of listen_path or connect_path\n");
		return -EINVAL;
	}

	if (is_initiator() && peer_address[0] == '\0') {
		pr_err("urp: initiator mode requires peer_address\n");
		return -EINVAL;
	}

	urp_ep = kzalloc(sizeof(*urp_ep), GFP_KERNEL);
	if (!urp_ep)
		return -ENOMEM;

	init_completion(&urp_ep->cm_done);
	urp_ep->is_initiator = is_initiator();

	ret = urp_proc_init();
	if (ret)
		goto err_free;

	ret = urp_rdma_init(urp_ep, peer_address, peer_port, bind_port,
			    is_initiator());
	if (ret)
		goto err_proc;

	ret = urp_socket_init(urp_ep, uds_path());
	if (ret)
		goto err_rdma;

	pr_info("urp: module loaded (%s mode, uds=%s)\n",
		is_initiator() ? "initiator" : "acceptor", uds_path());
	return 0;

err_rdma:
	urp_rdma_cleanup(urp_ep);
err_proc:
	urp_proc_cleanup();
err_free:
	kfree(urp_ep);
	urp_ep = NULL;
	return ret;
}

static void __exit urp_exit(void)
{
	if (!urp_ep)
		return;

	urp_pump_stop(urp_ep);
	urp_socket_cleanup(urp_ep);
	urp_rdma_cleanup(urp_ep);
	urp_proc_cleanup();
	kfree(urp_ep);
	urp_ep = NULL;

	pr_info("urp: module unloaded\n");
}

module_init(urp_init);
module_exit(urp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("UDS-RDMA Proxy Contributors");
MODULE_DESCRIPTION("UDS-RDMA Proxy kernel module");
MODULE_VERSION("0.0.1");
