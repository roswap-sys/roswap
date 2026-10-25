/* 默认兼容宏，面向 Linux 5.10 + MLNX_OFED 5.8。
 * 在目标机上执行 roswap probe 会按实际头文件覆盖本文件。
 */
#ifndef _RS_COMPAT_H
#define _RS_COMPAT_H

/* 5.10 起可在 CM 回调里调用，避免和 handler_mutex 死锁。 */
#define RS_HAVE_RDMA_CONNECT_LOCKED 1

/* 建连前设置 RC ack timeout（4.096us * 2^n）。 */
#define RS_HAVE_RDMA_SET_ACK_TIMEOUT 1

/* rdma_create_id(net, id, handler, ctx, ps, qp_type) */
#define RS_RDMA_CREATE_ID_NET 1

#define RS_HAVE_IB_ALLOC_CQ 1
#define RS_HAVE_IB_PROCESS_CQ_DIRECT 1

/* blk_mq_alloc_disk 从 5.14 才有；5.10 用 blk_mq_init_queue + alloc_disk。 */
#define RS_HAVE_BLK_MQ_ALLOC_DISK 0

/* 5.10 的 ib_client.add 返回 void；更晚的内核改为 int。 */
#define RS_IB_ADD_RETURNS_INT 0

#endif
