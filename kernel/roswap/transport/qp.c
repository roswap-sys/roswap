#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>

#include "roswap_internal.h"

static void rs_cqe_thunk(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rs_io *io = container_of(wc->wr_cqe, struct rs_io, cqe);
	unsigned long flags;

	if (atomic_cmpxchg(&io->finished, 0, 1) != 0)
		return;
	if (io->signaled && io->conn) {
		spin_lock_irqsave(&io->conn->sq_lock, flags);
		io->conn->sq_free += io->chain_len;
		WARN_ON_ONCE(io->conn->sq_free > io->conn->sq_depth);
		spin_unlock_irqrestore(&io->conn->sq_lock, flags);
	}
	if (io->done)
		io->done(io, wc);
}

void rs_io_software_fail(struct rs_io *io)
{
	struct ib_wc wc;

	if (atomic_cmpxchg(&io->finished, 0, 1) != 0)
		return;
	memset(&wc, 0, sizeof(wc));
	wc.status = IB_WC_GENERAL_ERR;
	wc.wr_cqe = &io->cqe;
	if (io->done)
		io->done(io, &wc);
}

int rs_conn_post(struct rs_conn *c, struct rs_io **ios, int n, bool selective)
{
	unsigned long flags;
	struct ib_send_wr *bad = NULL;
	int i, ret, run = 0;
	u32 half;

	if (!c || n <= 0)
		return -EINVAL;
	if (atomic_read(&c->state) != RS_CONN_UP)
		return -EIO;

	half = c->sq_depth / 2;
	if (half < 1)
		half = 1;

	spin_lock_irqsave(&c->sq_lock, flags);
	if (c->sq_free < (u32)n) {
		spin_unlock_irqrestore(&c->sq_lock, flags);
		return -EAGAIN;
	}
	for (i = 0; i < n; i++) {
		struct rs_io *io = ios[i];
		bool sig;

		run++;
		sig = !selective || i == n - 1 || (c->unsignaled + 1 >= half);
		io->wr.wr.next = (i + 1 < n) ? &ios[i + 1]->wr.wr : NULL;
		io->wr.wr.wr_cqe = &io->cqe;
		io->wr.wr.sg_list = &io->sge;
		io->wr.wr.num_sge = 1;
		io->wr.wr.opcode = io->opcode;
		io->wr.wr.send_flags = sig ? IB_SEND_SIGNALED : 0;
		io->wr.remote_addr = io->remote_addr;
		io->wr.rkey = io->remote_rkey;
		io->sge.addr = io->dma;
		io->sge.length = PAGE_SIZE;
		io->sge.lkey = rs_lkey;
		io->cqe.done = rs_cqe_thunk;
		io->conn = c;
		io->signaled = sig;
		if (sig) {
			io->chain_len = run;
			run = 0;
			c->unsignaled = 0;
		} else {
			io->chain_len = 0;
			c->unsignaled++;
		}
		atomic_set(&io->finished, 0);
	}
	ret = ib_post_send(c->cm_id->qp, &ios[0]->wr.wr, &bad);
	if (ret) {
		spin_unlock_irqrestore(&c->sq_lock, flags);
		atomic_set(&c->state, RS_CONN_ERROR);
		pr_err_ratelimited("ib_post_send: %d\n", ret);
		return ret;
	}
	c->sq_free -= n;
	spin_unlock_irqrestore(&c->sq_lock, flags);
	return 0;
}
