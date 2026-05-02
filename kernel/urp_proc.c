// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) — /proc/urp/stats
 *
 * Phase k0: Simple counters for tx_bytes, rx_bytes, tx_frames, rx_frames,
 * connections.
 */

#include "urp.h"

static struct proc_dir_entry *urp_proc_dir;

static int urp_stats_show(struct seq_file *m, void *v)
{
	struct urp_endpoint *ep = urp_ep;

	if (!ep) {
		seq_puts(m, "no endpoint\n");
		return 0;
	}

	seq_printf(m, "tx_bytes:     %lld\n",
		   atomic64_read(&ep->stats.tx_bytes));
	seq_printf(m, "rx_bytes:     %lld\n",
		   atomic64_read(&ep->stats.rx_bytes));
	seq_printf(m, "tx_frames:    %lld\n",
		   atomic64_read(&ep->stats.tx_frames));
	seq_printf(m, "rx_frames:    %lld\n",
		   atomic64_read(&ep->stats.rx_frames));
	seq_printf(m, "connections:  %lld\n",
		   atomic64_read(&ep->stats.connections));
	seq_printf(m, "connected:    %s\n",
		   ep->connected ? "yes" : "no");
	seq_printf(m, "active:       %s\n",
		   ep->conn.active ? "yes" : "no");

	return 0;
}

static int urp_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, urp_stats_show, NULL);
}

static const struct proc_ops urp_stats_ops = {
	.proc_open    = urp_stats_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

int urp_proc_init(void)
{
	urp_proc_dir = proc_mkdir(URP_PROC_DIR, NULL);
	if (!urp_proc_dir) {
		pr_err("urp: failed to create /proc/%s\n", URP_PROC_DIR);
		return -ENOMEM;
	}

	if (!proc_create(URP_PROC_STATS, 0444, urp_proc_dir, &urp_stats_ops)) {
		pr_err("urp: failed to create /proc/%s/%s\n",
		       URP_PROC_DIR, URP_PROC_STATS);
		proc_remove(urp_proc_dir);
		urp_proc_dir = NULL;
		return -ENOMEM;
	}

	return 0;
}

void urp_proc_cleanup(void)
{
	if (urp_proc_dir) {
		proc_remove(urp_proc_dir);
		urp_proc_dir = NULL;
	}
}
