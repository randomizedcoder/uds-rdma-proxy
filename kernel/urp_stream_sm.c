// SPDX-License-Identifier: GPL-2.0
/*
 * Pure stream state-machine core (design 28 E2, design 27 F1).
 *
 * urp_stream_next_state() lives here, split out of urp_stream.c, so it
 * compiles both in the kernel and standalone in userspace against the fuzz
 * shim (nix/fuzz/rx_seq_fuzz.c). It touches no sockets/kthreads/rhashtable --
 * a pure (state, event) -> transition function. Keep in lock-step with the
 * Rust twin (crates/uds-rdma-protocol/src/stream.rs) and the KUnit table.
 */
#ifdef __KERNEL__
#include "urp.h"
#else
#include "urp_fuzz_shim.h"
#include "urp_stream_sm.h"
#endif

/*
 * Note: the SYN that *creates* an inbound stream is a distinct event
 * (create -> SYN_RECEIVED), handled in urp_stream_rx_syn; this function models
 * RX_SYN on an already-known stream (idempotent handshake advance to
 * ESTABLISHED, or reject on a closing/closed stream).
 */
struct urp_stream_transition
urp_stream_next_state(enum urp_stream_state cur, enum urp_stream_event ev)
{
	struct urp_stream_transition t = {
		.next = cur, .actions = 0, .accepted = true,
	};

	switch (ev) {
	case URP_STREAM_EV_RX_SYN:
		if (cur == URP_STREAM_STATE_SYN_SENT ||
		    cur == URP_STREAM_STATE_SYN_RECEIVED ||
		    cur == URP_STREAM_STATE_ESTABLISHED)
			t.next = URP_STREAM_STATE_ESTABLISHED;
		else
			t.accepted = false;	/* -EEXIST: SYN on closing/closed */
		break;
	case URP_STREAM_EV_RX_FIN:
		/* Peer half-close: always SHUT_WR the local UDS; advance the
		 * two states with a defined FIN transition.
		 */
		t.actions = URP_STREAM_ACT_SHUTDOWN_WR;
		if (cur == URP_STREAM_STATE_ESTABLISHED)
			t.next = URP_STREAM_STATE_CLOSE_WAIT;
		else if (cur == URP_STREAM_STATE_FIN_WAIT)
			t.next = URP_STREAM_STATE_CLOSED;
		break;
	case URP_STREAM_EV_RX_RST:
		t.next = URP_STREAM_STATE_CLOSED;
		t.actions = URP_STREAM_ACT_SHUTDOWN_RDWR |
			    URP_STREAM_ACT_DESTROY;
		break;
	case URP_STREAM_EV_TX_FIN:
		if (cur == URP_STREAM_STATE_ESTABLISHED)
			t.next = URP_STREAM_STATE_FIN_WAIT;
		else if (cur == URP_STREAM_STATE_CLOSE_WAIT)
			t.next = URP_STREAM_STATE_CLOSED;
		break;
	case URP_STREAM_EV_TX_RST:
		t.next = URP_STREAM_STATE_CLOSED;
		t.actions = URP_STREAM_ACT_SHUTDOWN_RDWR;
		break;
	}
	return t;
}
