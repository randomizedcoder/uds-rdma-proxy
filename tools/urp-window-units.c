// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace table-driven tests for the byte-window flow-control arithmetic in
 * kernel/urp_window.h (status.md gap #6 Phase 2, PR3; design 35 §35.3). Drives
 * the exact same pure predicates the sender gate (urp_stream_tx_fn), the
 * receiver grant (urp_recv_done), and the reorder coupling (urp_stream_create)
 * use, and that the in-kernel KUnit suite (kernel/urp_test.c) pins -- one set of
 * decisions, two drivers; this fast sandboxed gate (nix check urp-window-units)
 * and the KUnit-in-VM pass must always agree.
 *
 * Coverage: sender-gate room (fits / one-over / oversized-frame progress),
 * grant threshold (window/4), grant idempotence under stale/duplicate/reordered
 * grants (max()), window clamp, and the window->reorder-depth coupling.
 */

#include <stdio.h>
#include <stdbool.h>

#include "urp_window.h"

static int failures;
static int checks;

#define CHECK_EQ(got, want, ...)                                          \
	do {                                                              \
		unsigned long long g_ = (unsigned long long)(got);        \
		unsigned long long w_ = (unsigned long long)(want);       \
		checks++;                                                 \
		if (g_ != w_) {                                           \
			failures++;                                       \
			printf("FAIL: ");                                 \
			printf(__VA_ARGS__);                              \
			printf(" (got %llu want %llu)\n", g_, w_);        \
		}                                                         \
	} while (0)

static void run_has_room(void)
{
	CHECK_EQ(urp_window_has_room(0, 0, 1000, 5000), true, "room idle oversized");
	CHECK_EQ(urp_window_has_room(100, 100, 1000, 5000), true, "room drained oversized");
	CHECK_EQ(urp_window_has_room(900, 0, 1000, 100), true, "room fits exactly");
	CHECK_EQ(urp_window_has_room(901, 0, 1000, 100), false, "room one over");
	CHECK_EQ(urp_window_has_room(1000, 100, 1000, 100), true, "room acked frees");
	CHECK_EQ(urp_window_has_room(1000, 100, 1000, 101), false, "room acked one over");
}

static void run_should_grant(void)
{
	CHECK_EQ(urp_window_should_grant(1000, 800, 1000), false, "grant below quarter");
	CHECK_EQ(urp_window_should_grant(1050, 800, 1000), true, "grant at quarter");
	CHECK_EQ(urp_window_should_grant(250, 0, 1000), true, "grant first quarter");
	CHECK_EQ(urp_window_should_grant(249, 0, 1000), false, "grant just under");
	CHECK_EQ(urp_window_should_grant(5000, 0, 1000), true, "grant well past");
}

static void run_apply_grant(void)
{
	CHECK_EQ(urp_window_apply_grant(100, 500), 500, "apply advance");
	CHECK_EQ(urp_window_apply_grant(500, 100), 500, "apply stale no-rewind");
	CHECK_EQ(urp_window_apply_grant(500, 500), 500, "apply duplicate");
	CHECK_EQ(urp_window_apply_grant(0, 0), 0, "apply zero");
}

static void run_clamp(void)
{
	CHECK_EQ(urp_window_clamp(0), URP_WINDOW_BYTES_MIN, "clamp zero to min");
	CHECK_EQ(urp_window_clamp(URP_WINDOW_BYTES_MIN - 1), URP_WINDOW_BYTES_MIN,
		 "clamp below min");
	CHECK_EQ(urp_window_clamp(URP_WINDOW_BYTES_DEFAULT), URP_WINDOW_BYTES_DEFAULT,
		 "clamp default");
	CHECK_EQ(urp_window_clamp(URP_WINDOW_BYTES_MAX + 1), URP_WINDOW_BYTES_MAX,
		 "clamp above max");
	CHECK_EQ(urp_window_clamp(~0ULL), URP_WINDOW_BYTES_MAX, "clamp saturated");
}

static void run_reorder_depth(void)
{
	CHECK_EQ(urp_reorder_depth_for_window(1UL << 20), URP_REORDER_MAX_ENTRIES,
		 "depth 1MiB at ceiling");
	CHECK_EQ(urp_reorder_depth_for_window(1UL << 16), 4096, "depth 64KiB");
	CHECK_EQ(urp_reorder_depth_for_window(1000), URP_REORDER_MIN_ENTRIES,
		 "depth tiny clamps to floor");
	CHECK_EQ(urp_reorder_depth_for_window(1UL << 26), URP_REORDER_MAX_ENTRIES,
		 "depth huge clamps to ceiling");
}

int main(void)
{
	run_has_room();
	run_should_grant();
	run_apply_grant();
	run_clamp();
	run_reorder_depth();

	printf("urp-window-units: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
