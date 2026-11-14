// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/hrtimer.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/sbitmap.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "compat.h"
#include "roswap.h"

/*
 * 锁顺序：rs_link_mu（节点增删）> qp->poll_lock。
 * queue_rq、完成回调、对冲回调不拿可睡眠的锁。QP 指针靠 RCU 读，
 * 拆除方先把状态改成 DRAINING，synchronize_rcu 之后才销毁。
 */

static char *server;
module_param(server, charp, 0444);
MODULE_PARM_DESC(server, "provider rdma_cm address, ip:port");

static ulong raddr;
module_param(raddr, ulong, 0444);
MODULE_PARM_DESC(raddr, "remote chunk base address");

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

static uint qp_sets = 16;
module_param(qp_sets, uint, 0444);
MODULE_PARM_DESC(qp_sets, "QP sets per node before the budget clamp");

static uint qp_budget = 4096;
module_param(qp_budget, uint, 0444);
MODULE_PARM_DESC(qp_budget, "upper bound on RC QPs across all nodes");

static uint queue_depth = ROSWAP_QUEUE_DEPTH;
module_param(queue_depth, uint, 0444);
MODULE_PARM_DESC(queue_depth, "blk-mq tags per hardware queue");

static uint qp_send_depth = 1024;
module_param(qp_send_depth, uint, 0444);
MODULE_PARM_DESC(qp_send_depth, "send WR depth of each RC QP");

static char *local_addrs;
module_param(local_addrs, charp, 0444);
MODULE_PARM_DESC(local_addrs, "comma-separated local RDMA IPv4 addresses");

struct rs_dev {
	struct ib_device *ibdev;
	struct ib_pd *pd;
	struct sockaddr_in src;
	int numa;
	int send_depth;
};

struct rs_bounce_pool {
	struct sbitmap_queue sb;
	struct rs_bounce *slot;
	int nr;
	bool ready;
};

struct rs_set {
	u16 cpu0;
	int nr_cpus;
	int dev_idx;
	struct rs_bounce_pool pool;
};

struct rs_link;

enum {
	RS_QP_OFF = 0,
	RS_QP_UP = 1,
	RS_QP_DRAINING = 2,
};

struct rs_qp {
	struct rs_link *link;
	struct rs_dev *dev;
	struct rdma_cm_id *cm_id;
	struct ib_cq *cq;
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_status;
	atomic_t send_used;
	atomic_t state;
	spinlock_t poll_lock;
	struct work_struct connect_work;
	struct work_struct poll_work;
	atomic_t poll_queued;
	int send_depth;
	bool cq_pool;
	bool poll_direct;
	u16 cpu;
	u8 set;
	u8 purpose;
};

struct rs_link {
	u16 node_id;
	char server[64];
	struct sockaddr_in dst;
	u32 epoch;
	u32 chunk_mb;
	atomic_t key_gen;
	atomic_t state;
	atomic_t pending;
	atomic_t recover_busy;
	unsigned long recover_deadline;
	bool stop;
	bool key_wait;
	bool connect_fail;
	bool force_down;
	int nr_sets;
	struct rs_qp *qps;
	struct delayed_work recover_dwork;
	wait_queue_head_t wq;
};

struct rs_wr {
	struct ib_cqe cqe;
	struct roswap_cmd *cmd;
	u8 which;
};

struct rs_bounce {
	struct ib_cqe cqe;
	struct page *page;
	struct rs_qp *qp;
	struct ib_device *dev;
	struct rs_bounce_pool *pool;
	dma_addr_t dma;
	struct roswap_cmd *cmd;
	unsigned int bit;
	unsigned int cpu;
	u8 which;
	atomic_t live;
};

struct roswap_cmd {
	struct rs_wr wr[RS_NR_COPIES];
	struct rs_qp *qp[RS_NR_COPIES];
	struct rs_bounce *bounce[RS_NR_COPIES];
	struct rs_bounce *publish_bounce;
	struct page *user_page;
	struct ib_device *dev;
	bool *done_flag;
	atomic_t copy_word;
	u64 dma;
	u64 off;
	u32 len;
	u32 gid[RS_NR_COPIES];
	u32 gen[RS_NR_COPIES];
	enum dma_data_direction dir;
	unsigned int user_off;
	u8 sent_mask;
	u8 hedge_which;
	u8 hedge_armed;
	u8 use_bounce;
	u8 dma_mapped;
	u32 hedge_rkey;
	u64 hedge_addr;
	struct hrtimer hedge_timer;
	struct work_struct hedge_work;
	struct work_struct done_work;
};

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

static u32 rs_capacity_mb;
static int roswap_major_id;
static struct gendisk *roswap_disk;
static struct blk_mq_tag_set roswap_tag_set;
static struct workqueue_struct *roswap_io;
static struct workqueue_struct *roswap_cm;
static DEFINE_XARRAY(rs_links);
static DEFINE_MUTEX(rs_link_mu);
static struct rs_dev *rs_devs;
static int rs_nr_devs;
static struct rs_set *rs_sets;
static int rs_nr_sets;
static u16 *rs_cpu_set;
static u16 *rs_hctx_cpu;
static int rs_qps_reserved;

static void rs_link_recover(struct work_struct *work);
static void rs_qp_connect_work(struct work_struct *work);
static void rs_fault_poll_work(struct work_struct *work);
static void rs_link_failed(struct rs_link *link);
static void rs_rdma_done(struct ib_cq *cq, struct ib_wc *wc);
static void rs_bounce_free(struct rs_bounce *bounce, bool put_credit);
static int rs_post_send(struct rs_qp *qp, struct ib_send_wr *wr);

static bool rs_give_up(struct rs_link *link)
{
	return READ_ONCE(link->stop) ||
	       (link->recover_deadline &&
		time_after(jiffies, link->recover_deadline));
}

static struct rs_dev *rs_dev_of_set(int set)
{
	int idx;

	if (!rs_sets || set < 0 || set >= rs_nr_sets)
		return NULL;
	idx = rs_sets[set].dev_idx;
	if (idx < 0 || idx >= rs_nr_devs)
		return NULL;
	return &rs_devs[idx];
}

static int rs_hctx_set(struct blk_mq_hw_ctx *hctx)
{
	unsigned int cpu;

	if (!rs_hctx_cpu || hctx->queue_num >= roswap_tag_set.nr_hw_queues)
		return 0;
	cpu = rs_hctx_cpu[hctx->queue_num];
	if (!rs_cpu_set || cpu >= nr_cpu_ids)
		return 0;
	return rs_cpu_set[cpu];
}

static void rs_kick_hctx(struct blk_mq_hw_ctx *hctx)
{
	if (hctx)
		blk_mq_delay_run_hw_queue(hctx, 1);
}

static void rs_run_queues(void)
{
	if (roswap_disk)
		blk_mq_run_hw_queues(roswap_disk->queue, true);
}

static void rs_link_kick(struct rs_link *link)
{
	int old;

	if (!link || READ_ONCE(link->stop))
		return;
	old = atomic_read(&link->state);
	if (old == RS_LINK_UP || old == RS_LINK_RECOVERING)
		return;
	if (atomic_cmpxchg(&link->state, old, RS_LINK_RECOVERING) != old)
		return;
	/*
	 * 上次放弃后 deadline 还停在过去。不刷新的话，新的一轮会立刻再放弃。
	 */
	if (!link->recover_deadline || time_after(jiffies, link->recover_deadline))
		link->recover_deadline = jiffies +
					 msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	mod_delayed_work(roswap_cm, &link->recover_dwork, 0);
}

static void rs_link_failed(struct rs_link *link)
{
	if (!link || READ_ONCE(link->stop))
		return;
	if (atomic_read(&link->state) == RS_LINK_FATAL)
		return;
	if (atomic_cmpxchg(&link->state, RS_LINK_UP, RS_LINK_RECOVERING) != RS_LINK_UP) {
		if (atomic_read(&link->state) == RS_LINK_OFF)
			rs_link_kick(link);
		return;
	}
	link->recover_deadline = jiffies + msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	mod_delayed_work(roswap_cm, &link->recover_dwork, 0);
}

static void rs_credit_put(struct rs_qp *qp)
{
	int cur;

	if (!qp)
		return;
	do {
		cur = atomic_read(&qp->send_used);
		if (cur <= 0)
			return;
	} while (atomic_cmpxchg(&qp->send_used, cur, cur - 1) != cur);
}

static int rs_credit_get(struct rs_qp *qp)
{
	int depth = qp->send_depth;
	int cur;

	if (depth < 1)
		depth = 1;
	do {
		cur = atomic_read(&qp->send_used);
		if (cur >= depth)
			return -EAGAIN;
	} while (atomic_cmpxchg(&qp->send_used, cur, cur + 1) != cur);
	return 0;
}

static void rs_queue_poll(struct rs_qp *qp)
{
	if (!qp || !qp->poll_direct)
		return;
	if (atomic_cmpxchg(&qp->poll_queued, 0, 1) == 0)
		queue_work_on(qp->cpu, roswap_io, &qp->poll_work);
}

static void rs_fault_poll(struct rs_qp *qp, struct roswap_cmd *cmd, bool *done)
{
	unsigned long flags;
	ktime_t end = ktime_add_us(ktime_get(), ROSWAP_FAULT_POLL_US);

	if (!spin_trylock_irqsave(&qp->poll_lock, flags)) {
		cmd->done_flag = NULL;
		rs_queue_poll(qp);
		return;
	}
	while (!*done && ktime_before(ktime_get(), end)) {
		ib_process_cq_direct(qp->cq, 16);
		if (!*done)
			cpu_relax();
	}
	if (!*done)
		cmd->done_flag = NULL;
	spin_unlock_irqrestore(&qp->poll_lock, flags);
	if (!*done && atomic_read(&qp->state) == RS_QP_UP)
		rs_queue_poll(qp);
}

static struct rs_qp *rs_qp_choose(struct rs_link *link, int hctx_set, bool read)
{
	struct rs_dev *want = rs_dev_of_set(hctx_set);
	/* 配对 rs_link_alloc_qps 的 store-release，读到的 QP 数组已经初始化完。 */
	struct rs_qp *qps = smp_load_acquire(&link->qps);
	int n, i;

	if (!qps || link->nr_sets <= 0)
		return NULL;
	if (atomic_read(&link->state) != RS_LINK_UP)
		return NULL;
	n = link->nr_sets;
	for (i = 0; i < n; i++) {
		int slot = (hctx_set + i) % n;
		struct rs_qp *qp = &qps[slot * 2 + (read ? 0 : 1)];

		if (atomic_read_acquire(&qp->state) != RS_QP_UP || !qp->cm_id)
			continue;
		if (want && qp->dev != want)
			continue;
		return qp;
	}
	return NULL;
}

static int rs_qp_kind(struct rs_link *link, int hctx_set, bool read,
		      struct rs_qp **out)
{
	struct rs_qp *qp;

	*out = NULL;
	if (!link || READ_ONCE(link->stop) ||
	    atomic_read(&link->state) == RS_LINK_FATAL)
		return RS_QP_DEAD;
	qp = rs_qp_choose(link, hctx_set, read);
	if (qp) {
		*out = qp;
		return RS_QP_READY;
	}
	if (rs_give_up(link) && atomic_read(&link->state) != RS_LINK_UP)
		return RS_QP_DEAD;
	if (atomic_read(&link->state) == RS_LINK_OFF)
		rs_link_kick(link);
	return RS_QP_RETRY;
}

static void rs_classify(const struct rs_copies *copies, bool read, int hctx_set,
			struct rs_pick *pick, bool *retry)
{
	u32 i;

	*retry = false;
	for (i = 0; i < RS_NR_COPIES; i++) {
		struct rs_link *link = NULL;

		pick[i].qp = NULL;
		pick[i].kind = RS_QP_NONE;
		if (!(copies->mask & (1u << i)))
			continue;
		rcu_read_lock();
		link = xa_load(&rs_links, copies->r[i].node_id);
		if (link)
			pick[i].kind = (u8)rs_qp_kind(link, hctx_set, read,
						      &pick[i].qp);
		rcu_read_unlock();
		if (!link)
			pick[i].kind = RS_QP_DEAD;
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

static void rs_fail_cmd(struct roswap_cmd *cmd, struct request *rq)
{
	u32 i;
	bool retry = false;

	for (i = 0; i < RS_NR_COPIES; i++) {
		struct rs_qp *qp;
		int state;

		if (!(READ_ONCE(cmd->sent_mask) & (1u << i)))
			continue;
		qp = cmd->qp[i];
		if (!qp || READ_ONCE(qp->link->stop))
			continue;
		state = atomic_read(&qp->link->state);
		if (state == RS_LINK_FATAL || state == RS_LINK_OFF)
			continue;
		if (rs_give_up(qp->link))
			continue;
		retry = true;
	}
	if (retry)
		blk_mq_requeue_request(rq, true);
	else
		blk_mq_end_request(rq, BLK_STS_IOERR);
}

static void rs_hedge_cancel(struct roswap_cmd *cmd)
{
	hrtimer_cancel(&cmd->hedge_timer);
	if (current_work() != &cmd->hedge_work)
		cancel_work_sync(&cmd->hedge_work);
	WRITE_ONCE(cmd->hedge_armed, 0);
}

static void rs_done_work(struct work_struct *work);
static void rs_finish_now(struct roswap_cmd *cmd);

static void rs_finish_cmd(struct roswap_cmd *cmd)
{
	if (READ_ONCE(cmd->hedge_armed)) {
		queue_work(roswap_io, &cmd->done_work);
		return;
	}
	rs_finish_now(cmd);
}

static void rs_finish_now(struct roswap_cmd *cmd)
{
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	u8 ok = rs_copy_ok((u32)atomic_read(&cmd->copy_word));

	if (cmd->dma_mapped)
		ib_dma_unmap_page(cmd->dev, cmd->dma, cmd->len, cmd->dir);
	cmd->dma_mapped = 0;
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

static void rs_copy_bounce(struct roswap_cmd *cmd, struct rs_bounce *bounce)
{
	unsigned int off = 0;

	if (!cmd->user_page || !bounce || !bounce->page || !cmd->len)
		return;
	while (off < cmd->len) {
		unsigned int n = cmd->len - off;
		char tmp[256];
		void *addr;

		if (n > sizeof(tmp))
			n = sizeof(tmp);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		addr = kmap_local_page(bounce->page);
		memcpy(tmp, (u8 *)addr + off, n);
		kunmap_local(addr);
		addr = kmap_local_page(cmd->user_page);
		memcpy((u8 *)addr + cmd->user_off + off, tmp, n);
		kunmap_local(addr);
#else
		addr = kmap_atomic(bounce->page);
		memcpy(tmp, (u8 *)addr + off, n);
		kunmap_atomic(addr);
		addr = kmap_atomic(cmd->user_page);
		memcpy((u8 *)addr + cmd->user_off + off, tmp, n);
		kunmap_atomic(addr);
#endif
		off += n;
	}
}

static void rs_done_work(struct work_struct *work)
{
	struct roswap_cmd *cmd = container_of(work, struct roswap_cmd, done_work);
	struct rs_bounce *bounce = cmd->publish_bounce;

	rs_hedge_cancel(cmd);
	if (bounce)
		rs_copy_bounce(cmd, bounce);
	rs_finish_now(cmd);
	rs_bounce_free(bounce, false);
}

static bool rs_settle(struct roswap_cmd *cmd, u32 which, bool success, bool posted)
{
	u32 old, new;

	if (cmd->done_flag)
		*cmd->done_flag = true;
	if (cmd->dir == DMA_FROM_DEVICE) {
		int act = RS_ACT_WAIT;
		bool took = false;

		do {
			old = (u32)atomic_read(&cmd->copy_word);
			act = rs_read_arrive(old, which, success, &new);
			if (new == old)
				break;
			if ((u32)atomic_cmpxchg(&cmd->copy_word, (int)old,
						(int)new) == old) {
				took = true;
				break;
			}
		} while (1);
		if (posted && (took || (old & RS_COPY_FINISH)))
			rs_credit_put(cmd->qp[which]);
		if (cmd->use_bounce &&
		    (act == RS_ACT_PUBLISH || act == RS_ACT_FAIL)) {
			if (act == RS_ACT_PUBLISH)
				WRITE_ONCE(cmd->publish_bounce, cmd->bounce[which]);
			else
				WRITE_ONCE(cmd->publish_bounce, NULL);
			queue_work(roswap_io, &cmd->done_work);
			return true;
		}
		if (act == RS_ACT_PUBLISH || act == RS_ACT_FAIL)
			rs_finish_now(cmd);
		return true;
	}
	do {
		old = (u32)atomic_read(&cmd->copy_word);
		new = rs_copy_note(old, which, success);
		if (new == old)
			return false;
	} while ((u32)atomic_cmpxchg(&cmd->copy_word, (int)old, (int)new) != old);
	if (posted)
		rs_credit_put(cmd->qp[which]);
	do {
		old = (u32)atomic_read(&cmd->copy_word);
		if (!rs_copy_claim_finish(old, &new))
			return true;
	} while ((u32)atomic_cmpxchg(&cmd->copy_word, (int)old, (int)new) != old);
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
	if (!ok && wc->status != IB_WC_WR_FLUSH_ERR) {
		pr_err_ratelimited("rdma %s failed: %s\n",
				   cmd->dir == DMA_TO_DEVICE ? "write" : "read",
				   ib_wc_status_msg(wc->status));
		if (qp && rs_wc_permanent(wc->status) &&
		    rs_map_chunk_gen(cmd->gid[wr->which]) == cmd->gen[wr->which])
			WRITE_ONCE(qp->link->key_wait, true);
		rs_link_failed(qp ? qp->link : NULL);
	}
	rs_settle(cmd, wr->which, ok, true);
}

static u8 rs_pick_hedge(const struct rs_pick *pick, u8 post, bool read)
{
	u8 hedge;
	u32 i, j;

	if (!read || rs_map_protection() != RS_PROT_REPLICA2)
		return 0;
	hedge = rs_ready_mask(pick, false) & (u8)~post;
	if (!hedge || !post)
		return 0;
	for (i = 0; i < RS_NR_COPIES; i++) {
		if (!(hedge & (1u << i)))
			continue;
		for (j = 0; j < RS_NR_COPIES; j++) {
			if (!(post & (1u << j)))
				continue;
			if (pick[i].qp == pick[j].qp)
				return 0;
		}
		return (u8)(1u << i);
	}
	return 0;
}

static struct rs_bounce *rs_bounce_get(int set)
{
	struct rs_bounce_pool *pool;
	struct rs_bounce *bounce;
	unsigned int cpu;
	int bit;

	if (set < 0 || set >= rs_nr_sets)
		return NULL;
	pool = &rs_sets[set].pool;
	if (!pool->ready)
		return NULL;
	bit = sbitmap_queue_get(&pool->sb, &cpu);
	if (bit < 0)
		return NULL;
	bounce = &pool->slot[bit];
	bounce->bit = bit;
	bounce->cpu = cpu;
	bounce->pool = pool;
	bounce->dma = 0;
	bounce->dev = NULL;
	bounce->qp = NULL;
	bounce->cmd = NULL;
	atomic_set(&bounce->live, 1);
	return bounce;
}

static void rs_bounce_free(struct rs_bounce *bounce, bool put_credit)
{
	struct rs_bounce_pool *pool;
	unsigned int bit, cpu;

	if (!bounce || atomic_cmpxchg(&bounce->live, 1, 0) != 1)
		return;
	if (put_credit && bounce->qp)
		rs_credit_put(bounce->qp);
	if (bounce->dev && bounce->dma)
		ib_dma_unmap_page(bounce->dev, bounce->dma, PAGE_SIZE,
				  DMA_FROM_DEVICE);
	bounce->dma = 0;
	bounce->dev = NULL;
	bounce->qp = NULL;
	bounce->cmd = NULL;
	pool = bounce->pool;
	bit = bounce->bit;
	cpu = bounce->cpu;
	if (pool && pool->ready)
		sbitmap_queue_clear(&pool->sb, bit, cpu);
}

static void rs_bounce_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rs_bounce *bounce = container_of(wc->wr_cqe, struct rs_bounce, cqe);
	struct roswap_cmd *cmd = READ_ONCE(bounce->cmd);
	bool ok = wc->status == IB_WC_SUCCESS;

	(void)cq;
	if (!cmd) {
		rs_bounce_free(bounce, true);
		return;
	}
	if (!ok && wc->status != IB_WC_WR_FLUSH_ERR && bounce->qp)
		rs_link_failed(bounce->qp->link);
	rs_settle(cmd, bounce->which, ok, true);
	if (READ_ONCE(cmd->publish_bounce) == bounce)
		return;
	rs_bounce_free(bounce, false);
}

static int rs_post_send(struct rs_qp *qp, struct ib_send_wr *wr)
{
	int ret;

	if (!qp)
		return -ENODEV;
	rcu_read_lock();
	if (atomic_read_acquire(&qp->state) != RS_QP_UP || !qp->cm_id ||
	    atomic_read(&qp->link->state) != RS_LINK_UP ||
	    READ_ONCE(qp->link->stop)) {
		rcu_read_unlock();
		return -ENODEV;
	}
	if (rs_credit_get(qp)) {
		rcu_read_unlock();
		return -EAGAIN;
	}
	ret = ib_post_send(qp->cm_id->qp, wr, NULL);
	if (ret)
		rs_credit_put(qp);
	rcu_read_unlock();
	if (ret)
		rs_link_failed(qp->link);
	return ret;
}

static int rs_post_bounce(struct roswap_cmd *cmd, u32 which, u64 remote,
			  u32 rkey, struct rs_qp *qp, int bounce_set)
{
	struct rs_bounce *bounce;
	struct ib_rdma_wr wr = {};
	struct ib_sge sge;
	struct ib_device *dev;
	int ret;

	if (!qp || !qp->dev)
		return -ENODEV;
	bounce = rs_bounce_get(bounce_set);
	if (!bounce)
		return -ENOMEM;
	dev = qp->dev->ibdev;
	bounce->which = (u8)which;
	bounce->qp = qp;
	bounce->cmd = cmd;
	cmd->bounce[which] = bounce;
	cmd->qp[which] = qp;
	bounce->dma = ib_dma_map_page(dev, bounce->page, 0, PAGE_SIZE,
				      DMA_FROM_DEVICE);
	if (ib_dma_mapping_error(dev, bounce->dma)) {
		bounce->dma = 0;
		cmd->bounce[which] = NULL;
		rs_bounce_free(bounce, false);
		return -EIO;
	}
	bounce->dev = dev;
	if (atomic_read(&cmd->copy_word) & RS_COPY_FINISH) {
		cmd->bounce[which] = NULL;
		rs_bounce_free(bounce, false);
		return -EALREADY;
	}
	if (cmd->hedge_which < RS_NR_COPIES && which != cmd->hedge_which) {
		WRITE_ONCE(cmd->hedge_armed, 1);
		hrtimer_start(&cmd->hedge_timer,
			      ns_to_ktime((u64)ROSWAP_HEDGE_US * NSEC_PER_USEC),
			      HRTIMER_MODE_REL);
	}
	sge.addr = bounce->dma;
	sge.length = PAGE_SIZE;
	sge.lkey = qp->dev->pd->local_dma_lkey;
	bounce->cqe.done = rs_bounce_done;
	wr.wr.opcode = IB_WR_RDMA_READ;
	wr.wr.wr_cqe = &bounce->cqe;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.send_flags = IB_SEND_SIGNALED;
	wr.remote_addr = remote;
	wr.rkey = rkey;
	ret = rs_post_send(qp, &wr.wr);
	if (ret) {
		if (which != cmd->hedge_which)
			hrtimer_cancel(&cmd->hedge_timer);
		cmd->bounce[which] = NULL;
		rs_bounce_free(bounce, false);
		return ret;
	}
	if (which != cmd->hedge_which && qp->poll_direct && cmd->done_flag) {
		rcu_read_lock();
		rs_fault_poll(qp, cmd, cmd->done_flag);
		rcu_read_unlock();
	} else if (qp->poll_direct) {
		rs_queue_poll(qp);
	}
	return 0;
}

static void rs_hedge_work(struct work_struct *work)
{
	struct roswap_cmd *cmd = container_of(work, struct roswap_cmd, hedge_work);
	u32 which = cmd->hedge_which;
	u32 old, new;
	int set = 0;

	if (which >= RS_NR_COPIES || !cmd->qp[which])
		return;
	do {
		old = (u32)atomic_read(&cmd->copy_word);
		if (!rs_hedge_should_post(old, which, true))
			return;
		new = rs_copy_mark_sent(old, which);
	} while ((u32)atomic_cmpxchg(&cmd->copy_word, (int)old, (int)new) != old);
	WRITE_ONCE(cmd->sent_mask,
		   (u8)(READ_ONCE(cmd->sent_mask) | (u8)(1u << which)));
	if (cmd->qp[which]->set < rs_nr_sets)
		set = cmd->qp[which]->set;
	if (rs_post_bounce(cmd, which, cmd->hedge_addr, cmd->hedge_rkey,
			   cmd->qp[which], set))
		rs_settle(cmd, which, false, false);
}

static enum hrtimer_restart rs_hedge_timer(struct hrtimer *timer)
{
	struct roswap_cmd *cmd = container_of(timer, struct roswap_cmd, hedge_timer);

	queue_work(roswap_io, &cmd->hedge_work);
	return HRTIMER_NORESTART;
}

static void rs_hedge_timer_init(struct hrtimer *timer)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	hrtimer_setup(timer, rs_hedge_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = rs_hedge_timer;
#endif
}

static blk_status_t rs_retry_status(struct blk_mq_hw_ctx *hctx, u64 off, bool retry)
{
	rs_map_finish(off);
	if (!retry)
		return BLK_STS_IOERR;
	rs_kick_hctx(hctx);
	return BLK_STS_DEV_RESOURCE;
}

static blk_status_t rs_post_copies(struct request *rq, struct roswap_cmd *cmd,
				   const struct rs_copies *copies,
				   const struct rs_pick *pick, u8 ready,
				   const struct bio_vec *bv, u64 off, bool read,
				   int bounce_set)
{
	struct ib_device *dev = NULL;
	struct rs_dev *rsdev = NULL;
	bool done = false;
	u32 i;
	int nposted = 0;
	u8 hedge;

	for (i = 0; i < RS_NR_COPIES; i++) {
		if (!(ready & (1u << i)) || !pick[i].qp || !pick[i].qp->dev)
			continue;
		rsdev = pick[i].qp->dev;
		dev = rsdev->ibdev;
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
	cmd->hedge_which = RS_NR_COPIES;
	cmd->use_bounce = 0;
	cmd->dma_mapped = 0;
	cmd->publish_bounce = NULL;
	cmd->bounce[0] = NULL;
	cmd->bounce[1] = NULL;
	WRITE_ONCE(cmd->hedge_armed, 0);
	{
		u32 sent_word = 0;

		hedge = rs_pick_hedge(pick, ready, read);
		for (i = 0; i < RS_NR_COPIES; i++) {
			cmd->qp[i] = NULL;
			cmd->wr[i].cmd = cmd;
			cmd->wr[i].which = (u8)i;
			cmd->wr[i].cqe.done = NULL;
			if (ready & (1u << i))
				sent_word = rs_copy_mark_sent(sent_word, i);
			if (!(ready & (1u << i)) && !(hedge & (1u << i)))
				continue;
			cmd->qp[i] = pick[i].qp;
			cmd->gid[i] = copies->r[i].gid;
			cmd->gen[i] = copies->r[i].gen;
			if (!(hedge & (1u << i)))
				continue;
			cmd->hedge_which = (u8)i;
			cmd->hedge_addr = copies->r[i].addr;
			cmd->hedge_rkey = copies->r[i].rkey;
		}
		atomic_set(&cmd->copy_word, (int)sent_word);
	}
	if (read && cmd->hedge_which < RS_NR_COPIES) {
		u32 primary = 0;
		struct rs_bounce *probe = rs_bounce_get(bounce_set);

		/* 池空就直接读到请求页，不因为对冲缓冲不够而失败。 */
		if (!probe)
			goto direct;
		rs_bounce_free(probe, false);
		while (!(ready & (1u << primary)))
			primary++;
		cmd->user_page = bv->bv_page;
		cmd->user_off = bv->bv_offset;
		cmd->use_bounce = 1;
		cmd->done_flag = &done;
		blk_mq_start_request(rq);
		if (rs_post_bounce(cmd, primary, copies->r[primary].addr,
				   copies->r[primary].rkey, pick[primary].qp,
				   bounce_set)) {
			hrtimer_cancel(&cmd->hedge_timer);
			cancel_work(&cmd->hedge_work);
			rs_map_finish(off);
			blk_mq_requeue_request(rq, true);
			return BLK_STS_OK;
		}
		return BLK_STS_OK;
	}
direct:
	cmd->hedge_which = RS_NR_COPIES;
	cmd->dma = ib_dma_map_page(dev, bv->bv_page, bv->bv_offset, bv->bv_len,
				   cmd->dir);
	if (ib_dma_mapping_error(dev, cmd->dma)) {
		rs_map_finish(off);
		return BLK_STS_RESOURCE;
	}
	cmd->dma_mapped = 1;
	blk_mq_start_request(rq);
	for (i = 0; i < RS_NR_COPIES; i++) {
		struct ib_rdma_wr wr = {};
		struct ib_sge sge;
		struct rs_qp *qp;
		int ret;

		if (!(ready & (1u << i)))
			continue;
		qp = cmd->qp[i];
		ret = -ENODEV;
		if (qp && qp->dev && qp->dev->ibdev == dev) {
			sge.addr = cmd->dma;
			sge.length = cmd->len;
			sge.lkey = qp->dev->pd->local_dma_lkey;
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
			ret = rs_post_send(qp, &wr.wr);
		}
		if (ret) {
			if (!nposted) {
				cmd->done_flag = NULL;
				cmd->sent_mask = 0;
				cmd->qp[0] = NULL;
				cmd->qp[1] = NULL;
				ib_dma_unmap_page(dev, cmd->dma, cmd->len, cmd->dir);
				cmd->dma_mapped = 0;
				rs_map_finish(off);
				blk_mq_requeue_request(rq, true);
				return BLK_STS_OK;
			}
			rs_settle(cmd, i, false, false);
			return BLK_STS_OK;
		}
		nposted++;
		if (read && qp->poll_direct) {
			rcu_read_lock();
			rs_fault_poll(qp, cmd, &done);
			rcu_read_unlock();
		}
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
	int set = rs_hctx_set(hctx);
	u64 off;
	u8 ready;
	int ret;

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
	rs_classify(&copies, read, set, pick, &retry);
	ready = rs_ready_mask(pick, read);
	if (!ready)
		return rs_retry_status(hctx, off, retry);
	return rs_post_copies(rq, cmd, &copies, pick, ready, &bv, off, read, set);
}

/* 7.2 起最后一个参数从 unsigned int 改成 int。 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
static int rs_init_request(struct blk_mq_tag_set *set, struct request *rq,
			   unsigned int hctx_idx, int numa_node)
#else
static int rs_init_request(struct blk_mq_tag_set *set, struct request *rq,
			   unsigned int hctx_idx, unsigned int numa_node)
#endif
{
	struct roswap_cmd *cmd = blk_mq_rq_to_pdu(rq);

	(void)set;
	(void)hctx_idx;
	(void)numa_node;
	memset(cmd, 0, sizeof(*cmd));
	rs_hedge_timer_init(&cmd->hedge_timer);
	INIT_WORK(&cmd->hedge_work, rs_hedge_work);
	INIT_WORK(&cmd->done_work, rs_done_work);
	cmd->hedge_which = RS_NR_COPIES;
	return 0;
}

static void rs_exit_request(struct blk_mq_tag_set *set, struct request *rq,
			    unsigned int hctx_idx)
{
	struct roswap_cmd *cmd = blk_mq_rq_to_pdu(rq);

	(void)set;
	(void)hctx_idx;
	hrtimer_cancel(&cmd->hedge_timer);
	cancel_work_sync(&cmd->hedge_work);
	cancel_work_sync(&cmd->done_work);
}

/*
 * 6.1 起 map_queues 改成 void。5.10 到 6.0 还要返回 0。
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void rs_map_queues(struct blk_mq_tag_set *set)
#else
static int rs_map_queues(struct blk_mq_tag_set *set)
#endif
{
	struct blk_mq_queue_map *qmap = &set->map[HCTX_TYPE_DEFAULT];
	unsigned int cpu, q = 0;

	for_each_possible_cpu(cpu)
		qmap->mq_map[cpu] = 0;
	for_each_online_cpu(cpu) {
		if (q >= qmap->nr_queues)
			break;
		qmap->mq_map[cpu] = q;
		if (rs_hctx_cpu)
			rs_hctx_cpu[q] = (u16)cpu;
		q++;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static const struct blk_mq_ops roswap_mq_ops = {
	.queue_rq = rs_queue_rq,
	.init_request = rs_init_request,
	.exit_request = rs_exit_request,
	.map_queues = rs_map_queues,
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
		if (atomic_read(&qp->state) == RS_QP_UP)
			rs_link_failed(qp->link);
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

		if (READ_ONCE(qp->link->stop))
			return -ECONNABORTED;
		if (rs_give_up(qp->link) &&
		    atomic_read(&qp->link->state) != RS_LINK_UP)
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
		pr_err_ratelimited("rdma_cm %s, status %d\n",
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

static int rs_comp_vector(struct ib_device *dev, unsigned int cpu)
{
	int n = dev->num_comp_vectors;
	int v;

	if (n <= 1)
		return 0;
	/*
	 * 7.1 删掉了 ib_get_vector_affinity。拿不到亲和性时按 CPU 取模，
	 * 完成中断仍然摊到不同的向量上。
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 1, 0)
	for (v = 0; v < n; v++) {
		const struct cpumask *mask = ib_get_vector_affinity(dev, v);

		if (mask && cpumask_test_cpu(cpu, mask))
			return v;
	}
#else
	(void)v;
#endif
	return (int)(cpu % n);
}

static int rs_clamp_depth(struct ib_device *dev)
{
	int depth = qp_send_depth ? qp_send_depth : 1024;

	if (dev->attrs.max_qp_wr && depth > dev->attrs.max_qp_wr)
		depth = dev->attrs.max_qp_wr;
	if (depth < 1)
		depth = 1;
	return depth;
}

static DEFINE_MUTEX(rs_dev_mu);

static int rs_adopt_device(struct ib_device *dev)
{
	struct rs_dev *ndev;
	struct ib_pd *pd;
	int i, ret = 0;

	mutex_lock(&rs_dev_mu);
	if (rs_nr_devs) {
		if (rs_devs[0].ibdev != dev)
			ret = -ENODEV;
		goto out;
	}
	pd = ib_alloc_pd(dev, 0);
	if (IS_ERR(pd)) {
		ret = PTR_ERR(pd);
		goto out;
	}
	ndev = kzalloc(sizeof(*ndev), GFP_KERNEL);
	if (!ndev) {
		ib_dealloc_pd(pd);
		ret = -ENOMEM;
		goto out;
	}
	ndev->ibdev = dev;
	ndev->pd = pd;
	ndev->numa = dev_to_node(&dev->dev);
	ndev->send_depth = rs_clamp_depth(dev);
	rs_devs = ndev;
	rs_nr_devs = 1;
	for (i = 0; i < rs_nr_sets; i++)
		rs_sets[i].dev_idx = 0;
out:
	mutex_unlock(&rs_dev_mu);
	return ret;
}

static struct ib_cq *rs_alloc_bulk_cq(struct ib_device *dev, void *ctx,
				      int depth, int vec, bool *pooled)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	struct ib_cq *cq = ib_cq_pool_get(dev, depth, vec, IB_POLL_SOFTIRQ);

	if (!IS_ERR_OR_NULL(cq)) {
		*pooled = true;
		return cq;
	}
#else
	(void)ctx;
#endif
	*pooled = false;
	return ib_alloc_cq(dev, ctx, depth, vec, IB_POLL_SOFTIRQ);
}

static void rs_free_cq(struct rs_qp *qp)
{
	if (!qp->cq)
		return;
	if (qp->cq_pool)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		ib_cq_pool_put(qp->cq, qp->send_depth);
#else
		ib_free_cq(qp->cq);
#endif
	else
		ib_free_cq(qp->cq);
	qp->cq = NULL;
	qp->cq_pool = false;
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
			.max_recv_wr = 1,
			.max_send_sge = 1,
			.max_recv_sge = 1,
		},
		.sq_sig_type = IB_SIGNAL_REQ_WR,
		.qp_type = IB_QPT_RC,
	};
	struct rdma_cm_id *id;
	struct sockaddr *src = NULL;
	int ret, vec;

	reinit_completion(&qp->cm_done);
	id = rdma_create_id(&init_net, rs_cm_handler, qp, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(id))
		return PTR_ERR(id);
	if (qp->dev)
		src = (struct sockaddr *)&qp->dev->src;
	ret = rdma_resolve_addr(id, src, (struct sockaddr *)&link->dst,
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
	if (qp->dev && qp->dev->ibdev != id->device) {
		ret = -ENODEV;
		goto err_id;
	}
	if (!qp->dev) {
		ret = rs_adopt_device(id->device);
		if (ret)
			goto err_id;
		qp->dev = rs_dev_of_set(qp->set);
		if (!qp->dev || qp->dev->ibdev != id->device) {
			ret = -ENODEV;
			goto err_id;
		}
	}
	qp->send_depth = qp->dev->send_depth;
	vec = rs_comp_vector(id->device, qp->cpu);
	if (qp->poll_direct)
		qp->cq = ib_alloc_cq(id->device, qp, qp->send_depth, vec,
				     IB_POLL_DIRECT);
	else
		qp->cq = rs_alloc_bulk_cq(id->device, qp, qp->send_depth, vec,
					  &qp->cq_pool);
	if (IS_ERR(qp->cq)) {
		ret = PTR_ERR(qp->cq);
		qp->cq = NULL;
		goto err_id;
	}
	qp_attr.cap.max_send_wr = qp->send_depth;
	qp_attr.send_cq = qp->cq;
	qp_attr.recv_cq = qp->cq;
	ret = rdma_create_qp(id, qp->dev->pd, &qp_attr);
	if (ret)
		goto err_cq;
	param.initiator_depth = min_t(int, id->device->attrs.max_qp_init_rd_atom, 16);
	rdma_set_ack_timeout(id, ROSWAP_ACK_TIMEOUT);
	reinit_completion(&qp->cm_done);
	ret = rdma_connect(id, &param);
	if (!ret)
		ret = rs_cm_wait(qp, RDMA_CM_EVENT_ESTABLISHED,
				 ROSWAP_CONNECT_TIMEOUT_MS);
	if (ret)
		goto err_qp;
	qp->cm_id = id;
	return 0;

err_qp:
	rdma_destroy_qp(id);
err_cq:
	rs_free_cq(qp);
err_id:
	rdma_destroy_id(id);
	return ret;
}

static void rs_qp_teardown(struct rs_qp *qp)
{
	if (!qp->cm_id)
		return;
	rdma_disconnect(qp->cm_id);
	if (qp->cm_id->qp)
		ib_drain_qp(qp->cm_id->qp);
	rdma_destroy_qp(qp->cm_id);
	rs_free_cq(qp);
	rdma_destroy_id(qp->cm_id);
	qp->cm_id = NULL;
	atomic_set(&qp->send_used, 0);
	atomic_set(&qp->state, RS_QP_OFF);
}

static void rs_link_drain(struct rs_link *link)
{
	int i, n;

	if (!link->qps)
		return;
	n = link->nr_sets * 2;
	for (i = 0; i < n; i++)
		atomic_set(&link->qps[i].state, RS_QP_DRAINING);
	synchronize_rcu();
	for (i = 0; i < n; i++)
		cancel_work_sync(&link->qps[i].poll_work);
	for (i = 0; i < n; i++)
		rs_qp_teardown(&link->qps[i]);
}

static int rs_link_alloc_k(void)
{
	int remain = (int)qp_budget - rs_qps_reserved;
	int k = rs_nr_sets;
	int min_k = rs_nr_devs > 1 ? rs_nr_devs : 1;

	if (k < 1)
		k = 1;
	if (remain < 2 * min_k)
		return -ENOSPC;
	if (k > remain / 2)
		k = remain / 2;
	if (k < min_k)
		return -ENOSPC;
	return k;
}

static void rs_qp_prep(struct rs_qp *qp, struct rs_link *link, int set, u8 purpose)
{
	qp->link = link;
	qp->set = (u8)set;
	qp->purpose = purpose;
	qp->poll_direct = purpose == RS_QP_FAULT;
	qp->cpu = rs_sets[set].cpu0;
	qp->dev = rs_dev_of_set(set);
	if (qp->dev)
		qp->send_depth = qp->dev->send_depth;
	init_completion(&qp->cm_done);
	INIT_WORK(&qp->connect_work, rs_qp_connect_work);
	INIT_WORK(&qp->poll_work, rs_fault_poll_work);
	spin_lock_init(&qp->poll_lock);
	atomic_set(&qp->state, RS_QP_OFF);
	atomic_set(&qp->send_used, 0);
	atomic_set(&qp->poll_queued, 0);
}

static int rs_link_alloc_qps(struct rs_link *link)
{
	struct rs_qp *qps;
	int k, s;

	if (link->qps)
		return 0;
	k = rs_link_alloc_k();
	if (k < 0)
		return k;
	qps = kcalloc(k * 2, sizeof(*qps), GFP_KERNEL);
	if (!qps)
		return -ENOMEM;
	for (s = 0; s < k; s++) {
		rs_qp_prep(&qps[s * 2], link, s, RS_QP_FAULT);
		rs_qp_prep(&qps[s * 2 + 1], link, s, RS_QP_BULK);
	}
	link->nr_sets = k;
	/* 数组里的 QP 都 prep 完再发布，queue_rq 才不会看到半初始化的指针。 */
	smp_store_release(&link->qps, qps);
	rs_qps_reserved += k * 2;
	return 0;
}

static void rs_link_fail(struct rs_link *link)
{
	if (!READ_ONCE(link->stop))
		pr_err("reconnect to %s gave up\n", link->server);
	atomic_set(&link->state, READ_ONCE(link->stop) ? RS_LINK_OFF : RS_LINK_FATAL);
	rs_link_drain(link);
	wake_up(&link->wq);
	if (!READ_ONCE(link->stop))
		rs_run_queues();
}

static void rs_connect_done(struct rs_link *link, bool ok)
{
	if (!ok)
		WRITE_ONCE(link->connect_fail, true);
	if (!atomic_dec_and_test(&link->pending))
		return;
	if (READ_ONCE(link->stop)) {
		atomic_set(&link->state, RS_LINK_OFF);
		rs_link_drain(link);
		atomic_set(&link->recover_busy, 0);
		wake_up(&link->wq);
		return;
	}
	if (READ_ONCE(link->force_down)) {
		WRITE_ONCE(link->force_down, false);
		atomic_set(&link->state, RS_LINK_RECOVERING);
		link->recover_deadline = jiffies +
					 msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
		atomic_set(&link->recover_busy, 0);
		mod_delayed_work(roswap_cm, &link->recover_dwork, 0);
		wake_up(&link->wq);
		return;
	}
	if (READ_ONCE(link->connect_fail) || rs_give_up(link)) {
		rs_link_fail(link);
		atomic_set(&link->recover_busy, 0);
		return;
	}
	atomic_set_release(&link->state, RS_LINK_UP);
	pr_info("connected to %s (%d qp sets)\n", link->server, link->nr_sets);
	atomic_set(&link->recover_busy, 0);
	wake_up(&link->wq);
	rs_run_queues();
}

static void rs_qp_connect_work(struct work_struct *work)
{
	struct rs_qp *qp = container_of(work, struct rs_qp, connect_work);
	unsigned int noio = memalloc_noio_save();

	while (!READ_ONCE(qp->link->stop) && !rs_give_up(qp->link)) {
		if (!rs_qp_connect(qp)) {
			atomic_set_release(&qp->state, RS_QP_UP);
			rs_connect_done(qp->link, true);
			memalloc_noio_restore(noio);
			return;
		}
		if (wait_event_timeout(qp->link->wq, READ_ONCE(qp->link->stop), HZ))
			break;
	}
	atomic_set(&qp->state, RS_QP_OFF);
	rs_connect_done(qp->link, false);
	memalloc_noio_restore(noio);
}

static void rs_link_recover(struct work_struct *work)
{
	struct rs_link *link = container_of(to_delayed_work(work), struct rs_link,
					    recover_dwork);
	unsigned int noio;
	int i, n, ret;

	if (atomic_cmpxchg(&link->recover_busy, 0, 1) != 0)
		return;
	noio = memalloc_noio_save();
	if (READ_ONCE(link->stop))
		goto off;
	if (atomic_read(&link->state) == RS_LINK_UP && !READ_ONCE(link->key_wait) &&
	    !READ_ONCE(link->force_down))
		goto done;
	if (READ_ONCE(link->key_wait)) {
		u32 seen = (u32)atomic_read(&link->key_gen);

		if (!link->recover_deadline)
			link->recover_deadline = jiffies +
				msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
		if (rs_give_up(link))
			goto fail;
		if ((u32)atomic_read(&link->key_gen) == seen) {
			atomic_set(&link->state, RS_LINK_KEY_WAIT);
			atomic_set(&link->recover_busy, 0);
			mod_delayed_work(roswap_cm, &link->recover_dwork, HZ);
			memalloc_noio_restore(noio);
			return;
		}
		WRITE_ONCE(link->key_wait, false);
	}
	atomic_set(&link->state, RS_LINK_RECOVERING);
	if (!link->recover_deadline)
		link->recover_deadline = jiffies +
					 msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	if (rs_give_up(link))
		goto fail;
	ret = rs_link_alloc_qps(link);
	if (ret)
		goto fail;
	rs_link_drain(link);
	if (rs_give_up(link) || READ_ONCE(link->stop))
		goto fail;
	n = link->nr_sets * 2;
	WRITE_ONCE(link->connect_fail, false);
	atomic_set(&link->pending, n);
	for (i = 0; i < n; i++)
		queue_work(roswap_cm, &link->qps[i].connect_work);
	memalloc_noio_restore(noio);
	return;

fail:
	rs_link_fail(link);
done:
	atomic_set(&link->recover_busy, 0);
	memalloc_noio_restore(noio);
	return;
off:
	atomic_set(&link->state, RS_LINK_OFF);
	atomic_set(&link->recover_busy, 0);
	wake_up(&link->wq);
	memalloc_noio_restore(noio);
}

static void rs_fault_poll_work(struct work_struct *work)
{
	struct rs_qp *qp = container_of(work, struct rs_qp, poll_work);

	while (!READ_ONCE(qp->link->stop) && atomic_read(&qp->send_used) > 0 &&
	       atomic_read(&qp->state) == RS_QP_UP) {
		unsigned long flags;
		int n;

		if (!spin_trylock_irqsave(&qp->poll_lock, flags)) {
			cpu_relax();
			continue;
		}
		n = ib_process_cq_direct(qp->cq, 16);
		spin_unlock_irqrestore(&qp->poll_lock, flags);
		if (n <= 0)
			usleep_range(20, 50);
	}
	atomic_set(&qp->poll_queued, 0);
	if (!READ_ONCE(qp->link->stop) && atomic_read(&qp->send_used) > 0 &&
	    atomic_read(&qp->state) == RS_QP_UP &&
	    atomic_cmpxchg(&qp->poll_queued, 0, 1) == 0)
		queue_work_on(qp->cpu, roswap_io, &qp->poll_work);
}

static int rs_link_wait(struct rs_link *link)
{
	long left;

	left = wait_event_interruptible_timeout(link->wq,
			atomic_read(&link->state) == RS_LINK_UP ||
			atomic_read(&link->state) == RS_LINK_FATAL ||
			READ_ONCE(link->stop),
			msecs_to_jiffies(ROSWAP_CONNECT_TIMEOUT_MS));
	if (left < 0)
		return left;
	if (atomic_read(&link->state) == RS_LINK_UP)
		return 0;
	return -EIO;
}

static int rs_ensure_connected(struct rs_link *link)
{
	if (READ_ONCE(link->stop))
		return -ENODEV;
	if (!link->chunk_mb)
		return -EINVAL;
	if (atomic_read(&link->state) == RS_LINK_UP)
		return 0;
	rs_link_kick(link);
	return rs_link_wait(link);
}

static int rs_add_disk(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	struct request_queue *q;
#endif
	unsigned int queues = num_online_cpus();
	int ret;

	if (!queues)
		queues = 1;
	rs_hctx_cpu = kcalloc(queues, sizeof(*rs_hctx_cpu), GFP_KERNEL);
	if (!rs_hctx_cpu)
		return -ENOMEM;
	memset(&roswap_tag_set, 0, sizeof(roswap_tag_set));
	roswap_tag_set.ops = &roswap_mq_ops;
	roswap_tag_set.nr_hw_queues = queues;
	roswap_tag_set.queue_depth = queue_depth;
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
	kfree(rs_hctx_cpu);
	rs_hctx_cpu = NULL;
	return ret;
}

int roswap_node_add(u16 node_id, const char *addr)
{
	struct rs_link *link, *old;
	struct sockaddr_in dst;
	int ret;

	ret = rs_parse_server(addr, &dst);
	if (ret) {
		pr_err("invalid server '%s', expect ip:port\n", addr ? addr : "");
		return ret;
	}
	mutex_lock(&rs_link_mu);
	old = xa_load(&rs_links, node_id);
	if (old) {
		ret = strcmp(old->server, addr) ? -EEXIST : 0;
		goto out;
	}
	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link) {
		ret = -ENOMEM;
		goto out;
	}
	link->node_id = node_id;
	link->dst = dst;
	strscpy(link->server, addr, sizeof(link->server));
	init_waitqueue_head(&link->wq);
	INIT_DELAYED_WORK(&link->recover_dwork, rs_link_recover);
	atomic_set(&link->state, RS_LINK_OFF);
	ret = rs_map_node_add(node_id);
	if (ret) {
		kfree(link);
		goto out;
	}
	ret = xa_insert(&rs_links, node_id, link, GFP_KERNEL);
	if (ret) {
		kfree(link);
		if (ret == -EBUSY)
			ret = -EEXIST;
	}
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
	link = xa_load(&rs_links, node_id);
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

	rcu_read_lock();
	link = xa_load(&rs_links, node_id);
	if (!link) {
		rcu_read_unlock();
		return;
	}
	atomic_inc(&link->key_gen);
	WRITE_ONCE(link->key_wait, false);
	/*
	 * rkey 换了就要重新给这一轮计时。链路已经放弃时，旧 deadline 在过去，
	 * 不重置的话 recover 会马上再打印 gave up。
	 */
	link->recover_deadline = jiffies + msecs_to_jiffies(ROSWAP_RECOVER_LIMIT_MS);
	wake_up(&link->wq);
	if (atomic_read(&link->state) != RS_LINK_UP) {
		if (atomic_read(&link->state) == RS_LINK_FATAL)
			atomic_cmpxchg(&link->state, RS_LINK_FATAL, RS_LINK_OFF);
		rs_link_kick(link);
		/* 正在收尾的 recover 会忽略 kick。再排一次，用新的 deadline 重连。 */
		mod_delayed_work(roswap_cm, &link->recover_dwork, 0);
	}
	rcu_read_unlock();
}

int roswap_chunk_update(u16 node_id, u32 local_gid, u32 epoch, u64 base,
			u32 remote_key)
{
	int changed;

	changed = rs_map_chunk_update(node_id, local_gid, epoch, base, remote_key);
	if (changed < 0)
		return changed;
	if (!changed)
		return 0;
	rs_key_updated(node_id);
	pr_info("node %u chunk %u rkey updated to 0x%x\n", node_id, local_gid,
		remote_key);
	return 0;
}

int roswap_links_up(void)
{
	struct rs_link *link;
	unsigned long idx = 0;
	int n = 0;

	rcu_read_lock();
	for (;;) {
		int i;

		link = xa_find(&rs_links, &idx, ULONG_MAX, XA_PRESENT);
		if (!link)
			break;
		if (link->qps) {
			for (i = 0; i < link->nr_sets * 2; i++) {
				if (atomic_read(&link->qps[i].state) == RS_QP_UP)
					n++;
			}
		}
		if (idx == ULONG_MAX)
			break;
		idx++;
	}
	rcu_read_unlock();
	return n;
}

static int rs_drop_set(const char *val, const struct kernel_param *kp)
{
	struct rs_link *link;
	unsigned long idx = 0;

	(void)val;
	(void)kp;
	rcu_read_lock();
	for (;;) {
		link = xa_find(&rs_links, &idx, ULONG_MAX, XA_PRESENT);
		if (!link)
			break;
		WRITE_ONCE(link->force_down, true);
		WRITE_ONCE(link->key_wait, false);
		rs_link_failed(link);
		if (idx == ULONG_MAX)
			break;
		idx++;
	}
	rcu_read_unlock();
	return 0;
}

static int rs_drop_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return sysfs_emit(buf, "0\n");
}

static const struct kernel_param_ops rs_drop_ops = {
	.set = rs_drop_set,
	.get = rs_drop_get,
};

module_param_cb(drop_connection, &rs_drop_ops, NULL, 0200);
MODULE_PARM_DESC(drop_connection, "write anything to drop the RC connections");

static void rs_stop_links(void)
{
	struct rs_link *link;
	unsigned long idx = 0;

	xa_for_each(&rs_links, idx, link) {
		WRITE_ONCE(link->stop, true);
		atomic_set(&link->state, RS_LINK_OFF);
		wake_up(&link->wq);
	}
}

static void rs_cancel_links(void)
{
	struct rs_link *link;
	unsigned long idx = 0;

	xa_for_each(&rs_links, idx, link) {
		int i;

		cancel_delayed_work_sync(&link->recover_dwork);
		if (!link->qps)
			continue;
		for (i = 0; i < link->nr_sets * 2; i++) {
			cancel_work_sync(&link->qps[i].connect_work);
			cancel_work_sync(&link->qps[i].poll_work);
		}
	}
}

static void rs_teardown_links(void)
{
	struct rs_link *link;
	unsigned long idx = 0;

	xa_for_each(&rs_links, idx, link)
		rs_link_drain(link);
}

static void rs_free_links(void)
{
	struct rs_link *link;
	unsigned long idx = 0;

	xa_for_each(&rs_links, idx, link) {
		xa_erase(&rs_links, link->node_id);
		kfree(link->qps);
		kfree(link);
	}
	xa_destroy(&rs_links);
}

static int rs_bounce_pool_init(struct rs_bounce_pool *pool, int nr, int node)
{
	int i;

	if (nr < 1)
		nr = 1;
	if (nr > 256)
		nr = 256;
	pool->slot = kcalloc(nr, sizeof(*pool->slot), GFP_KERNEL);
	if (!pool->slot)
		return -ENOMEM;
	if (sbitmap_queue_init_node(&pool->sb, nr, -1, false, GFP_KERNEL, node)) {
		kfree(pool->slot);
		pool->slot = NULL;
		return -ENOMEM;
	}
	for (i = 0; i < nr; i++) {
		pool->slot[i].page = alloc_pages_node(node, GFP_KERNEL, 0);
		if (!pool->slot[i].page) {
			while (i--)
				__free_page(pool->slot[i].page);
			sbitmap_queue_free(&pool->sb);
			kfree(pool->slot);
			pool->slot = NULL;
			return -ENOMEM;
		}
		atomic_set(&pool->slot[i].live, 0);
	}
	pool->nr = nr;
	pool->ready = true;
	return 0;
}

static void rs_bounce_pools_free(void)
{
	int s, i;

	if (!rs_sets)
		return;
	for (s = 0; s < rs_nr_sets; s++) {
		struct rs_bounce_pool *pool = &rs_sets[s].pool;

		if (!pool->ready)
			continue;
		for (i = 0; i < pool->nr; i++)
			if (pool->slot[i].page)
				__free_page(pool->slot[i].page);
		sbitmap_queue_free(&pool->sb);
		kfree(pool->slot);
		pool->slot = NULL;
		pool->ready = false;
	}
}

static int rs_build_sets(void)
{
	unsigned int cpu;
	int online = num_online_cpus();
	int k, n = 0, s, ret;

	if (online < 1)
		online = 1;
	k = qp_sets ? qp_sets : 16;
	if (k > online)
		k = online;
	if (rs_nr_devs > k) {
		if (rs_nr_devs > online)
			return -EINVAL;
		k = rs_nr_devs;
	}
	if (k < 1)
		k = 1;
	rs_sets = kcalloc(k, sizeof(*rs_sets), GFP_KERNEL);
	rs_cpu_set = kcalloc(nr_cpu_ids, sizeof(*rs_cpu_set), GFP_KERNEL);
	if (!rs_sets || !rs_cpu_set)
		return -ENOMEM;
	rs_nr_sets = k;
	for (s = 0; s < k; s++)
		rs_sets[s].dev_idx = -1;
	for_each_online_cpu(cpu) {
		int set = n % k;

		if (!rs_sets[set].nr_cpus)
			rs_sets[set].cpu0 = (u16)cpu;
		rs_sets[set].nr_cpus++;
		rs_cpu_set[cpu] = (u16)set;
		n++;
	}
	if (!n) {
		rs_sets[0].cpu0 = 0;
		rs_sets[0].nr_cpus = 1;
	}
	for (s = 0; s < k; s++) {
		int node = cpu_to_node(rs_sets[s].cpu0);
		int match = 0, m, pick = -1;
		int nr;

		for (m = 0; m < rs_nr_devs; m++) {
			if (rs_devs[m].numa == node)
				match++;
		}
		if (match) {
			int seen = 0;

			for (m = 0; m < rs_nr_devs; m++) {
				if (rs_devs[m].numa != node)
					continue;
				if (seen++ == s % match) {
					pick = m;
					break;
				}
			}
		} else if (rs_nr_devs) {
			pick = s % rs_nr_devs;
		}
		rs_sets[s].dev_idx = pick;
		nr = 2 * (int)queue_depth * max(rs_sets[s].nr_cpus, 1);
		if (nr > 256)
			nr = 256;
		ret = rs_bounce_pool_init(&rs_sets[s].pool, nr, node);
		if (ret)
			return ret;
	}
	return 0;
}

static int rs_add_dev_ip(const char *text)
{
	__be32 ip;
	struct net_device *ndev;
	struct ib_device *ibdev;
	struct ib_pd *pd;
	struct rs_dev *dev, *grown;
	int depth;

	if (!in4_pton(text, -1, (u8 *)&ip, '\0', NULL))
		return -EINVAL;
	ndev = ip_dev_find(&init_net, ip);
	if (!ndev)
		return -ENODEV;
	ibdev = ib_device_get_by_netdev(ndev, RDMA_DRIVER_UNKNOWN);
	dev_put(ndev);
	if (!ibdev)
		return -ENODEV;
	pd = ib_alloc_pd(ibdev, 0);
	if (IS_ERR(pd)) {
		ib_device_put(ibdev);
		return PTR_ERR(pd);
	}
	depth = rs_clamp_depth(ibdev);
	ib_device_put(ibdev);
	grown = krealloc(rs_devs, (rs_nr_devs + 1) * sizeof(*rs_devs), GFP_KERNEL);
	if (!grown) {
		ib_dealloc_pd(pd);
		return -ENOMEM;
	}
	rs_devs = grown;
	dev = &rs_devs[rs_nr_devs];
	memset(dev, 0, sizeof(*dev));
	dev->ibdev = pd->device;
	dev->pd = pd;
	dev->src.sin_family = AF_INET;
	dev->src.sin_addr.s_addr = ip;
	dev->numa = dev_to_node(&pd->device->dev);
	dev->send_depth = depth;
	rs_nr_devs++;
	return 0;
}

static int rs_parse_local_addrs(void)
{
	char *buf, *cursor, *comma;
	int ret = 0;

	if (!local_addrs || !local_addrs[0])
		return 0;
	buf = kstrdup(local_addrs, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	cursor = strim(buf);
	if (!cursor[0]) {
		kfree(buf);
		return 0;
	}
	while (cursor && *cursor) {
		comma = strchr(cursor, ',');
		if (comma)
			*comma = '\0';
		cursor = strim(cursor);
		if (!*cursor) {
			ret = -EINVAL;
			break;
		}
		ret = rs_add_dev_ip(cursor);
		if (ret)
			break;
		if (!comma)
			break;
		cursor = comma + 1;
	}
	kfree(buf);
	return ret;
}

static void rs_free_devs(void)
{
	int i;

	for (i = 0; i < rs_nr_devs; i++) {
		if (rs_devs[i].pd)
			ib_dealloc_pd(rs_devs[i].pd);
	}
	kfree(rs_devs);
	rs_devs = NULL;
	rs_nr_devs = 0;
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

	if (!queue_depth || queue_depth > 4096)
		return -EINVAL;
	if (!qp_budget)
		qp_budget = 4096;
	if (qp_budget < 2)
		return -EINVAL;
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
	ret = rs_parse_local_addrs();
	if (ret) {
		pr_err("local_addrs: %d\n", ret);
		goto err_dev;
	}
	ret = rs_map_init(rs_capacity_mb);
	if (ret)
		goto err_dev;
	ret = rs_map_set_protection((u32)mode);
	if (ret)
		goto err_map;
	ret = rs_build_sets();
	if (ret)
		goto err_map;
	roswap_io = alloc_workqueue("roswap_io", WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);
	roswap_cm = alloc_workqueue("roswap_cm",
				    WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!roswap_io || !roswap_cm) {
		ret = -ENOMEM;
		goto err_wq;
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
	pr_info("added /dev/roswap0, major %d, %u MiB, %d qp sets\n",
		roswap_major_id, rs_capacity_mb, rs_nr_sets);
	return 0;

err_disk:
	del_gendisk(roswap_disk);
	roswap_cleanup_disk(roswap_disk);
	roswap_disk = NULL;
	blk_mq_free_tag_set(&roswap_tag_set);
	kfree(rs_hctx_cpu);
	rs_hctx_cpu = NULL;
err_conn:
	rs_stop_links();
	rs_cancel_links();
	rs_teardown_links();
	unregister_blkdev(roswap_major_id, "roswap");
err_wq:
	if (roswap_io)
		destroy_workqueue(roswap_io);
	if (roswap_cm)
		destroy_workqueue(roswap_cm);
	roswap_io = NULL;
	roswap_cm = NULL;
err_map:
	rs_bounce_pools_free();
	kfree(rs_sets);
	kfree(rs_cpu_set);
	rs_sets = NULL;
	rs_cpu_set = NULL;
	rs_map_exit();
err_dev:
	rs_free_devs();
	return ret;
}

static void __exit roswap_exit(void)
{
	rs_genl_unregister();
	rs_stop_links();
	rs_cancel_links();
	if (roswap_disk)
		blk_mq_quiesce_queue(roswap_disk->queue);
	synchronize_rcu();
	rs_teardown_links();
	if (roswap_disk) {
		blk_mq_unquiesce_queue(roswap_disk->queue);
		del_gendisk(roswap_disk);
		roswap_cleanup_disk(roswap_disk);
		roswap_disk = NULL;
		blk_mq_free_tag_set(&roswap_tag_set);
	}
	if (roswap_io)
		destroy_workqueue(roswap_io);
	if (roswap_cm)
		destroy_workqueue(roswap_cm);
	synchronize_rcu();
	rs_free_links();
	rs_bounce_pools_free();
	kfree(rs_sets);
	kfree(rs_cpu_set);
	kfree(rs_hctx_cpu);
	rs_free_devs();
	unregister_blkdev(roswap_major_id, "roswap");
	rs_map_exit();
	pr_info("unregistered roswap major %d\n", roswap_major_id);
}

module_init(roswap_init);
module_exit(roswap_exit);
MODULE_LICENSE("GPL");
