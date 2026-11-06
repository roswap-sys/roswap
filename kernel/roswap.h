// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/inet.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/version.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

// private magic
#define RS_CONN_MAGIC		0x52535750
#define RS_CONN_VERSION		1
#define RS_QP_BULK		2

#define ROSWAP_QUEUE_DEPTH	128
#define ROSWAP_CM_TIMEOUT_MS	2000
#define ROSWAP_CONNECT_TIMEOUT_MS 30000

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define roswap_cleanup_disk(disk) put_disk(disk)
#else
#define roswap_cleanup_disk(disk) blk_cleanup_disk(disk)
#endif





static char *server;
module_param(server, charp, 0444);
MODULE_PARM_DESC(server, " provider rdma_cm address, ip:port");

static ulong raddr;
module_param(raddr, ulong, 0444);
MODULE_PARM_DESC(raddr, " remote chunk base address");

static uint rkey;
module_param(rkey, uint, 0444);
MODULE_PARM_DESC(rkey, " remote chunk rkey");

static uint chunk_size_mb = 64;
module_param(chunk_size_mb, uint, 0444);
MODULE_PARM_DESC(chunk_size_mb, " remote chunk size in MiB, also the disk capacity");


static int roswap_major_id;
static struct gendisk *roswap_disk;
static struct blk_mq_tag_set roswap_tag_set;
static struct roswap_conn roswap_conn;


struct roswap_conn {
	struct rdma_cm_id *cm_id;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_status;
	bool connected;
};

struct roswap_cmd {
	struct ib_cqe cqe;
	u64 dma;
	u32 len;
	enum dma_data_direction dir;
};

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

static void __exit roswap_exit(void);
static int __init roswap_init(void);

//roswap function
static void roswap_rdma_done(struct ib_cq *cq, struct ib_wc *wc);
static int roswap_add_disk(void);
static int roswap_parse_server(struct sockaddr_in *sin);
static int roswap_cm_wait(struct roswap_conn *conn, 
    enum rdma_cm_event_type expected,
    unsigned int timeout_ms);

//roswap rdma function
static int roswap_rdma_connect(struct roswap_conn *conn);
static int roswap_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev);
static blk_status_t roswap_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd);