/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp_frame.h -- pure wire-format codec + RX frame classifier.
 *
 * Split out of urp.h (design 28) so the codec inlines and the RX
 * classifier (design 28 E1) are free of kernel infrastructure and can be
 * compiled BOTH into the module and into a standalone userspace
 * libFuzzer harness (design 27 F1, nix/fuzz/). This is the surface where
 * the design 27 27.8 #1 length-guard bug lived, so it is the highest-value
 * thing to fuzz.
 *
 * Includer contract -- before including this header, provide:
 *   - fixed-width types u8/u16/u32/u64 (kernel: <linux/types.h>;
 *     fuzz shim: nix/fuzz/urp_fuzz_shim.h),
 *   - put_unaligned_le{16,32,64} / get_unaligned_le{16,32,64},
 *   - memcpy / memset,
 *   - the UAPI frame constants (URP_FRAME_HEADER_SIZE, URP_FRAME_TYPE_*,
 *     URP_CTRL_FLAG_CREDIT, ... from include/uapi/linux/urp.h),
 *   - URP_MAX_PAYLOAD (kernel: urp.h; fuzz shim: same value).
 * The kernel's urp.h satisfies all of these before including this file.
 */
#ifndef _URP_FRAME_H
#define _URP_FRAME_H

/* Frame encode/decode (inline, matches shared Rust crate wire format) */

/*
 * Wire format (20 bytes, little-endian):
 *   [0..4)   stream_id       u32
 *   [4..12)  sequence_number u64
 *   [12]     frame_type      u8
 *   [13]     flags           u8
 *   [14..16) credits_granted u16
 *   [16..20) payload_length  u32
 */

static inline void urp_frame_encode(void *buf, u32 stream_id, u64 seq,
				    u8 frame_type, u8 flags,
				    u16 credits, u32 payload_len)
{
	u8 *p = buf;

	put_unaligned_le32(stream_id, p);
	put_unaligned_le64(seq, p + 4);
	p[12] = frame_type;
	p[13] = flags;
	put_unaligned_le16(credits, p + 14);
	put_unaligned_le32(payload_len, p + 16);
}

static inline u32 urp_frame_decode_payload_len(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le32(p + 16);
}

static inline u32 urp_frame_decode_stream_id(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le32(p);
}

static inline u16 urp_frame_decode_credits(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le16(p + 14);
}

/*
 * QP health probe wire payloads (Phase 3b, design 08a section 8a.2).
 * Carried inside a frame_type == URP_FRAME_TYPE_PROBE frame; flags
 * distinguish PING (0) from PONG (URP_PROBE_FLAG_PONG).
 *
 * PING (32 bytes, little-endian):
 *   [0..4)   probe_seq        u32
 *   [4..6)   qp_index         u16
 *   [6]      clock_flags      u8
 *   [7]      reserved         u8
 *   [8..16)  t_send_mono      u64  (initiator's CLOCK_MONOTONIC at send)
 *   [16..24) t_send_real      u64  (initiator's CLOCK_REALTIME at send)
 *   [24..32) padding          u64
 *
 * PONG (48 bytes): same layout for fields 0..16 as PING (echoed),
 *   then [24..32) t_recv_real, [32..40) t_pong_mono, [40..48) t_pong_real.
 *
 * Sizes match crates/uds-rdma-protocol/src/probe.rs.
 */
#define URP_PING_PAYLOAD_SIZE	32
#define URP_PONG_PAYLOAD_SIZE	48
#define URP_PROBE_FLAG_PONG	BIT(0)

static inline void urp_ping_encode(void *buf, u32 probe_seq, u16 qp_index,
				   u64 t_send_mono, u64 t_send_real)
{
	u8 *p = buf;

	put_unaligned_le32(probe_seq, p);
	put_unaligned_le16(qp_index, p + 4);
	p[6] = 0;	/* clock_flags */
	p[7] = 0;	/* reserved */
	put_unaligned_le64(t_send_mono, p + 8);
	put_unaligned_le64(t_send_real, p + 16);
	put_unaligned_le64(0, p + 24);
}

static inline u32 urp_ping_decode_seq(const void *buf)
{
	return get_unaligned_le32((const u8 *)buf);
}

static inline u16 urp_ping_decode_qp_index(const void *buf)
{
	return get_unaligned_le16((const u8 *)buf + 4);
}

static inline u64 urp_ping_decode_t_send_mono(const void *buf)
{
	return get_unaligned_le64((const u8 *)buf + 8);
}

static inline u64 urp_ping_decode_t_send_real(const void *buf)
{
	return get_unaligned_le64((const u8 *)buf + 16);
}

/* Encode a PONG by echoing the PING fields and appending responder
 * timestamps. ping must point to the received PING payload.
 */
static inline void urp_pong_encode(void *buf, const void *ping,
				   u64 t_recv_real, u64 t_pong_mono,
				   u64 t_pong_real)
{
	u8 *p = buf;
	const u8 *q = ping;

	/* Echo PING [0..24) -- probe_seq, qp_index, clock_flags, reserved,
	 * t_send_mono, t_send_real.
	 */
	memcpy(p, q, 24);
	put_unaligned_le64(t_recv_real, p + 24);
	put_unaligned_le64(t_pong_mono, p + 32);
	put_unaligned_le64(t_pong_real, p + 40);
}

static inline u64 urp_pong_decode_t_pong_mono(const void *buf)
{
	return get_unaligned_le64((const u8 *)buf + 32);
}

static inline u64 urp_frame_decode_seq(const void *buf)
{
	const u8 *p = buf;

	return get_unaligned_le64(p + 4);
}

static inline u8 urp_frame_decode_type(const void *buf)
{
	const u8 *p = buf;

	return p[12];
}

static inline u8 urp_frame_decode_flags(const void *buf)
{
	const u8 *p = buf;

	return p[13];
}

/*
 * Pure RX frame classifier (design 28 E1). Extracts the classify/validate/
 * route decision out of urp_recv_done so it can be table-tested in KUnit
 * without an ib_wc / DMA buffer, fuzzed in userspace (design 27 F1), and
 * diffed against the Rust frame model. Given the received length and the
 * 20 header bytes, it decodes the header fields into @out and returns the
 * action urp_recv_done must take. All the length-vs-payload security guards
 * (design 27 27.8 #1) live here.
 */
enum urp_rx_action {
	URP_RX_DROP_SHORT,		/* byte_len < header: header itself stale */
	URP_RX_DROP_OVERSIZE,		/* payload_len > URP_MAX_PAYLOAD */
	URP_RX_DROP_PAYLOAD_OVERRUN,	/* payload_len > bytes received */
	URP_RX_DROP_SHORT_PROBE,	/* PROBE shorter than a ping payload */
	URP_RX_CREDIT,			/* CONTROL frame: apply peer's credit grant */
	URP_RX_PROBE_PING,		/* PROBE ping: emit a PONG */
	URP_RX_PROBE_PONG,		/* PROBE pong: RTT / EWMA update */
	URP_RX_DELIVER_LEGACY,		/* DATA stream_id == 0: ep->conn */
	URP_RX_DELIVER_STREAM,		/* DATA stream_id != 0: per-stream */
};

struct urp_rx_decoded {
	u32	payload_len;
	u32	stream_id;
	u16	credits;
	u8	type;
	u8	flags;
};

enum urp_rx_action
urp_classify_frame(u32 byte_len, const u8 *hdr, struct urp_rx_decoded *out);

#endif /* _URP_FRAME_H */
