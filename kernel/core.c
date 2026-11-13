// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/inet.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/version.h>

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
MODULE_PARM_DESC(chunk_size_mb, "remote chunk size in MiB");

static uint capacity_mb;
module_param(capacity_mb, uint, 0444);
MODULE_PARM_DESC(capacity_mb, "logical disk size in MiB, defaults to chunk_size_mb");

static char *protection = "single";
module_param(protection, charp, 0444);
MODULE_PARM_DESC(protection, "single or replica2");

struct rs_link;

struct rs_qp {
	struct rs_link *link;
	struct rdma_cm_id *cm_id;
	struct ib_cq *cq;
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_status;
	atomic_t inflight;
	atomic_t state;
	wait_queue_head_t stop_wq;
	struct work_struct recover_work;
	struct work_struct poll_work;
	spinlock_t poll_lock;
	unsigned long recover_deadline;
	bool stop;
	bool key_wait;
	bool link_lost;
	bool poll_direct;
	u16 cpu;
	u8 purpose;
};

struct rs_link {
	u16 node_id;
	char server[64];
	struct sockaddr_in dst;
	u32 epoch;
	u32 chunk_mb;
	u32 key_gen;
	bool stop;
	bool fatal;
	bool rest_queued;
	struct rs_qp *qps;
	struct work_struct connect_rest;
};

struct roswap_cmd;

struct rs_wr {
	struct ib_cqe cqe;
	struct roswap_cmd *cmd;
	u8 which;
};

struct roswap_cmd {
	struct rs_wr wr[RS_NR_COPIES];
	struct rs_qp *qp[RS_NR_COPIES];
	struct ib_device *dev;
	bool *done_flag;
	atomic_t copy_word;
	u64 dma;
	u64 off;
	u32 len;
	u32 gid[RS_NR_COPIES];
	u32 gen[RS_NR_COPIES];
	enum dma_data_direction dir;
	u8 sent_mask;
};

static u32 rs_capacity_mb;
static int roswap_major_id;
static struct gendisk *roswap_disk;
static struct blk_mq_tag_set roswap_tag_set;
static struct workqueue_struct *roswap_wq;
static struct ib_pd *rs_pd;
static DEFINE_MUTEX(rs_qp_mu);
static struct rs_link __rcu *rs_links[RS_MAX_NODES];
static DEFINE_MUTEX(rs_link_mu);

static void rs_qp_recover(struct work_struct *work);
static void rs_fault_poll_work(struct work_struct *work);
static void rs_connect_rest(struct work_struct *work);
static void rs_link_failed(struct rs_qp *qp);
static void rs_rdma_done(struct ib_cq *cq, struct ib_wc *wc);
static void rs_reap_orphans(struct rs_qp *qp, struct ib_device *dev);

static unsigned int rs_primary_cpu(void)
{
	unsigned int cpu;

	for_each_online_cpu(cpu)
		return cpu;
	return 0;
}

static struct rs_link *rs_link_find(u16 node_id)
{
	int i;

	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference(rs_links[i]);

		if (link && link->node_id == node_id)
			return link;
	}
	return NULL;
}

static struct rs_qp *rs_qp_for(struct rs_link *link, unsigned int cpu, bool read)
{
	if (!link || cpu >= nr_cpu_ids)
		return NULL;
	return &link->qps[cpu * 2 + (read ? 0 : 1)];
}

static void rs_zero_page(struct page *page, unsigned int off, unsigned int len)
{
	void *addr;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	addr = kmap_local_page(page);
	memset(addr + off, 0, len);
	kunmap_local(addr);
#else
	addr = kmap_atomic(page);
	memset(addr + off, 0, len);
	kunmap_atomic(addr);
#endif
}

static void rs_qp_kick(struct rs_qp *qp)
{
	if (!qp || READ_ONCE(qp->stop) || READ_ONCE(qp->link->fatal))
		return;
	if (atomic_cmpxchg(&qp->state, RS_LINK_OFF, RS_LINK_RECOVERING) != RS_LINK_OFF)
		return;
	qp->recover_deadline = jiffies + msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	queue_work(roswap_wq, &qp->recover_work);
}

static void rs_link_failed(struct rs_qp *qp)
{
	if (!qp || READ_ONCE(qp->stop) || READ_ONCE(qp->link->fatal))
		return;
	if (atomic_cmpxchg(&qp->state, RS_LINK_UP, RS_LINK_RECOVERING) != RS_LINK_UP)
		return;
	qp->recover_deadline = jiffies + msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	queue_work(roswap_wq, &qp->recover_work);
}

static void rs_release_waiting_io(void)
{
	if (!roswap_disk)
		return;
	blk_mq_start_hw_queues(roswap_disk->queue);
	blk_mq_kick_requeue_list(roswap_disk->queue);
}

static bool rs_wc_permanent(enum ib_wc_status status)
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

/*
 * done 在 post 之前就挂到 cmd 上。完成可能被别的轮询者先收走，
 * 之后 cmd 会被复用，post 之后再碰 cmd 就是写别人的请求。
 */
static void rs_fault_poll(struct rs_qp *qp, struct roswap_cmd *cmd, bool *done)
{
	unsigned long flags;
	ktime_t end = ktime_add_us(ktime_get(), ROSWAP_FAULT_POLL_US);

	spin_lock_irqsave(&qp->poll_lock, flags);
	while (!*done && ktime_before(ktime_get(), end)) {
		ib_process_cq_direct(qp->cq, 16);
		if (!*done)
			cpu_relax();
	}
	if (!*done)
		cmd->done_flag = NULL;
	spin_unlock_irqrestore(&qp->poll_lock, flags);
	if (!*done && atomic_read(&qp->state) == RS_LINK_UP)
		queue_work(roswap_wq, &qp->poll_work);
}

enum {
	RS_QP_NONE = 0,
	RS_QP_READY,
	RS_QP_RETRY,
	RS_QP_DEAD,
};

struct rs_pick {
	struct rs_qp *qp;
	u8 kind;
};

static int rs_qp_kind(struct rs_qp *qp, struct rs_link *link)
{
	if (qp && atomic_read_acquire(&qp->state) == RS_LINK_UP && qp->cm_id)
		return RS_QP_READY;
	if (!qp || !link || READ_ONCE(link->fatal) || READ_ONCE(qp->stop))
		return RS_QP_DEAD;
	if (atomic_read(&qp->state) == RS_LINK_OFF)
		rs_qp_kick(qp);
	return RS_QP_RETRY;
}

/* 输入副本表。输出每个副本的 QP 状态，*retry 表示还有链路能再试。 */
static void rs_classify(const struct rs_copies *copies, bool read,
			struct rs_pick *pick, bool *retry)
{
	u32 i;

	*retry = false;
	for (i = 0; i < RS_NR_COPIES; i++) {
		struct rs_link *link = NULL;
		struct rs_qp *qp = NULL;

		pick[i].qp = NULL;
		pick[i].kind = RS_QP_NONE;
		if (!(copies->mask & (1u << i)))
			continue;
		rcu_read_lock();
		link = rs_link_find(copies->r[i].node_id);
		/* 只用来挑 QP，被迁走也没关系；queue_rq 不保证关抢占。 */
		qp = rs_qp_for(link, raw_smp_processor_id(), read);
		rcu_read_unlock();
		pick[i].qp = qp;
		pick[i].kind = (u8)rs_qp_kind(qp, link);
		if (pick[i].kind == RS_QP_RETRY)
			*retry = true;
	}
}

static u8 rs_ready_mask(const struct rs_pick *pick, bool first_only)
{
	u8 mask = 0;
	u32 i;

	for (i = 0; i < RS_NR_COPIES; i++) {
		if (pick[i].kind != RS_QP_READY)
			continue;
		mask |= (u8)(1u << i);
		if (first_only)
			break;
	}
	return mask;
}

static void rs_inflight_put(struct rs_qp *qp)
{
	int cur;

	if (!qp)
		return;
	do {
		cur = atomic_read(&qp->inflight);
		if (cur <= 0)
			return;
	} while (atomic_cmpxchg(&qp->inflight, cur, cur - 1) != cur);
}

/* 发出去的副本都没了路可退才报错，否则交回 blk-mq 再试。 */
static void rs_fail_cmd(struct roswap_cmd *cmd, struct request *rq)
{
	u32 i;
	bool retry = false;

	for (i = 0; i < RS_NR_COPIES; i++) {
		struct rs_qp *qp;

		if (!(cmd->sent_mask & (1u << i)))
			continue;
		qp = cmd->qp[i];
		if (!qp || READ_ONCE(qp->stop) || READ_ONCE(qp->link->fatal) ||
		    atomic_read(&qp->state) == RS_LINK_OFF)
			continue;
		retry = true;
	}
	if (retry)
		blk_mq_requeue_request(rq, true);
	else
		blk_mq_end_request(rq, BLK_STS_IOERR);
}

/* 输入已经到齐的完成字。解除这一页的 DMA，再按成功掩码结束请求。 */
static void rs_finish_cmd(struct roswap_cmd *cmd)
{
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	u8 ok = rs_copy_ok((u32)atomic_read(&cmd->copy_word));

	ib_dma_unmap_page(cmd->dev, cmd->dma, cmd->len, cmd->dir);
	if (cmd->dir == DMA_FROM_DEVICE) {
		rs_map_finish(cmd->off);
		if (ok)
			blk_mq_end_request(rq, BLK_STS_OK);
		else
			rs_fail_cmd(cmd, rq);
		return;
	}
	rs_map_finish_write(cmd->off, ok);
	if (ok)
		blk_mq_end_request(rq, BLK_STS_OK);
	else
		rs_fail_cmd(cmd, rq);
}

/* 登记一份副本。重复登记直接忽略。返回这次是不是新到达。 */
static bool rs_settle(struct roswap_cmd *cmd, u32 which, bool success, bool posted)
{
	u32 old, new;

	if (cmd->done_flag)
		*cmd->done_flag = true;
	do {
		old = (u32)atomic_read(&cmd->copy_word);
		new = rs_copy_note(old, which, success);
		if (new == old)
			return false;
	} while ((u32)atomic_cmpxchg(&cmd->copy_word, (int)old, (int)new) != old);

	if (posted)
		rs_inflight_put(cmd->qp[which]);
	if (rs_copy_settled(new, cmd->sent_mask))
		rs_finish_cmd(cmd);
	return true;
}

static void rs_rdma_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rs_wr *wr = container_of(wc->wr_cqe, struct rs_wr, cqe);
	struct roswap_cmd *cmd = wr->cmd;
	struct rs_qp *qp = cmd->qp[wr->which];
	bool ok = wc->status == IB_WC_SUCCESS;

	(void)cq;
	if (!ok) {
		pr_err_ratelimited("rdma %s failed: %s\n",
				   cmd->dir == DMA_TO_DEVICE ? "write" : "read",
				   ib_wc_status_msg(wc->status));
		/*
		 * rkey 已经换过就直接重试。还是这一把钥匙，就停在原地等
		 * CHUNK_UPDATE，不要拿旧钥匙重连空转。
		 */
		if (qp && rs_wc_permanent(wc->status) &&
		    rs_map_chunk_gen(cmd->gid[wr->which]) == cmd->gen[wr->which])
			WRITE_ONCE(qp->key_wait, true);
		rs_link_failed(qp);
	}
	rs_settle(cmd, wr->which, ok, true);
}

static blk_status_t rs_post_copies(struct request *rq, struct roswap_cmd *cmd,
				   const struct rs_copies *copies,
				   const struct rs_pick *pick, u8 ready,
				   const struct bio_vec *bv, u64 off, bool read)
{
	struct ib_device *dev = NULL;
	bool done = false;
	u32 i;
	int nposted = 0;

	for (i = 0; i < RS_NR_COPIES; i++) {
		if (!(ready & (1u << i)))
			continue;
		dev = pick[i].qp->cm_id->device;
		break;
	}
	if (!dev) {
		rs_map_finish(off);
		return BLK_STS_IOERR;
	}

	cmd->dev = dev;
	cmd->off = off;
	cmd->len = bv->bv_len;
	cmd->dir = read ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	cmd->sent_mask = ready;
	cmd->done_flag = NULL;
	atomic_set(&cmd->copy_word, 0);
	for (i = 0; i < RS_NR_COPIES; i++) {
		cmd->qp[i] = NULL;
		cmd->wr[i].cmd = cmd;
		cmd->wr[i].which = (u8)i;
		cmd->wr[i].cqe.done = NULL;
		if (!(ready & (1u << i)))
			continue;
		cmd->qp[i] = pick[i].qp;
		cmd->gid[i] = copies->r[i].gid;
		cmd->gen[i] = copies->r[i].gen;
	}
	cmd->dma = ib_dma_map_page(dev, bv->bv_page, bv->bv_offset, bv->bv_len,
				   cmd->dir);
	if (ib_dma_mapping_error(dev, cmd->dma)) {
		rs_map_finish(off);
		return BLK_STS_RESOURCE;
	}

	blk_mq_start_request(rq);
	for (i = 0; i < RS_NR_COPIES; i++) {
		struct ib_rdma_wr wr = {};
		struct ib_sge sge;
		struct rs_qp *qp;

		if (!(ready & (1u << i)))
			continue;
		qp = cmd->qp[i];
		sge.addr = cmd->dma;
		sge.length = cmd->len;
		sge.lkey = rs_pd->local_dma_lkey;
		cmd->wr[i].cqe.done = rs_rdma_done;
		if (read && qp->poll_direct)
			cmd->done_flag = &done;
		wr.wr.opcode = read ? IB_WR_RDMA_READ : IB_WR_RDMA_WRITE;
		wr.wr.wr_cqe = &cmd->wr[i].cqe;
		wr.wr.sg_list = &sge;
		wr.wr.num_sge = 1;
		wr.wr.send_flags = IB_SEND_SIGNALED;
		wr.remote_addr = copies->r[i].addr;
		wr.rkey = copies->r[i].rkey;
		atomic_inc(&qp->inflight);
		if (ib_post_send(qp->cm_id->qp, &wr.wr, NULL)) {
			u32 j;

			rs_inflight_put(qp);
			rs_link_failed(qp);
			if (!nposted) {
				/*
				 * 请求还在 tag 里，回收会按 qp 指针再结算一次。
				 * 清掉发送掩码，这次失败只由这里收尾。
				 */
				cmd->done_flag = NULL;
				cmd->sent_mask = 0;
				for (j = 0; j < RS_NR_COPIES; j++)
					cmd->qp[j] = NULL;
				ib_dma_unmap_page(dev, cmd->dma, cmd->len, cmd->dir);
				rs_map_finish(off);
				blk_mq_requeue_request(rq, true);
				return BLK_STS_OK;
			}
			rs_settle(cmd, i, false, false);
			return BLK_STS_OK;
		}
		nposted++;
		if (read && qp->poll_direct)
			rs_fault_poll(qp, cmd, &done);
	}
	return BLK_STS_OK;
}

static blk_status_t rs_queue_rq(struct blk_mq_hw_ctx *hctx,
				const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct roswap_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct rs_copies copies;
	struct rs_pick pick[RS_NR_COPIES];
	struct bio_vec bv;
	bool read = req_op(rq) == REQ_OP_READ;
	bool retry = false;
	u64 off;
	u8 ready;
	int ret;

	(void)hctx;

	if (req_op(rq) == REQ_OP_DISCARD) {
		off = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
		ret = rs_map_discard(off, blk_rq_bytes(rq));
		if (ret == -EBUSY)
			return BLK_STS_RESOURCE;
		if (ret)
			return BLK_STS_IOERR;
		blk_mq_start_request(rq);
		blk_mq_end_request(rq, BLK_STS_OK);
		return BLK_STS_OK;
	}
	if (!read && req_op(rq) != REQ_OP_WRITE)
		return BLK_STS_NOTSUPP;

	bv = req_bvec(rq);
	if (bv.bv_len != PAGE_SIZE)
		return BLK_STS_IOERR;
	off = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	if (read)
		ret = rs_map_lookup_read(off, bv.bv_len, &copies);
	else
		ret = rs_map_prepare_write(off, bv.bv_len, &copies);
	if (ret == RS_MAP_ZERO) {
		rs_zero_page(bv.bv_page, bv.bv_offset, bv.bv_len);
		blk_mq_start_request(rq);
		blk_mq_end_request(rq, BLK_STS_OK);
		return BLK_STS_OK;
	}
	if (ret == -EBUSY || ret == -ENOMEM)
		return BLK_STS_RESOURCE;
	if (ret == -ENOSPC)
		return BLK_STS_NOSPC;
	if (ret)
		return BLK_STS_IOERR;

	rs_classify(&copies, read, pick, &retry);
	ready = rs_ready_mask(pick, read);
	if (!ready) {
		rs_map_finish(off);
		/* stop 之后没人会再拉起链路，返回 RESOURCE 会让 del_gendisk 永远等。 */
		return retry ? BLK_STS_RESOURCE : BLK_STS_IOERR;
	}
	return rs_post_copies(rq, cmd, &copies, pick, ready, &bv, off, read);
}

struct rs_reap_ctx {
	struct rs_qp *qp;
	int n;
};

static bool rs_reap_one(ROSWAP_BUSY_ITER_ARGS)
{
	struct rs_reap_ctx *ctx = data;
	struct roswap_cmd *cmd = blk_mq_rq_to_pdu(rq);
	u32 i;

	for (i = 0; i < RS_NR_COPIES; i++) {
		if ((cmd->sent_mask & (1u << i)) && cmd->qp[i] == ctx->qp)
			break;
	}
	if (i == RS_NR_COPIES)
		return true;
	if (rs_settle(cmd, i, false, true))
		ctx->n++;
	return true;
}

/*
 * QP 和 CQ 都已销毁，硬件不会再 DMA，也不会再有完成回调。
 * 还挂在这个 QP 上的请求只能由这里结束，否则提交者永远停在 D 状态，
 * 对应的槽位也一直 BUSY。调用方必须保证没有 queue_rq 在跑。
 */
static void rs_reap_orphans(struct rs_qp *qp, struct ib_device *dev)
{
	struct rs_reap_ctx ctx = { .qp = qp };

	(void)dev;
	if (!roswap_disk || atomic_read(&qp->inflight) <= 0)
		return;
	blk_mq_tagset_busy_iter(&roswap_tag_set, rs_reap_one, &ctx);
	pr_err("cpu %u failed %d requests left on a dead QP\n", qp->cpu, ctx.n);
	atomic_set(&qp->inflight, 0);
}

static const struct blk_mq_ops roswap_mq_ops = {
	.queue_rq = rs_queue_rq,
};

static const struct block_device_operations roswap_fops = {
	.owner = THIS_MODULE,
};

static int rs_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
	struct rs_qp *qp = id->context;

	if (ev->event == RDMA_CM_EVENT_DISCONNECTED ||
	    ev->event == RDMA_CM_EVENT_DEVICE_REMOVAL) {
		pr_warn("cpu %u %s\n", qp->cpu, rdma_event_msg(ev->event));
		qp->link_lost = true;
		rs_link_failed(qp);
	}
	qp->cm_event = ev->event;
	qp->cm_status = ev->status;
	complete(&qp->cm_done);
	return 0;
}

static int rs_cm_wait(struct rs_qp *qp, enum rdma_cm_event_type expected,
		      unsigned int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	for (;;) {
		unsigned int slice;
		long left;

		if (READ_ONCE(qp->stop) || READ_ONCE(qp->link->fatal))
			return -ECONNABORTED;
		if (atomic_read(&qp->state) == RS_LINK_RECOVERING &&
		    qp->recover_deadline &&
		    time_after(jiffies, qp->recover_deadline))
			return -ETIMEDOUT;
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		slice = jiffies_to_msecs(end - jiffies);
		if (!slice)
			return -ETIMEDOUT;
		if (slice > 200)
			slice = 200;
		left = wait_for_completion_killable_timeout(&qp->cm_done,
						msecs_to_jiffies(slice));
		if (left < 0)
			return left;
		if (left)
			break;
	}
	if (qp->cm_event != expected) {
		pr_err("rdma_cm %s, status %d\n",
		       rdma_event_msg(qp->cm_event), qp->cm_status);
		return -ECONNREFUSED;
	}
	return 0;
}

static int rs_parse_server(const char *addr, struct sockaddr_in *sin)
{
	const char *end;
	u16 port;

	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	if (!addr || !in4_pton(addr, -1, (u8 *)&sin->sin_addr.s_addr, ':', &end) ||
	    *end != ':' || kstrtou16(end + 1, 10, &port))
		return -EINVAL;
	sin->sin_port = htons(port);
	return 0;
}

static int rs_ensure_pd(struct ib_device *dev)
{
	if (rs_pd)
		return rs_pd->device == dev ? 0 : -ENODEV;
	rs_pd = ib_alloc_pd(dev, 0);
	if (IS_ERR(rs_pd)) {
		int ret = PTR_ERR(rs_pd);

		rs_pd = NULL;
		return ret;
	}
	return 0;
}

static int rs_qp_connect(struct rs_qp *qp)
{
	struct rs_link *link = qp->link;
	struct rs_conn_pdata pdata = {
		.magic = cpu_to_be32(RS_CONN_MAGIC),
		.version = cpu_to_be16(RS_CONN_VERSION),
		.purpose = qp->purpose,
		.cpu = cpu_to_be16(qp->cpu),
		.node_id = cpu_to_be16(link->node_id),
		.node_epoch = cpu_to_be32(link->epoch),
		.rsvd2 = { cpu_to_be32(link->chunk_mb), 0 },
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
	struct rdma_cm_id *id;
	enum ib_poll_context poll;
	int ret;

	qp->link_lost = false;
	reinit_completion(&qp->cm_done);
	id = rdma_create_id(&init_net, rs_cm_handler, qp, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(id))
		return PTR_ERR(id);

	ret = rdma_resolve_addr(id, NULL, (struct sockaddr *)&link->dst,
				ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = rs_cm_wait(qp, RDMA_CM_EVENT_ADDR_RESOLVED,
				 2 * ROSWAP_CM_TIMEOUT_MS);
	if (ret)
		goto err_id;

	reinit_completion(&qp->cm_done);
	ret = rdma_resolve_route(id, ROSWAP_CM_TIMEOUT_MS);
	if (!ret)
		ret = rs_cm_wait(qp, RDMA_CM_EVENT_ROUTE_RESOLVED,
				 2 * ROSWAP_CM_TIMEOUT_MS);
	if (ret)
		goto err_id;

	ret = rs_ensure_pd(id->device);
	if (ret)
		goto err_id;

	poll = qp->poll_direct ? IB_POLL_DIRECT : IB_POLL_SOFTIRQ;
	qp->cq = ib_alloc_cq(id->device, qp, ROSWAP_QUEUE_DEPTH, 0, poll);
	if (IS_ERR(qp->cq)) {
		ret = PTR_ERR(qp->cq);
		qp->cq = NULL;
		goto err_id;
	}
	qp_attr.send_cq = qp->cq;
	qp_attr.recv_cq = qp->cq;
	ret = rdma_create_qp(id, rs_pd, &qp_attr);
	if (ret)
		goto err_cq;

	param.initiator_depth = min_t(int, id->device->attrs.max_qp_init_rd_atom, 16);
	rdma_set_ack_timeout(id, ROSWAP_ACK_TIMEOUT);
	reinit_completion(&qp->cm_done);
	ret = rdma_connect(id, &param);
	if (!ret)
		ret = rs_cm_wait(qp, RDMA_CM_EVENT_ESTABLISHED, ROSWAP_CONNECT_TIMEOUT_MS);
	if (ret)
		goto err_qp;

	qp->cm_id = id;
	if (qp->cpu == rs_primary_cpu())
		pr_info("cpu %u %s connected to %s via %s\n", qp->cpu,
			qp->purpose == RS_QP_FAULT ? "fault" : "bulk",
			link->server, dev_name(&id->device->dev));
	return 0;

err_qp:
	rdma_destroy_qp(id);
err_cq:
	ib_free_cq(qp->cq);
	qp->cq = NULL;
err_id:
	rdma_destroy_id(id);
	return ret;
}

static void rs_qp_teardown(struct rs_qp *qp)
{
	struct ib_device *dev;
	unsigned long end;

	if (!qp->cm_id)
		return;
	rdma_disconnect(qp->cm_id);
	end = jiffies + 5 * HZ;
	while (atomic_read(&qp->inflight) > 0 && time_before(jiffies, end)) {
		if (qp->poll_direct) {
			unsigned long flags;

			spin_lock_irqsave(&qp->poll_lock, flags);
			ib_process_cq_direct(qp->cq, 16);
			spin_unlock_irqrestore(&qp->poll_lock, flags);
		}
		usleep_range(100, 200);
	}
	if (atomic_read(&qp->inflight) > 0)
		pr_err("cpu %u drained with %d requests still in flight\n",
		       qp->cpu, atomic_read(&qp->inflight));
	dev = qp->cm_id->device;
	rdma_destroy_qp(qp->cm_id);
	ib_free_cq(qp->cq);
	rs_reap_orphans(qp, dev);
	rdma_destroy_id(qp->cm_id);
	qp->cm_id = NULL;
	qp->cq = NULL;
}

static void rs_unquiesce(bool *quiesced)
{
	if (*quiesced && roswap_disk)
		blk_mq_unquiesce_queue(roswap_disk->queue);
	*quiesced = false;
}

static void rs_mark_up(struct rs_qp *qp)
{
	/* release 让 queue_rq 看到 UP 时，也能看到上面刚写好的 cm_id。 */
	atomic_set_release(&qp->state, RS_LINK_UP);
	if (READ_ONCE(qp->link_lost) && !READ_ONCE(qp->stop))
		rs_link_failed(qp);
}

static int rs_bring_up_locked(struct rs_qp *qp)
{
	int ret;

	if (atomic_read(&qp->state) == RS_LINK_UP && qp->cm_id)
		return 0;
	if (!qp->cm_id) {
		ret = rs_qp_connect(qp);
		if (ret)
			return ret;
	}
	rs_mark_up(qp);
	return 0;
}

static int rs_connect_cpu_locked(struct rs_link *link, unsigned int cpu)
{
	int ret;

	ret = rs_bring_up_locked(&link->qps[cpu * 2]);
	if (ret)
		return ret;
	return rs_bring_up_locked(&link->qps[cpu * 2 + 1]);
}

static bool rs_give_up(struct rs_qp *qp)
{
	return READ_ONCE(qp->stop) ||
	       (qp->recover_deadline && time_after(jiffies, qp->recover_deadline));
}

static void rs_qp_recover(struct work_struct *work)
{
	struct rs_qp *qp = container_of(work, struct rs_qp, recover_work);
	unsigned int noio = memalloc_noio_save();
	bool quiesced = false;
	int ret;

	if (READ_ONCE(qp->stop))
		goto out;
	if (READ_ONCE(qp->key_wait)) {
		u32 seen = READ_ONCE(qp->link->key_gen);

		if (!qp->recover_deadline)
			qp->recover_deadline = jiffies +
					       msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
		while (!rs_give_up(qp) && READ_ONCE(qp->link->key_gen) == seen)
			wait_event_timeout(qp->stop_wq,
					   READ_ONCE(qp->stop) ||
					   READ_ONCE(qp->link->key_gen) != seen,
					   HZ);
		if (rs_give_up(qp))
			goto fail;
		WRITE_ONCE(qp->key_wait, false);
	}

	mutex_lock(&rs_qp_mu);
	if (!READ_ONCE(qp->key_wait) && atomic_read(&qp->state) == RS_LINK_UP &&
	    qp->cm_id) {
		mutex_unlock(&rs_qp_mu);
		goto out;
	}
	atomic_set_release(&qp->state, RS_LINK_RECOVERING);
	if (!qp->recover_deadline)
		qp->recover_deadline = jiffies + msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	if (roswap_disk) {
		blk_mq_quiesce_queue(roswap_disk->queue);
		quiesced = true;
	}
	cancel_work_sync(&qp->poll_work);
	rs_qp_teardown(qp);
	/*
	 * replica2 还有另一份副本。拆完旧 QP 就放开队列，重连期间别的节点继续服务。
	 * single 只有这一条链路，保持冻结到重连结束。
	 */
	if (rs_map_protection() == RS_PROT_REPLICA2)
		rs_unquiesce(&quiesced);
	while (!rs_give_up(qp)) {
		ret = rs_qp_connect(qp);
		if (rs_give_up(qp))
			break;
		if (!ret) {
			rs_mark_up(qp);
			rs_unquiesce(&quiesced);
			rs_release_waiting_io();
			mutex_unlock(&rs_qp_mu);
			memalloc_noio_restore(noio);
			return;
		}
		pr_warn_ratelimited("cpu %u reconnect to %s failed: %d\n",
				    qp->cpu, qp->link->server, ret);
		if (wait_event_timeout(qp->stop_wq, READ_ONCE(qp->stop), HZ))
			break;
	}
	rs_qp_teardown(qp);
	mutex_unlock(&rs_qp_mu);
fail:
	if (!READ_ONCE(qp->stop)) {
		pr_err("cpu %u reconnect to %s gave up\n", qp->cpu, qp->link->server);
		WRITE_ONCE(qp->link->fatal, true);
	}
	atomic_set(&qp->state, RS_LINK_OFF);
	rs_unquiesce(&quiesced);
	if (!READ_ONCE(qp->stop))
		rs_release_waiting_io();
out:
	memalloc_noio_restore(noio);
}

static void rs_fault_poll_work(struct work_struct *work)
{
	struct rs_qp *qp = container_of(work, struct rs_qp, poll_work);

	while (!READ_ONCE(qp->stop) && atomic_read(&qp->inflight) > 0 &&
	       atomic_read(&qp->state) == RS_LINK_UP) {
		unsigned long flags;
		int n;

		spin_lock_irqsave(&qp->poll_lock, flags);
		n = ib_process_cq_direct(qp->cq, 16);
		spin_unlock_irqrestore(&qp->poll_lock, flags);
		if (n <= 0)
			usleep_range(20, 50);
	}
}

static void rs_connect_rest(struct work_struct *work)
{
	struct rs_link *link = container_of(work, struct rs_link, connect_rest);
	unsigned int primary = rs_primary_cpu();
	unsigned int cpu;
	unsigned int noio = memalloc_noio_save();

	for_each_online_cpu(cpu) {
		int ret;

		if (cpu == primary || READ_ONCE(link->stop))
			continue;
		mutex_lock(&rs_qp_mu);
		ret = rs_connect_cpu_locked(link, cpu);
		mutex_unlock(&rs_qp_mu);
		if (ret)
			pr_warn("cpu %u connect to %s failed: %d\n",
				cpu, link->server, ret);
	}
	memalloc_noio_restore(noio);
}

static void rs_qp_prep(struct rs_qp *qp, struct rs_link *link, unsigned int cpu,
		       u8 purpose)
{
	qp->link = link;
	qp->cpu = cpu;
	qp->purpose = purpose;
	qp->poll_direct = purpose == RS_QP_FAULT;
	init_completion(&qp->cm_done);
	init_waitqueue_head(&qp->stop_wq);
	INIT_WORK(&qp->recover_work, rs_qp_recover);
	INIT_WORK(&qp->poll_work, rs_fault_poll_work);
	spin_lock_init(&qp->poll_lock);
}

static int rs_ensure_connected(struct rs_link *link)
{
	unsigned int primary = rs_primary_cpu();
	unsigned int noio;
	int ret;

	if (READ_ONCE(link->stop))
		return -ENODEV;
	if (!link->chunk_mb)
		return -EINVAL;
	noio = memalloc_noio_save();
	mutex_lock(&rs_qp_mu);
	ret = rs_connect_cpu_locked(link, primary);
	mutex_unlock(&rs_qp_mu);
	memalloc_noio_restore(noio);
	if (ret)
		return ret;
	if (!link->rest_queued) {
		link->rest_queued = true;
		queue_work(roswap_wq, &link->connect_rest);
	}
	return 0;
}

int roswap_node_add(u16 node_id, const char *addr)
{
	struct rs_link *link;
	struct sockaddr_in dst;
	unsigned int cpu;
	int i, slot = -1, ret;

	ret = rs_parse_server(addr, &dst);
	if (ret) {
		pr_err("invalid server '%s', expect ip:port\n", addr ? addr : "");
		return ret;
	}
	mutex_lock(&rs_link_mu);
	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *old = rcu_dereference_protected(rs_links[i],
						lockdep_is_held(&rs_link_mu));

		if (old && old->node_id == node_id) {
			ret = strcmp(old->server, addr) ? -EEXIST : 0;
			goto out;
		}
		if (!old && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		ret = -ENOSPC;
		goto out;
	}
	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link) {
		ret = -ENOMEM;
		goto out;
	}
	link->qps = kcalloc(nr_cpu_ids * 2, sizeof(*link->qps), GFP_KERNEL);
	if (!link->qps) {
		kfree(link);
		ret = -ENOMEM;
		goto out;
	}
	link->node_id = node_id;
	link->dst = dst;
	strscpy(link->server, addr, sizeof(link->server));
	INIT_WORK(&link->connect_rest, rs_connect_rest);
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		rs_qp_prep(&link->qps[cpu * 2], link, cpu, RS_QP_FAULT);
		rs_qp_prep(&link->qps[cpu * 2 + 1], link, cpu, RS_QP_BULK);
	}
	ret = rs_map_node_add(node_id);
	if (ret) {
		kfree(link->qps);
		kfree(link);
		goto out;
	}
	rcu_assign_pointer(rs_links[slot], link);
	ret = 0;
out:
	mutex_unlock(&rs_link_mu);
	return ret;
}

int roswap_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 base, u32 remote_key,
		     u32 chunk_mb)
{
	struct rs_link *link;
	int ret;

	ret = rs_map_chunk_add(node_id, gid, epoch, base, remote_key, chunk_mb);
	if (ret)
		return ret;
	rcu_read_lock();
	link = rs_link_find(node_id);
	rcu_read_unlock();
	if (!link)
		return -ENOENT;
	if (!link->chunk_mb) {
		link->chunk_mb = chunk_mb;
		link->epoch = epoch;
	} else if (link->chunk_mb != chunk_mb) {
		return -EINVAL;
	}
	return rs_ensure_connected(link);
}

static void rs_key_updated(u16 node_id)
{
	struct rs_link *link;
	unsigned int cpu;

	rcu_read_lock();
	link = rs_link_find(node_id);
	if (!link) {
		rcu_read_unlock();
		return;
	}
	WRITE_ONCE(link->fatal, false);
	WRITE_ONCE(link->key_gen, link->key_gen + 1);
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		struct rs_qp *fault = &link->qps[cpu * 2];
		struct rs_qp *bulk = &link->qps[cpu * 2 + 1];

		wake_up(&fault->stop_wq);
		wake_up(&bulk->stop_wq);
		if (atomic_read(&fault->state) == RS_LINK_OFF)
			rs_qp_kick(fault);
		if (atomic_read(&bulk->state) == RS_LINK_OFF)
			rs_qp_kick(bulk);
	}
	rcu_read_unlock();
}

int roswap_chunk_update(u32 gid, u32 epoch, u64 base, u32 remote_key)
{
	u16 node_id;
	int changed;

	changed = rs_map_chunk_update(gid, epoch, base, remote_key);
	if (changed < 0)
		return changed;
	if (!changed)
		return 0;
	if (rs_map_chunk_node(gid, &node_id))
		return 0;
	rs_key_updated(node_id);
	pr_info("chunk %u rkey updated to 0x%x\n", gid, remote_key);
	return 0;
}

int roswap_links_up(void)
{
	unsigned int cpu;
	int i, n = 0;

	rcu_read_lock();
	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference(rs_links[i]);

		if (!link)
			continue;
		for_each_online_cpu(cpu) {
			if (atomic_read(&link->qps[cpu * 2].state) == RS_LINK_UP)
				n++;
			if (atomic_read(&link->qps[cpu * 2 + 1].state) == RS_LINK_UP)
				n++;
		}
	}
	rcu_read_unlock();
	return n;
}

static int rs_drop_set(const char *val, const struct kernel_param *kp)
{
	unsigned int cpu;
	int i;

	rcu_read_lock();
	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference(rs_links[i]);

		if (!link)
			continue;
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			link->qps[cpu * 2].link_lost = true;
			link->qps[cpu * 2 + 1].link_lost = true;
			rs_link_failed(&link->qps[cpu * 2]);
			rs_link_failed(&link->qps[cpu * 2 + 1]);
		}
	}
	rcu_read_unlock();
	return 0;
}

static int rs_drop_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "0\n");
}

static const struct kernel_param_ops rs_drop_ops = {
	.set = rs_drop_set,
	.get = rs_drop_get,
};

module_param_cb(drop_connection, &rs_drop_ops, NULL, 0200);
MODULE_PARM_DESC(drop_connection, "write anything to drop the RC connections");

static int rs_add_disk(void)
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
	set_capacity(roswap_disk, (sector_t)rs_capacity_mb << (20 - SECTOR_SHIFT));
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

static void rs_stop_links(void)
{
	unsigned int cpu;
	int i;

	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference_protected(rs_links[i], true);

		if (!link)
			continue;
		WRITE_ONCE(link->stop, true);
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			struct rs_qp *fault = &link->qps[cpu * 2];
			struct rs_qp *bulk = &link->qps[cpu * 2 + 1];

			WRITE_ONCE(fault->stop, true);
			WRITE_ONCE(bulk->stop, true);
			atomic_set(&fault->state, RS_LINK_OFF);
			atomic_set(&bulk->state, RS_LINK_OFF);
			wake_up(&fault->stop_wq);
			wake_up(&bulk->stop_wq);
		}
	}
}

static void rs_cancel_links(void)
{
	unsigned int cpu;
	int i;

	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference_protected(rs_links[i], true);

		if (!link)
			continue;
		cancel_work_sync(&link->connect_rest);
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			cancel_work_sync(&link->qps[cpu * 2].recover_work);
			cancel_work_sync(&link->qps[cpu * 2].poll_work);
			cancel_work_sync(&link->qps[cpu * 2 + 1].recover_work);
			cancel_work_sync(&link->qps[cpu * 2 + 1].poll_work);
		}
	}
}

static void rs_teardown_links(void)
{
	unsigned int cpu;
	int i;

	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference_protected(rs_links[i], true);

		if (!link)
			continue;
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			rs_qp_teardown(&link->qps[cpu * 2]);
			rs_qp_teardown(&link->qps[cpu * 2 + 1]);
		}
	}
}

static void rs_free_links(void)
{
	int i;

	for (i = 0; i < RS_MAX_NODES; i++) {
		struct rs_link *link = rcu_dereference_protected(rs_links[i], true);

		if (!link)
			continue;
		RCU_INIT_POINTER(rs_links[i], NULL);
		kfree(link->qps);
		kfree(link);
	}
}

static int rs_protection_mode(void)
{
	if (protection && !strcmp(protection, "single"))
		return RS_PROT_SINGLE;
	if (protection && !strcmp(protection, "replica2"))
		return RS_PROT_REPLICA2;
	return -EINVAL;
}

static int __init roswap_init(void)
{
	int ret, mode;

	rs_capacity_mb = capacity_mb ? capacity_mb : chunk_size_mb;
	if (!rs_capacity_mb) {
		pr_err("capacity_mb must be non-zero\n");
		return -EINVAL;
	}
	if (server && !chunk_size_mb) {
		pr_err("chunk_size_mb must be non-zero\n");
		return -EINVAL;
	}
	mode = rs_protection_mode();
	if (mode < 0) {
		pr_err("protection must be single or replica2\n");
		return -EINVAL;
	}
	ret = rs_map_init(rs_capacity_mb);
	if (ret)
		return ret;
	ret = rs_map_set_protection((u32)mode);
	if (ret)
		goto err_map;
	roswap_wq = alloc_workqueue("roswap", WQ_MEM_RECLAIM, 0);
	if (!roswap_wq) {
		ret = -ENOMEM;
		goto err_map;
	}
	roswap_major_id = register_blkdev(0, "roswap");
	if (roswap_major_id < 0) {
		ret = roswap_major_id;
		pr_err("register_blkdev failed: %d\n", ret);
		goto err_wq;
	}
	if (server) {
		ret = roswap_node_add(0, server);
		if (ret)
			goto err_conn;
		ret = roswap_chunk_add(0, 0, 0, raddr, rkey, chunk_size_mb);
		if (ret) {
			pr_err("connect %s failed: %d\n", server, ret);
			goto err_conn;
		}
	}
	ret = rs_add_disk();
	if (ret) {
		pr_err("add_disk failed: %d\n", ret);
		goto err_conn;
	}
	ret = rs_genl_register();
	if (ret) {
		pr_err("genl register failed: %d\n", ret);
		goto err_disk;
	}
	pr_info("added /dev/roswap0, major %d, %u MiB\n",
		roswap_major_id, rs_capacity_mb);
	return 0;

err_disk:
	del_gendisk(roswap_disk);
	roswap_cleanup_disk(roswap_disk);
	roswap_disk = NULL;
	blk_mq_free_tag_set(&roswap_tag_set);
err_conn:
	rs_stop_links();
	rs_cancel_links();
	rs_teardown_links();
	if (rs_pd)
		ib_dealloc_pd(rs_pd);
	rs_pd = NULL;
	unregister_blkdev(roswap_major_id, "roswap");
err_wq:
	destroy_workqueue(roswap_wq);
	roswap_wq = NULL;
err_map:
	rs_free_links();
	rs_map_exit();
	return ret;
}

static void __exit roswap_exit(void)
{
	rs_genl_unregister();
	rs_stop_links();
	/*
	 * 先拆 QP 再删盘。stop 之后 poll_work 不再轮询 fault QP，
	 * 在途请求只能靠 teardown 收尾，否则 del_gendisk 会一直等它们。
	 */
	rs_cancel_links();
	if (roswap_disk)
		blk_mq_quiesce_queue(roswap_disk->queue);
	rs_teardown_links();
	if (roswap_disk) {
		blk_mq_unquiesce_queue(roswap_disk->queue);
		del_gendisk(roswap_disk);
		roswap_cleanup_disk(roswap_disk);
		roswap_disk = NULL;
		blk_mq_free_tag_set(&roswap_tag_set);
	}
	if (rs_pd)
		ib_dealloc_pd(rs_pd);
	destroy_workqueue(roswap_wq);
	synchronize_rcu();
	rs_free_links();
	unregister_blkdev(roswap_major_id, "roswap");
	rs_map_exit();
	pr_info("unregistered roswap major %d\n", roswap_major_id);
}

module_init(roswap_init);
module_exit(roswap_exit);
MODULE_LICENSE("GPL");
