/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp_fuzz_shim.h -- userspace stand-ins for the handful of kernel
 * primitives kernel/urp_frame.{c,h} need, so the real codec + RX
 * classifier compile into a libFuzzer harness (design 27 F1).
 *
 * Satisfies urp_frame.h's includer contract: fixed-width types,
 * put/get_unaligned_le*, memcpy/memset, the UAPI frame constants,
 * URP_MAX_PAYLOAD, and BIT. Then includes urp_frame.h itself.
 */
#ifndef _URP_FUZZ_SHIM_H
#define _URP_FUZZ_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(n) (1U << (n))

/* Little-endian unaligned access, matching the kernel helpers' signatures. */
static inline u16 get_unaligned_le16(const void *p)
{
	const u8 *b = p;

	return (u16)b[0] | ((u16)b[1] << 8);
}

static inline u32 get_unaligned_le32(const void *p)
{
	const u8 *b = p;

	return (u32)b[0] | ((u32)b[1] << 8) |
	       ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

static inline u64 get_unaligned_le64(const void *p)
{
	const u8 *b = p;

	return (u64)get_unaligned_le32(b) |
	       ((u64)get_unaligned_le32(b + 4) << 32);
}

static inline void put_unaligned_le16(u16 v, void *p)
{
	u8 *b = p;

	b[0] = (u8)v;
	b[1] = (u8)(v >> 8);
}

static inline void put_unaligned_le32(u32 v, void *p)
{
	u8 *b = p;

	b[0] = (u8)v;
	b[1] = (u8)(v >> 8);
	b[2] = (u8)(v >> 16);
	b[3] = (u8)(v >> 24);
}

static inline void put_unaligned_le64(u64 v, void *p)
{
	put_unaligned_le32((u32)v, p);
	put_unaligned_le32((u32)(v >> 32), (u8 *)p + 4);
}

/* UAPI frame constants (URP_FRAME_HEADER_SIZE, URP_FRAME_TYPE_*, ...). The
 * header is plain C with no kernel dependencies.
 */
#include "include/uapi/linux/urp.h"

/*
 * URP_MAX_PAYLOAD is a kernel buffer-sizing constant (URP_BUF_SIZE 4096 -
 * URP_FRAME_HEADER_SIZE) defined in urp.h, which we can't include here.
 * Mirror the value; if the kernel buffer size ever changes this must too
 * (the KUnit test uses the kernel's definition, so a drift shows up there).
 */
#ifndef URP_MAX_PAYLOAD
#define URP_MAX_PAYLOAD (4096 - URP_FRAME_HEADER_SIZE)
#endif

#include "urp_frame.h"

#endif /* _URP_FUZZ_SHIM_H */
