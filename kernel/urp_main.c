// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) kernel module -- main entry point
 *
 * Phase 2: the module loads idle. Endpoints are created at runtime via
 * the "urp" generic netlink family (see urp_netlink.c). All Phase 1
 * module_param configuration has been removed.
 *
 * Initialization order matters:
 *   urp_endpoint_table_init  -> install rhashtable so lookups never see NULL
 *   urp_proc_init            -> create /proc/urp/ root (subdirs added per-ep)
 *   urp_genl_register        -> open the netlink control surface LAST
 *
 * Teardown reverses that order, draining all live endpoints before the
 * GENL family is unregistered so racing CLI calls fail cleanly with
 * -ENODEV rather than touching half-freed state.
 */

#include "urp.h"

static int __init urp_init(void)
{
	int ret;

	ret = urp_endpoint_table_init();
	if (ret) {
		pr_err("urp: endpoint table init failed: %d\n", ret);
		return ret;
	}

	ret = urp_proc_init();
	if (ret)
		goto err_table;

	ret = urp_genl_register();
	if (ret) {
		pr_err("urp: genl_register_family failed: %d\n", ret);
		goto err_proc;
	}

	pr_info("urp: module loaded (idle; configure via `urp add`)\n");
	return 0;

err_proc:
	urp_proc_cleanup();
err_table:
	urp_endpoint_table_destroy();
	return ret;
}

static void __exit urp_exit(void)
{
	/*
	 * Unregister GENL FIRST so no new requests can target endpoints we
	 * are about to drain. urp_endpoint_drain_all then walks the
	 * rhashtable, drains each endpoint, and waits for RCU-deferred
	 * frees before destroying the table.
	 */
	urp_genl_unregister();
	urp_endpoint_drain_all();
	urp_proc_cleanup();
	urp_endpoint_table_destroy();
	pr_info("urp: module unloaded\n");
}

module_init(urp_init);
module_exit(urp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("UDS-RDMA Proxy Contributors");
MODULE_DESCRIPTION("UDS-RDMA Proxy kernel module");
MODULE_VERSION("0.0.2");
