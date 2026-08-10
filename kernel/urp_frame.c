// SPDX-License-Identifier: GPL-2.0
/*
 * urp_frame.c -- pure RX frame classifier (design 28 E1).
 *
 * Compiled both into urp.ko (Kbuild) and into the standalone userspace
 * libFuzzer harness (design 27 F1, nix/fuzz/). The __KERNEL__ split picks
 * the right way to satisfy urp_frame.h's includer contract: the module
 * pulls the real kernel primitives via urp.h; the fuzzer pulls a small
 * userspace shim.
 */
#ifdef __KERNEL__
#include "urp.h"
#else
#include "urp_fuzz_shim.h"
#endif

/*
 * See urp_frame.h for the contract. Table-tested by urp_test.c
 * test_classify_frame; fuzzed by nix/fuzz classify_fuzz. The length guards
 * here are the design 27 27.8 #1 fix (never route more than was received).
 *
 * Note: an unknown frame_type (3..255) is routed to the DATA path -- this
 * preserves the historical behaviour; rejecting unknown types is a
 * separate behaviour change, not part of this extraction.
 */
enum urp_rx_action
urp_classify_frame(u32 byte_len, const u8 *hdr, struct urp_rx_decoded *out)
{
	memset(out, 0, sizeof(*out));

	/* A frame shorter than the header would read the header itself from
	 * stale pool memory -- reject before decoding anything.
	 */
	if (byte_len < URP_FRAME_HEADER_SIZE)
		return URP_RX_DROP_SHORT;

	out->payload_len = urp_frame_decode_payload_len(hdr);
	out->stream_id = urp_frame_decode_stream_id(hdr);
	out->type = urp_frame_decode_type(hdr);
	out->flags = urp_frame_decode_flags(hdr);
	out->credits = urp_frame_decode_credits(hdr);

	if (out->payload_len > URP_MAX_PAYLOAD)
		return URP_RX_DROP_OVERSIZE;
	/* The declared payload must fit within the bytes actually received;
	 * otherwise we'd route stale DMA-pool memory to the local app.
	 */
	if (out->payload_len > byte_len - URP_FRAME_HEADER_SIZE)
		return URP_RX_DROP_PAYLOAD_OVERRUN;

	if (out->type == URP_FRAME_TYPE_CONTROL)
		return URP_RX_CREDIT;

	if (out->type == URP_FRAME_TYPE_PROBE) {
		/* PROBE handlers read fixed payload offsets without consulting
		 * payload_len, so require a full ping payload of received bytes.
		 */
		if (byte_len < URP_FRAME_HEADER_SIZE + URP_PING_PAYLOAD_SIZE)
			return URP_RX_DROP_SHORT_PROBE;
		return (out->flags & URP_PROBE_FLAG_PONG) ?
			URP_RX_PROBE_PONG : URP_RX_PROBE_PING;
	}

	return out->stream_id == 0 ?
		URP_RX_DELIVER_LEGACY : URP_RX_DELIVER_STREAM;
}
