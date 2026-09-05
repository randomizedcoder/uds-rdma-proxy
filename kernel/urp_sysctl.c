// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- runtime tunables (design 33 Phase 1)
 *
 * Exposes the initiator connect-retry policy as operator knobs under
 * /proc/sys/urp/. These are global (site-wide) policy, not per-endpoint
 * configuration, so a sysctl is the right surface (endpoint config goes
 * through the GENL family; this module dropped module_param long ago).
 *
 *   connect_max_attempts    0 disables retry; else the bounded attempt budget
 *   connect_backoff_base_ms first backoff, doubled each attempt (min 1)
 *   connect_backoff_ceil_ms backoff ceiling the exponential saturates at
 *
 * The urp_rdma.c CM error path reads these live via urp_should_retry_connect /
 * urp_connect_backoff_ms, so `sysctl -w urp.connect_backoff_ceil_ms=500` takes
 * effect on the next retry with no reload.
 *
 * Portability (6.1 -> 7.x): register_sysctl("path", table) has a stable
 * signature across the range; only the ctl_table *terminating sentinel*
 * changed -- required through 6.10, removed in 6.11+ (the core now sizes the
 * array via ARRAY_SIZE, so a stray empty entry would register a bogus knob).
 * We gate just the sentinel on LINUX_VERSION_CODE and keep one register call.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "urp.h"
#include <linux/sysctl.h>
#include <linux/version.h>
#include <linux/limits.h>

/* Live, sysctl-writable tunables (declared extern in urp.h). */
unsigned int urp_connect_max_attempts	 = URP_CONNECT_MAX_ATTEMPTS_DEFAULT;
unsigned int urp_connect_backoff_base_ms = URP_CONNECT_BACKOFF_BASE_MS_DEFAULT;
unsigned int urp_connect_backoff_ceil_ms = URP_CONNECT_BACKOFF_CEIL_MS_DEFAULT;
/* gap #6 Phase 2: advertise byte-windowing capability (PR3: default on). */
unsigned int urp_window_bytes_advertise	 = URP_WINDOW_BYTES_ADVERTISE_DEFAULT;
/* gap #6 Phase 2 (PR3): per-stream byte window; clamped at stream create. */
unsigned int urp_window_bytes		 = URP_WINDOW_BYTES_DEFAULT;
/* design 40 §40.1: RX inter-arrival histogram sampling (default on). */
unsigned int urp_interarrival_hist	 = URP_INTERARRIVAL_HIST_DEFAULT;

/* proc_douintvec_minmax bounds (extra1/extra2 are void*, so non-const). */
static unsigned int urp_uint_zero;			/* 0 */
static unsigned int urp_uint_one = 1;
static unsigned int urp_uint_max = UINT_MAX;
/* gap #6 Phase 2 (PR3): byte-window bounds (mirror urp_window_clamp). */
static unsigned int urp_window_bytes_min = URP_WINDOW_BYTES_MIN;
static unsigned int urp_window_bytes_max = URP_WINDOW_BYTES_MAX;

static struct ctl_table urp_sysctls[] = {
	{
		.procname	= "connect_max_attempts",
		.data		= &urp_connect_max_attempts,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &urp_uint_zero,	/* 0 disables retry */
		.extra2		= &urp_uint_max,
	},
	{
		.procname	= "connect_backoff_base_ms",
		.data		= &urp_connect_backoff_base_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &urp_uint_one,	/* >=1: never a 0ms busy-retry */
		.extra2		= &urp_uint_max,
	},
	{
		.procname	= "connect_backoff_ceil_ms",
		.data		= &urp_connect_backoff_ceil_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &urp_uint_one,
		.extra2		= &urp_uint_max,
	},
	{
		/* gap #6 Phase 2: 0 = don't advertise byte-windowing (default),
		 * non-zero = advertise URP_CONN_CAP_WINDOW_BYTES in the trailer.
		 */
		.procname	= "window_bytes_advertise",
		.data		= &urp_window_bytes_advertise,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &urp_uint_zero,
		.extra2		= &urp_uint_one,
	},
	{
		/* gap #6 Phase 2 (PR3): per-stream byte window, applied at
		 * stream create (and re-clamped by urp_window_clamp there).
		 */
		.procname	= "window_bytes",
		.data		= &urp_window_bytes,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &urp_window_bytes_min,
		.extra2		= &urp_window_bytes_max,
	},
	{
		/* design 40 §40.1: 0 = disable RX inter-arrival histogram
		 * sampling (and its netlink nest), non-zero = enable (default).
		 */
		.procname	= "interarrival_hist",
		.data		= &urp_interarrival_hist,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &urp_uint_zero,
		.extra2		= &urp_uint_one,
	},
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	{ }	/* sentinel: required before 6.11, forbidden after */
#endif
};

static struct ctl_table_header *urp_sysctl_header;

int urp_sysctl_register(void)
{
	urp_sysctl_header = register_sysctl("urp", urp_sysctls);
	if (!urp_sysctl_header) {
		pr_err("register_sysctl(urp) failed\n");
		return -ENOMEM;
	}
	return 0;
}

void urp_sysctl_unregister(void)
{
	if (urp_sysctl_header) {
		unregister_sysctl_table(urp_sysctl_header);
		urp_sysctl_header = NULL;
	}
}
