/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure byte-windowing flow-control decisions (status.md gap #6 Phase 2, PR3;
 * design 35 §35.3). Split out -- like urp_conn_plan.h / urp_credit_plan.h --
 * so the arithmetic compiles both in the kernel and standalone in userspace
 * (nix check urp-window-units) and is table-tested in KUnit, with no sockets /
 * kthreads / RDMA. Keep in lock-step with the KUnit cases in kernel/urp_test.c.
 *
 * Model (per stream, single sender kthread + single receive-completion
 * context): the sender may have at most @window_bytes of DATA payload in flight
 * -- sent but not yet acked by a cumulative CREDIT-BYTES grant. tx_bytes (sent)
 * and rx_bytes (delivered to the app) are the existing per-stream counters;
 * tx_bytes_acked is the high-water grant the peer has echoed. Grants are
 * cumulative-absolute and applied with max(), so a lost / reordered / duplicate
 * grant is idempotent and self-healing.
 */
#ifndef _URP_WINDOW_H
#define _URP_WINDOW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
#endif

/*
 * Window sizing (design 35 §35.3, scalability revision). The per-stream window
 * is resolved from the bandwidth-delay product so it tracks the link: BDP =
 * link_rate x RTT, sized from ib_query_port's active_speed x width and the
 * per-QP rtt_ewma_ns. This auto-scales 25 -> 800 GbE (a fixed 64 MiB ceiling
 * couldn't hold an 800 GbE / 1 ms BDP of ~100 MB). The sysctl (urp.window_bytes)
 * is a floor/override; a frame-count floor (URP_WINDOW_TARGET_FRAMES) keeps
 * large frames pipelined when the network RTT is tiny; the whole thing is capped
 * by the peer's recv pool (num_bufs/2 buffers) and the [MIN,MAX] range. MAX is a
 * generous sanity ceiling -- the recv pool is the real bound, so high-speed
 * deployments scale buffer_count to admit their BDP.
 */
#define URP_WINDOW_BYTES_DEFAULT	(1UL << 20)	/* 1 MiB (sysctl floor) */
#define URP_WINDOW_BYTES_MIN		(1UL << 16)	/* 64 KiB */
#define URP_WINDOW_BYTES_MAX		(1UL << 30)	/* 1 GiB (covers 800 GbE BDP) */

/*
 * Fallback RTT for BDP sizing before PROBE/PONG has populated rtt_ewma_ns (a
 * freshly-created stream may not have a sample yet). 100 us is a conservative
 * RoCEv2 datacenter round-trip; once real samples arrive, later streams size
 * from them. Under-estimating only falls back to the frame-count floor, never to
 * zero.
 */
#define URP_WINDOW_FALLBACK_RTT_NS	100000ULL	/* 100 us */

/* Emit a fresh CREDIT-BYTES grant once the receiver has delivered another
 * window/DIVISOR bytes to the app -- amortizes control-frame overhead while
 * keeping the sender's window from draining (design 35 §35.3).
 */
#define URP_WINDOW_GRANT_DIVISOR	4

/*
 * Pipelining floor: a stream's window must admit at least this many max-size
 * frames, else a large frame degenerates to stop-and-wait (one frame in flight,
 * then block a full grant round-trip -- HW-measured: a 1 MiB frame into a fixed
 * 1 MiB window ran at ~800 MB/s, vs ~1090 once several frames pipeline). The
 * fixed sysctl window is sized for the common (small) frame; large slots scale
 * the window up so the pipeline stays full, bounded by the peer's recv pool and
 * the [MIN,MAX] range. See urp_window_for_stream().
 */
#define URP_WINDOW_TARGET_FRAMES	8u

/*
 * Mandatory reorder coupling (design 35 §35.3): the per-stream reorder buffer
 * must be able to hold every frame the window admits, else a well-behaved
 * in-window burst would be dropped. The count is window / MIN_FRAME, where
 * MIN_FRAME is a conservative floor on the wire frame's payload -- it must be
 * SMALLER than the real minimum, or the cap under-sizes and drops. A 64-byte
 * buffer_size yields ~44-byte payloads, so ~window/44 frames are in flight;
 * MIN_FRAME=16 keeps the cap (window/16) safely above that for any realistic
 * frame. Reorder nodes are allocated on demand (kmalloc per insert), so a high
 * cap costs nothing until actually buffered, and the byte window bounds the
 * buffered bytes regardless. Clamp to a sane [MIN, MAX] entry range (MIN
 * preserves the historical 256 default).
 */
#define URP_REORDER_MIN_FRAME		16u
#define URP_REORDER_MIN_ENTRIES		256u
#define URP_REORDER_MAX_ENTRIES		65536u

/* Clamp a configured window-bytes value into the supported range. */
static inline u64 urp_window_clamp(u64 v)
{
	if (v < URP_WINDOW_BYTES_MIN)
		return URP_WINDOW_BYTES_MIN;
	if (v > URP_WINDOW_BYTES_MAX)
		return URP_WINDOW_BYTES_MAX;
	return v;
}

/*
 * Resolve a stream's byte window (design 35 §35.3, scalability revision). The
 * window is the largest of three sizings, so it serves every regime:
 *   - BDP = @link_mbps x @rtt_ns  -- the bandwidth-delay product; this is what
 *     scales the window with the NIC (25 -> 800 GbE) and the path RTT.
 *   - URP_WINDOW_TARGET_FRAMES x @buf_size  -- a pipelining floor so a large
 *     frame stays pipelined even when the network RTT is tiny (the copy path's
 *     grant round-trip is dominated by app delivery, not the wire RTT).
 *   - @sysctl_window  -- an operator floor / override.
 * It is then capped by the peer's recv-pool bytes (num_bufs/2 buffers of
 * @buf_size) -- never admit more in-flight frames than the receiver can hold --
 * and clamped to [MIN,MAX]. Pure (no sockets/RDMA), table-tested in KUnit +
 * urp-window-units. @link_mbps or @rtt_ns == 0 simply drops the BDP term.
 */
static inline u64 urp_window_for_stream(u64 sysctl_window, u32 buf_size,
					u32 num_bufs, u32 link_mbps, u64 rtt_ns)
{
	u64 recv_pool = (u64)(num_bufs / 2) * buf_size;
	u64 frame_floor = (u64)URP_WINDOW_TARGET_FRAMES * buf_size;
	/* BDP bytes = link_mbps(1e6 bit/s) * rtt_ns(1e-9 s) / 8 = *rtt/8000. */
	u64 bdp = (u64)link_mbps * rtt_ns / 8000;
	u64 w = sysctl_window;

	if (bdp > w)
		w = bdp;
	if (frame_floor > w)
		w = frame_floor;
	w = urp_window_clamp(w);		/* [MIN, MAX] */
	if (w > recv_pool)			/* never exceed peer recv capacity */
		w = recv_pool;
	return w;
}

/*
 * May the sender post a @len-byte DATA frame now? @sent and @acked are the
 * stream's cumulative tx and acked byte counts (sent >= acked always). Room
 * exists when the resulting in-flight total fits the window. A frame is always
 * allowed when nothing is in flight (inflight == 0) so a single frame larger
 * than the window still makes progress -- never deadlocks.
 */
static inline bool
urp_window_has_room(u64 sent, u64 acked, u64 window, u32 len)
{
	u64 inflight = sent - acked;

	if (inflight == 0)
		return true;
	return inflight + (u64)len <= window;
}

/*
 * Should the receiver emit a CREDIT-BYTES grant now? True once it has delivered
 * another window/DIVISOR bytes to the app since @last_granted. @window is read
 * live; guard divide-by-zero defensively (window is always clamped >= MIN).
 */
static inline bool
urp_window_should_grant(u64 delivered, u64 last_granted, u64 window)
{
	u64 step = window / URP_WINDOW_GRANT_DIVISOR;

	if (step == 0)
		step = 1;
	return delivered - last_granted >= step;
}

/*
 * Apply a cumulative-absolute grant: the new acked high-water is the max of the
 * current value and the granted value. Monotonic and idempotent, so a lost /
 * reordered / duplicate grant never rewinds the window (self-healing).
 */
static inline u64 urp_window_apply_grant(u64 cur_acked, u64 granted)
{
	return granted > cur_acked ? granted : cur_acked;
}

/*
 * Reorder entry-count cap coupled to the window (see URP_REORDER_* above).
 */
static inline u32 urp_reorder_depth_for_window(u64 window)
{
	u64 d = window / URP_REORDER_MIN_FRAME;

	if (d < URP_REORDER_MIN_ENTRIES)
		return URP_REORDER_MIN_ENTRIES;
	if (d > URP_REORDER_MAX_ENTRIES)
		return URP_REORDER_MAX_ENTRIES;
	return (u32)d;
}

#endif /* _URP_WINDOW_H */
