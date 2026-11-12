// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/netlink.h>
#include <net/genetlink.h>

#include "roswap.h"

static struct genl_family rs_family;

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
};

static int rs_need(struct genl_info *info, int attr)
{
	if (info->attrs[attr])
		return 0;
	return -EINVAL;
}

static int rs_cmd_node_add(struct sk_buff *skb, struct genl_info *info)
{
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
	return roswap_node_add(node_id, addr);
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
	ret = rs_need(info, RS_A_CHUNK_GID) ?: rs_need(info, RS_A_EPOCH) ?:
	      rs_need(info, RS_A_RADDR) ?: rs_need(info, RS_A_RKEY);
	if (ret)
		return ret;
	return roswap_chunk_update(nla_get_u32(info->attrs[RS_A_CHUNK_GID]),
				   nla_get_u32(info->attrs[RS_A_EPOCH]),
				   nla_get_u64(info->attrs[RS_A_RADDR]),
				   nla_get_u32(info->attrs[RS_A_RKEY]));
}

static int rs_put_u32(struct sk_buff *msg, int attr, u32 value)
{
	return nla_put_u32(msg, attr, value);
}

static int rs_cmd_stats(struct sk_buff *skb, struct genl_info *info)
{
	struct rs_map_stats st;
	struct sk_buff *msg;
	void *hdr;

	(void)skb;
	rs_map_stats(&st);
	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	hdr = genlmsg_put_reply(msg, info, &rs_family, 0, RS_CMD_GET_STATS);
	if (!hdr) {
		nlmsg_free(msg);
		return -ENOMEM;
	}
	if (rs_put_u32(msg, RS_A_NR_SLOTS, st.nr_slots) ||
	    rs_put_u32(msg, RS_A_USED_SLOTS, st.used_slots) ||
	    rs_put_u32(msg, RS_A_FREE_EXTENTS, st.free_extents) ||
	    rs_put_u32(msg, RS_A_NR_NODES, st.nr_nodes) ||
	    rs_put_u32(msg, RS_A_NR_CHUNKS, st.nr_chunks) ||
	    rs_put_u32(msg, RS_A_LINKS_UP, roswap_links_up())) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
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
};

static struct genl_family rs_family = {
	.name = "roswap",
	.version = 1,
	.maxattr = RS_A_MAX,
	.policy = rs_policy,
	.ops = rs_ops,
	.n_ops = ARRAY_SIZE(rs_ops),
	.module = THIS_MODULE,
};

int rs_genl_register(void)
{
	return genl_register_family(&rs_family);
}

void rs_genl_unregister(void)
{
	genl_unregister_family(&rs_family);
}
