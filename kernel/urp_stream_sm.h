/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure stream state-machine core (design 28 E2, design 27 F1).
 *
 * Extracted from urp_stream.c so the SYN/FIN/RST transition logic can be
 * table-tested in KUnit AND fuzzed in userspace (nix/fuzz/rx_seq_fuzz.c)
 * without sockets / kthreads / rhashtable. Mirrored 1:1 by the Rust `stream`
 * module (crates/uds-rdma-protocol/src/stream.rs) for a differential check --
 * keep the two in lock-step.
 *
 * This header carries only the pure decision types + prototype. It depends on:
 *   - enum urp_stream_state (UAPI, include/uapi/linux/urp.h)
 *   - fixed-width u32 + bool + BIT()
 * all supplied by the includer: urp.h in the kernel build, urp_fuzz_shim.h in
 * the userspace fuzz build. It includes no kernel headers of its own.
 */
#ifndef _URP_STREAM_SM_H
#define _URP_STREAM_SM_H

/*
 * Events are internal (not on the wire). urp_stream_next_state() is a pure
 * function of (current state, event): it returns the next state, a bitmask of
 * side effects the caller must apply (socket shutdown / destroy), and whether
 * the event was accepted (RX_SYN on a closing/closed stream is not).
 */
enum urp_stream_event {
	URP_STREAM_EV_RX_SYN,
	URP_STREAM_EV_RX_FIN,
	URP_STREAM_EV_RX_RST,
	URP_STREAM_EV_TX_FIN,
	URP_STREAM_EV_TX_RST,
};

#define URP_STREAM_ACT_SHUTDOWN_WR	BIT(0)	/* kernel_sock_shutdown SHUT_WR */
#define URP_STREAM_ACT_SHUTDOWN_RDWR	BIT(1)	/* kernel_sock_shutdown SHUT_RDWR */
#define URP_STREAM_ACT_DESTROY		BIT(2)	/* urp_stream_destroy */

struct urp_stream_transition {
	enum urp_stream_state	next;
	u32			actions;	/* URP_STREAM_ACT_* bitmask */
	bool			accepted;	/* false => event invalid in state */
};

struct urp_stream_transition
urp_stream_next_state(enum urp_stream_state cur, enum urp_stream_event ev);

#endif /* _URP_STREAM_SM_H */
