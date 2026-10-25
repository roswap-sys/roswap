#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/random.h>

#include "roswap_internal.h"

static int rs_resv_pop(struct rs_dev *dev, u16 node_id, u32 *loc)
{
	struct rs_pcpu_resv *r;
	unsigned long flags;
	int cpu, ret = -ENOSPC;

	cpu = get_cpu();
	r = per_cpu_ptr(dev->resv, cpu);
	spin_lock_irqsave(&r->lock, flags);
	if (r->n[node_id] > 0) {
		*loc = r->loc[node_id][--r->n[node_id]];
		ret = 0;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	put_cpu();
	return ret;
}

static int rs_resv_refill(struct rs_dev *dev, u16 node_id)
{
	struct rs_node *n = rs_node_get(node_id);
	struct rs_pcpu_resv *r;
	u32 locs[RS_EXT_RESERVE];
	unsigned long flags;
	int got = 0, cpu, ret;

	if (!n)
		return -ENOENT;
	ret = rs_chunk_alloc_locs(n, RS_EXT_RESERVE, locs, &got);
	if (ret || got <= 0)
		return -ENOSPC;
	cpu = get_cpu();
	r = per_cpu_ptr(dev->resv, cpu);
	spin_lock_irqsave(&r->lock, flags);
	while (got > 0 && r->n[node_id] < RS_EXT_RESERVE)
		r->loc[node_id][r->n[node_id]++] = locs[--got];
	spin_unlock_irqrestore(&r->lock, flags);
	put_cpu();
	while (got > 0)
		rs_chunk_free_loc(locs[--got]);
	return 0;
}

static bool rs_node_usable(struct rs_dev *dev, struct rs_node *n)
{
	struct rs_pcpu_resv *r;

	if (atomic_read(&n->state) != RS_N_ACTIVE)
		return false;
	if (atomic_read(&n->free_ext) > 0)
		return true;
	r = raw_cpu_ptr(dev->resv);
	return READ_ONCE(r->n[n->id]) > 0;
}

static u16 rs_pick_node(struct rs_dev *dev)
{
	u16 ids[RS_MAX_NODES];
	u16 a, b;
	int n = 0, i, fa, fb;
	u32 r1, r2;
	struct rs_node *na, *nb;

	for (i = 1; i < RS_MAX_NODES; i++) {
		struct rs_node *node = rs_node_get(i);

		if (node && rs_node_usable(dev, node))
			ids[n++] = node->id;
	}
	if (!n)
		return 0;
	if (n == 1)
		return ids[0];
	r1 = prandom_u32();
	r2 = prandom_u32();
	a = ids[r1 % n];
	b = ids[r2 % n];
	if (a == b)
		return a;
	na = rs_node_get(a);
	nb = rs_node_get(b);
	fa = na ? atomic_read(&na->free_ext) : 0;
	fb = nb ? atomic_read(&nb->free_ext) : 0;
	return fa >= fb ? a : b;
}

static int rs_take_loc(struct rs_dev *dev, u32 *loc)
{
	u16 node;
	int tries;

	/* A chosen node can run dry between selection and refill. */
	for (tries = 0; tries < 4; tries++) {
		node = rs_pick_node(dev);
		if (!node)
			return -ENOSPC;
		if (!rs_resv_pop(dev, node, loc))
			return 0;
		if (!rs_resv_refill(dev, node) && !rs_resv_pop(dev, node, loc))
			return 0;
	}
	return -ENOSPC;
}

int rs_write_pin(struct rs_dev *dev, pgoff_t slot, u32 *loc_out)
{
	pgoff_t ext = slot >> 8;
	spinlock_t *lk;
	unsigned long flags;
	u32 fresh = RS_LOC_NONE;
	int ret;

	if (slot >= dev->nr_slots)
		return -EINVAL;
	lk = &dev->ext_locks[ext & (RS_EXT_LOCKS - 1)];

	spin_lock_irqsave(lk, flags);
	if (dev->extents[ext].loc[0] != RS_LOC_NONE) {
		dev->ext_inflight[ext]++;
		*loc_out = dev->extents[ext].loc[0];
		spin_unlock_irqrestore(lk, flags);
		return 0;
	}
	spin_unlock_irqrestore(lk, flags);

	ret = rs_take_loc(dev, &fresh);
	if (ret)
		return ret;

	spin_lock_irqsave(lk, flags);
	if (dev->extents[ext].loc[0] != RS_LOC_NONE) {
		*loc_out = dev->extents[ext].loc[0];
		dev->ext_inflight[ext]++;
		spin_unlock_irqrestore(lk, flags);
		rs_chunk_free_loc(fresh);
		return 0;
	}
	dev->extents[ext].loc[0] = fresh;
	dev->extents[ext].loc[1] = RS_LOC_NONE;
	dev->ext_inflight[ext]++;
	spin_unlock_irqrestore(lk, flags);
	rs_node_mapped_add(fresh, 1);
	*loc_out = fresh;
	return 0;
}

void rs_resv_flush(struct rs_dev *dev)
{
	int cpu, node;

	if (!dev || !dev->resv)
		return;
	for_each_possible_cpu(cpu) {
		struct rs_pcpu_resv *r = per_cpu_ptr(dev->resv, cpu);
		u32 tmp[RS_EXT_RESERVE];
		unsigned long flags;
		int n, i;

		for (node = 1; node < RS_MAX_NODES; node++) {
			spin_lock_irqsave(&r->lock, flags);
			n = r->n[node];
			for (i = 0; i < n; i++)
				tmp[i] = r->loc[node][i];
			r->n[node] = 0;
			spin_unlock_irqrestore(&r->lock, flags);
			for (i = 0; i < n; i++)
				rs_chunk_free_loc(tmp[i]);
		}
	}
}

unsigned int rs_resv_cached(struct rs_dev *dev)
{
	unsigned int total = 0;
	int cpu, node;

	if (!dev || !dev->resv)
		return 0;
	for_each_possible_cpu(cpu) {
		struct rs_pcpu_resv *r = per_cpu_ptr(dev->resv, cpu);
		unsigned long flags;

		spin_lock_irqsave(&r->lock, flags);
		for (node = 1; node < RS_MAX_NODES; node++)
			total += r->n[node];
		spin_unlock_irqrestore(&r->lock, flags);
	}
	return total;
}
