/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_COMPAT_H
#define _ROSWAP_COMPAT_H

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <net/netlink.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
#define WQ_PERCPU 0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define nla_strscpy(dst, nla, size) ((ssize_t)nla_strlcpy(dst, nla, size))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0) && !defined(GENHD_FL_NO_PART)
#define GENHD_FL_NO_PART GENHD_FL_NO_PART_SCAN
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static inline struct gendisk *roswap_alloc_disk(struct blk_mq_tag_set *set)
{
	struct queue_limits lim = {
		.logical_block_size = PAGE_SIZE,
		.physical_block_size = PAGE_SIZE,
		.max_hw_sectors = PAGE_SIZE >> SECTOR_SHIFT,
		.max_segments = 1,
		.dma_alignment = PAGE_SIZE - 1,
		.max_hw_discard_sectors = (1U << 20) >> SECTOR_SHIFT,
		.max_discard_sectors = (1U << 20) >> SECTOR_SHIFT,
		.discard_granularity = 1U << 20,
	};

	return blk_mq_alloc_disk(set, &lim, NULL);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
static inline struct gendisk *roswap_alloc_disk(struct blk_mq_tag_set *set)
{
	return blk_mq_alloc_disk(set, NULL);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define ROSWAP_BUSY_ITER_ARGS struct request *rq, void *data
#else
#define ROSWAP_BUSY_ITER_ARGS struct request *rq, void *data, bool reserved
#endif

static inline void roswap_apply_queue_limits(struct gendisk *disk)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
	struct request_queue *q = disk->queue;

	blk_queue_logical_block_size(q, PAGE_SIZE);
	blk_queue_physical_block_size(q, PAGE_SIZE);
	blk_queue_max_hw_sectors(q, PAGE_SIZE >> SECTOR_SHIFT);
	blk_queue_max_segments(q, 1);
	blk_queue_dma_alignment(q, PAGE_SIZE - 1);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	blk_queue_flag_set(QUEUE_FLAG_NONROT, disk->queue);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, disk->queue);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
	disk->queue->limits.discard_granularity = 1U << 20;
	blk_queue_max_discard_sectors(disk->queue, (1U << 20) >> SECTOR_SHIFT);
#endif
}

static inline int roswap_register_disk(struct gendisk *disk)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	add_disk(disk);
	return 0;
#else
	return add_disk(disk);
#endif
}

static inline void roswap_cleanup_disk(struct gendisk *disk)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
	struct request_queue *q = disk->queue;

	put_disk(disk);
	if (q)
		blk_cleanup_queue(q);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
	blk_cleanup_disk(disk);
#else
	put_disk(disk);
#endif
}

#endif
