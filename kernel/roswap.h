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

#define ROSWAP_QUEUE_DEPTH	128
#define ROSWAP_CM_TIMEOUT_MS	2000
#define ROSWAP_CONNECT_TIMEOUT_MS 30000
/* 设计里的 dead_after_s。到点还没连上就结束等待中的 I/O，避免回收路径一直堵着。 */
#define ROSWAP_RECOVER_LIMIT_MS	10000
#define ROSWAP_FAULT_POLL_US	20
/* 4.096us * 2^14 ≈ 67ms。retry_count 4 时在途请求大约 335ms 报错。 */
#define ROSWAP_ACK_TIMEOUT	14

enum rs_link_state {
	RS_LINK_OFF = 0,
	RS_LINK_UP = 1,
	RS_LINK_RECOVERING = 2,
};

/*
 * Generic Netlink "roswap"。数字是用户态 ABI，改之前要一起改 src/genl.rs。
 * NODE_ADD 只登记地址；CHUNK_ADD / CHUNK_UPDATE 把 raddr、rkey 交进来。
 */
enum rs_genl_cmd {
	RS_CMD_UNSPEC,
	RS_CMD_NODE_ADD,
	RS_CMD_CHUNK_ADD,
	RS_CMD_CHUNK_UPDATE,
	RS_CMD_GET_STATS,
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
	__RS_A_MAX,
};
#define RS_A_MAX (__RS_A_MAX - 1)

/*
 * 48 字节，网络序。rsvd2[0] 是客户端声称的 Chunk 大小，单位 MiB。
 * 提供端据此拒绝比 MR 更大的磁盘，避免越界 RDMA 把 QP 打进 ERR。
 */
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

int roswap_node_add(u16 node_id, const char *server);
int roswap_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 raddr, u32 rkey,
		     u32 chunk_mb);
int roswap_chunk_update(u32 gid, u32 epoch, u64 raddr, u32 rkey);
int roswap_links_up(void);
int rs_genl_register(void);
void rs_genl_unregister(void);

#endif
