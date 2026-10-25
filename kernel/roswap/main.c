#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/inet.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <linux/highmem.h>

#include "roswap_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RoSwap");
MODULE_DESCRIPTION("RoCEv2 swap block device (M1 W1-W3)");
MODULE_VERSION("0.1");

struct ib_device *rs_ibdev;
struct ib_pd *rs_pd;
u32 rs_lkey;
DEFINE_MUTEX(rs_ctl_lock);
struct rs_dev *rs_device;

int rs_sq_depth = 128;
int rs_nr_conns;
int rs_ack_timeout = 14;
int rs_retry_count = 4;
int rs_tos = 106;
int rs_max_segs = 32;
int rs_queue_depth = 64;
int rs_initiator_depth = 16;
char rs_src_addr[64];
u8 rs_cluster_uuid[16];

static char rs_rdma_dev[64] = "mlx5_0";
static char rs_node_addr[64];
static char rs_raddr_str[32];
static char rs_rkey_str[32];
static char rs_len_str[32] = "1G";
static char rs_size_str[32];
static char rs_uuid_str[40];
static int rs_selftest;
static int rs_meta_test;
static int rs_epoch = 1;
static int rs_node_id = 1;
static int rs_gid = 1;
static int rs_port = RS_AGENT_PORT;
static bool rs_xa_ready;

module_param_string(rdma_dev, rs_rdma_dev, sizeof(rs_rdma_dev), 0444);
MODULE_PARM_DESC(rdma_dev, "RDMA device name (empty = first device)");
module_param_string(node_addr, rs_node_addr, sizeof(rs_node_addr), 0444);
MODULE_PARM_DESC(node_addr, "Shortcut peer IP or IP:port for W1/W2");
module_param_string(src_addr, rs_src_addr, sizeof(rs_src_addr), 0444);
module_param_string(raddr, rs_raddr_str, sizeof(rs_raddr_str), 0444);
module_param_string(rkey, rs_rkey_str, sizeof(rs_rkey_str), 0444);
module_param_string(len, rs_len_str, sizeof(rs_len_str), 0444);
module_param_string(size, rs_size_str, sizeof(rs_size_str), 0444);
MODULE_PARM_DESC(size, "Block device size, 0 or empty skips /dev/roswap0");
module_param_string(cluster_uuid, rs_uuid_str, sizeof(rs_uuid_str), 0444);
module_param_named(selftest, rs_selftest, int, 0444);
module_param_named(meta_test, rs_meta_test, int, 0444);
module_param_named(sq_depth, rs_sq_depth, int, 0444);
module_param_named(nr_conns, rs_nr_conns, int, 0444);
module_param_named(ack_timeout, rs_ack_timeout, int, 0444);
module_param_named(retry_count, rs_retry_count, int, 0444);
module_param_named(tos, rs_tos, int, 0444);
module_param_named(max_segs, rs_max_segs, int, 0444);
module_param_named(queue_depth, rs_queue_depth, int, 0444);
module_param_named(epoch, rs_epoch, int, 0444);
module_param_named(node_id, rs_node_id, int, 0444);
module_param_named(gid, rs_gid, int, 0444);
module_param_named(port, rs_port, int, 0444);

void rs_globals_init(void)
{
	if (rs_xa_ready)
		return;
	xa_init_flags(&rs_chunks, XA_FLAGS_LOCK_IRQ);
	rs_xa_ready = true;
}

static void rs_clamp_params(void)
{
	if (rs_sq_depth < 16)
		rs_sq_depth = 16;
	if (rs_sq_depth > 4096)
		rs_sq_depth = 4096;
	if (rs_retry_count < 0)
		rs_retry_count = 0;
	if (rs_retry_count > 7)
		rs_retry_count = 7;
	if (rs_ack_timeout < 1)
		rs_ack_timeout = 1;
	if (rs_ack_timeout > 31)
		rs_ack_timeout = 31;
	if (rs_tos < 0)
		rs_tos = 0;
	if (rs_tos > 255)
		rs_tos = 255;
	if (rs_max_segs < 1)
		rs_max_segs = 1;
	if (rs_max_segs > RS_MAX_SEGS_HARD)
		rs_max_segs = RS_MAX_SEGS_HARD;
	if (rs_queue_depth < 8)
		rs_queue_depth = 8;
	if (rs_queue_depth > 256)
		rs_queue_depth = 256;
}

static int rs_parse_uuid(const char *s)
{
	int i;

	if (!s || !s[0])
		return 0;
	if (strlen(s) != 32)
		return -EINVAL;
	for (i = 0; i < 16; i++) {
		char hex[3] = { s[i * 2], s[i * 2 + 1], 0 };
		u8 v;

		if (kstrtou8(hex, 16, &v))
			return -EINVAL;
		rs_cluster_uuid[i] = v;
	}
	return 0;
}

int rs_dev_alloc(struct rs_dev **out, u64 bytes)
{
	struct rs_dev *dev;
	int i, cpu;

	bytes &= ~((1ULL << 20) - 1);
	if (!bytes)
		return -EINVAL;
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->nr_slots = bytes >> PAGE_SHIFT;
	dev->nr_extents = bytes >> 20;
	dev->sstate = vzalloc(dev->nr_slots);
	dev->extents = vzalloc(dev->nr_extents * sizeof(*dev->extents));
	dev->ext_live = vzalloc(dev->nr_extents * sizeof(*dev->ext_live));
	dev->ext_inflight = vzalloc(dev->nr_extents * sizeof(*dev->ext_inflight));
	dev->ext_locks = kvmalloc_array(RS_EXT_LOCKS, sizeof(spinlock_t), GFP_KERNEL);
	dev->resv = alloc_percpu(struct rs_pcpu_resv);
	if (!dev->sstate || !dev->extents || !dev->ext_live || !dev->ext_inflight ||
	    !dev->ext_locks || !dev->resv) {
		rs_dev_free(dev);
		return -ENOMEM;
	}
	memset(dev->extents, 0xff, dev->nr_extents * sizeof(*dev->extents));
	for (i = 0; i < RS_EXT_LOCKS; i++)
		spin_lock_init(&dev->ext_locks[i]);
	for_each_possible_cpu(cpu)
		spin_lock_init(&per_cpu_ptr(dev->resv, cpu)->lock);
	*out = dev;
	return 0;
}

void rs_dev_free(struct rs_dev *dev)
{
	if (!dev)
		return;
	if (dev->blk)
		rs_blk_detach(dev);
	free_percpu(dev->resv);
	kvfree(dev->ext_locks);
	vfree(dev->ext_inflight);
	vfree(dev->ext_live);
	vfree(dev->extents);
	vfree(dev->sstate);
	kfree(dev);
}

int rs_dev_create(u64 bytes)
{
	struct rs_dev *dev;
	int ret;

	if (rs_device)
		return -EEXIST;
	if (!rs_ibdev)
		return -ENODEV;
	ret = rs_dev_alloc(&dev, bytes);
	if (ret)
		return ret;
	ret = rs_blk_attach(dev);
	if (ret) {
		rs_dev_free(dev);
		return ret;
	}
	rs_device = dev;
	return 0;
}

void rs_dev_destroy(void)
{
	struct rs_dev *dev = rs_device;

	if (!dev)
		return;
	if (atomic_read(&dev->openers)) {
		pr_err("device is open, not destroying\n");
		return;
	}
	rs_device = NULL;
	rs_resv_flush(dev);
	rs_dev_free(dev);
}

struct rs_sync_op {
	struct rs_io io;
	struct completion done;
	int status;
};

static void rs_sync_done(struct rs_io *io, const struct ib_wc *wc)
{
	struct rs_sync_op *s = io->priv;

	s->status = wc->status == IB_WC_SUCCESS ? 0 : -EIO;
	if (wc->status != IB_WC_SUCCESS)
		pr_err("selftest wc %d (%s)\n", wc->status, ib_wc_status_msg(wc->status));
	complete(&s->done);
}

static int rs_sync_once(struct rs_conn *conn, struct page *page, u64 raddr,
			u32 rkey, int opcode, enum dma_data_direction dir)
{
	struct rs_sync_op *s;
	struct rs_io *ios[1];
	dma_addr_t dma;
	int ret;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	init_completion(&s->done);
	s->status = -EIO;
	dma = ib_dma_map_page(rs_ibdev, page, 0, PAGE_SIZE, dir);
	if (ib_dma_mapping_error(rs_ibdev, dma)) {
		kfree(s);
		return -EIO;
	}
	s->io.dma = dma;
	s->io.opcode = opcode;
	s->io.remote_addr = raddr;
	s->io.remote_rkey = rkey;
	s->io.done = rs_sync_done;
	s->io.priv = s;
	atomic_set(&s->io.finished, 0);
	ios[0] = &s->io;
	ret = rs_conn_post(conn, ios, 1, false);
	if (ret) {
		ib_dma_unmap_page(rs_ibdev, dma, PAGE_SIZE, dir);
		kfree(s);
		return ret;
	}
	if (!wait_for_completion_timeout(&s->done, 5 * HZ)) {
		pr_err("selftest timeout, disconnecting\n");
		rdma_disconnect(conn->cm_id);
		if (!wait_for_completion_timeout(&s->done, 2 * HZ)) {
			pr_err("selftest completion lost; leaking its pages\n");
			return -ETIMEDOUT;
		}
	}
	ret = s->status;
	ib_dma_unmap_page(rs_ibdev, dma, PAGE_SIZE, dir);
	kfree(s);
	return ret;
}

static int rs_selftest_at(struct rs_conn *conn, u64 raddr, u32 rkey, u64 off)
{
	struct page *src, *dst;
	u8 *s, *d;
	int i, ret;

	src = alloc_page(GFP_KERNEL);
	dst = alloc_page(GFP_KERNEL);
	if (!src || !dst) {
		if (src)
			__free_page(src);
		if (dst)
			__free_page(dst);
		return -ENOMEM;
	}
	s = page_address(src);
	d = page_address(dst);
	for (i = 0; i < PAGE_SIZE; i++)
		s[i] = (u8)(i + (off >> 12));
	memset(d, 0x5a, PAGE_SIZE);
	ret = rs_sync_once(conn, src, raddr + off, rkey, IB_WR_RDMA_WRITE, DMA_TO_DEVICE);
	if (ret == -ETIMEDOUT)
		return ret;
	if (ret)
		goto out;
	ret = rs_sync_once(conn, dst, raddr + off, rkey, IB_WR_RDMA_READ, DMA_FROM_DEVICE);
	if (ret == -ETIMEDOUT)
		return ret;
	if (ret)
		goto out;
	if (memcmp(s, d, PAGE_SIZE)) {
		pr_err("selftest mismatch at offset 0x%llx\n", off);
		ret = -EIO;
	} else {
		pr_info("selftest offset 0x%llx ok\n", off);
	}
out:
	__free_page(src);
	__free_page(dst);
	return ret;
}

int rs_rdma_selftest(u32 gid)
{
	struct rs_chunk *c;
	struct rs_node *n;
	struct rs_conn *conn;
	u64 offs[3];
	int i, ret;

	c = rs_chunk_lookup(gid);
	if (!c)
		return -ENOENT;
	n = rs_node_get(c->node_id);
	conn = n ? rs_node_conn_up(n, 0) : NULL;
	if (!conn) {
		rs_chunk_put(c);
		return -ENOTCONN;
	}
	offs[0] = 0;
	offs[1] = (c->len / 2) & ~(u64)(PAGE_SIZE - 1);
	offs[2] = c->len - PAGE_SIZE;
	ret = 0;
	for (i = 0; i < 3; i++) {
		ret = rs_selftest_at(conn, c->raddr, c->rkey, offs[i]);
		if (ret)
			break;
	}
	rs_chunk_put(c);
	if (!ret)
		pr_info("selftest OK\n");
	return ret;
}

static int rs_bootstrap(void)
{
	char ip[64];
	const char *colon;
	u16 port = rs_port;
	u64 raddr, len;
	u32 rkey;
	size_t nlen;
	int ret;

	if (!rs_ibdev)
		return -ENODEV;
	colon = strrchr(rs_node_addr, ':');
	if (colon) {
		nlen = colon - rs_node_addr;
		if (!nlen || nlen >= sizeof(ip))
			return -EINVAL;
		memcpy(ip, rs_node_addr, nlen);
		ip[nlen] = '\0';
		if (kstrtou16(colon + 1, 10, &port))
			return -EINVAL;
	} else {
		strlcpy(ip, rs_node_addr, sizeof(ip));
	}
	ret = rs_node_add(rs_node_id, ip, port, rs_epoch);
	if (ret)
		return ret;
	if (rs_parse_u64(rs_raddr_str, &raddr) || rs_parse_u32(rs_rkey_str, &rkey))
		return -EINVAL;
	ret = rs_parse_size(rs_len_str, &len);
	if (ret)
		return ret;
	ret = rs_chunk_add(rs_node_id, rs_gid, rs_epoch, raddr, rkey, len);
	if (ret)
		return ret;
	if (rs_size_str[0] && strcmp(rs_size_str, "0") != 0) {
		u64 bytes;

		ret = rs_parse_size(rs_size_str, &bytes);
		if (ret)
			return ret;
		if (bytes) {
			ret = rs_dev_create(bytes);
			if (ret)
				return ret;
		}
	}
	if (rs_selftest)
		ret = rs_rdma_selftest(rs_gid);
	return ret;
}

#if RS_IB_ADD_RETURNS_INT
static int
#else
static void
#endif
rs_add_device(struct ib_device *dev)
{
	int depth;

	if (rs_ibdev)
		goto out;
	if (rs_rdma_dev[0] && strcmp(dev->name, rs_rdma_dev) != 0)
		goto out;
	rs_pd = ib_alloc_pd(dev, 0);
	if (IS_ERR(rs_pd)) {
		pr_err("ib_alloc_pd(%s): %ld\n", dev->name, PTR_ERR(rs_pd));
		rs_pd = NULL;
		goto out;
	}
	if (!rs_pd->local_dma_lkey) {
		pr_err("%s local_dma_lkey is 0\n", dev->name);
		ib_dealloc_pd(rs_pd);
		rs_pd = NULL;
		goto out;
	}
	rs_ibdev = dev;
	rs_lkey = rs_pd->local_dma_lkey;
	depth = dev->attrs.max_qp_init_rd_atom;
	if (depth < 1)
		depth = 1;
	if (depth > 16)
		depth = 16;
	rs_initiator_depth = depth;
	pr_info("attached %s lkey %u initiator_depth %d max_qp_rd_atom %d\n",
		dev->name, rs_lkey, depth, dev->attrs.max_qp_rd_atom);
out:
#if RS_IB_ADD_RETURNS_INT
	return 0;
#else
	return;
#endif
}

static void rs_remove_device(struct ib_device *dev, void *client_data)
{
	if (dev != rs_ibdev)
		return;
	mutex_lock(&rs_ctl_lock);
	rs_dev_destroy();
	rs_node_shutdown_all();
	if (rs_pd) {
		ib_dealloc_pd(rs_pd);
		rs_pd = NULL;
	}
	rs_ibdev = NULL;
	mutex_unlock(&rs_ctl_lock);
}

static struct ib_client rs_client = {
	.name	= "roswap",
	.add	= rs_add_device,
	.remove	= rs_remove_device,
};

static void rs_shutdown(void)
{
	mutex_lock(&rs_ctl_lock);
	rs_dev_destroy();
	rs_chunks_destroy();
	rs_node_shutdown_all();
	if (rs_pd) {
		ib_dealloc_pd(rs_pd);
		rs_pd = NULL;
	}
	rs_ibdev = NULL;
	mutex_unlock(&rs_ctl_lock);
}

static int __init rs_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct rs_conn_pdata) != 48);
	BUILD_BUG_ON(PAGE_SIZE != 4096);
	rs_globals_init();
	rs_clamp_params();
	ret = rs_parse_uuid(rs_uuid_str);
	if (ret)
		return ret;
	ret = rs_debugfs_init();
	if (ret)
		return ret;
	if (rs_meta_test) {
		ret = rs_meta_selftest();
		if (ret)
			goto out_dbg;
	}
	ret = ib_register_client(&rs_client);
	if (ret)
		goto out_dbg;
	if (rs_node_addr[0]) {
		mutex_lock(&rs_ctl_lock);
		ret = rs_bootstrap();
		mutex_unlock(&rs_ctl_lock);
		if (ret)
			goto out_ib;
	} else if (rs_selftest) {
		pr_err("selftest=1 requires node_addr, raddr and rkey\n");
		ret = -EINVAL;
		goto out_ib;
	}
	return 0;

out_ib:
	rs_shutdown();
	ib_unregister_client(&rs_client);
out_dbg:
	rs_debugfs_exit();
	return ret;
}

static void __exit rs_exit(void)
{
	rs_debugfs_exit();
	rs_shutdown();
	ib_unregister_client(&rs_client);
}

module_init(rs_init);
module_exit(rs_exit);
