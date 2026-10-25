#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/genhd.h>
#include <linux/module.h>

#include "roswap_internal.h"

static int rs_open(struct block_device *bdev, fmode_t mode)
{
	struct rs_dev *dev = bdev->bd_disk->private_data;

	atomic_inc(&dev->openers);
	return 0;
}

static void rs_release(struct gendisk *disk, fmode_t mode)
{
	struct rs_dev *dev = disk->private_data;

	atomic_dec(&dev->openers);
}

static const struct block_device_operations rs_fops = {
	.owner		= THIS_MODULE,
	.open		= rs_open,
	.release	= rs_release,
};

static blk_status_t rs_queue_rq(struct blk_mq_hw_ctx *hctx,
				const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct rs_dev *dev = rq->q->queuedata;

	return rs_io_queue(dev, rq, hctx->queue_num);
}

static const struct blk_mq_ops rs_mq_ops = {
	.queue_rq = rs_queue_rq,
};

static void rs_blk_free(struct rs_blk *b)
{
	if (!b)
		return;
	if (b->disk)
		put_disk(b->disk);
	if (b->q)
		blk_cleanup_queue(b->q);
	if (b->tag_set.tags)
		blk_mq_free_tag_set(&b->tag_set);
	if (b->major > 0)
		unregister_blkdev(b->major, "roswap");
	kfree(b);
}

int rs_blk_attach(struct rs_dev *dev)
{
	struct rs_blk *b;
	int segs, ret;

	segs = rs_max_segs;
	if (segs < 1)
		segs = 1;
	if (segs > RS_MAX_SEGS_HARD)
		segs = RS_MAX_SEGS_HARD;
	if (rs_initiator_depth > 0 && segs > rs_initiator_depth) {
		pr_info("max_segs %d clamped to initiator_depth %d\n",
			segs, rs_initiator_depth);
		segs = rs_initiator_depth;
	}

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->tag_set.ops = &rs_mq_ops;
	b->tag_set.nr_hw_queues = nr_cpu_ids;
	b->tag_set.nr_maps = 1;
	b->tag_set.queue_depth = rs_queue_depth;
	b->tag_set.numa_node = NUMA_NO_NODE;
	b->tag_set.cmd_size = ALIGN(sizeof(struct rs_blk_cmd) +
				    sizeof(struct rs_io) * segs, sizeof(void *));
	b->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	b->tag_set.driver_data = dev;
	ret = blk_mq_alloc_tag_set(&b->tag_set);
	if (ret) {
		pr_err("blk_mq_alloc_tag_set: %d\n", ret);
		kfree(b);
		return ret;
	}

	ret = register_blkdev(0, "roswap");
	if (ret < 0) {
		blk_mq_free_tag_set(&b->tag_set);
		kfree(b);
		return ret;
	}
	b->major = ret;

#if RS_HAVE_BLK_MQ_ALLOC_DISK
	b->disk = blk_mq_alloc_disk(&b->tag_set, dev);
	if (IS_ERR(b->disk)) {
		ret = PTR_ERR(b->disk);
		b->disk = NULL;
		rs_blk_free(b);
		return ret;
	}
	b->q = b->disk->queue;
#else
	b->q = blk_mq_init_queue(&b->tag_set);
	if (IS_ERR(b->q)) {
		ret = PTR_ERR(b->q);
		b->q = NULL;
		rs_blk_free(b);
		return ret;
	}
	b->disk = alloc_disk(1);
	if (!b->disk) {
		rs_blk_free(b);
		return -ENOMEM;
	}
	b->disk->queue = b->q;
#endif
	b->q->queuedata = dev;
	blk_queue_logical_block_size(b->q, PAGE_SIZE);
	blk_queue_physical_block_size(b->q, PAGE_SIZE);
	blk_queue_io_min(b->q, PAGE_SIZE);
	blk_queue_max_hw_sectors(b->q, segs * (PAGE_SIZE >> 9));
	blk_queue_max_segments(b->q, segs);
	blk_queue_flag_set(QUEUE_FLAG_NONROT, b->q);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, b->q);
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, b->q);
	b->q->limits.discard_granularity = 1 << 20;
	b->q->limits.discard_alignment = 0;
	blk_queue_max_discard_sectors(b->q, (1U << 20) >> 9);
	blk_queue_max_write_zeroes_sectors(b->q, (1U << 20) >> 9);

	b->disk->major = b->major;
	b->disk->first_minor = 0;
	b->disk->fops = &rs_fops;
	b->disk->private_data = dev;
	b->disk->flags |= GENHD_FL_NO_PART_SCAN;
	sprintf(b->disk->disk_name, "roswap0");
	set_capacity(b->disk, dev->nr_slots << 3);
	add_disk(b->disk);

	dev->max_segs = segs;
	dev->blk = b;
	pr_info("/dev/roswap0 slots %llu segs %d queues %u depth %d\n",
		dev->nr_slots, segs, b->tag_set.nr_hw_queues, rs_queue_depth);
	return 0;
}

void rs_blk_detach(struct rs_dev *dev)
{
	struct rs_blk *b;

	if (!dev || !dev->blk)
		return;
	b = dev->blk;
	dev->blk = NULL;
	if (b->disk)
		del_gendisk(b->disk);
	if (b->q) {
		blk_cleanup_queue(b->q);
		b->q = NULL;
	}
	blk_mq_free_tag_set(&b->tag_set);
	if (b->disk) {
		put_disk(b->disk);
		b->disk = NULL;
	}
	if (b->major > 0)
		unregister_blkdev(b->major, "roswap");
	b->major = 0;
	kfree(b);
}
