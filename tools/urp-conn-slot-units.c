// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace table-driven tests for the pure acceptor connection-plan predicates
 * in kernel/urp_conn_plan.h (status.md gap #6): urp_acceptor_slot_decide
 * (Phase 1) and urp_window_negotiate (Phase 2). Drives the exact same pure
 * functions the CM path and the in-kernel KUnit suite (kernel/urp_test.c) use --
 * one decision, two drivers; this fast sandboxed gate (nix check
 * urp-conn-slot-units) and the KUnit-in-VM pass must always agree.
 *
 * Coverage: slot decision -- identity allocation (positive), out-of-range
 * rejects (negative), the num_qps==1 / max-index boundaries, occupied-slot
 * reuse, and the legacy counter fallback for an old / single-QP peer that
 * advertises no qp_index. Window negotiation -- the both-must-advertise AND
 * truth table (design 35 §35.3 interop gate).
 */

#include <stdio.h>
#include <stdbool.h>

#include "urp_conn_plan.h"

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

struct slot_case {
	const char		*name;
	bool			have_peer_index;
	unsigned int		peer_qp_index;
	unsigned int		counter_index;
	unsigned int		num_qps;
	bool			slot_occupied;
	enum urp_slot_decision	want;
	unsigned int		want_index;	/* checked unless want == REJECT */
};

static const struct slot_case cases[] = {
	/* --- identity allocation (initiator advertised its qp_index) --- */
	{ "id_first_empty",   true, 0, 0, 8, false, URP_SLOT_FRESH, 0 },
	{ "id_mid_empty",     true, 3, 0, 8, false, URP_SLOT_FRESH, 3 },
	{ "id_mid_occupied",  true, 3, 0, 8, true,  URP_SLOT_REUSE, 3 },
	{ "id_last_empty",    true, 7, 0, 8, false, URP_SLOT_FRESH, 7 },
	/* the counter is IGNORED when a peer index is present (kills the storm:
	 * a retry for QP 3 reclaims slot 3, not the next counter value).
	 */
	{ "id_ignores_counter", true, 2, 6, 8, false, URP_SLOT_FRESH, 2 },

	/* --- out-of-range peer index => reject (malformed / hostile) --- */
	{ "id_eq_num_qps",    true, 8,  0, 8, false, URP_SLOT_REJECT, 0 },
	{ "id_gt_num_qps",    true, 99, 0, 8, false, URP_SLOT_REJECT, 0 },
	{ "id_single_qp_bad", true, 31, 0, 1, false, URP_SLOT_REJECT, 0 },

	/* --- boundaries --- */
	{ "id_single_qp_ok",  true, 0,  0, 1,  false, URP_SLOT_FRESH,  0 },
	{ "id_max_qps_ok",    true, 31, 0, 32, false, URP_SLOT_FRESH,  31 },
	{ "id_max_qps_bad",   true, 32, 0, 32, false, URP_SLOT_REJECT, 0 },

	/* --- legacy counter fallback (old / single-QP peer, no qp_index) --- */
	{ "ctr_first_empty",   false, 0, 0, 1, false, URP_SLOT_FRESH,  0 },
	{ "ctr_first_reuse",   false, 0, 0, 1, true,  URP_SLOT_REUSE,  0 },
	{ "ctr_surplus_single", false, 0, 1, 1, false, URP_SLOT_REJECT, 0 },
	{ "ctr_multi_resolves", false, 0, 3, 8, false, URP_SLOT_FRESH,  3 },
	{ "ctr_multi_overflow", false, 0, 8, 8, false, URP_SLOT_REJECT, 0 },
};

static const char *decision_name(enum urp_slot_decision d)
{
	switch (d) {
	case URP_SLOT_REJECT:	return "REJECT";
	case URP_SLOT_FRESH:	return "FRESH";
	case URP_SLOT_REUSE:	return "REUSE";
	}
	return "?";
}

static void run_cases(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct slot_case *c = &cases[i];
		unsigned int out = 0xDEADu;
		enum urp_slot_decision got;

		got = urp_acceptor_slot_decide(c->have_peer_index,
					       c->peer_qp_index, c->counter_index,
					       c->num_qps, c->slot_occupied, &out);

		CHECK_EQ(got, c->want, "%s decision (want %s got %s)",
			 c->name, decision_name(c->want), decision_name(got));

		if (c->want == URP_SLOT_REJECT) {
			/* out_index must be left untouched on reject. */
			CHECK_EQ(out, 0xDEADu, "%s left out_index untouched",
				 c->name);
		} else {
			CHECK_EQ(out, c->want_index, "%s out_index", c->name);
		}
	}
}

/* gap #6 Phase 2: byte-windowing is on iff BOTH peers advertise. */
static void run_negotiate_cases(void)
{
	const struct {
		const char	*name;
		bool		local_adv;
		bool		peer_adv;
		bool		want;
	} cases[] = {
		{ "neither",   false, false, false },
		{ "local_only", true, false, false },
		{ "peer_only", false, true,  false },
		{ "both",       true, true,  true },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		CHECK_EQ(urp_window_negotiate(cases[i].local_adv,
					      cases[i].peer_adv),
			 cases[i].want, "window_negotiate %s", cases[i].name);
}

int main(void)
{
	run_cases();
	run_negotiate_cases();

	printf("urp-conn-slot-units: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
