// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/delay.h>
#include <linux/inet.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched/mm.h>
#include "compat.h"
#include "roswap.h"

static char *server;
module_param(server, charp, 0444);
MODULE_PARM_DESC(server, "provider rdma_cm address, ip:port");

static ulong raddr;
module_param(raddr, ulong, 0444);
MODULE_PARM_DESC(raddr, "remote chunk base address");

/* rkey 是远端写权限。0444 会把它挂进 sysfs，所以只允许 insmod 传入。 */
static uint rkey;
module_param(rkey, uint, 0);
MODULE_PARM_DESC(rkey, "remote chunk rkey, insmod only");

static uint chunk_size_mb = 64;
module_param(chunk_size_mb, uint, 0444);
MODULE_PARM_DESC(chunk_size_mb, "remote chunk size in MiB, also the disk capacity");

static void roswap_link_failed(void);

static int roswap_drop_set(const char *val, const struct kernel_param *kp)
{
	roswap_link_failed();
	return 0;
}

static int roswap_drop_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "0\n");
}

static const struct kernel_param_ops roswap_drop_ops = {
	.set = roswap_drop_set,
	.get = roswap_drop_get,
};

module_param_cb(drop_connection, &roswap_drop_ops, NULL, 0200);
MODULE_PARM_DESC(drop_connection, "write anything to drop the RC connection");

static int roswap_major_id;
static struct gendisk *roswap_disk;
static struct blk_mq_tag_set roswap_tag_set;
static struct roswap_conn roswap_conn;
static struct workqueue_struct *roswap_wq;

static void roswap_rdma_done(struct ib_cq *cq, struct ib_wc *wc);
static void roswap_link_failed(void);
static void roswap_rdma_teardown(struct roswap_conn *conn);
static void roswap_recover(struct work_struct *work);

static bool roswap_range_in_chunk(u64 off, u32 len)
{
	u64 cap = (u64)chunk_size_mb << 20;

	if (!len || len > cap)
		return false;
	return off <= cap - len;
}

static blk_status_t roswap_queue_rq(struct blk_mq_hw_ctx *hctx,
				    const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct roswap_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct roswap_conn *conn = &roswap_conn;
	struct ib_device *dev;
	struct ib_rdma_wr wr = {};
	struct ib_sge sge;
	struct bio_vec bv;
	u64 off;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		cmd->dir = DMA_FROM_DEVICE;
		wr.wr.opcode = IB_WR_RDMA_READ;
		break;
	case REQ_OP_WRITE:
		cmd->dir = DMA_TO_DEVICE;
		wr.wr.opcode = IB_WR_RDMA_WRITE;
		break;
	default:
		return BLK_STS_NOTSUPP;
	}

	switch (atomic_read(&conn->state)) {
	case RS_LINK_UP:
		break;
	case RS_LINK_RECOVERING:
		return BLK_STS_RESOURCE;
	default:
		return BLK_STS_IOERR;
	}

	dev = conn->cm_id->device;
	bv = req_bvec(rq);
	cmd->len = bv.bv_len;
	off = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	if (!roswap_range_in_chunk(off, cmd->len))
		return BLK_STS_IOERR;

	cmd->dma = ib_dma_map_page(dev, bv.bv_page, bv.bv_offset, bv.bv_len,
				   cmd->dir);
	if (ib_dma_mapping_error(dev, cmd->dma))
		return BLK_STS_RESOURCE;

	sge.addr = cmd->dma;
	sge.length = cmd->len;
	sge.lkey = conn->pd->local_dma_lkey;
	cmd->cqe.done = roswap_rdma_done;
	wr.wr.wr_cqe = &cmd->cqe;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.remote_addr = raddr + off;
	wr.rkey = rkey;

	blk_mq_start_request(rq);
	atomic_inc(&conn->inflight);
	if (ib_post_send(conn->cm_id->qp, &wr.wr, NULL)) {
		ib_dma_unmap_page(dev, cmd->dma, cmd->len, cmd->dir);
		atomic_dec(&conn->inflight);
		roswap_link_failed();
		blk_mq_requeue_request(rq, false);
		return BLK_STS_OK;
	}
	return BLK_STS_OK;
}

static const struct blk_mq_ops roswap_mq_ops = {
	.queue_rq = roswap_queue_rq,
};

static const struct block_device_operations roswap_fops = {
	.owner = THIS_MODULE,
};

static void roswap_link_failed(void)
{
	if (READ_ONCE(roswap_conn.fatal) || READ_ONCE(roswap_conn.stop))
		return;
	if (atomic_cmpxchg(&roswap_conn.state, RS_LINK_UP,
			   RS_LINK_RECOVERING) != RS_LINK_UP)
		return;
	roswap_conn.recover_deadline = jiffies +
				       msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	WRITE_ONCE(roswap_conn.link_lost, true);
	if (roswap_disk)
		blk_mq_stop_hw_queues(roswap_disk->queue);
	queue_work(roswap_wq, &roswap_conn.recover_work);
}

/* rkey / raddr 已经对不上。再重连只会用同一对参数打出访问错误。 */
static void roswap_link_dead(void)
{
	atomic_set(&roswap_conn.state, RS_LINK_OFF);
	WRITE_ONCE(roswap_conn.fatal, true);
	wake_up(&roswap_conn.stop_wq);
}

static bool roswap_wc_permanent(enum ib_wc_status status)
{
	switch (status) {
	case IB_WC_LOC_ACCESS_ERR:
	case IB_WC_LOC_PROT_ERR:
	case IB_WC_REM_ACCESS_ERR:
	case IB_WC_REM_INV_REQ_ERR:
	case IB_WC_REM_OP_ERR:
		return true;
	default:
		return false;
	}
}

static void roswap_release_waiting_io(void)
{
	if (!roswap_disk)
		return;
	blk_mq_start_hw_queues(roswap_disk->queue);
	blk_mq_kick_requeue_list(roswap_disk->queue);
}

static int roswap_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
	struct roswap_conn *conn = id->context;

	if (ev->event == RDMA_CM_EVENT_DISCONNECTED ||
	    ev->event == RDMA_CM_EVENT_DEVICE_REMOVAL) {
		pr_warn("%s\n", rdma_event_msg(ev->event));
		WRITE_ONCE(conn->link_lost, true);
		roswap_link_failed();
	}
	conn->cm_event = ev->event;
	conn->cm_status = ev->status;
	complete(&conn->cm_done);
	return 0;
}

static int roswap_cm_wait(struct roswap_conn *conn,
			  enum rdma_cm_event_type expected,
			  unsigned int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	for (;;) {
		unsigned int slice;
		long left;

		if (READ_ONCE(conn->stop) || READ_ONCE(conn->fatal))
			return -ECONNABORTED;
		/*
		 * 初次建连的 deadline 是 0，不能拿来判断。只有恢复中才提前结束，
		 * 否则一次 30 秒的 resolve 会把 10 秒上限吃掉。
		 */
		if (atomic_read(&conn->state) == RS_LINK_RECOVERING &&
		    time_after(jiffies, conn->recover_deadline))
			return -ETIMEDOUT;
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		slice = jiffies_to_msecs(end - jiffies);
		if (!slice)
			return -ETIMEDOUT;
		if (slice > 200)
			slice = 200;
		left = wait_for_completion_killable_timeout(&conn->cm_done,
						msecs_to_jiffies(slice));
		if (left < 0)
			return left;
		if (left)
			break;
	}
	if (conn->cm_event != expected) {
		pr_err("rdma_cm %s, status %d\n",
		       rdma_event_msg(conn->cm_event), conn->cm_status);
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
		.rsvd2 = { cpu_to_be32(chunk_size_mb), 0 },
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

	ret = roswap_parse_server(&sin);
	if (ret) {
		pr_err("invalid server '%s', expect ip:port\n", server);
		return ret;
	}

	reinit_completion(&conn->cm_done);
	id = rdma_create_id(&init_net, roswap_cm_handler, conn, RDMA_PS_TCP,
			    IB_QPT_RC);
	if (IS_ERR(id))
		return PTR_ERR(id);

	ret = rdma_resolve_addr(id, NULL, (struct sockaddr *)&sin,
				ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ADDR_RESOLVED,
				     2 * ROSWAP_CM_TIMEOUT_MS);
	if (ret)
		goto err_id;

	reinit_completion(&conn->cm_done);
	ret = rdma_resolve_route(id, ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ROUTE_RESOLVED,
				     2 * ROSWAP_CM_TIMEOUT_MS);
	if (ret)
		goto err_id;

	conn->pd = ib_alloc_pd(id->device, 0);
	if (IS_ERR(conn->pd)) {
		ret = PTR_ERR(conn->pd);
		conn->pd = NULL;
		goto err_id;
	}

	conn->cq = ib_alloc_cq(id->device, conn, ROSWAP_QUEUE_DEPTH, 0,
			       IB_POLL_SOFTIRQ);
	if (IS_ERR(conn->cq)) {
		ret = PTR_ERR(conn->cq);
		conn->cq = NULL;
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
	ret = rdma_connect(id, &param);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ESTABLISHED,
				     ROSWAP_CONNECT_TIMEOUT_MS);
	if (ret)
		goto err_qp;

	conn->cm_id = id;
	pr_info("connected to %s via %s, chunk %u MiB\n", server,
		dev_name(&id->device->dev), chunk_size_mb);
	return 0;

err_qp:
	rdma_destroy_qp(id);
err_cq:
	ib_free_cq(conn->cq);
	conn->cq = NULL;
err_pd:
	ib_dealloc_pd(conn->pd);
	conn->pd = NULL;
err_id:
	rdma_destroy_id(id);
	return ret;
}

static void roswap_rdma_teardown(struct roswap_conn *conn)
{
	if (!conn->cm_id)
		return;
	rdma_disconnect(conn->cm_id);
	while (atomic_read(&conn->inflight) > 0)
		msleep(1);
	rdma_destroy_qp(conn->cm_id);
	ib_free_cq(conn->cq);
	ib_dealloc_pd(conn->pd);
	rdma_destroy_id(conn->cm_id);
	conn->cm_id = NULL;
	conn->cq = NULL;
	conn->pd = NULL;
}

static void roswap_unquiesce(bool *quiesced)
{
	if (*quiesced && roswap_disk)
		blk_mq_unquiesce_queue(roswap_disk->queue);
	*quiesced = false;
}

static bool roswap_recover_aborted(struct roswap_conn *conn)
{
	return READ_ONCE(conn->stop) || READ_ONCE(conn->fatal) ||
	       time_after(jiffies, conn->recover_deadline);
}

static void roswap_recover(struct work_struct *work)
{
	struct roswap_conn *conn = container_of(work, struct roswap_conn, recover_work);
	unsigned int noio_flags = memalloc_noio_save();
	bool quiesced = false;
	int ret;

	/*
	 * stop 只挡住新的 queue_rq。已经读过 RS_LINK_UP 的那个还拿着 cm_id，
	 * 先等它返回再拆 QP。建链分配走 GFP_NOIO，避免回收再换到这块盘上。
	 */
	if (roswap_disk) {
		blk_mq_quiesce_queue(roswap_disk->queue);
		quiesced = true;
	}
	roswap_rdma_teardown(conn);
	while (!roswap_recover_aborted(conn)) {
		ret = roswap_rdma_connect(conn);
		if (roswap_recover_aborted(conn))
			break;
		if (ret) {
			unsigned long slice = HZ;
			unsigned long left;

			pr_warn_ratelimited("reconnect to %s failed: %d\n",
					    server, ret);
			if (roswap_recover_aborted(conn))
				break;
			left = conn->recover_deadline - jiffies;
			if (slice > left)
				slice = left;
			if (!slice || wait_event_timeout(conn->stop_wq,
					READ_ONCE(conn->stop) ||
					READ_ONCE(conn->fatal), slice))
				break;
			continue;
		}
		WRITE_ONCE(conn->link_lost, false);
		if (atomic_cmpxchg(&conn->state, RS_LINK_RECOVERING,
				   RS_LINK_UP) != RS_LINK_RECOVERING)
			break;
		roswap_unquiesce(&quiesced);
		roswap_release_waiting_io();
		if (READ_ONCE(conn->link_lost) && !READ_ONCE(conn->fatal))
			roswap_link_failed();
		memalloc_noio_restore(noio_flags);
		return;
	}
	roswap_rdma_teardown(conn);
	if (!READ_ONCE(conn->fatal) && !READ_ONCE(conn->stop))
		pr_err("reconnect to %s gave up\n", server);
	atomic_cmpxchg(&conn->state, RS_LINK_RECOVERING, RS_LINK_OFF);
	roswap_unquiesce(&quiesced);
	if (!READ_ONCE(conn->stop))
		roswap_release_waiting_io();
	memalloc_noio_restore(noio_flags);
}

static void roswap_rdma_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct roswap_cmd *cmd = container_of(wc->wr_cqe, struct roswap_cmd, cqe);
	struct request *rq = blk_mq_rq_from_pdu(cmd);

	ib_dma_unmap_page(cq->device, cmd->dma, cmd->len, cmd->dir);
	if (wc->status == IB_WC_SUCCESS) {
		blk_mq_end_request(rq, BLK_STS_OK);
	} else {
		pr_err_ratelimited("rdma %s failed: %s\n",
				   cmd->dir == DMA_TO_DEVICE ? "write" : "read",
				   ib_wc_status_msg(wc->status));
		if (roswap_wc_permanent(wc->status))
			roswap_link_dead();
		if (roswap_wc_permanent(wc->status) ||
		    READ_ONCE(roswap_conn.fatal) ||
		    atomic_read(&roswap_conn.state) == RS_LINK_OFF) {
			blk_mq_end_request(rq, BLK_STS_IOERR);
		} else {
			roswap_link_failed();
			blk_mq_requeue_request(rq, false);
		}
	}
	atomic_dec(&roswap_conn.inflight);
}

static int roswap_add_disk(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	struct request_queue *q;
#endif
	int ret;

	memset(&roswap_tag_set, 0, sizeof(roswap_tag_set));
	roswap_tag_set.ops = &roswap_mq_ops;
	roswap_tag_set.nr_hw_queues = 1;
	roswap_tag_set.queue_depth = ROSWAP_QUEUE_DEPTH;
	roswap_tag_set.numa_node = NUMA_NO_NODE;
	roswap_tag_set.cmd_size = sizeof(struct roswap_cmd);

	ret = blk_mq_alloc_tag_set(&roswap_tag_set);
	if (ret)
		return ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	q = blk_mq_init_queue(&roswap_tag_set);
	if (IS_ERR(q)) {
		ret = PTR_ERR(q);
		goto err_tag;
	}
	roswap_disk = alloc_disk(1);
	if (!roswap_disk) {
		blk_cleanup_queue(q);
		ret = -ENOMEM;
		goto err_tag;
	}
	roswap_disk->queue = q;
#else
	roswap_disk = roswap_alloc_disk(&roswap_tag_set);
	if (IS_ERR(roswap_disk)) {
		ret = PTR_ERR(roswap_disk);
		roswap_disk = NULL;
		goto err_tag;
	}
#endif

	roswap_apply_queue_limits(roswap_disk);
	roswap_disk->major = roswap_major_id;
	roswap_disk->first_minor = 0;
	roswap_disk->minors = 1;
	roswap_disk->fops = &roswap_fops;
	roswap_disk->flags = GENHD_FL_NO_PART;
	strscpy(roswap_disk->disk_name, "roswap0", DISK_NAME_LEN);
	set_capacity(roswap_disk, (sector_t)chunk_size_mb << (20 - SECTOR_SHIFT));

	ret = roswap_register_disk(roswap_disk);
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

	init_waitqueue_head(&roswap_conn.stop_wq);
	INIT_WORK(&roswap_conn.recover_work, roswap_recover);
	init_completion(&roswap_conn.cm_done);
	roswap_wq = alloc_workqueue("roswap", WQ_MEM_RECLAIM, 1);
	if (!roswap_wq)
		return -ENOMEM;

	if (server && !chunk_size_mb) {
		pr_err("chunk_size_mb must be non-zero\n");
		ret = -EINVAL;
		goto err_wq;
	}

	roswap_major_id = register_blkdev(0, "roswap");
	if (roswap_major_id < 0) {
		pr_err("register_blkdev failed: %d\n", roswap_major_id);
		ret = roswap_major_id;
		goto err_wq;
	}

	if (server) {
		ret = roswap_rdma_connect(&roswap_conn);
		if (ret) {
			pr_err("connect %s failed: %d\n", server, ret);
			goto err_blkdev;
		}
	}

	ret = roswap_add_disk();
	if (ret) {
		pr_err("add_disk failed: %d\n", ret);
		goto err_conn;
	}

	if (server) {
		atomic_set(&roswap_conn.state, RS_LINK_UP);
		if (READ_ONCE(roswap_conn.link_lost))
			roswap_link_failed();
	}

	pr_info("added /dev/roswap0, major %d\n", roswap_major_id);
	return 0;

err_conn:
	roswap_rdma_teardown(&roswap_conn);
err_blkdev:
	unregister_blkdev(roswap_major_id, "roswap");
err_wq:
	destroy_workqueue(roswap_wq);
	roswap_wq = NULL;
	return ret;
}

static void __exit roswap_exit(void)
{
	WRITE_ONCE(roswap_conn.stop, true);
	wake_up(&roswap_conn.stop_wq);
	atomic_set(&roswap_conn.state, RS_LINK_OFF);
	cancel_work_sync(&roswap_conn.recover_work);
	roswap_release_waiting_io();

	del_gendisk(roswap_disk);
	roswap_cleanup_disk(roswap_disk);
	roswap_disk = NULL;
	blk_mq_free_tag_set(&roswap_tag_set);
	roswap_rdma_teardown(&roswap_conn);
	destroy_workqueue(roswap_wq);
	unregister_blkdev(roswap_major_id, "roswap");
	pr_info("unregistered roswap major %d\n", roswap_major_id);
}

module_init(roswap_init);
module_exit(roswap_exit);

MODULE_LICENSE("GPL");
