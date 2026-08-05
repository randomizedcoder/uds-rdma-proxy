/* SPDX-License-Identifier: GPL-2.0 */
/*
 * urp_credit.h -- per-QP credit-based flow control
 *
 * 1:1 C port of uds_rdma_protocol::credit::CreditState. Step 4 wires it
 * into the per-QP struct urp_qp so the TX path can gate ib_post_send on
 * available credits and the RX path can emit CONTROL/CREDIT frames once
 * accumulated grants reach the threshold.
 *
 * The semantics deliberately mirror the Rust reference implementation
 * field-for-field so the KUnit suite in Step 9 can be diffed against
 * the existing 8 Rust unit tests.
 */
#ifndef _URP_CREDIT_H
#define _URP_CREDIT_H

#include <linux/types.h>

struct urp_credit {
	u16 send_credits;
	u16 credits_to_grant;
	u16 threshold;
	u16 initial_credits;
};

/* Initialize state with @initial credits. Threshold = initial / 4. */
void urp_credit_init(struct urp_credit *cs, u16 initial);

/* True when the sender still has credits available. */
static inline bool urp_credit_can_send(const struct urp_credit *cs)
{
	return cs->send_credits > 0;
}

/* Consume one send credit. Returns -EAGAIN if exhausted. */
int  urp_credit_consume(struct urp_credit *cs);

/* Grant @n credits (received from peer). Saturating add. */
void urp_credit_grant(struct urp_credit *cs, u16 n);

/* Record that a recv buffer has been consumed and reposted. */
void urp_credit_record_recv(struct urp_credit *cs);

/* True when accumulated grants have reached the threshold. */
static inline bool urp_credit_should_grant(const struct urp_credit *cs)
{
	return cs->threshold == 0 || cs->credits_to_grant >= cs->threshold;
}

static inline u16 urp_credit_pending_grants(const struct urp_credit *cs)
{
	return cs->credits_to_grant;
}

/* Atomically read and clear the pending-grants counter. */
u16  urp_credit_take_grants(struct urp_credit *cs);

#endif /* _URP_CREDIT_H */
