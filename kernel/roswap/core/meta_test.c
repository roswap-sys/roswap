#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>

#include "roswap_internal.h"

#define RS_TN1	250
#define RS_TN2	251
#define RS_TG1	0x3f0001u
#define RS_TG2	0x3f0002u

static int rs_meta_check_free(u32 gid)
{
	struct rs_chunk *c = xa_load(&rs_chunks, gid);

	if (!c || atomic_read(&c->free_ext) != (int)c->nr_ext) {
		pr_err("meta_test gid %u free %d nr %u\n", gid,
		       c ? atomic_read(&c->free_ext) : -1, c ? c->nr_ext : 0);
		return -EIO;
	}
	return 0;
}

int rs_meta_selftest(void)
{
	struct rs_dev *dev = NULL;
	struct rs_node *n1, *n2;
	u32 loc, a = 0, b = 0;
	int ret, i, n_a = 0, n_b = 0;

	rs_globals_init();
	mutex_lock(&rs_ctl_lock);
	ret = rs_dev_alloc(&dev, 256ULL << 20);
	if (ret)
		goto out;
	ret = rs_node_add_local(RS_TN1, 1);
	if (ret)
		goto out;
	ret = rs_node_add_local(RS_TN2, 1);
	if (ret)
		goto out;
	ret = rs_chunk_add(RS_TN1, RS_TG1, 1, 0x100000000ULL, 0x11, 1ULL << 30);
	if (ret)
		goto out;
	ret = rs_chunk_add(RS_TN2, RS_TG2, 1, 0x200000000ULL, 0x22, 1ULL << 30);
	if (ret)
		goto out;

	for (i = 0; i < (int)dev->nr_extents; i++) {
		u32 gid, idx;

		ret = rs_write_pin(dev, (pgoff_t)i << 8, &loc);
		if (ret)
			goto out;
		rs_write_commit(dev, (pgoff_t)i << 8);
		rs_unpack_loc(loc, &gid, &idx);
		if (gid == RS_TG1)
			n_a++;
		else if (gid == RS_TG2)
			n_b++;
	}
	if (n_a < 32 || n_b < 32) {
		pr_err("meta_test placement %d/%d\n", n_a, n_b);
		ret = -EIO;
		goto out;
	}
	ret = rs_write_pin(dev, 0, &a);
	if (ret)
		goto out;
	ret = rs_write_pin(dev, 1, &b);
	if (ret) {
		rs_pin_put(dev, 0);
		goto out;
	}
	if (a != b) {
		pr_err("meta_test same extent got different locs\n");
		ret = -EIO;
	}
	rs_write_commit(dev, 1);
	rs_pin_put(dev, 0);
	if (ret)
		goto out;

	rs_discard_range(dev, 0, (u32)dev->nr_slots);
	rs_resv_flush(dev);
	ret = rs_meta_check_free(RS_TG1);
	if (!ret)
		ret = rs_meta_check_free(RS_TG2);
	n1 = rs_node_get(RS_TN1);
	n2 = rs_node_get(RS_TN2);
	if (!ret && n1 && atomic_read(&n1->mapped) != 0)
		ret = -EIO;
	if (!ret && n2 && atomic_read(&n2->mapped) != 0)
		ret = -EIO;
	if (!ret && rs_resv_cached(dev) != 0)
		ret = -EIO;
	if (!ret)
		pr_info("meta_test OK placement %d/%d\n", n_a, n_b);

out:
	if (ret)
		pr_err("meta_test failed: %d\n", ret);
	if (dev) {
		rs_discard_range(dev, 0, (u32)dev->nr_slots);
		rs_resv_flush(dev);
	}
	rs_chunk_del(RS_TG1);
	rs_chunk_del(RS_TG2);
	rs_node_del(RS_TN1);
	rs_node_del(RS_TN2);
	rs_dev_free(dev);
	mutex_unlock(&rs_ctl_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rs_meta_selftest);
