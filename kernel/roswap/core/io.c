#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/highmem.h>
#include <linux/mm.h>

#include "roswap_internal.h"

static blk_status_t rs_fail_req(struct request *rq, blk_status_t st)
{
	blk_mq_start_request(rq);
	blk_mq_end_request(rq, st);
	return BLK_STS_OK;
}

static void rs_rollback(struct rs_dev *dev, struct rs_io *ios, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (ios[i].mapped && rs_ibdev) {
			ib_dma_unmap_page(rs_ibdev, ios[i].dma, PAGE_SIZE,
					  ios[i].is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			ios[i].mapped = 0;
		}
		rs_chunk_put(ios[i].chunk);
		ios[i].chunk = NULL;
		rs_pin_put(dev, ios[i].slot);
	}
}

static void rs_cmd_finish(struct rs_blk_cmd *cmd)
{
	struct rs_dev *dev = cmd->dev;
	int status, i, write;

	if (atomic_cmpxchg(&cmd->cmd_done, 0, 1) != 0)
		return;
	status = atomic_read(&cmd->wc_status);
	write = cmd->nios > 0 && cmd->ios[0].is_write;
	for (i = 0; i < cmd->nios; i++) {
		struct rs_io *io = &cmd->ios[i];

		if (io->mapped && rs_ibdev) {
			ib_dma_unmap_page(rs_ibdev, io->dma, PAGE_SIZE,
					  io->is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			io->mapped = 0;
		}
		if (io->is_write) {
			if (status)
				rs_pin_put(dev, io->slot);
			else
				rs_write_commit(dev, io->slot);
		} else {
			rs_pin_put(dev, io->slot);
		}
		rs_chunk_put(io->chunk);
		io->chunk = NULL;
	}
	if (status) {
		pr_err_ratelimited("I/O error slot %lu wc %d (%s)\n",
				   cmd->nios ? (unsigned long)cmd->ios[0].slot : 0UL,
				   status, ib_wc_status_msg(status));
		if (write)
			atomic64_inc(&dev->write_err);
		else
			atomic64_inc(&dev->read_err);
		blk_mq_end_request(cmd->rq, BLK_STS_IOERR);
	} else {
		if (write)
			atomic64_inc(&dev->write_ok);
		else
			atomic64_inc(&dev->read_ok);
		blk_mq_end_request(cmd->rq, BLK_STS_OK);
	}
}

static void rs_blk_io_done(struct rs_io *io, const struct ib_wc *wc)
{
	struct rs_blk_cmd *cmd = io->cmd;
	struct rs_node *n;
	int arrived;

	if (!cmd)
		return;
	if (wc->status != IB_WC_SUCCESS) {
		atomic_cmpxchg(&cmd->wc_status, 0, (int)wc->status);
		rs_node_note_conn_error(io->conn);
	} else if (io->conn) {
		n = rs_node_get(io->conn->node_id);
		if (n && n->epoch != io->node_epoch)
			atomic_cmpxchg(&cmd->wc_status, 0, (int)IB_WC_GENERAL_ERR);
	}
	arrived = atomic_inc_return(&cmd->arrived);
	/*
	 * Single group: the tail WR is always signaled (or software-failed)
	 * and completes after every earlier WR on the same QP.
	 */
	if (cmd->single_group) {
		if (io == &cmd->ios[cmd->nios - 1])
			rs_cmd_finish(cmd);
	} else if (arrived == cmd->nios) {
		rs_cmd_finish(cmd);
	}
}

static int rs_bind_io(struct rs_dev *dev, struct rs_io *io, struct page *page,
		      u32 loc, int cpu, bool write)
{
	struct rs_chunk *c;
	struct rs_node *n;
	struct rs_conn *conn;
	enum dma_data_direction dir;
	u32 gid, idx;
	dma_addr_t dma;

	rs_unpack_loc(loc, &gid, &idx);
	c = rs_chunk_lookup(gid);
	if (!c)
		return -EIO;
	n = rs_node_get(c->node_id);
	conn = n ? rs_node_conn_up(n, cpu) : NULL;
	if (!conn) {
		if (n && atomic_read(&n->state) == RS_N_ACTIVE)
			atomic_set(&n->state, RS_N_ERROR);
		rs_chunk_put(c);
		return -EIO;
	}
	dir = write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	dma = ib_dma_map_page(rs_ibdev, page, 0, PAGE_SIZE, dir);
	if (ib_dma_mapping_error(rs_ibdev, dma)) {
		rs_chunk_put(c);
		return -EIO;
	}
	io->chunk = c;
	io->conn = conn;
	io->dma = dma;
	io->mapped = 1;
	io->is_write = write;
	io->opcode = write ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ;
	io->remote_rkey = c->rkey;
	io->node_epoch = c->node_epoch;
	io->remote_addr = c->raddr + ((u64)idx << 20) +
			  ((u64)(io->slot & (RS_EXTENT_PAGES - 1)) << PAGE_SHIFT);
	io->done = rs_blk_io_done;
	rs_chunk_set_owner(c, idx, (u32)(io->slot >> 8));
	return 0;
}

static void rs_zero_page(struct page *page)
{
	void *p = page_address(page);

	if (p) {
		memset(p, 0, PAGE_SIZE);
		return;
	}
	p = kmap_atomic(page);
	memset(p, 0, PAGE_SIZE);
	kunmap_atomic(p);
}

static bool rs_sq_tight(struct rs_blk_cmd *cmd, int nios)
{
	u8 seen[RS_MAX_SEGS_HARD];
	int i, j;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < nios; i++) {
		struct rs_conn *c;
		int bn = 0;

		if (seen[i] || !cmd->ios[i].conn)
			continue;
		c = cmd->ios[i].conn;
		for (j = i; j < nios; j++) {
			if (cmd->ios[j].conn == c) {
				seen[j] = 1;
				bn++;
			}
		}
		if ((u32)bn > READ_ONCE(c->sq_free))
			return true;
	}
	return false;
}

static int rs_count_groups(struct rs_blk_cmd *cmd, int nios)
{
	u8 seen[RS_MAX_SEGS_HARD];
	int i, j, groups = 0;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < nios; i++) {
		if (seen[i])
			continue;
		groups++;
		for (j = i; j < nios; j++) {
			if (cmd->ios[j].conn == cmd->ios[i].conn)
				seen[j] = 1;
		}
	}
	return groups;
}

static void rs_fail_unposted(struct rs_blk_cmd *cmd, int nios, const u8 *posted)
{
	int i;

	atomic_cmpxchg(&cmd->wc_status, 0, (int)IB_WC_GENERAL_ERR);
	for (i = 0; i < nios; i++) {
		if (!posted[i])
			rs_io_software_fail(&cmd->ios[i]);
	}
}

static int rs_post_groups(struct rs_blk_cmd *cmd, int nios)
{
	u8 taken[RS_MAX_SEGS_HARD];
	u8 posted[RS_MAX_SEGS_HARD];
	struct rs_io *batch[RS_MAX_SEGS_HARD];
	int i, j, nposted = 0;

	memset(taken, 0, sizeof(taken));
	memset(posted, 0, sizeof(posted));
	for (i = 0; i < nios; i++) {
		struct rs_conn *c;
		int bn = 0, ret, k;

		if (taken[i])
			continue;
		c = cmd->ios[i].conn;
		for (j = i; j < nios; j++) {
			if (taken[j] || cmd->ios[j].conn != c)
				continue;
			batch[bn++] = &cmd->ios[j];
			taken[j] = 1;
		}
		ret = rs_conn_post(c, batch, bn, cmd->single_group);
		if (ret) {
			if (ret == -EAGAIN && nposted == 0)
				return -EAGAIN;
			rs_fail_unposted(cmd, nios, posted);
			return 0;
		}
		for (k = 0; k < nios; k++) {
			if (cmd->ios[k].conn == c && taken[k])
				posted[k] = 1;
		}
		nposted += bn;
	}
	return 0;
}

static blk_status_t rs_queue_rw(struct rs_dev *dev, struct request *rq, int cpu, bool write)
{
	struct rs_blk_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct req_iterator iter;
	struct bio_vec bv;
	sector_t pos = blk_rq_pos(rq);
	unsigned int segs = dev->max_segs ? dev->max_segs : RS_MAX_SEGS_HARD;
	u32 pages = 0;
	pgoff_t slot;
	int nios = 0;
	int ret;

	if ((pos & 7) || (blk_rq_sectors(rq) & 7) || !blk_rq_sectors(rq))
		return rs_fail_req(rq, BLK_STS_IOERR);
	slot = pos >> 3;
	if (slot >= dev->nr_slots || (blk_rq_sectors(rq) >> 3) > dev->nr_slots - slot)
		return rs_fail_req(rq, BLK_STS_IOERR);

	atomic_set(&cmd->arrived, 0);
	atomic_set(&cmd->wc_status, 0);
	atomic_set(&cmd->cmd_done, 0);
	cmd->rq = rq;
	cmd->dev = dev;
	cmd->nios = 0;
	cmd->single_group = 0;

	rq_for_each_segment(bv, rq, iter) {
		unsigned int k, np;

		if ((bv.bv_offset | bv.bv_len) & (PAGE_SIZE - 1) || !bv.bv_len)
			goto misaligned;
		np = bv.bv_len >> PAGE_SHIFT;
		for (k = 0; k < np; k++) {
			struct page *page = nth_page(bv.bv_page, k);
			struct rs_io *io;
			bool zero = false;
			u32 loc = RS_LOC_NONE;

			if ((unsigned int)nios >= segs)
				goto misaligned;
			io = &cmd->ios[nios];
			memset(io, 0, sizeof(*io));
			atomic_set(&io->finished, 0);
			io->cmd = cmd;
			io->done = rs_blk_io_done;
			io->slot = slot;
			io->is_write = write;
			if (write)
				ret = rs_write_pin(dev, slot, &loc);
			else
				ret = rs_read_pin(dev, slot, &loc, &zero);
			if (ret) {
				rs_rollback(dev, cmd->ios, nios);
				if (ret == -ENOSPC) {
					atomic64_inc(&dev->alloc_fail);
					pr_warn_ratelimited("no free remote extent for slot %lu\n",
							    (unsigned long)slot);
				}
				return rs_fail_req(rq, BLK_STS_IOERR);
			}
			if (zero) {
				rs_zero_page(page);
				atomic64_inc(&dev->zero_read);
				slot++;
				pages++;
				continue;
			}
			ret = rs_bind_io(dev, io, page, loc, cpu, write);
			if (ret) {
				rs_pin_put(dev, slot);
				rs_rollback(dev, cmd->ios, nios);
				return rs_fail_req(rq, BLK_STS_IOERR);
			}
			nios++;
			slot++;
			pages++;
		}
	}
	if (pages != (blk_rq_sectors(rq) >> 3))
		goto misaligned;
	if (nios == 0)
		return rs_fail_req(rq, BLK_STS_OK);

	cmd->nios = nios;
	cmd->single_group = rs_count_groups(cmd, nios) == 1;
	if (rs_sq_tight(cmd, nios)) {
		rs_rollback(dev, cmd->ios, nios);
		return BLK_STS_RESOURCE;
	}
	blk_mq_start_request(rq);
	ret = rs_post_groups(cmd, nios);
	if (ret == -EAGAIN) {
		rs_rollback(dev, cmd->ios, nios);
		blk_mq_requeue_request(rq, true);
	}
	return BLK_STS_OK;

misaligned:
	rs_rollback(dev, cmd->ios, nios);
	return rs_fail_req(rq, BLK_STS_IOERR);
}

static blk_status_t rs_queue_discard(struct rs_dev *dev, struct request *rq)
{
	sector_t pos = blk_rq_pos(rq);
	unsigned int sectors = blk_rq_sectors(rq);
	pgoff_t slot;
	u32 nr;

	if ((pos & 7) || (sectors & 7))
		return rs_fail_req(rq, BLK_STS_IOERR);
	slot = pos >> 3;
	nr = sectors >> 3;
	if (slot > dev->nr_slots || (u64)slot + nr > dev->nr_slots)
		return rs_fail_req(rq, BLK_STS_IOERR);
	rs_discard_range(dev, slot, nr);
	return rs_fail_req(rq, BLK_STS_OK);
}

blk_status_t rs_io_queue(struct rs_dev *dev, struct request *rq, int cpu)
{
	if (!dev || !rs_ibdev)
		return rs_fail_req(rq, BLK_STS_IOERR);
	switch (req_op(rq)) {
	case REQ_OP_READ:
		return rs_queue_rw(dev, rq, cpu, false);
	case REQ_OP_WRITE:
		return rs_queue_rw(dev, rq, cpu, true);
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		return rs_queue_discard(dev, rq);
	case REQ_OP_FLUSH:
		return rs_fail_req(rq, BLK_STS_OK);
	default:
		return rs_fail_req(rq, BLK_STS_NOTSUPP);
	}
}
