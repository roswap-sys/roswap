#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>

#include "roswap_internal.h"

void rs_node_mapped_add(u32 loc, int delta)
{
	struct rs_chunk *c;
	struct rs_node *n;
	u32 gid, idx;

	if (loc == RS_LOC_NONE || !delta)
		return;
	rs_unpack_loc(loc, &gid, &idx);
	rcu_read_lock();
	c = xa_load(&rs_chunks, gid);
	if (c) {
		n = rs_node_get(c->node_id);
		if (n)
			atomic_add(delta, &n->mapped);
	}
	rcu_read_unlock();
}

static void rs_release_loc(struct rs_dev *dev, pgoff_t ext)
{
	u32 loc = dev->extents[ext].loc[0];

	dev->extents[ext].loc[0] = RS_LOC_NONE;
	dev->extents[ext].loc[1] = RS_LOC_NONE;
	if (loc == RS_LOC_NONE)
		return;
	rs_chunk_free_loc(loc);
	rs_node_mapped_add(loc, -1);
}

static void rs_inflight_drop(struct rs_dev *dev, pgoff_t ext)
{
	if (dev->ext_inflight[ext] == 0) {
		WARN_ON_ONCE(1);
		return;
	}
	dev->ext_inflight[ext]--;
	if (dev->ext_inflight[ext] == 0 && dev->ext_live[ext] == 0)
		rs_release_loc(dev, ext);
}

int rs_read_pin(struct rs_dev *dev, pgoff_t slot, u32 *loc, bool *zero)
{
	pgoff_t ext = slot >> 8;
	spinlock_t *lk;
	unsigned long flags;

	*zero = false;
	if (slot >= dev->nr_slots)
		return -EINVAL;
	lk = &dev->ext_locks[ext & (RS_EXT_LOCKS - 1)];
	spin_lock_irqsave(lk, flags);
	if (!(dev->sstate[slot] & RS_S_USED) ||
	    dev->extents[ext].loc[0] == RS_LOC_NONE) {
		spin_unlock_irqrestore(lk, flags);
		*zero = true;
		return 0;
	}
	dev->ext_inflight[ext]++;
	*loc = dev->extents[ext].loc[0];
	spin_unlock_irqrestore(lk, flags);
	return 0;
}

void rs_write_commit(struct rs_dev *dev, pgoff_t slot)
{
	pgoff_t ext = slot >> 8;
	spinlock_t *lk;
	unsigned long flags;

	if (slot >= dev->nr_slots)
		return;
	lk = &dev->ext_locks[ext & (RS_EXT_LOCKS - 1)];
	spin_lock_irqsave(lk, flags);
	if (!(dev->sstate[slot] & RS_S_USED)) {
		dev->sstate[slot] |= RS_S_USED | RS_S_C0;
		dev->ext_live[ext]++;
	}
	rs_inflight_drop(dev, ext);
	spin_unlock_irqrestore(lk, flags);
}

void rs_pin_put(struct rs_dev *dev, pgoff_t slot)
{
	pgoff_t ext = slot >> 8;
	spinlock_t *lk;
	unsigned long flags;

	if (slot >= dev->nr_slots)
		return;
	lk = &dev->ext_locks[ext & (RS_EXT_LOCKS - 1)];
	spin_lock_irqsave(lk, flags);
	rs_inflight_drop(dev, ext);
	spin_unlock_irqrestore(lk, flags);
}

void rs_discard_range(struct rs_dev *dev, pgoff_t first, u32 nr)
{
	pgoff_t slot = first;

	if (first >= dev->nr_slots)
		return;
	if ((u64)first + nr > dev->nr_slots)
		nr = dev->nr_slots - first;

	while (nr) {
		pgoff_t ext = slot >> 8;
		u32 off = slot & (RS_EXTENT_PAGES - 1);
		u32 chunk = RS_EXTENT_PAGES - off;
		spinlock_t *lk = &dev->ext_locks[ext & (RS_EXT_LOCKS - 1)];
		unsigned long flags;
		u32 i;

		if (chunk > nr)
			chunk = nr;
		spin_lock_irqsave(lk, flags);
		for (i = 0; i < chunk; i++) {
			pgoff_t s = slot + i;

			if (dev->sstate[s] & RS_S_USED) {
				dev->sstate[s] = 0;
				if (dev->ext_live[ext])
					dev->ext_live[ext]--;
			}
		}
		if (dev->ext_live[ext] == 0 && dev->ext_inflight[ext] == 0)
			rs_release_loc(dev, ext);
		spin_unlock_irqrestore(lk, flags);
		slot += chunk;
		nr -= chunk;
		atomic64_add(chunk, &dev->discard_pages);
	}
}
