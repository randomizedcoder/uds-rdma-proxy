// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace table-driven tests for the urp-fast command validators
 * (design 31 section 31.10, the app->kernel trust boundary).
 *
 * Compiles kernel/urp_cmd_validate.c in its userspace mode (see
 * urp_cmd_compat.h) and drives the exact same urp_cmd_validate_data /
 * urp_cmd_validate_reg entry points the ->uring_cmd handler and the KUnit
 * suite use. This is the fast sandboxed gate (nix check
 * urp-fast-validate-units); KUnit re-runs the same logic in-kernel against
 * the real PAGE_SIZE. The two must always agree.
 */

#include <stdio.h>
#include <string.h>

#include "urp_cmd_compat.h"
#include "include/uapi/linux/urp_cmd.h"
#include "urp_cmd.h"

static int failures;
static int checks;

#define CHECK_EQ(got, want, ...)                                          \
	do {                                                              \
		long g_ = (long)(got), w_ = (long)(want);                 \
		checks++;                                                 \
		if (g_ != w_) {                                           \
			failures++;                                       \
			printf("FAIL: ");                                 \
			printf(__VA_ARGS__);                              \
			printf(" (got %ld want %ld)\n", g_, w_);          \
		}                                                         \
	} while (0)

static void test_validate_data(void)
{
	const u32 GOOD_COUNT = 4;
	const u32 GOOD_BSZ = 4096;
	static const struct {
		u32 cmd_op;
		u32 buf_index;
		u32 len;
		u16 stream_id;
		u16 flags;
		u32 resv;
		u32 count;
		u32 buf_size;
		int want;
	} cases[] = {
		/* valid */
		{ URP_CMD_SEND, 0, 100, 7, 0, 0, 4, 4096, 0 },
		{ URP_CMD_SEND, 0, 100, 7, URP_CMD_F_FIN, 0, 4, 4096, 0 },
		{ URP_CMD_RECV, 3, 4096, 0, 0, 0, 4, 4096, 0 },
		/* boundaries */
		{ URP_CMD_SEND, 3, 4096, 0, 0, 0, 4, 4096, 0 },
		{ URP_CMD_RECV, 0, 1, 0, 0, 0, 4, 4096, 0 },
		/* opcode gate */
		{ URP_CMD_REGISTER, 0, 100, 0, 0, 0, 4, 4096, -EOPNOTSUPP },
		{ URP_CMD_UNREGISTER, 0, 100, 0, 0, 0, 4, 4096, -EOPNOTSUPP },
		{ 99, 0, 100, 0, 0, 0, 4, 4096, -EOPNOTSUPP },
		/* reserved / flags */
		{ URP_CMD_SEND, 0, 100, 0, 0, 1, 4, 4096, -EINVAL },
		{ URP_CMD_RECV, 0, 100, 0, URP_CMD_F_FIN, 0, 4, 4096, -EINVAL },
		{ URP_CMD_SEND, 0, 100, 0, (1 << 5), 0, 4, 4096, -EINVAL },
		/* length */
		{ URP_CMD_SEND, 0, 0, 0, 0, 0, 4, 4096, -EINVAL },
		{ URP_CMD_SEND, 0, 4097, 0, 0, 0, 4, 4096, -EMSGSIZE },
		/* pool / index */
		{ URP_CMD_SEND, 0, 100, 0, 0, 0, 0, 4096, -ENXIO },
		{ URP_CMD_SEND, 4, 100, 0, 0, 0, 4, 4096, -ERANGE },
		{ URP_CMD_RECV, 0xffffffffu, 100, 0, 0, 0, 4, 4096, -ERANGE },
	};
	size_t i;

	(void)GOOD_COUNT;
	(void)GOOD_BSZ;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct urp_cmd_data in = {
			.buf_index = cases[i].buf_index,
			.len = cases[i].len,
			.stream_id = cases[i].stream_id,
			.flags = cases[i].flags,
			.__resv = cases[i].resv,
		};
		struct urp_cmd_pool_geom geom = {
			.count = cases[i].count,
			.buf_size = cases[i].buf_size,
		};
		struct urp_cmd_req out;
		int ret;

		memset(&out, 0, sizeof(out));
		ret = urp_cmd_validate_data(cases[i].cmd_op, &in, &geom, &out);
		CHECK_EQ(ret, cases[i].want, "validate_data case %zu ret", i);
		if (ret == 0) {
			CHECK_EQ(out.op, cases[i].cmd_op, "data case %zu op", i);
			CHECK_EQ(out.buf_index, cases[i].buf_index,
				 "data case %zu idx", i);
			CHECK_EQ(out.len, cases[i].len, "data case %zu len", i);
			CHECK_EQ(out.stream_id, cases[i].stream_id,
				 "data case %zu sid", i);
			CHECK_EQ(out.flags, cases[i].flags,
				 "data case %zu flags", i);
		}
	}
}

static void test_validate_reg(void)
{
	const u64 PG = PAGE_SIZE;
	static const struct {
		const char *name;
		u64 base;
		u64 len;
		u32 buf_size;
		u32 count;
		u32 flags;
		u32 resv;
		int want;
	} cases[] = {
		{ "page-bufs", 4096, 4 * 4096, 4096, 4, 0, 0, 0 },
		{ "subpage", 4096, 4096, 64, 64, 0, 0, 0 },
		{ "flags-set", 4096, 4 * 4096, 4096, 4, 1, 0, -EINVAL },
		{ "resv-set", 4096, 4 * 4096, 4096, 4, 0, 1, -EINVAL },
		{ "base-zero", 0, 4 * 4096, 4096, 4, 0, 0, -EINVAL },
		{ "base-misalign", 4097, 4 * 4096, 4096, 4, 0, 0, -EINVAL },
		{ "bsz-small", 4096, 4 * 4096, URP_CMD_BUF_SIZE_MIN - 1, 4, 0, 0, -EINVAL },
		{ "bsz-big", 4096, 4 * 4096, URP_CMD_BUF_SIZE_MAX + 1, 4, 0, 0, -EINVAL },
		{ "len-zero", 4096, 0, 4096, 4, 0, 0, -EINVAL },
		{ "len-notmult", 4096, 4096, 3072, 4, 0, 0, -EINVAL },
		{ "len-notpage", 4096, 64 * 63, 64, 63, 0, 0, -EINVAL },
		{ "count-zero", 4096, 4096, 4096, 0, 0, 0, -EINVAL },
		{ "count-toobig", 4096,
		  (u64)(URP_CMD_POOL_COUNT_MAX + 1) * 4096, 4096,
		  URP_CMD_POOL_COUNT_MAX + 1, 0, 0, -E2BIG },
		{ "count-mismatch", 4096, 4 * 4096, 4096, 3, 0, 0, -EINVAL },
	};
	size_t i;

	(void)PG;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct urp_cmd_reg reg = {
			.base = cases[i].base,
			.len = cases[i].len,
			.buf_size = cases[i].buf_size,
			.count = cases[i].count,
			.flags = cases[i].flags,
			.__resv = cases[i].resv,
		};
		int ret = urp_cmd_validate_reg(&reg);

		CHECK_EQ(ret, cases[i].want, "validate_reg %s", cases[i].name);
	}
}

int main(void)
{
	test_validate_data();
	test_validate_reg();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
