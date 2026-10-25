#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>

#include "roswap_internal.h"

struct xarray rs_chunks;

static void rs_chunk_release(struct percpu_ref *ref)
{
	struct rs_chunk *c = container_of(ref, struct rs_chunk, ioref);

	complete(&c->dead);
}

int rs_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 raddr, u32 rkey, u64 len)
{
	struct rs_node *n;
	struct rs_chunk *c;
	unsigned int nr_ext;
	unsigned long flags;
	int ret;

	if (gid == 0 || gid > RS_GID_MAX)
		return -EINVAL;
	nr_ext = len >> 20;
	if (nr_ext == 0 || nr_ext > RS_CHUNK_EXTENTS)
		return -EINVAL;
	n = rs_node_get(node_id);
	if (!n)
		return -ENOENT;
	if (n->epoch != epoch)
		return -EINVAL;
	if (xa_load(&rs_chunks, gid))
		return -EEXIST;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->gid = gid;
	c->node_id = node_id;
	c->node_epoch = epoch;
	c->nr_ext = nr_ext;
	c->raddr = raddr;
	c->rkey = rkey;
	c->len = (u64)nr_ext << 20;
	atomic_set(&c->free_ext, nr_ext);
	spin_lock_init(&c->lock);
	INIT_LIST_HEAD(&c->node_link);
	init_completion(&c->dead);
	ret = percpu_ref_init(&c->ioref, rs_chunk_release, 0, GFP_KERNEL);
	if (ret) {
		kfree(c);
		return ret;
	}
	ret = xa_err(xa_store(&rs_chunks, gid, c, GFP_KERNEL));
	if (ret) {
		percpu_ref_exit(&c->ioref);
		kfree(c);
		return ret;
	}
	spin_lock_irqsave(&n->lock, flags);
	list_add_tail(&c->node_link, &n->chunks);
	n->nr_chunks++;
	atomic_add(nr_ext, &n->free_ext);
	spin_unlock_irqrestore(&n->lock, flags);
	pr_info("chunk gid %u node %u raddr 0x%llx rkey 0x%x extents %u\n",
		gid, node_id, raddr, rkey, nr_ext);
	return 0;
}

static void rs_chunk_kill(struct rs_chunk *c)
{
	struct rs_node *n = rs_node_get(c->node_id);
	unsigned long flags, flags2;

	if (n) {
		spin_lock_irqsave(&n->lock, flags);
		spin_lock_irqsave(&c->lock, flags2);
		list_del_init(&c->node_link);
		n->nr_chunks--;
		atomic_sub(atomic_read(&c->free_ext), &n->free_ext);
		atomic_set(&c->free_ext, 0);
		spin_unlock_irqrestore(&c->lock, flags2);
		spin_unlock_irqrestore(&n->lock, flags);
	}
	percpu_ref_kill(&c->ioref);
	wait_for_completion(&c->dead);
	xa_erase(&rs_chunks, c->gid);
	synchronize_rcu();
	percpu_ref_exit(&c->ioref);
	kfree(c);
}

void rs_chunk_del(u32 gid)
{
	struct rs_chunk *c;

	c = xa_load(&rs_chunks, gid);
	if (!c)
		return;
	rs_chunk_kill(c);
}

void rs_chunks_destroy(void)
{
	struct rs_chunk *c;
	unsigned long gid;

	gid = 0;
	while ((c = xa_find(&rs_chunks, &gid, ULONG_MAX, XA_PRESENT)) != NULL)
		rs_chunk_kill(c);
}

struct rs_chunk *rs_chunk_lookup(u32 gid)
{
	struct rs_chunk *c;

	rcu_read_lock();
	c = xa_load(&rs_chunks, gid);
	if (c && !percpu_ref_tryget_live(&c->ioref))
		c = NULL;
	rcu_read_unlock();
	return c;
}

void rs_chunk_put(struct rs_chunk *c)
{
	if (c)
		percpu_ref_put(&c->ioref);
}

int rs_chunk_alloc_locs(struct rs_node *n, int want, u32 *locs, int *got)
{
	struct rs_chunk *c;
	unsigned long flags, flags2;
	int nloc = 0;

	if (!n || want <= 0)
		return -EINVAL;
	spin_lock_irqsave(&n->lock, flags);
	list_for_each_entry(c, &n->chunks, node_link) {
		unsigned int bit = 0;

		spin_lock_irqsave(&c->lock, flags2);
		while (nloc < want) {
			bit = find_next_zero_bit(c->ext_used, c->nr_ext, bit);
			if (bit >= c->nr_ext)
				break;
			__set_bit(bit, c->ext_used);
			locs[nloc++] = rs_pack_loc(c->gid, bit);
			atomic_dec(&c->free_ext);
			atomic_dec(&n->free_ext);
			bit++;
		}
		spin_unlock_irqrestore(&c->lock, flags2);
		if (nloc >= want)
			break;
	}
	spin_unlock_irqrestore(&n->lock, flags);
	*got = nloc;
	return nloc ? 0 : -ENOSPC;
}

void rs_chunk_free_loc(u32 loc)
{
	struct rs_chunk *c;
	struct rs_node *n;
	u32 gid, idx;
	unsigned long flags;

	if (loc == RS_LOC_NONE)
		return;
	rs_unpack_loc(loc, &gid, &idx);
	rcu_read_lock();
	c = xa_load(&rs_chunks, gid);
	if (!c) {
		rcu_read_unlock();
		return;
	}
	spin_lock_irqsave(&c->lock, flags);
	if (idx < c->nr_ext && test_bit(idx, c->ext_used)) {
		__clear_bit(idx, c->ext_used);
		c->owner[idx] = 0;
		/* A chunk off its node list is dying and no longer counted. */
		if (!list_empty(&c->node_link)) {
			atomic_inc(&c->free_ext);
			n = rs_node_get(c->node_id);
			if (n)
				atomic_inc(&n->free_ext);
		}
	}
	spin_unlock_irqrestore(&c->lock, flags);
	rcu_read_unlock();
}

void rs_chunk_set_owner(struct rs_chunk *c, u32 ext_idx, u32 extent)
{
	unsigned long flags;

	if (!c || ext_idx >= c->nr_ext)
		return;
	spin_lock_irqsave(&c->lock, flags);
	c->owner[ext_idx] = extent;
	spin_unlock_irqrestore(&c->lock, flags);
}
