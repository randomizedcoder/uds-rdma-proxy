// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- per-QP credit-based flow control
 *
 * 1:1 C port of uds_rdma_protocol::credit::CreditState. See urp_credit.h
 * for the public surface; this file holds the operational helpers.
 *
 * Concurrency: each struct urp_credit lives inside one struct urp_qp.
 * The TX path (urp_pump_tx kthread) is the sole consumer of consume(),
 * and the RX completion path (workqueue context) is the sole consumer
 * of record_recv() / take_grants(). Cross-direction reads
 * (urp_credit_should_grant, urp_credit_can_send) tolerate stale values
 * because:
 *   - the TX gate retries on next iteration if a grant arrives
 *     immediately after a stale can_send=false read; and
 *   - should_grant just decides whether to emit a grant frame -- a
 *     late grant is harmless, an extra grant is harmless.
 *
 * If/when these helpers grow concurrent writers (multiple TX threads
 * per QP, multiple RX paths), upgrade to atomics; until then plain
 * fields are correct.
 */

#include "urp_credit.h"
#include "urp.h"

void urp_credit_init(struct urp_credit *cs, u16 initial)
{
	cs->send_credits = initial;
	cs->credits_to_grant = 0;
	cs->threshold = initial / 4;
	cs->initial_credits = initial;
}

int urp_credit_consume(struct urp_credit *cs)
{
	if (cs->send_credits == 0)
		return -EAGAIN;
	cs->send_credits--;
	return 0;
}

void urp_credit_grant(struct urp_credit *cs, u16 n)
{
	u32 sum = (u32)cs->send_credits + (u32)n;

	/* Saturating add: match the Rust impl's overflow behavior so the
	 * KUnit/Rust diff suite stays clean.
	 */
	cs->send_credits = sum > U16_MAX ? U16_MAX : (u16)sum;
}

void urp_credit_record_recv(struct urp_credit *cs)
{
	if (cs->credits_to_grant < U16_MAX)
		cs->credits_to_grant++;
}

u16 urp_credit_take_grants(struct urp_credit *cs)
{
	u16 grants = cs->credits_to_grant;

	cs->credits_to_grant = 0;
	return grants;
}
