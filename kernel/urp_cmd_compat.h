/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace compatibility shim for compiling the urp-fast command validators
 * (urp_cmd_validate.c) outside the kernel. Provides the handful of kernel
 * spellings the validators use -- the short integer types, PAGE_SIZE, and
 * IS_ALIGNED -- in terms of standard userspace headers. Never included by the
 * kbuild path (guarded by #ifndef __KERNEL__ in urp_cmd_validate.c).
 *
 * PAGE_SIZE is pinned to 4096 here: the userspace validator test only needs
 * internal consistency, and the KUnit suite exercises the same code against
 * the running kernel's real PAGE_SIZE.
 */
#ifndef URP_CMD_COMPAT_H
#define URP_CMD_COMPAT_H

#include <stdint.h>
#include <stdbool.h>	/* bool/true/false for the ownership SM (urp_cmd_own.h) */
#include <errno.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif

#ifndef IS_ALIGNED
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#endif

#endif /* URP_CMD_COMPAT_H */
