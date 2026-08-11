// SPDX-License-Identifier: GPL-2.0
/*
 * rx_seq_fuzz -- stateful sequence fuzzer for the urp RX decision pipeline
 * (design 27 F1, item 3). Feeds a fuzzer-chosen SEQUENCE of frames through the
 * REAL kernel C -- urp_classify_frame (kernel/urp_frame.c) -> the flag->event
 * dispatch -> urp_stream_next_state (kernel/urp_stream_sm.c) -- maintaining an
 * in-memory per-stream state table. This covers the COMPOSITION the
 * single-function harnesses (classify_fuzz, the KUnit/Rust tables) miss:
 * SYN then RST then DATA on a destroyed stream, id reuse, SYN|RST in one frame,
 * interleaved half-closes, etc.
 *
 * The live wire fuzzer (design 27 F2) drives this same path in-kernel but
 * blindly; this one is coverage-guided under -fsanitize=fuzzer,address,undefined
 * and hermetic (no VM), so it explores the state space fast.
 *
 * Oracles: ASAN/UBSan (no OOB/UB in the real C); plus decision invariants --
 * next_state never yields an out-of-range state, a rejected event never changes
 * state, and a delivered frame's payload fits its claimed byte_len (the 27.8
 * disclosure guard).
 *
 * Input: a stream of 24-byte records, each [4B byte_len][20B frame header].
 * byte_len is fuzzer-controlled independently of the header (as in classify_fuzz).
 */
#include "urp_fuzz_shim.h"
#include "urp_stream_sm.h"
#include <stddef.h>
#include <stdlib.h>

#define REC_SIZE	(4 + URP_FRAME_HEADER_SIZE)	/* 24 */
#define MAX_STREAMS	64

struct sm_stream {
	u32			id;
	enum urp_stream_state	state;
	int			used;
};

static struct sm_stream tab[MAX_STREAMS];

static struct sm_stream *sm_find(u32 id)
{
	int i;

	for (i = 0; i < MAX_STREAMS; i++)
		if (tab[i].used && tab[i].id == id)
			return &tab[i];
	return NULL;
}

/* Create-on-SYN mirrors urp_stream_rx_syn: a fresh inbound stream starts in
 * SYN_RECEIVED. Returns NULL if the (bounded) table is full.
 */
static struct sm_stream *sm_create(u32 id)
{
	int i;

	for (i = 0; i < MAX_STREAMS; i++)
		if (!tab[i].used) {
			tab[i].used = 1;
			tab[i].id = id;
			tab[i].state = URP_STREAM_STATE_SYN_RECEIVED;
			return &tab[i];
		}
	return NULL;
}

/* Apply one internal event, asserting the state-machine invariants. */
static void sm_apply(struct sm_stream *s, enum urp_stream_event ev)
{
	struct urp_stream_transition t = urp_stream_next_state(s->state, ev);

	/* Invariant 1: the next state is always a defined enum value. */
	if ((unsigned int)t.next > URP_STREAM_STATE_MAX)
		abort();
	/* Invariant 2: a rejected event must not change state. */
	if (!t.accepted && t.next != s->state)
		abort();

	s->state = t.next;
	if (t.actions & URP_STREAM_ACT_DESTROY)
		s->used = 0;		/* torn down; id may be reused later */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t off;

	/* Fresh table per input -> independent, reproducible runs. */
	memset(tab, 0, sizeof(tab));

	for (off = 0; off + REC_SIZE <= size; off += REC_SIZE) {
		u32 byte_len = get_unaligned_le32(data + off);
		struct urp_rx_decoded dec;
		enum urp_rx_action action;
		u8 *hdr;

		/* Exact-sized header buffer so any read past 20 bytes faults. */
		hdr = malloc(URP_FRAME_HEADER_SIZE);
		if (!hdr)
			return 0;
		memcpy(hdr, data + off + 4, URP_FRAME_HEADER_SIZE);

		action = urp_classify_frame(byte_len, hdr, &dec);

		/* 27.8 guard: a delivered frame's payload must fit byte_len. */
		if (action == URP_RX_DELIVER_LEGACY ||
		    action == URP_RX_DELIVER_STREAM) {
			if (byte_len < URP_FRAME_HEADER_SIZE ||
			    dec.payload_len > byte_len - URP_FRAME_HEADER_SIZE)
				abort();
		}

		if (action == URP_RX_DELIVER_STREAM && dec.stream_id != 0) {
			struct sm_stream *s = sm_find(dec.stream_id);

			/* Dispatch order mirrors urp_stream_rx_dispatch:
			 * SYN (create-or-advance), then RST, then FIN.
			 */
			if (dec.flags & URP_DATA_FLAG_SYN) {
				if (!s)
					s = sm_create(dec.stream_id);
				else
					sm_apply(s, URP_STREAM_EV_RX_SYN);
			}
			if (s && s->used && (dec.flags & URP_DATA_FLAG_RST))
				sm_apply(s, URP_STREAM_EV_RX_RST);
			if (s && s->used && (dec.flags & URP_DATA_FLAG_FIN))
				sm_apply(s, URP_STREAM_EV_RX_FIN);

			/* Occasionally drive the TX-side events (not wire-derived)
			 * so the TX_FIN/TX_RST transitions are covered too, keyed
			 * off the frame's credits field to stay fuzzer-controlled.
			 */
			if (s && s->used && (dec.credits & 1))
				sm_apply(s, URP_STREAM_EV_TX_FIN);
			if (s && s->used && (dec.credits & 2))
				sm_apply(s, URP_STREAM_EV_TX_RST);
		}

		free(hdr);
	}
	return 0;
}
