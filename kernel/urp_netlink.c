// SPDX-License-Identifier: GPL-2.0
/*
 * UDS-RDMA Proxy (urp) -- generic netlink ("urp") family
 *
 * Phase 2 control plane. Endpoints are created/inspected/torn down via
 * the URP_GENL_NAME family using the commands defined in the UAPI header.
 * State-change notifications fan out via the "events" multicast group
 * (urp_send_event()).
 *
 *   URP_CMD_NEW_ENDPOINT   admin only -- alloc + activate
 *   URP_CMD_DEL_ENDPOINT   admin only -- drain + destroy
 *   URP_CMD_SET_ENDPOINT   admin only -- mutate num_qps/buffer_count/password/state
 *   URP_CMD_GET_ENDPOINT   unprivileged -- doit (single by name) or dumpit (all)
 *
 * The endpoint store itself lives in urp_endpoint.c; this file is purely
 * the netlink encode/decode layer plus dispatch.
 */
#include "urp.h"

#include <net/genetlink.h>
#include <net/sock.h>
#include <linux/string.h>

/* ---------------------------------------------------------------- */
/* Policies                                                         */
/* ---------------------------------------------------------------- */

/* Endpoint nested policy: validates URP_A_ENDPOINT contents */
static const struct nla_policy urp_endpoint_policy[URP_ENDPOINT_A_MAX + 1] = {
	[URP_ENDPOINT_A_NAME]		= { .type = NLA_NUL_STRING,
					    .len  = URP_NAME_MAX - 1 },
	[URP_ENDPOINT_A_LISTEN_PATH]	= { .type = NLA_NUL_STRING,
					    .len  = URP_PATH_MAX_LEN - 1 },
	[URP_ENDPOINT_A_CONNECT_PATH]	= { .type = NLA_NUL_STRING,
					    .len  = URP_PATH_MAX_LEN - 1 },
	[URP_ENDPOINT_A_RDMA_DEVICE]	= { .type = NLA_NUL_STRING,
					    .len  = URP_DEVICE_MAX - 1 },
	[URP_ENDPOINT_A_PEER_ADDR]	= NLA_POLICY_EXACT_LEN(sizeof(struct sockaddr_in6)),
	[URP_ENDPOINT_A_BIND_ADDR]	= NLA_POLICY_EXACT_LEN(sizeof(struct sockaddr_in6)),
	[URP_ENDPOINT_A_NUM_QPS]	= NLA_POLICY_RANGE(NLA_U32, URP_NUM_QPS_MIN,
							   URP_NUM_QPS_MAX),
	[URP_ENDPOINT_A_BUFFER_COUNT]	= NLA_POLICY_MIN(NLA_U32, URP_BUFFER_COUNT_MIN),
	[URP_ENDPOINT_A_BUFFER_SIZE]	= NLA_POLICY_RANGE(NLA_U32, URP_BUFFER_SIZE_MIN,
							   URP_BUFFER_SIZE_MAX),
	[URP_ENDPOINT_A_PASSWORD]	= { .type = NLA_NUL_STRING,
					    .len  = URP_PASSWORD_MAX - 1 },
	[URP_ENDPOINT_A_STATE]		= NLA_POLICY_RANGE(NLA_U8, 0, URP_STATE_MAX),
};

/* Top-level policy: only URP_A_ENDPOINT is accepted at the outer level */
static const struct nla_policy urp_top_policy[URP_A_MAX + 1] = {
	[URP_A_ENDPOINT]	= NLA_POLICY_NESTED(urp_endpoint_policy),
};

/* Forward decl for genl_family below */
static const struct genl_multicast_group urp_mcgrps[];

/* ---------------------------------------------------------------- */
/* Serialization helpers                                            */
/* ---------------------------------------------------------------- */

/*
 * Fill a nested URP_A_ENDPOINT block describing @ep into @skb.
 * Used by GET (doit + dumpit) and by urp_send_event() for multicast
 * notifications. Returns 0 on success or a negative errno on nlmsg overflow
 * (in which case the caller should genlmsg_cancel).
 *
 * Rules per UAPI header:
 *   - PASSWORD is write-only and must NEVER be returned.
 *   - QPS / STREAMS / STATS are nested children -- only emitted when
 *     @verbose so dump replies stay small.
 */
static int urp_fill_endpoint(struct sk_buff *skb, struct urp_endpoint *ep,
			     bool verbose)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, URP_A_ENDPOINT);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_string(skb, URP_ENDPOINT_A_NAME, ep->name))
		goto cancel;

	if (ep->listen_path[0] &&
	    nla_put_string(skb, URP_ENDPOINT_A_LISTEN_PATH, ep->listen_path))
		goto cancel;

	if (ep->connect_path[0] &&
	    nla_put_string(skb, URP_ENDPOINT_A_CONNECT_PATH, ep->connect_path))
		goto cancel;

	if (ep->rdma_device[0] &&
	    nla_put_string(skb, URP_ENDPOINT_A_RDMA_DEVICE, ep->rdma_device))
		goto cancel;

	if (ep->has_peer_addr &&
	    nla_put(skb, URP_ENDPOINT_A_PEER_ADDR,
		    sizeof(ep->peer_addr), &ep->peer_addr))
		goto cancel;

	if (ep->has_bind_addr &&
	    nla_put(skb, URP_ENDPOINT_A_BIND_ADDR,
		    sizeof(ep->bind_addr), &ep->bind_addr))
		goto cancel;

	if (nla_put_u32(skb, URP_ENDPOINT_A_NUM_QPS, ep->num_qps))
		goto cancel;
	if (nla_put_u32(skb, URP_ENDPOINT_A_BUFFER_COUNT, ep->buffer_count))
		goto cancel;
	if (nla_put_u32(skb, URP_ENDPOINT_A_BUFFER_SIZE, ep->buffer_size))
		goto cancel;
	if (nla_put_u8(skb, URP_ENDPOINT_A_STATE, (u8)ep->state))
		goto cancel;

	if (verbose) {
		struct nlattr *qps_nest, *streams_nest, *stats_nest;
		u32 active_streams = 0;
		u32 i;

		/* Per-QP nested blocks (Phase 3a Step 8). One entry per QP
		 * in ep->qps[] -- the array is allocated by urp_qps_init at
		 * activate time. */
		qps_nest = nla_nest_start(skb, URP_ENDPOINT_A_QPS);
		if (!qps_nest)
			goto cancel;

		if (ep->qps) {
			for (i = 0; i < ep->num_qps; i++) {
				struct urp_qp *q = &ep->qps[i];
				struct nlattr *qp_entry;
				u8 qp_state = q->established
					? q->health
					: URP_QP_STATE_QUALIFYING;

				qp_entry = nla_nest_start(skb, i + 1);
				if (!qp_entry)
					goto cancel;

				if (nla_put_u32(skb, URP_QP_A_INDEX, q->index) ||
				    nla_put_u8(skb, URP_QP_A_STATE, qp_state) ||
				    /* Step 6: real per-QP EWMA RTT (was 0). */
				    nla_put_u64_64bit(skb, URP_QP_A_RTT_NS,
						      q->rtt_ewma_ns, 0) ||
				    nla_put_u64_64bit(skb, URP_QP_A_TX_BYTES,
						      atomic64_read(&q->tx_bytes), 0) ||
				    nla_put_u64_64bit(skb, URP_QP_A_RX_BYTES,
						      atomic64_read(&q->rx_bytes), 0) ||
				    nla_put_u64_64bit(skb, URP_QP_A_TX_FRAMES,
						      atomic64_read(&q->tx_frames), 0) ||
				    nla_put_u64_64bit(skb, URP_QP_A_RX_FRAMES,
						      atomic64_read(&q->rx_frames), 0))
					goto cancel;

				nla_nest_end(skb, qp_entry);
			}
		}
		nla_nest_end(skb, qps_nest);

		/* Per-stream nested blocks (Phase 3a Step 8). Walks the
		 * rhashtable under RCU; entries are added by Step 7b's
		 * urp_stream_create call sites. With no active streams the
		 * nest is emitted empty, which is intentional. */
		streams_nest = nla_nest_start(skb, URP_ENDPOINT_A_STREAMS);
		if (!streams_nest)
			goto cancel;

		if (ep->streams_inited) {
			struct rhashtable_iter iter;
			struct urp_stream *s;
			u32 idx = 1;

			rhashtable_walk_enter(&ep->streams, &iter);
			rhashtable_walk_start(&iter);

			while ((s = rhashtable_walk_next(&iter))) {
				struct nlattr *s_entry;

				if (IS_ERR(s))
					continue;

				s_entry = nla_nest_start(skb, idx++);
				if (!s_entry) {
					rhashtable_walk_stop(&iter);
					rhashtable_walk_exit(&iter);
					goto cancel;
				}

				if (nla_put_u32(skb, URP_STREAM_A_ID, s->id) ||
				    nla_put_u8(skb, URP_STREAM_A_STATE, (u8)s->state) ||
				    nla_put_u64_64bit(skb, URP_STREAM_A_TX_BYTES, 0, 0) ||
				    nla_put_u64_64bit(skb, URP_STREAM_A_RX_BYTES, 0, 0) ||
				    nla_put_u32(skb, URP_STREAM_A_REORDER_DEPTH,
						(u32)urp_reorder_gap_count(s->reorder)) ||
				    nla_put_u16(skb, URP_STREAM_A_CREDITS_LOCAL,
						s->credit.send_credits) ||
				    nla_put_u16(skb, URP_STREAM_A_CREDITS_REMOTE,
						s->credit.credits_to_grant)) {
					rhashtable_walk_stop(&iter);
					rhashtable_walk_exit(&iter);
					goto cancel;
				}

				nla_nest_end(skb, s_entry);
				active_streams++;
			}

			rhashtable_walk_stop(&iter);
			rhashtable_walk_exit(&iter);
		}

		/* k0 single-connection compat: surface the legacy ep->conn
		 * as a synthetic stream while Step 7b hasn't yet migrated
		 * UDS accepts to urp_stream_create. */
		if (active_streams == 0 && ep->conn.active) {
			struct nlattr *s_entry = nla_nest_start(skb, 1);

			if (!s_entry)
				goto cancel;
			if (nla_put_u32(skb, URP_STREAM_A_ID, 0) ||
			    nla_put_u8(skb, URP_STREAM_A_STATE,
				       URP_STREAM_STATE_ESTABLISHED) ||
			    nla_put_u64_64bit(skb, URP_STREAM_A_TX_BYTES,
					      atomic64_read(&ep->stats.tx_bytes), 0) ||
			    nla_put_u64_64bit(skb, URP_STREAM_A_RX_BYTES,
					      atomic64_read(&ep->stats.rx_bytes), 0) ||
			    nla_put_u32(skb, URP_STREAM_A_REORDER_DEPTH, 0) ||
			    nla_put_u16(skb, URP_STREAM_A_CREDITS_LOCAL, 0) ||
			    nla_put_u16(skb, URP_STREAM_A_CREDITS_REMOTE, 0))
				goto cancel;
			nla_nest_end(skb, s_entry);
			active_streams = 1;
		}
		nla_nest_end(skb, streams_nest);

		/* Aggregate stats */
		stats_nest = nla_nest_start(skb, URP_ENDPOINT_A_STATS);
		if (!stats_nest)
			goto cancel;
		if (nla_put_u32(skb, URP_STATS_A_ACTIVE_STREAMS, active_streams) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_TX_BYTES,
				      atomic64_read(&ep->stats.tx_bytes), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_RX_BYTES,
				      atomic64_read(&ep->stats.rx_bytes), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_TX_FRAMES,
				      atomic64_read(&ep->stats.tx_frames), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_RX_FRAMES,
				      atomic64_read(&ep->stats.rx_frames), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_CREDIT_STALLS,
				      atomic64_read(&ep->stats.credit_stalls), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_REORDER_INSERTIONS,
				      atomic64_read(&ep->stats.reorder_insertions), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_REORDER_DROPS,
				      atomic64_read(&ep->stats.reorder_drops), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_BUFFER_ALLOC_FAILS,
				      atomic64_read(&ep->stats.buffer_alloc_fails), 0) ||
		    nla_put_u64_64bit(skb, URP_STATS_A_AUTH_FAILURES,
				      atomic64_read(&ep->stats.auth_failures), 0))
			goto cancel;
		nla_nest_end(skb, stats_nest);
	}

	nla_nest_end(skb, nest);
	return 0;

cancel:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

/*
 * Parse the nested URP_A_ENDPOINT block from @info into @tb. Returns 0 on
 * success and writes URP_ENDPOINT_A_MAX+1 attribute pointers, or a
 * negative errno on policy violation.
 */
static int urp_parse_endpoint(struct genl_info *info,
			      struct nlattr **tb)
{
	struct nlattr *outer = info->attrs[URP_A_ENDPOINT];

	if (!outer) {
		GENL_SET_ERR_MSG(info, "missing URP_A_ENDPOINT");
		return -EINVAL;
	}

	return nla_parse_nested(tb, URP_ENDPOINT_A_MAX, outer,
				urp_endpoint_policy, info->extack);
}

/* ---------------------------------------------------------------- */
/* Family declaration (forward; populated at the bottom)            */
/* ---------------------------------------------------------------- */

static struct genl_family urp_genl_family;

/* ---------------------------------------------------------------- */
/* Command handlers                                                 */
/* ---------------------------------------------------------------- */

/*
 * URP_CMD_NEW_ENDPOINT (admin)
 *
 * Required: NAME, plus exactly one of LISTEN_PATH/CONNECT_PATH, plus the
 * matching PEER_ADDR (initiator) or BIND_ADDR (acceptor). Optional fields
 * fall back to per-endpoint defaults in urp_endpoint_create().
 *
 * On success no reply attributes are needed -- ack is sufficient. The
 * follow-up state-change events are delivered via the "events" mcgrp.
 */
static int urp_new_endpoint_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *tb[URP_ENDPOINT_A_MAX + 1];
	struct urp_endpoint cfg = {};
	struct urp_endpoint *ep;
	int ret;

	ret = urp_parse_endpoint(info, tb);
	if (ret)
		return ret;

	if (!tb[URP_ENDPOINT_A_NAME]) {
		GENL_SET_ERR_MSG(info, "endpoint name required");
		return -EINVAL;
	}
	nla_strscpy(cfg.name, tb[URP_ENDPOINT_A_NAME], URP_NAME_MAX);

	if (tb[URP_ENDPOINT_A_LISTEN_PATH])
		nla_strscpy(cfg.listen_path, tb[URP_ENDPOINT_A_LISTEN_PATH],
			    URP_PATH_MAX_LEN);
	if (tb[URP_ENDPOINT_A_CONNECT_PATH])
		nla_strscpy(cfg.connect_path, tb[URP_ENDPOINT_A_CONNECT_PATH],
			    URP_PATH_MAX_LEN);
	if (tb[URP_ENDPOINT_A_RDMA_DEVICE])
		nla_strscpy(cfg.rdma_device, tb[URP_ENDPOINT_A_RDMA_DEVICE],
			    URP_DEVICE_MAX);

	if (tb[URP_ENDPOINT_A_PEER_ADDR]) {
		memcpy(&cfg.peer_addr, nla_data(tb[URP_ENDPOINT_A_PEER_ADDR]),
		       sizeof(cfg.peer_addr));
		cfg.has_peer_addr = true;
	}
	if (tb[URP_ENDPOINT_A_BIND_ADDR]) {
		memcpy(&cfg.bind_addr, nla_data(tb[URP_ENDPOINT_A_BIND_ADDR]),
		       sizeof(cfg.bind_addr));
		cfg.has_bind_addr = true;
	}

	if (tb[URP_ENDPOINT_A_NUM_QPS])
		cfg.num_qps = nla_get_u32(tb[URP_ENDPOINT_A_NUM_QPS]);
	if (tb[URP_ENDPOINT_A_BUFFER_COUNT])
		cfg.buffer_count = nla_get_u32(tb[URP_ENDPOINT_A_BUFFER_COUNT]);
	if (tb[URP_ENDPOINT_A_BUFFER_SIZE])
		cfg.buffer_size = nla_get_u32(tb[URP_ENDPOINT_A_BUFFER_SIZE]);

	if (tb[URP_ENDPOINT_A_PASSWORD]) {
		nla_strscpy((char *)cfg.password,
			    tb[URP_ENDPOINT_A_PASSWORD], URP_PASSWORD_MAX);
		cfg.has_password = true;
	}

	ret = urp_endpoint_create(&cfg, &ep);
	if (ret) {
		if (ret == -EEXIST)
			GENL_SET_ERR_MSG(info, "endpoint already exists");
		else if (ret == -EINVAL)
			GENL_SET_ERR_MSG(info, "invalid endpoint configuration");
		return ret;
	}

	ret = urp_endpoint_activate(ep);
	if (ret) {
		urp_endpoint_drain(ep);
		urp_endpoint_destroy(ep);
		GENL_SET_ERR_MSG_FMT(info, "endpoint activation failed: %d", ret);
		return ret;
	}

	return 0;
}

/*
 * URP_CMD_DEL_ENDPOINT (admin) -- drain then destroy by name.
 * Idempotent for endpoints already in DRAINING/STOPPED. Returns -ENOENT
 * if @name is unknown.
 */
static int urp_del_endpoint_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *tb[URP_ENDPOINT_A_MAX + 1];
	struct urp_endpoint *ep;
	char name[URP_NAME_MAX];
	int ret;

	ret = urp_parse_endpoint(info, tb);
	if (ret)
		return ret;
	if (!tb[URP_ENDPOINT_A_NAME]) {
		GENL_SET_ERR_MSG(info, "endpoint name required");
		return -EINVAL;
	}
	nla_strscpy(name, tb[URP_ENDPOINT_A_NAME], URP_NAME_MAX);

	rcu_read_lock();
	ep = urp_endpoint_lookup(name);
	rcu_read_unlock();
	if (!ep) {
		GENL_SET_ERR_MSG(info, "endpoint not found");
		return -ENOENT;
	}

	urp_endpoint_drain(ep);
	urp_endpoint_destroy(ep);
	return 0;
}

/*
 * URP_CMD_SET_ENDPOINT (admin) -- mutate live endpoint.
 *
 * Phase 2 honours: NUM_QPS, BUFFER_COUNT, PASSWORD, and STATE
 * (only DRAINING is accepted -- it triggers a drain, equivalent to
 * `urp drain`). Other attributes are silently ignored to match the UAPI
 * contract (PATH/ADDR/BUFFER_SIZE are immutable after creation).
 */
static int urp_set_endpoint_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *tb[URP_ENDPOINT_A_MAX + 1];
	struct urp_endpoint *ep;
	char name[URP_NAME_MAX];
	int ret;
	bool drain_requested = false;

	ret = urp_parse_endpoint(info, tb);
	if (ret)
		return ret;
	if (!tb[URP_ENDPOINT_A_NAME]) {
		GENL_SET_ERR_MSG(info, "endpoint name required");
		return -EINVAL;
	}
	nla_strscpy(name, tb[URP_ENDPOINT_A_NAME], URP_NAME_MAX);

	rcu_read_lock();
	ep = urp_endpoint_lookup(name);
	rcu_read_unlock();
	if (!ep) {
		GENL_SET_ERR_MSG(info, "endpoint not found");
		return -ENOENT;
	}

	if (tb[URP_ENDPOINT_A_STATE]) {
		u8 want = nla_get_u8(tb[URP_ENDPOINT_A_STATE]);

		if (want != URP_STATE_DRAINING) {
			GENL_SET_ERR_MSG(info, "only DRAINING state is settable");
			return -EINVAL;
		}
		drain_requested = true;
	}

	mutex_lock(&ep->lock);
	if (tb[URP_ENDPOINT_A_NUM_QPS])
		ep->num_qps = nla_get_u32(tb[URP_ENDPOINT_A_NUM_QPS]);
	if (tb[URP_ENDPOINT_A_BUFFER_COUNT])
		ep->buffer_count = nla_get_u32(tb[URP_ENDPOINT_A_BUFFER_COUNT]);
	if (tb[URP_ENDPOINT_A_PASSWORD]) {
		nla_strscpy((char *)ep->password,
			    tb[URP_ENDPOINT_A_PASSWORD], URP_PASSWORD_MAX);
		ep->has_password = true;
	}
	mutex_unlock(&ep->lock);

	if (drain_requested)
		urp_endpoint_drain(ep);

	return 0;
}

/*
 * URP_CMD_GET_ENDPOINT doit -- fetch a single endpoint by name.
 * Returns the full verbose payload (qps/streams/stats inclusive).
 */
static int urp_get_endpoint_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *tb[URP_ENDPOINT_A_MAX + 1];
	struct sk_buff *msg;
	struct urp_endpoint *ep;
	char name[URP_NAME_MAX];
	void *hdr;
	int ret;

	ret = urp_parse_endpoint(info, tb);
	if (ret)
		return ret;
	if (!tb[URP_ENDPOINT_A_NAME]) {
		GENL_SET_ERR_MSG(info, "endpoint name required");
		return -EINVAL;
	}
	nla_strscpy(name, tb[URP_ENDPOINT_A_NAME], URP_NAME_MAX);

	rcu_read_lock();
	ep = urp_endpoint_lookup(name);
	rcu_read_unlock();
	if (!ep) {
		GENL_SET_ERR_MSG(info, "endpoint not found");
		return -ENOENT;
	}

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_iput(msg, info);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	ret = urp_fill_endpoint(msg, ep, true);
	if (ret) {
		genlmsg_cancel(msg, hdr);
		nlmsg_free(msg);
		return ret;
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
}

/*
 * URP_CMD_GET_ENDPOINT dumpit -- iterate the entire rhashtable.
 * cb->args[0..3] hold the rhashtable_iter (we stash the struct directly
 * in cb->args by allocating a heap copy in start, freed in done).
 */
struct urp_dump_ctx {
	struct rhashtable_iter iter;
	bool started;
};

static int urp_get_endpoint_start(struct netlink_callback *cb)
{
	struct urp_dump_ctx *ctx;

	if (!urp_endpoints_inited)
		return -ENODEV;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	rhashtable_walk_enter(&urp_endpoints, &ctx->iter);
	ctx->started = true;
	cb->args[0] = (long)ctx;
	return 0;
}

static int urp_get_endpoint_dumpit(struct sk_buff *skb,
				   struct netlink_callback *cb)
{
	struct urp_dump_ctx *ctx = (void *)cb->args[0];
	struct urp_endpoint *ep;
	void *hdr;
	int ret = 0;

	if (!ctx)
		return -EINVAL;

	rhashtable_walk_start(&ctx->iter);

	while ((ep = rhashtable_walk_next(&ctx->iter))) {
		if (IS_ERR(ep)) {
			if (PTR_ERR(ep) == -EAGAIN)
				continue;
			ret = PTR_ERR(ep);
			break;
		}

		hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
				  cb->nlh->nlmsg_seq, &urp_genl_family,
				  NLM_F_MULTI, URP_CMD_GET_ENDPOINT);
		if (!hdr) {
			ret = -EMSGSIZE;
			break;
		}

		if (urp_fill_endpoint(skb, ep, false)) {
			genlmsg_cancel(skb, hdr);
			ret = -EMSGSIZE;
			break;
		}
		genlmsg_end(skb, hdr);
	}

	rhashtable_walk_stop(&ctx->iter);

	if (ret == -EMSGSIZE)
		return skb->len;	/* partial fill, resume next call */
	if (ret)
		return ret;
	return skb->len;
}

static int urp_get_endpoint_done(struct netlink_callback *cb)
{
	struct urp_dump_ctx *ctx = (void *)cb->args[0];

	if (ctx) {
		if (ctx->started)
			rhashtable_walk_exit(&ctx->iter);
		kfree(ctx);
		cb->args[0] = 0;
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* Multicast event delivery                                         */
/* ---------------------------------------------------------------- */

/*
 * Notify "events" subscribers about an endpoint state change. Best-effort:
 * any allocation/serialization failure is logged at debug level only.
 * Called from urp_endpoint_activate / urp_endpoint_drain at every
 * transition.
 */
void urp_send_event(struct urp_endpoint *ep)
{
	struct sk_buff *msg;
	void *hdr;

	if (!ep || urp_genl_family.id == 0)
		return;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return;

	hdr = genlmsg_put(msg, 0, 0, &urp_genl_family, 0, URP_CMD_GET_ENDPOINT);
	if (!hdr)
		goto drop;

	if (urp_fill_endpoint(msg, ep, false))
		goto cancel;

	genlmsg_end(msg, hdr);
	genlmsg_multicast(&urp_genl_family, msg, 0, 0, GFP_KERNEL);
	return;

cancel:
	genlmsg_cancel(msg, hdr);
drop:
	nlmsg_free(msg);
}

/* ---------------------------------------------------------------- */
/* Family / ops table                                               */
/* ---------------------------------------------------------------- */

static const struct genl_ops urp_genl_ops[] = {
	{
		.cmd	= URP_CMD_NEW_ENDPOINT,
		.flags	= GENL_ADMIN_PERM,
		.doit	= urp_new_endpoint_doit,
	},
	{
		.cmd	= URP_CMD_DEL_ENDPOINT,
		.flags	= GENL_ADMIN_PERM,
		.doit	= urp_del_endpoint_doit,
	},
	{
		.cmd	= URP_CMD_SET_ENDPOINT,
		.flags	= GENL_ADMIN_PERM,
		.doit	= urp_set_endpoint_doit,
	},
	{
		.cmd	= URP_CMD_GET_ENDPOINT,
		.doit	= urp_get_endpoint_doit,
		.start	= urp_get_endpoint_start,
		.dumpit	= urp_get_endpoint_dumpit,
		.done	= urp_get_endpoint_done,
	},
};

static const struct genl_multicast_group urp_mcgrps[] = {
	{ .name = URP_GENL_MCGRP_EVENTS },
};

static struct genl_family urp_genl_family = {
	.name		= URP_GENL_NAME,
	.version	= URP_GENL_VERSION,
	.maxattr	= URP_A_MAX,
	.policy		= urp_top_policy,
	.module		= THIS_MODULE,
	.ops		= urp_genl_ops,
	.n_ops		= ARRAY_SIZE(urp_genl_ops),
	.mcgrps		= urp_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(urp_mcgrps),
	.netnsok	= false,
};

int urp_genl_register(void)
{
	return genl_register_family(&urp_genl_family);
}

void urp_genl_unregister(void)
{
	genl_unregister_family(&urp_genl_family);
}
