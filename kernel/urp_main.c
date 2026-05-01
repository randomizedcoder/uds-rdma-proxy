// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) kernel module
 *
 * Tunnels Unix Domain Socket connections over RDMA (RoCEv2),
 * eliminating 2 of the 4 memory copies in the userspace proxy.
 *
 * Phase k0: Empty skeleton for build system validation.
 */

#include <linux/module.h>
#include <linux/kernel.h>

static int __init urp_init(void)
{
	pr_info("urp: module loaded\n");
	return 0;
}

static void __exit urp_exit(void)
{
	pr_info("urp: module unloaded\n");
}

module_init(urp_init);
module_exit(urp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("UDS-RDMA Proxy Contributors");
MODULE_DESCRIPTION("UDS-RDMA Proxy kernel module");
MODULE_VERSION("0.0.1");
