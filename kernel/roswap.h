/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_H
#define _ROSWAP_H

#include <linux/build_bug.h>
#include <linux/types.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "map.h"

#define RS_CONN_MAGIC		0x52535750
#define RS_CONN_VERSION		1
#define RS_QP_FAULT		1
#define RS_QP_BULK		2
#define RS_QP_HB		3

#define ROSWAP_QUEUE_DEPTH	128
#define ROSWAP_CM_TIMEOUT_MS	2000
#define ROSWAP_CONNECT_TIMEOUT_MS 30000
#define ROSWAP_RECOVER_LIMIT_MS	10000
#define ROSWAP_FAULT_POLL_US	20
#define ROSWAP_ACK_TIMEOUT	14
#define ROSWAP_HEDGE_US		1500

enum rs_link_state {
	RS_LINK_OFF = 0,
	RS_LINK_UP = 1,
	RS_LINK_RECOVERING = 2,
	RS_LINK_KEY_WAIT = 3,
	RS_LINK_FATAL = 4,
};

enum rs_genl_cmd {
	RS_CMD_UNSPEC,
	RS_CMD_NODE_ADD,
	RS_CMD_CHUNK_ADD,
	RS_CMD_CHUNK_UPDATE,
	RS_CMD_GET_STATS,
	RS_CMD_NODE_DEL,
	RS_CMD_NODE_SET_STATE,
	RS_CMD_CHUNK_RETIRE,
	RS_EVT_NODE_STATE,
	RS_EVT_DATA_LOST,
};

enum rs_genl_attr {
	RS_A_UNSPEC,
	RS_A_NODE_ID,
	RS_A_ADDR,
	RS_A_CHUNK_GID,
	RS_A_EPOCH,
	RS_A_RADDR,
	RS_A_RKEY,
	RS_A_CHUNK_MB,
	RS_A_NR_SLOTS,
	RS_A_USED_SLOTS,
	RS_A_FREE_EXTENTS,
	RS_A_NR_NODES,
	RS_A_NR_CHUNKS,
	RS_A_LINKS_UP,
	RS_A_HB_RADDR,
	RS_A_HB_RKEY,
	RS_A_NODE_STATE,
	RS_A_BOOT_ID,
	RS_A_DEGRADED,
	RS_A_LOST,
	RS_A_NODE_LIST,
	RS_A_PAD,
	__RS_A_MAX,
};
#define RS_A_MAX (__RS_A_MAX - 1)

struct rs_conn_pdata {
	__be32 magic;
	__be16 version;
	u8 purpose;
	u8 rsvd;
	__be16 cpu;
	__be16 node_id;
	__be32 node_epoch;
	u8 cluster_uuid[16];
	__be64 master_boot_id;
	__be32 rsvd2[2];
};
static_assert(sizeof(struct rs_conn_pdata) == 48);

struct rs_hb_cfg {
	u64 raddr;
	u32 rkey;
	u64 boot_id;
};

int roswap_node_add(u16 node_id, const char *server, const struct rs_hb_cfg *hb);
int roswap_node_del(u16 node_id);
int roswap_node_set_state(u16 node_id, u32 state);
int roswap_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 raddr, u32 rkey,
		     u32 chunk_mb);
int roswap_chunk_update(u16 node_id, u32 local_gid, u32 epoch, u64 raddr,
		      u32 rkey);
int roswap_chunk_retire(u16 node_id, u32 local_gid);
int roswap_links_up(void);
void rs_genl_node_state(u16 node_id, u32 state, u64 boot_id);
void rs_genl_data_lost(u16 node_id, u32 lost);
int rs_genl_register(void);
void rs_genl_unregister(void);
void rs_genl_exit(void);

#endif
