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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"
#include "urp_cmd.h"

static int __init urp_init(void)
{
	int ret;

	ret = urp_endpoint_table_init();
	if (ret) {
		pr_err("endpoint table init failed: %d\n", ret);
		return ret;
	}

	ret = urp_proc_init();
	if (ret)
		goto err_table;

	/*
	 * Open the fast-path char device (design 31) before GENL so an aware
	 * app can REGISTER a pool the moment the module is live. It shares no
	 * state with the GENL surface, so ordering vs GENL is not critical --
	 * we tear it down symmetrically in exit.
	 */
	ret = urp_cmd_dev_register();
	if (ret)
		goto err_proc;

	ret = urp_genl_register();
	if (ret) {
		pr_err("genl_register_family failed: %d\n", ret);
		goto err_cmd;
	}

	pr_info("module loaded (idle; configure via `urp add`)\n");
	return 0;

err_cmd:
	urp_cmd_dev_unregister();
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
	urp_cmd_dev_unregister();
	urp_proc_cleanup();
	urp_endpoint_table_destroy();
	pr_info("module unloaded\n");
}

module_init(urp_init);
module_exit(urp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dave Seddon <dave.seddon.ca@gmail.com>");
MODULE_DESCRIPTION("UDS-RDMA Proxy kernel module");
MODULE_VERSION("0.0.2");
