// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt
#include "roswap.h"


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
	if (!READ_ONCE(conn->connected))
		return BLK_STS_IOERR;

	dev = conn->cm_id->device;
	bv = req_bvec(rq);
	cmd->len = bv.bv_len;
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
	wr.remote_addr = raddr + ((u64)blk_rq_pos(rq) << SECTOR_SHIFT);
	wr.rkey = rkey;

	blk_mq_start_request(rq);
	if (ib_post_send(conn->cm_id->qp, &wr.wr, NULL)) {
		ib_dma_unmap_page(dev, cmd->dma, cmd->len, cmd->dir);
		return BLK_STS_IOERR;
	}
	return BLK_STS_OK;
}

static const struct blk_mq_ops roswap_mq_ops = {
	.queue_rq = roswap_queue_rq,
};

static const struct block_device_operations roswap_fops = {
	.owner = THIS_MODULE,
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
					enum rdma_cm_event_type expected,
					unsigned int timeout_ms)
{
	long left = wait_for_completion_killable_timeout(&conn->cm_done,
					msecs_to_jiffies(timeout_ms));
	if (left < 0)
		return left;
	if (!left)
		return -ETIMEDOUT;
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

	init_completion(&conn->cm_done);
	id = rdma_create_id(&init_net, roswap_cm_handler, conn, RDMA_PS_TCP,
			    IB_QPT_RC);
	if (IS_ERR(id))
		return PTR_ERR(id);

	ret = rdma_resolve_addr(id, NULL, (struct sockaddr *)&sin,
				ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ADDR_RESOLVED,2 * ROSWAP_CM_TIMEOUT_MS);
	if (ret)
		goto err_id;
	
	reinit_completion(&conn->cm_done);
	ret = rdma_resolve_route(id, ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ROUTE_RESOLVED,2* ROSWAP_CM_TIMEOUT_MS);
	if (ret)
		goto err_id;

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
		ret = roswap_cm_wait(conn, RDMA_CM_EVENT_ESTABLISHED,ROSWAP_CONNECT_TIMEOUT_MS);
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


static void roswap_rdma_disconnect(struct roswap_conn *conn)
{
	if (!conn->cm_id)
		return;
	WRITE_ONCE(conn->connected, false);
	rdma_disconnect(conn->cm_id);
	rdma_destroy_qp(conn->cm_id);
	ib_free_cq(conn->cq);
	ib_dealloc_pd(conn->pd);
	rdma_destroy_id(conn->cm_id);
	conn->cm_id = NULL;
	return; 
}

static void roswap_rdma_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct roswap_cmd *cmd = container_of(wc->wr_cqe, struct roswap_cmd, cqe);
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	blk_status_t status = BLK_STS_OK;

	ib_dma_unmap_page(cq->device, cmd->dma, cmd->len, cmd->dir);
	if (wc->status != IB_WC_SUCCESS) {
		pr_err_ratelimited("rdma %s failed: %s\n",
				   cmd->dir == DMA_TO_DEVICE ? "write" : "read",
				   ib_wc_status_msg(wc->status));
		WRITE_ONCE(roswap_conn.connected, false);
		status = BLK_STS_IOERR;
	}
	blk_mq_end_request(rq, status);
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
	roswap_tag_set.cmd_size = sizeof(struct roswap_cmd);


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
		pr_err("add_disk failed: %d\n", ret);
		goto err_conn;
	}

	pr_info("added /dev/roswap0, major %d\n", roswap_major_id);
	return 0;

err_conn:
	roswap_rdma_disconnect(&roswap_conn);
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
	roswap_rdma_disconnect(&roswap_conn);
	unregister_blkdev(roswap_major_id, "roswap");
	pr_info("unregistered roswap major %d\n", roswap_major_id);
}

module_init(roswap_init);
module_exit(roswap_exit);

MODULE_LICENSE("GPL");
