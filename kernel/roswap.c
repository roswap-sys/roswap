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



#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define roswap_cleanup_disk(disk) put_disk(disk)
#else
#define roswap_cleanup_disk(disk) blk_cleanup_disk(disk)
#endif

// private magic
#define RS_CONN_MAGIC		0x52535750
#define RS_CONN_VERSION		1
#define RS_QP_BULK		2

#define ROSWAP_QUEUE_DEPTH	128
#define ROSWAP_CM_TIMEOUT_MS	2000

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


static blk_status_t roswap_queue_rq(struct blk_mq_hw_ctx *hctx,
				    const struct blk_mq_queue_data *bd)
{
	blk_mq_start_request(bd->rq);
	blk_mq_end_request(bd->rq, BLK_STS_IOERR);
	return BLK_STS_OK;
}

static const struct blk_mq_ops roswap_mq_ops = {
	.queue_rq = roswap_queue_rq,
};

static const struct block_device_operations roswap_fops = {
	.owner = THIS_MODULE,
};

struct roswap_conn {
	struct rdma_cm_id *cm_id;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_status;
	bool connected;
};

static int roswap_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
	struct roswap_conn *conn = id->context;
	if (ev->event == RDMA_CM_EVENT_DISCONNECTED || ev->event == RDMA_CM_EVENT_DEVICE_REMOVAL) {
		WRITE_ONCE(conn->connected, false);
		pr_warn("%s\n", rdma_event_msg(ev->event));
	}
	conn->cm_event = ev->event;
	conn->cm_status = ev->status;
	complete(&conn->cm_done);
	return 0;
}

static int roswap_cm_wait(struct roswap_conn *conn, 
					enum rdma_cm_event_type expected)
{
	if(!wait_for_completion_timeout(&conn->cm_done,
			msecs_to_jiffies(3*ROSWAP_CM_TIMEOUT_MS)))
			return -ETIMEDOUT;
	if (conn->cm_event != expected) {
		pr_err("rdma_cm %s, status %d \n", rdma_event_msg(conn->cm_event), conn->cm_status);
		return -ECONNREFUSED;
	}
	return 0;
}

static int roswap_parse_server(struct sockaddr_in *sin)
{
	const char *end;
	u16 port;

	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	if (!in4_pton(server, -1, (u8 *)&sin->sin_addr.s_addr, ':', &end) ||
	    *end != ':' || kstrtou16(end + 1, 10, &port))
		return -EINVAL;
	sin->sin_port = htons(port);
	return 0;
}



static int roswap_rdma_connect(struct roswap_conn *conn)
{
	struct rs_conn_pdata pdata = {
		.magic = cpu_to_be32(RS_CONN_MAGIC),
		.version = cpu_to_be16(RS_CONN_VERSION),
		.purpose = RS_QP_BULK,
	};
	struct rdma_conn_param param = {
		.private_data = &pdata,
		.private_data_len = sizeof(pdata),
		.retry_count = 4,
		.rnr_retry_count = 3,
	};
	struct ib_qp_init_attr qp_attr = {
		.cap = {
			.max_send_wr = ROSWAP_QUEUE_DEPTH,
			.max_recv_wr = 1,
			.max_send_sge = 1,
			.max_recv_sge = 1,
		},
		.sq_sig_type = IB_SIGNAL_REQ_WR,
		.qp_type = IB_QPT_RC,
	};
	struct sockaddr_in sin;
	struct rdma_cm_id *id;
	int ret;
	pr_info("info 1");

	ret = roswap_parse_server(&sin);
	if (ret) {
		pr_err("invalid server '%s', expect ip:port\n", server);
		return ret;
	}

	init_completion(&conn->cm_done);
	id = rdma_create_id(&init_net, roswap_cm_handler, conn, RDMA_PS_TCP,
			    IB_QPT_RC);
	if (IS_ERR(id))
		return PTR_ERR(id);

	ret = rdma_resolve_addr(id, NULL, (struct sockaddr *)&sin,
				ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ADDR_RESOLVED);
	if (ret)
		goto err_id;
	
	pr_info("info 2");

	reinit_completion(&conn->cm_done);
	ret = rdma_resolve_route(id, ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ROUTE_RESOLVED);
	if (ret)
		goto err_id;


	pr_info("info 3");

	conn->pd = ib_alloc_pd(id->device, 0);
	if (IS_ERR(conn->pd)) {
		ret = PTR_ERR(conn->pd);
		goto err_id;
	}

	conn->cq = ib_alloc_cq(id->device, conn, ROSWAP_QUEUE_DEPTH, 0,
			       IB_POLL_SOFTIRQ);
	if (IS_ERR(conn->cq)) {
		ret = PTR_ERR(conn->cq);
		goto err_pd;
	}

	qp_attr.send_cq = conn->cq;
	qp_attr.recv_cq = conn->cq;
	ret = rdma_create_qp(id, conn->pd, &qp_attr);
	if (ret)
		goto err_cq;

	param.initiator_depth = min_t(int, id->device->attrs.max_qp_init_rd_atom,
				      16);
	reinit_completion(&conn->cm_done);
	pr_info("info 4");
	ret = rdma_connect(id, &param);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ESTABLISHED);
	if (ret)
		goto err_qp;

	conn->cm_id = id;
	WRITE_ONCE(conn->connected, true);
	pr_info("connected to %s via %s\n", server, dev_name(&id->device->dev));
	return 0;

err_qp:
	rdma_destroy_qp(id);
err_cq:
	ib_free_cq(conn->cq);
err_pd:
	ib_dealloc_pd(conn->pd);
err_id:
	rdma_destroy_id(id);
	return ret;
}






static int roswap_add_disk(void)
{
	int ret;
	struct request_queue *q;

	memset(&roswap_tag_set, 0, sizeof(roswap_tag_set));
	roswap_tag_set.ops = &roswap_mq_ops;
	roswap_tag_set.nr_hw_queues = 1;
	roswap_tag_set.queue_depth = 128;
	roswap_tag_set.numa_node = NUMA_NO_NODE;

	ret = blk_mq_alloc_tag_set(&roswap_tag_set);
	if (ret)
		return ret;

	roswap_disk = blk_mq_alloc_disk(&roswap_tag_set, NULL);
	if (IS_ERR(roswap_disk)) {
		ret = PTR_ERR(roswap_disk);
		roswap_disk = NULL;
		goto err_tag;
	}

	q = roswap_disk->queue;
	blk_queue_logical_block_size(q, PAGE_SIZE);
	blk_queue_physical_block_size(q, PAGE_SIZE);
	blk_queue_max_hw_sectors(q, PAGE_SIZE >> SECTOR_SHIFT);
	blk_queue_max_segments(q, 1);
	blk_queue_dma_alignment(q, PAGE_SIZE - 1);
	blk_queue_flag_set(QUEUE_FLAG_NONROT, q);

	roswap_disk->major = roswap_major_id;
	roswap_disk->first_minor = 0;
	roswap_disk->minors = 1;
	roswap_disk->fops = &roswap_fops;
	roswap_disk->flags = GENHD_FL_NO_PART;
	strscpy(roswap_disk->disk_name, "roswap0", DISK_NAME_LEN);
	set_capacity(roswap_disk,  (sector_t)chunk_size_mb << (20 - SECTOR_SHIFT));




	ret = add_disk(roswap_disk);
	if (ret)
		goto err_disk;



	return 0;

err_disk:
	roswap_cleanup_disk(roswap_disk);
	roswap_disk = NULL;
err_tag:
	blk_mq_free_tag_set(&roswap_tag_set);
	return ret;
}

static int __init roswap_init(void)
{
	int ret;

	roswap_major_id = register_blkdev(0, "roswap");
	if (roswap_major_id < 0) {
		pr_err("register_blkdev failed: %d\n", roswap_major_id);
		return roswap_major_id;
	}

	if (server) {
		ret = roswap_rdma_connect(&roswap_conn);
		if (ret) {
			pr_err("connect %s failed: %d\n",server,ret);
			goto err_blkdev;
		}
	}


	ret = roswap_add_disk();
	if (ret) {
		unregister_blkdev(roswap_major_id, "roswap");
		pr_err("add_disk failed: %d\n", ret);
		return ret;
	}

	pr_info("added /dev/roswap0, major %d\n", roswap_major_id);
	return 0;

err_blkdev:
	unregister_blkdev(roswap_major_id,"roswap");
	return ret;
}


static void __exit roswap_exit(void)
{
	del_gendisk(roswap_disk);
	roswap_cleanup_disk(roswap_disk);
	roswap_disk = NULL;
	blk_mq_free_tag_set(&roswap_tag_set);
	unregister_blkdev(roswap_major_id, "roswap");
	pr_info("unregistered roswap major %d\n", roswap_major_id);
}

module_init(roswap_init);
module_exit(roswap_exit);

MODULE_LICENSE("GPL");
