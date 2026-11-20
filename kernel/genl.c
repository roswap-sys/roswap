// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/netlink.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>

#include "compat.h"
#include "roswap.h"

static struct genl_family rs_family;
static struct workqueue_struct *rs_evt_wq;

static const struct nla_policy rs_policy[RS_A_MAX + 1] = {
	[RS_A_NODE_ID] = { .type = NLA_U16 },
	[RS_A_ADDR] = { .type = NLA_NUL_STRING, .len = 63 },
	[RS_A_CHUNK_GID] = { .type = NLA_U32 },
	[RS_A_EPOCH] = { .type = NLA_U32 },
	[RS_A_RADDR] = { .type = NLA_U64 },
	[RS_A_RKEY] = { .type = NLA_U32 },
	[RS_A_CHUNK_MB] = { .type = NLA_U32 },
	[RS_A_NR_SLOTS] = { .type = NLA_U32 },
	[RS_A_USED_SLOTS] = { .type = NLA_U32 },
	[RS_A_FREE_EXTENTS] = { .type = NLA_U32 },
	[RS_A_NR_NODES] = { .type = NLA_U32 },
	[RS_A_NR_CHUNKS] = { .type = NLA_U32 },
	[RS_A_LINKS_UP] = { .type = NLA_U32 },
	[RS_A_HB_RADDR] = { .type = NLA_U64 },
	[RS_A_HB_RKEY] = { .type = NLA_U32 },
	[RS_A_NODE_STATE] = { .type = NLA_U32 },
	[RS_A_BOOT_ID] = { .type = NLA_U64 },
	[RS_A_DEGRADED] = { .type = NLA_U32 },
	[RS_A_LOST] = { .type = NLA_U32 },
	[RS_A_NODE_LIST] = { .type = NLA_BINARY, .len = RS_MAX_NODES * 4 },
	[RS_A_HEDGE_SENT] = { .type = NLA_U32 },
	[RS_A_HEDGE_WON] = { .type = NLA_U32 },
	[RS_A_KEY_WAIT] = { .type = NLA_U32 },
};

static const struct genl_multicast_group rs_mcgrps[] = {
	{ .name = "events", },
};

struct rs_evt {
	struct work_struct work;
	u16 node_id;
	u32 state;
	u64 boot_id;
	u32 lost;
	u8 kind;
};

static int rs_need(struct genl_info *info, int attr)
{
	if (info->attrs[attr])
		return 0;
	return -EINVAL;
}

static int rs_cmd_node_add(struct sk_buff *skb, struct genl_info *info)
{
	struct rs_hb_cfg hb, *hbp = NULL;
	char addr[64];
	u16 node_id;
	int ret;

	ret = rs_need(info, RS_A_NODE_ID) ?: rs_need(info, RS_A_ADDR);
	if (ret)
		return ret;
	(void)skb;
	node_id = nla_get_u16(info->attrs[RS_A_NODE_ID]);
	ret = nla_strscpy(addr, info->attrs[RS_A_ADDR], sizeof(addr));
	if (ret < 0)
		return ret;
	if (info->attrs[RS_A_HB_RADDR] && info->attrs[RS_A_HB_RKEY]) {
		hb.raddr = nla_get_u64(info->attrs[RS_A_HB_RADDR]);
		hb.rkey = nla_get_u32(info->attrs[RS_A_HB_RKEY]);
		hb.boot_id = info->attrs[RS_A_BOOT_ID] ?
			nla_get_u64(info->attrs[RS_A_BOOT_ID]) : 0;
		if (hb.raddr)
			hbp = &hb;
	}
	return roswap_node_add(node_id, addr, hbp);
}

static int rs_cmd_node_del(struct sk_buff *skb, struct genl_info *info)
{
	int ret;

	(void)skb;
	ret = rs_need(info, RS_A_NODE_ID);
	if (ret)
		return ret;
	return roswap_node_del(nla_get_u16(info->attrs[RS_A_NODE_ID]));
}

static int rs_cmd_node_state(struct sk_buff *skb, struct genl_info *info)
{
	int ret;

	(void)skb;
	ret = rs_need(info, RS_A_NODE_ID) ?: rs_need(info, RS_A_NODE_STATE);
	if (ret)
		return ret;
	return roswap_node_set_state(nla_get_u16(info->attrs[RS_A_NODE_ID]),
				     nla_get_u32(info->attrs[RS_A_NODE_STATE]));
}

static int rs_cmd_chunk_add(struct sk_buff *skb, struct genl_info *info)
{
	int ret;

	(void)skb;
	ret = rs_need(info, RS_A_NODE_ID) ?: rs_need(info, RS_A_CHUNK_GID) ?:
	      rs_need(info, RS_A_EPOCH) ?: rs_need(info, RS_A_RADDR) ?:
	      rs_need(info, RS_A_RKEY) ?: rs_need(info, RS_A_CHUNK_MB);
	if (ret)
		return ret;
	return roswap_chunk_add(nla_get_u16(info->attrs[RS_A_NODE_ID]),
				nla_get_u32(info->attrs[RS_A_CHUNK_GID]),
				nla_get_u32(info->attrs[RS_A_EPOCH]),
				nla_get_u64(info->attrs[RS_A_RADDR]),
				nla_get_u32(info->attrs[RS_A_RKEY]),
				nla_get_u32(info->attrs[RS_A_CHUNK_MB]));
}

static int rs_cmd_chunk_update(struct sk_buff *skb, struct genl_info *info)
{
	int ret;

	(void)skb;
	ret = rs_need(info, RS_A_NODE_ID) ?: rs_need(info, RS_A_CHUNK_GID) ?:
	      rs_need(info, RS_A_EPOCH) ?: rs_need(info, RS_A_RADDR) ?:
	      rs_need(info, RS_A_RKEY);
	if (ret)
		return ret;
	return roswap_chunk_update(nla_get_u16(info->attrs[RS_A_NODE_ID]),
				   nla_get_u32(info->attrs[RS_A_CHUNK_GID]),
				   nla_get_u32(info->attrs[RS_A_EPOCH]),
				   nla_get_u64(info->attrs[RS_A_RADDR]),
				   nla_get_u32(info->attrs[RS_A_RKEY]));
}

static int rs_cmd_chunk_retire(struct sk_buff *skb, struct genl_info *info)
{
	int ret;

	(void)skb;
	ret = rs_need(info, RS_A_NODE_ID) ?: rs_need(info, RS_A_CHUNK_GID);
	if (ret)
		return ret;
	return roswap_chunk_retire(nla_get_u16(info->attrs[RS_A_NODE_ID]),
				   nla_get_u32(info->attrs[RS_A_CHUNK_GID]));
}

static int rs_put_u32(struct sk_buff *msg, int attr, u32 value)
{
	return nla_put_u32(msg, attr, value);
}

static int rs_cmd_stats(struct sk_buff *skb, struct genl_info *info)
{
	struct rs_node_info *nodes = NULL;
	struct rs_map_stats st;
	struct sk_buff *msg;
	void *hdr;
	int nr, i;

	(void)skb;
	rs_map_stats(&st);
	nr = rs_map_node_list(NULL, 0);
	if (nr < 0)
		nr = 0;
	if (nr) {
		nodes = kcalloc((size_t)nr, sizeof(*nodes), GFP_KERNEL);
		if (!nodes)
			return -ENOMEM;
		if (rs_map_node_list(nodes, (u32)nr) < 0) {
			kfree(nodes);
			return -EIO;
		}
	}
	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg) {
		kfree(nodes);
		return -ENOMEM;
	}
	hdr = genlmsg_put_reply(msg, info, &rs_family, 0, RS_CMD_GET_STATS);
	if (!hdr) {
		nlmsg_free(msg);
		kfree(nodes);
		return -ENOMEM;
	}
	if (rs_put_u32(msg, RS_A_NR_SLOTS, st.nr_slots) ||
	    rs_put_u32(msg, RS_A_USED_SLOTS, st.used_slots) ||
	    rs_put_u32(msg, RS_A_FREE_EXTENTS, st.free_extents) ||
	    rs_put_u32(msg, RS_A_NR_NODES, st.nr_nodes) ||
	    rs_put_u32(msg, RS_A_NR_CHUNKS, st.nr_chunks) ||
	    rs_put_u32(msg, RS_A_LINKS_UP, roswap_links_up()) ||
	    rs_put_u32(msg, RS_A_DEGRADED, st.degraded_slots) ||
	    rs_put_u32(msg, RS_A_LOST, st.lost_slots) ||
	    rs_put_u32(msg, RS_A_HEDGE_SENT, roswap_hedge_sent()) ||
	    rs_put_u32(msg, RS_A_HEDGE_WON, roswap_hedge_won()) ||
	    rs_put_u32(msg, RS_A_KEY_WAIT, roswap_key_wait_hits()))
		goto nospace;
	if (nr) {
		u8 *raw = kmalloc((size_t)nr * 4, GFP_KERNEL);

		if (!raw)
			goto nospace;
		for (i = 0; i < nr; i++) {
			raw[i * 4] = (u8)nodes[i].id;
			raw[i * 4 + 1] = (u8)(nodes[i].id >> 8);
			raw[i * 4 + 2] = nodes[i].state;
			raw[i * 4 + 3] = 0;
		}
		if (nla_put(msg, RS_A_NODE_LIST, nr * 4, raw)) {
			kfree(raw);
			goto nospace;
		}
		kfree(raw);
	}
	kfree(nodes);
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
nospace:
	nlmsg_free(msg);
	kfree(nodes);
	return -EMSGSIZE;
}

static void rs_evt_send(struct work_struct *work)
{
	struct rs_evt *evt = container_of(work, struct rs_evt, work);
	struct sk_buff *msg;
	void *hdr;
	int err;

	if (!rs_evt_wq)
		goto out;
	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		goto out;
	hdr = genlmsg_put(msg, 0, 0, &rs_family, 0,
			  evt->kind ? RS_EVT_DATA_LOST : RS_EVT_NODE_STATE);
	if (!hdr) {
		nlmsg_free(msg);
		goto out;
	}
	if (nla_put_u16(msg, RS_A_NODE_ID, evt->node_id))
		goto nospace;
	if (!evt->kind) {
		if (nla_put_u32(msg, RS_A_NODE_STATE, evt->state) ||
		    nla_put_u64_64bit(msg, RS_A_BOOT_ID, evt->boot_id, RS_A_PAD))
			goto nospace;
	} else if (nla_put_u32(msg, RS_A_LOST, evt->lost)) {
		goto nospace;
	}
	genlmsg_end(msg, hdr);
	err = genlmsg_multicast(&rs_family, msg, 0, 0, GFP_KERNEL);
	if (err && err != -ESRCH)
		pr_warn_ratelimited("event dropped: %d\n", err);
	goto out;
nospace:
	nlmsg_free(msg);
out:
	kfree(evt);
}

static int rs_evt_queue(u16 node_id, u32 state, u64 boot_id, u32 lost, u8 kind)
{
	struct rs_evt *evt;

	if (!rs_evt_wq)
		return -ENODEV;
	evt = kzalloc(sizeof(*evt), GFP_ATOMIC);
	if (!evt)
		return -ENOMEM;
	evt->node_id = node_id;
	evt->state = state;
	evt->boot_id = boot_id;
	evt->lost = lost;
	evt->kind = kind;
	INIT_WORK(&evt->work, rs_evt_send);
	queue_work(rs_evt_wq, &evt->work);
	return 0;
}

void rs_genl_node_state(u16 node_id, u32 state, u64 boot_id)
{
	rs_evt_queue(node_id, state, boot_id, 0, 0);
}

void rs_genl_data_lost(u16 node_id, u32 lost)
{
	rs_evt_queue(node_id, 0, 0, lost, 1);
}

static const struct genl_ops rs_ops[] = {
	{
		.cmd = RS_CMD_NODE_ADD,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_node_add,
	},
	{
		.cmd = RS_CMD_CHUNK_ADD,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_chunk_add,
	},
	{
		.cmd = RS_CMD_CHUNK_UPDATE,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_chunk_update,
	},
	{
		.cmd = RS_CMD_GET_STATS,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_stats,
	},
	{
		.cmd = RS_CMD_NODE_DEL,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_node_del,
	},
	{
		.cmd = RS_CMD_NODE_SET_STATE,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_node_state,
	},
	{
		.cmd = RS_CMD_CHUNK_RETIRE,
		.flags = GENL_ADMIN_PERM,
		.doit = rs_cmd_chunk_retire,
	},
};

static struct genl_family rs_family = {
	.name = "roswap",
	.version = 1,
	.maxattr = RS_A_MAX,
	.policy = rs_policy,
	.ops = rs_ops,
	.n_ops = ARRAY_SIZE(rs_ops),
	.mcgrps = rs_mcgrps,
	.n_mcgrps = ARRAY_SIZE(rs_mcgrps),
	.module = THIS_MODULE,
};

int rs_genl_register(void)
{
	int ret;

	rs_evt_wq = alloc_workqueue("roswap_evt", WQ_UNBOUND, 1);
	if (!rs_evt_wq)
		return -ENOMEM;
	ret = genl_register_family(&rs_family);
	if (ret) {
		destroy_workqueue(rs_evt_wq);
		rs_evt_wq = NULL;
	}
	return ret;
}

void rs_genl_unregister(void)
{
	genl_unregister_family(&rs_family);
}

void rs_genl_exit(void)
{
	if (!rs_evt_wq)
		return;
	flush_workqueue(rs_evt_wq);
	destroy_workqueue(rs_evt_wq);
	rs_evt_wq = NULL;
}
