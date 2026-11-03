// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "RoSwap: " fmt

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/inet.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/version.h>



#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define roswap_cleanup_disk(disk) put_disk(disk)
#else
#define roswap_cleanup_disk(disk) blk_cleanup_disk(disk)
#endif

// private magic
#define RS_CONN_MAGIC		0x52535750
#define RS_CONN_VERSION		1
#define RS_QP_BULK		2

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

	ret = roswap_add_disk();
	if (ret) {
		unregister_blkdev(roswap_major_id, "roswap");
		pr_err("add_disk failed: %d\n", ret);
		return ret;
	}

	pr_info("added /dev/roswap0, major %d\n", roswap_major_id);
	return 0;
}

static int roswap_parse_server(struct sockaddr_in *sin)
{
	const char * end;
	u16 port;
	memset(sin, 0, sizeof(*sin));
	
	sin->sin_family = AF_INET;
	
	if (!in4_pton(server, -1, (u8 *)&sin->sin_addr.s_addr, ':', &end) ||
		*end != ':' || kstrtou16(end + 1, 10, &port))
		return -EINVAL;
	
	sin->sin_port = htons(port);

	return 0;
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
