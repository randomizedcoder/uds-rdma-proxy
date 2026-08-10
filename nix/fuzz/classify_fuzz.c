// SPDX-License-Identifier: GPL-2.0
/*
 * libFuzzer harness for urp_classify_frame (design 27 F1 / design 28 E1).
 *
 * The classifier is the RX frame validation surface where the design 27
 * 27.8 #1 kernel-memory-disclosure bug lived. This fuzzes the REAL kernel
 * C (kernel/urp_frame.c), not a reimplementation.
 *
 * Input layout: first 4 bytes = the attacker-claimed received length
 * (byte_len), fully fuzzer-controlled and independent of the real buffer;
 * the next 20 bytes = the frame header. The header is copied into an
 * exactly-sized, ASAN-red-zoned heap buffer so ANY read past the 20-byte
 * header (the 27.8 pattern) trips AddressSanitizer -- regardless of what
 * byte_len claims. The classifier must be memory-safe and must never crash
 * for any (byte_len, header).
 */
#include "urp_fuzz_shim.h"
#include <stddef.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	u32 byte_len;
	u8 *hdr;
	struct urp_rx_decoded dec;
	enum urp_rx_action action;

	if (size < 4 + URP_FRAME_HEADER_SIZE)
		return 0;

	byte_len = get_unaligned_le32(data);

	/* Exact-sized allocation so an over-read past the header faults. */
	hdr = malloc(URP_FRAME_HEADER_SIZE);
	if (!hdr)
		return 0;
	memcpy(hdr, data + 4, URP_FRAME_HEADER_SIZE);

	action = urp_classify_frame(byte_len, hdr, &dec);

	/* Cross-check the invariant the caller relies on: whenever the frame
	 * is routed for delivery, the declared payload must fit within the
	 * claimed received length (the 27.8 guard). If this ever fails the
	 * classifier has regressed into re-introducing the disclosure.
	 */
	if (action == URP_RX_DELIVER_LEGACY ||
	    action == URP_RX_DELIVER_STREAM) {
		if (byte_len < URP_FRAME_HEADER_SIZE ||
		    dec.payload_len > byte_len - URP_FRAME_HEADER_SIZE)
			abort();
	}

	free(hdr);
	return 0;
}
