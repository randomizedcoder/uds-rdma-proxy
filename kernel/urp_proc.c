// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- /proc/urp/<name>/stats
 *
 * Phase 2: per-endpoint subdirectories. Each endpoint gets a /proc/urp/<name>/
 * directory containing a "stats" file. The endpoint pointer is attached as
 * pde_data on the stats inode so urp_stats_show can resolve it without
 * holding any global state.
 */

#include "urp.h"

static struct proc_dir_entry *urp_proc_dir;

static int urp_stats_show(struct seq_file *m, void *v)
{
	struct urp_endpoint *ep = m->private;

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
	return single_open(file, urp_stats_show, pde_data(inode));
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

	return 0;
}

void urp_proc_cleanup(void)
{
	if (urp_proc_dir) {
		proc_remove(urp_proc_dir);
		urp_proc_dir = NULL;
	}
}

int urp_endpoint_proc_create(struct urp_endpoint *ep)
{
	struct proc_dir_entry *dir, *stats;

	if (!urp_proc_dir)
		return -ENOENT;

	dir = proc_mkdir(ep->name, urp_proc_dir);
	if (!dir) {
		pr_err("urp: failed to create /proc/%s/%s\n",
		       URP_PROC_DIR, ep->name);
		return -ENOMEM;
	}

	stats = proc_create_data(URP_PROC_STATS, 0444, dir, &urp_stats_ops, ep);
	if (!stats) {
		pr_err("urp: failed to create /proc/%s/%s/%s\n",
		       URP_PROC_DIR, ep->name, URP_PROC_STATS);
		proc_remove(dir);
		return -ENOMEM;
	}

	ep->proc_dir = dir;
	return 0;
}

void urp_endpoint_proc_remove(struct urp_endpoint *ep)
{
	if (ep->proc_dir) {
		proc_remove(ep->proc_dir);
		ep->proc_dir = NULL;
	}
}
