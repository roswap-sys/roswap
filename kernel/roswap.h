/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_H
#define _ROSWAP_H

#include <linux/atomic.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#define RS_CONN_MAGIC		0x52535750
#define RS_CONN_VERSION		1
#define RS_QP_BULK		2

#define ROSWAP_QUEUE_DEPTH	128
#define ROSWAP_CM_TIMEOUT_MS	2000
#define ROSWAP_CONNECT_TIMEOUT_MS 30000
/* 设计里的 dead_after_s。到点还没连上就结束等待中的 I/O，避免回收路径一直堵着。 */
#define ROSWAP_RECOVER_LIMIT_MS	10000

enum rs_link_state {
	RS_LINK_OFF = 0,
	RS_LINK_UP = 1,
	RS_LINK_RECOVERING = 2,
};

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

struct roswap_conn {
	struct rdma_cm_id *cm_id;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_status;
	atomic_t state;
	atomic_t inflight;
	struct work_struct recover_work;
	wait_queue_head_t stop_wq;
	bool stop;
	bool link_lost;
	bool fatal;
	unsigned long recover_deadline;
};

struct roswap_cmd {
	struct ib_cqe cqe;
	u64 dma;
	u32 len;
	enum dma_data_direction dir;
};

#endif
