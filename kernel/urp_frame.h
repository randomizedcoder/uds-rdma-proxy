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
 * URP_PING_PAYLOAD_SIZE (32) and URP_PONG_PAYLOAD_SIZE (48) are wire constants
 * defined in include/uapi/linux/urp.h (they also set URP_BUFFER_SIZE_MIN); the
 * kernel's urp.h pulls that in before this file. Sizes match
 * crates/uds-rdma-protocol/src/probe.rs.
 */
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

/*
 * How the zero-copy fast RECV path (urp_fast_recv_done) must dispose of a
 * classified frame. The copy path routes everything through the pump; the fast
 * path has no pump, so it handles the peer's liveness/flow-control protocol
 * inline and delivers only DATA to the app. Pure so the completion handler and
 * KUnit agree on the mapping (design 31 D1 + design 33 recv-path PONG).
 */
enum urp_fast_rx_disp {
	URP_FAST_RX_DELIVER,	/* DATA: hand the payload to the app */
	URP_FAST_RX_PONG,	/* PROBE PING: emit a PONG, then empty completion */
	URP_FAST_RX_ABSORB,	/* PROBE PONG / CREDIT: swallow, empty completion */
	URP_FAST_RX_REJECT,	/* malformed: surface -EBADMSG to the app */
};

static inline enum urp_fast_rx_disp
urp_fast_rx_disposition(enum urp_rx_action action)
{
	switch (action) {
	case URP_RX_DELIVER_STREAM:
	case URP_RX_DELIVER_LEGACY:
		return URP_FAST_RX_DELIVER;
	case URP_RX_PROBE_PING:
		return URP_FAST_RX_PONG;
	case URP_RX_PROBE_PONG:
	case URP_RX_CREDIT:
		return URP_FAST_RX_ABSORB;
	default:
		return URP_FAST_RX_REJECT;
	}
}

/*
 * CM private_data kind-advertisement trailer (design 31 D1 interop). Each side
 * appends [MAGIC0][MAGIC1][kind] AFTER its optional PSK auth bytes, so:
 *   - an old peer's fixed-offset auth memcmp (bytes [0..auth_len)) is unaffected,
 *   - a peer that sends no trailer (old build) reads back as "kind unknown",
 *     which the caller treats as UDS -- the safe, probe-as-before default.
 * A uds *initiator* uses the peer's advertised kind to suppress its keepalive
 * probe against a *fast* acceptor, whose PONG a pumpless fast endpoint cannot
 * answer during bring-up (design 33 silent-drop churn otherwise). The trailer
 * sits at offset @auth_len; auth is symmetric for a successful connection, so a
 * receiver locates it using its own auth length.
 */
#define URP_CONN_PRIV_MAGIC0		0x55	/* 'U' */
#define URP_CONN_PRIV_MAGIC1		0x52	/* 'R' */
#define URP_CONN_PRIV_TRAILER_LEN	3	/* magic0, magic1, kind */
/*
 * gap #6 Phase 1: a wider trailer the *initiator* appends when num_qps > 1 so
 * the acceptor can allocate the target ep->qps[] slot by peer identity instead
 * of a monotonic CONNECT_REQUEST counter (which races the per-QP retry storm).
 * [MAGIC0][MAGIC1][kind][qp_index]. num_qps is capped at URP_NUM_QPS_MAX == 32,
 * so a u8 index is always sufficient. The base 3-byte trailer (kind only) is
 * unchanged, so the acceptor's rdma_accept reply and every single-QP / fast
 * path keep the exact legacy wire; only a multi-QP connect carries byte 4.
 */
#define URP_CONN_PRIV_QP_TRAILER_LEN	4	/* magic0, magic1, kind, qp_index */

/*
 * Append the kind trailer after @auth_len existing bytes in @buf; return the new
 * total private_data length. @buf must have room for auth_len + trailer.
 */
static inline u8 urp_conn_priv_build(u8 *buf, u8 auth_len, u8 kind)
{
	buf[auth_len]	  = URP_CONN_PRIV_MAGIC0;
	buf[auth_len + 1] = URP_CONN_PRIV_MAGIC1;
	buf[auth_len + 2] = kind;
	return auth_len + URP_CONN_PRIV_TRAILER_LEN;
}

/*
 * Read the peer's advertised endpoint kind from CM private_data. @auth_len is
 * the receiver's own auth length (== the peer's, auth being symmetric). Returns
 * true and sets *out_kind when a valid trailer is present; false (peer predates
 * the trailer, or truncated) otherwise -- the caller then assumes UDS.
 */
static inline bool urp_conn_priv_peer_kind(const void *priv, u8 priv_len,
					   u8 auth_len, u8 *out_kind)
{
	const u8 *p = priv;

	if (!p || priv_len < (u8)(auth_len + URP_CONN_PRIV_TRAILER_LEN))
		return false;
	if (p[auth_len] != URP_CONN_PRIV_MAGIC0 ||
	    p[auth_len + 1] != URP_CONN_PRIV_MAGIC1)
		return false;
	*out_kind = p[auth_len + 2];
	return true;
}

/*
 * gap #6 Phase 1: append the wide [MAGIC0][MAGIC1][kind][qp_index] trailer the
 * initiator sends when num_qps > 1. @buf must have room for auth_len + 4. The
 * base kind trailer above is a strict prefix, so a peer that only reads the
 * kind (urp_conn_priv_peer_kind) still sees a valid trailer.
 */
static inline u8 urp_conn_priv_build_qp(u8 *buf, u8 auth_len, u8 kind,
					u8 qp_index)
{
	buf[auth_len]	  = URP_CONN_PRIV_MAGIC0;
	buf[auth_len + 1] = URP_CONN_PRIV_MAGIC1;
	buf[auth_len + 2] = kind;
	buf[auth_len + 3] = qp_index;
	return auth_len + URP_CONN_PRIV_QP_TRAILER_LEN;
}

/*
 * Read the initiator's advertised QP index from the wide trailer. Returns true
 * and sets *out_qp_index only when a full 4-byte trailer with valid magic is
 * present; false (old/single-QP peer sent the 3-byte trailer or nothing)
 * otherwise -- the acceptor then falls back to its legacy counter allocation.
 */
static inline bool urp_conn_priv_peer_qp_index(const void *priv, u8 priv_len,
					       u8 auth_len, u8 *out_qp_index)
{
	const u8 *p = priv;

	if (!p || priv_len < (u8)(auth_len + URP_CONN_PRIV_QP_TRAILER_LEN))
		return false;
	if (p[auth_len] != URP_CONN_PRIV_MAGIC0 ||
	    p[auth_len + 1] != URP_CONN_PRIV_MAGIC1)
		return false;
	*out_qp_index = p[auth_len + 3];
	return true;
}

/*
 * gap #6 Phase 2 (PR2): the full trailer [MAGIC0][MAGIC1][kind][qp_index][caps].
 * @caps advertises connection capabilities (URP_CONN_CAP_*), negotiated only
 * when BOTH peers advertise (design 35 §35.3). This is now the single canonical
 * trailer both the connect and accept paths emit: it is a strict superset of
 * the kind (3) and qp (4) trailers, so an old/PR1 peer reading only kind or
 * qp_index still sees valid magic and its own prefix. @qp_index is 0 for a
 * single-QP endpoint. @buf must have room for auth_len + 5.
 */
#define URP_CONN_PRIV_CAP_TRAILER_LEN	5	/* magic0, magic1, kind, qp_index, caps */

static inline u8 urp_conn_priv_build_full(u8 *buf, u8 auth_len, u8 kind,
					  u8 qp_index, u8 caps)
{
	buf[auth_len]	  = URP_CONN_PRIV_MAGIC0;
	buf[auth_len + 1] = URP_CONN_PRIV_MAGIC1;
	buf[auth_len + 2] = kind;
	buf[auth_len + 3] = qp_index;
	buf[auth_len + 4] = caps;
	return auth_len + URP_CONN_PRIV_CAP_TRAILER_LEN;
}

/*
 * Read the peer's advertised capability bits from the full trailer. Returns
 * true and sets *out_caps only when a full 5-byte trailer with valid magic is
 * present; false (old/PR1 peer sent a shorter trailer, or nothing) otherwise --
 * the caller then treats the peer as not supporting any capability, so the
 * byte-window path stays disabled and both sides fall back to frame credits.
 */
static inline bool urp_conn_priv_peer_caps(const void *priv, u8 priv_len,
					   u8 auth_len, u8 *out_caps)
{
	const u8 *p = priv;

	if (!p || priv_len < (u8)(auth_len + URP_CONN_PRIV_CAP_TRAILER_LEN))
		return false;
	if (p[auth_len] != URP_CONN_PRIV_MAGIC0 ||
	    p[auth_len + 1] != URP_CONN_PRIV_MAGIC1)
		return false;
	*out_caps = p[auth_len + 4];
	return true;
}

/*
 * gap #6 Phase 2 (PR2): CREDIT-BYTES CONTROL payload codec. A CONTROL frame
 * carrying URP_CTRL_FLAG_CREDIT_BYTES puts a u64 cumulative rx_bytes_delivered
 * (the absolute high-water mark the receiver has handed to the app) in the
 * frame *payload* -- the first CONTROL frame with a non-zero payload. Modeled
 * on the PROBE payload codecs above. Mirrors crates/uds-rdma-protocol
 * src/frame.rs CreditBytesPayload.
 */
static inline void urp_credit_bytes_encode(void *payload, u64 abs_bytes)
{
	put_unaligned_le64(abs_bytes, (u8 *)payload);
}

/*
 * Decode the cumulative byte grant. Returns true and sets *out only when at
 * least URP_CREDIT_BYTES_PAYLOAD_SIZE bytes are present; false (short/absent
 * payload) otherwise -- the apply path then ignores the malformed grant.
 */
static inline bool urp_credit_bytes_decode(const void *payload, u32 payload_len,
					   u64 *out)
{
	if (!payload || payload_len < URP_CREDIT_BYTES_PAYLOAD_SIZE)
		return false;
	*out = get_unaligned_le64((const u8 *)payload);
	return true;
}

#endif /* _URP_FRAME_H */
