// SPDX-License-Identifier: GPL-2.0
#ifdef RS_MAP_TEST
#include "map_user.h"
#else
#define pr_fmt(fmt) "RoSwap: " fmt
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/random.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#endif

#include "map.h"

#define RS_PAGE_SHIFT		12
#define RS_PAGE_SIZE		(1u << RS_PAGE_SHIFT)
#define RS_EXTENT_SHIFT		20
#define RS_EXTENT_PAGES		(1u << (RS_EXTENT_SHIFT - RS_PAGE_SHIFT))
#define RS_LOC_NONE		0xffffffffu
#define RS_CPU_RESERVE		8

struct rs_extent {
	u32 loc0;
	u32 loc1;
	u16 used;
	u16 pad;
};

struct rs_chunk {
	bool active;
	u16 node_id;
	u32 epoch;
	u32 gen;
	u64 base;
	u32 rkey;
	u32 nr_ext;
	u32 free_ext;
	unsigned long *bm;
};

struct rs_node {
	bool active;
	u16 id;
	u32 weight;
	u32 rtt_us;
	u32 free_ext;
};

struct rs_reserve {
	u32 loc[RS_CPU_RESERVE];
	u8 n;
};

/* 这一位只在 map.c 里用：该页已经算进 degraded_slots。 */
#define RS_S_SHORT		0x80

struct rs_map {
	u32 nr_slots;
	u32 nr_extents;
	u32 used_slots;
	u32 degraded_slots;
	u8 prot;
	u8 *sstate;
	struct rs_extent *extents;
	struct rs_chunk chunks[RS_MAX_CHUNKS];
	struct rs_node nodes[RS_MAX_NODES];
	spinlock_t lock;
};

/*
 * 副本完成状态字。bit0、bit1 是已到达，bit4、bit5 是成功。
 * 输入旧字和这一份的结果，输出新字。重复到达时原样返回。
 * 不修改调用方以外的任何状态。
 */
u32 rs_copy_note(u32 word, u32 which, bool success)
{
	u32 arrived;

	if (which >= RS_NR_COPIES)
		return word;
	arrived = 1u << which;
	if (word & arrived)
		return word;
	word |= arrived;
	if (success)
		word |= arrived << 4;
	return word;
}

bool rs_copy_settled(u32 word, u8 sent_mask)
{
	sent_mask &= (u8)((1u << RS_NR_COPIES) - 1);
	return ((u8)word & sent_mask) == sent_mask;
}

u8 rs_copy_ok(u32 word)
{
	return (u8)((word >> 4) & ((1u << RS_NR_COPIES) - 1));
}

static struct rs_map *rs_map;

#ifdef RS_MAP_TEST
static struct rs_reserve rs_reserve;
static u32 (*rs_random_fn)(void);

void rs_map_set_random(u32 (*fn)(void))
{
	rs_random_fn = fn;
}

void rs_map_test_set_gen(u32 gid, u32 gen)
{
	if (!rs_map || gid >= RS_MAX_CHUNKS || !rs_map->chunks[gid].active)
		return;
	rs_map->chunks[gid].gen = gen;
}

static struct rs_reserve *rs_reserve_this(void)
{
	return &rs_reserve;
}

static u32 rs_random(void)
{
	if (rs_random_fn)
		return rs_random_fn();
	return (u32)rand();
}

static u32 rs_reserve_held(void)
{
	return rs_reserve.n;
}

static void rs_reserve_reset(void)
{
	rs_reserve.n = 0;
}
#else
static DEFINE_PER_CPU(struct rs_reserve, rs_reserve);

static struct rs_reserve *rs_reserve_this(void)
{
	return this_cpu_ptr(&rs_reserve);
}

static u32 rs_random(void)
{
	return get_random_u32();
}

static u32 rs_reserve_held(void)
{
	u32 n = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		n += per_cpu(rs_reserve, cpu).n;
	return n;
}

static void rs_reserve_reset(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(rs_reserve, cpu).n = 0;
}
#endif

static bool rs_bit(const unsigned long *bm, u32 i)
{
	return (bm[i / BITS_PER_LONG] >> (i % BITS_PER_LONG)) & 1UL;
}

static void rs_bit_set(unsigned long *bm, u32 i)
{
	bm[i / BITS_PER_LONG] |= 1UL << (i % BITS_PER_LONG);
}

static void rs_bit_clear(unsigned long *bm, u32 i)
{
	bm[i / BITS_PER_LONG] &= ~(1UL << (i % BITS_PER_LONG));
}

static struct rs_node *rs_node(struct rs_map *m, u16 id)
{
	u32 i;

	for (i = 0; i < RS_MAX_NODES; i++) {
		if (m->nodes[i].active && m->nodes[i].id == id)
			return &m->nodes[i];
	}
	return NULL;
}

static u32 rs_score(const struct rs_node *n)
{
	u32 rtt = n->rtt_us ? n->rtt_us : 1;

	return n->free_ext * n->weight / rtt;
}

/* 两个候选里留容量更大的。只有一个 ACTIVE 节点时退化为那一个。 */
static struct rs_node *rs_pick_node(struct rs_map *m)
{
	struct rs_node *cands[RS_MAX_NODES];
	u32 n = 0;
	u32 i, a, b;

	for (i = 0; i < RS_MAX_NODES; i++) {
		if (!m->nodes[i].active || !m->nodes[i].free_ext)
			continue;
		cands[n++] = &m->nodes[i];
	}
	if (!n)
		return NULL;
	if (n == 1)
		return cands[0];
	a = rs_random() % n;
	b = rs_random() % n;
	return rs_score(cands[a]) >= rs_score(cands[b]) ? cands[a] : cands[b];
}

static int rs_claim(struct rs_map *m, struct rs_node *node, u32 *loc)
{
	u32 gid, ext;

	/* 调用方关着中断，整字跳过已满的位图，压住最坏扫描长度。 */
	for (gid = 0; gid < RS_MAX_CHUNKS; gid++) {
		struct rs_chunk *c = &m->chunks[gid];

		if (!c->active || c->node_id != node->id || !c->free_ext)
			continue;
		for (ext = 0; ext < c->nr_ext; ext++) {
			if (!(ext % BITS_PER_LONG) &&
			    c->bm[ext / BITS_PER_LONG] == ~0UL) {
				ext += BITS_PER_LONG - 1;
				continue;
			}
			if (rs_bit(c->bm, ext))
				continue;
			rs_bit_set(c->bm, ext);
			c->free_ext--;
			node->free_ext--;
			*loc = (gid << RS_EXT_IDX_BITS) | ext;
			return 0;
		}
	}
	return -ENOSPC;
}

static void rs_refill(struct rs_map *m)
{
	struct rs_reserve *r = rs_reserve_this();

	while (r->n < RS_CPU_RESERVE) {
		struct rs_node *node = rs_pick_node(m);
		u32 loc;

		if (!node || rs_claim(m, node, &loc))
			return;
		r->loc[r->n++] = loc;
	}
}

static int rs_pop_loc(struct rs_map *m, u32 *loc)
{
	struct rs_reserve *r = rs_reserve_this();

	if (!r->n)
		rs_refill(m);
	if (!r->n)
		return -ENOSPC;
	*loc = r->loc[--r->n];
	return 0;
}

static u16 rs_loc_node(struct rs_map *m, u32 loc)
{
	u32 gid = loc >> RS_EXT_IDX_BITS;

	if (gid >= RS_MAX_CHUNKS || !m->chunks[gid].active)
		return (u16)RS_MAX_NODES;
	return m->chunks[gid].node_id;
}

static int rs_claim_other(struct rs_map *m, u16 exclude, u32 *loc)
{
	struct rs_node *cands[RS_MAX_NODES];
	struct rs_node *pick;
	u32 n = 0;
	u32 i, a, b;

	for (i = 0; i < RS_MAX_NODES; i++) {
		if (!m->nodes[i].active || !m->nodes[i].free_ext)
			continue;
		if (m->nodes[i].id == exclude)
			continue;
		cands[n++] = &m->nodes[i];
	}
	if (!n)
		return -ENOSPC;
	pick = cands[0];
	if (n > 1) {
		a = rs_random() % n;
		b = rs_random() % n;
		if (rs_score(cands[b]) > rs_score(cands[a]))
			pick = cands[b];
		else
			pick = cands[a];
	}
	return rs_claim(m, pick, loc);
}

/*
 * loc0 可能已经把另一台机器的 extent 放进本 CPU 预留。
 * 第二份先从预留里拿不同节点的，再去空闲位图里领。
 */
static int rs_take_other(struct rs_map *m, u16 exclude, u32 *loc)
{
	struct rs_reserve *r = rs_reserve_this();
	u8 i;

	for (i = 0; i < r->n; i++) {
		if (rs_loc_node(m, r->loc[i]) == exclude)
			continue;
		*loc = r->loc[i];
		r->loc[i] = r->loc[--r->n];
		return 0;
	}
	return rs_claim_other(m, exclude, loc);
}

static void rs_release_loc(struct rs_map *m, u32 loc)
{
	struct rs_reserve *r = rs_reserve_this();
	u32 gid = loc >> RS_EXT_IDX_BITS;
	u32 ext = loc & (RS_MAX_EXT_PER_CHUNK - 1);
	struct rs_chunk *c;
	struct rs_node *node;

	if (gid >= RS_MAX_CHUNKS)
		return;
	c = &m->chunks[gid];
	if (r->n < RS_CPU_RESERVE) {
		r->loc[r->n++] = loc;
		return;
	}
	if (ext < c->nr_ext)
		rs_bit_clear(c->bm, ext);
	if (c->free_ext < c->nr_ext)
		c->free_ext++;
	node = rs_node(m, c->node_id);
	if (node)
		node->free_ext++;
}

static int rs_slot(struct rs_map *m, u64 off, u32 len, u32 *slot)
{
	u64 end;

	if (!len || (off & (RS_PAGE_SIZE - 1)) || (len & (RS_PAGE_SIZE - 1)))
		return -EINVAL;
	if (off / RS_PAGE_SIZE > 0xffffffffu)
		return -EINVAL;
	end = off + len;
	if (end < off || end > (u64)m->nr_slots * RS_PAGE_SIZE)
		return -EINVAL;
	*slot = off >> RS_PAGE_SHIFT;
	return 0;
}

static int rs_fill(struct rs_map *m, u32 loc, u32 slot, struct rs_remote *out)
{
	u32 gid = loc >> RS_EXT_IDX_BITS;
	u32 ext = loc & (RS_MAX_EXT_PER_CHUNK - 1);
	u32 pg = slot & (RS_EXTENT_PAGES - 1);
	struct rs_chunk *c;

	if (gid >= RS_MAX_CHUNKS)
		return -EIO;
	c = &m->chunks[gid];
	if (!c->active || ext >= c->nr_ext)
		return -EIO;
	out->node_id = c->node_id;
	out->gid = gid;
	out->gen = c->gen;
	out->rkey = c->rkey;
	out->addr = c->base + ((u64)ext << RS_EXTENT_SHIFT) +
		    ((u64)pg << RS_PAGE_SHIFT);
	return 0;
}

static u8 rs_bound_mask(const struct rs_extent *ext)
{
	u8 bound = 0;

	if (ext->loc0 != RS_LOC_NONE)
		bound |= RS_S_C0;
	if (ext->loc1 != RS_LOC_NONE)
		bound |= RS_S_C1;
	return bound;
}

static u8 rs_wanted_mask(const struct rs_map *m, u8 bound)
{
	if (m->prot == RS_PROT_REPLICA2)
		return RS_S_C0 | RS_S_C1;
	return bound & RS_S_C0;
}

static int rs_bind_extent(struct rs_map *m, struct rs_extent *ext)
{
	u16 node;
	int ret;

	if (ext->loc0 != RS_LOC_NONE)
		return 0;
	ret = rs_pop_loc(m, &ext->loc0);
	if (ret)
		return ret;
	if (m->prot != RS_PROT_REPLICA2)
		return 0;
	node = rs_loc_node(m, ext->loc0);
	if (rs_take_other(m, node, &ext->loc1))
		ext->loc1 = RS_LOC_NONE;
	return 0;
}

static int rs_emit_copies(struct rs_map *m, struct rs_extent *ext, u32 slot,
			  u8 bits, struct rs_copies *out)
{
	u32 locs[RS_NR_COPIES];
	u32 i;
	u8 mask = 0;

	locs[0] = ext->loc0;
	locs[1] = ext->loc1;
	memset(out, 0, sizeof(*out));
	for (i = 0; i < RS_NR_COPIES; i++) {
		if (!(bits & (1u << i)) || locs[i] == RS_LOC_NONE)
			continue;
		if (rs_fill(m, locs[i], slot, &out->r[i]))
			return -EIO;
		mask |= (u8)(1u << i);
	}
	out->mask = mask;
	return 0;
}

int rs_map_init(u32 capacity_mb)
{
	struct rs_map *m;
	u32 i;

	if (rs_map)
		return -EBUSY;
	if (!capacity_mb || capacity_mb > (1u << 20))
		return -EINVAL;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->nr_extents = capacity_mb;
	m->nr_slots = capacity_mb << (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	m->sstate = kvzalloc(m->nr_slots, GFP_KERNEL);
	m->extents = kvzalloc(sizeof(*m->extents) * m->nr_extents, GFP_KERNEL);
	if (!m->sstate || !m->extents) {
		kvfree(m->sstate);
		kvfree(m->extents);
		kfree(m);
		return -ENOMEM;
	}
	for (i = 0; i < m->nr_extents; i++) {
		m->extents[i].loc0 = RS_LOC_NONE;
		m->extents[i].loc1 = RS_LOC_NONE;
	}
	spin_lock_init(&m->lock);
	rs_reserve_reset();
	rs_map = m;
	return 0;
}

void rs_map_exit(void)
{
	u32 i;

	if (!rs_map)
		return;
	for (i = 0; i < RS_MAX_CHUNKS; i++)
		kfree(rs_map->chunks[i].bm);
	kvfree(rs_map->sstate);
	kvfree(rs_map->extents);
	kfree(rs_map);
	rs_map = NULL;
	rs_reserve_reset();
}

int rs_map_node_add(u16 node_id)
{
	struct rs_map *m = rs_map;
	u32 i, free_slot = RS_MAX_NODES;
	unsigned long flags;
	int ret = 0;

	if (!m)
		return -ENODEV;
	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < RS_MAX_NODES; i++) {
		if (m->nodes[i].active && m->nodes[i].id == node_id) {
			ret = 0;
			goto out;
		}
		if (!m->nodes[i].active && free_slot == RS_MAX_NODES)
			free_slot = i;
	}
	if (free_slot == RS_MAX_NODES) {
		ret = -ENOSPC;
		goto out;
	}
	m->nodes[free_slot].active = true;
	m->nodes[free_slot].id = node_id;
	m->nodes[free_slot].weight = 1;
	m->nodes[free_slot].rtt_us = 1;
	m->nodes[free_slot].free_ext = 0;
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

static unsigned long *rs_bitmap(u32 nr_ext)
{
	size_t bytes = ((nr_ext + BITS_PER_LONG - 1) / BITS_PER_LONG) *
		       sizeof(unsigned long);

	return kzalloc(bytes, GFP_KERNEL);
}

int rs_map_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 raddr, u32 rkey,
		     u32 chunk_mb)
{
	struct rs_map *m = rs_map;
	struct rs_chunk *c;
	struct rs_node *node;
	unsigned long *bm;
	unsigned long flags;
	int ret = 0;

	if (!m)
		return -ENODEV;
	if (gid >= RS_MAX_CHUNKS || !chunk_mb || chunk_mb > RS_MAX_EXT_PER_CHUNK)
		return -EINVAL;
	bm = rs_bitmap(chunk_mb);
	if (!bm)
		return -ENOMEM;
	spin_lock_irqsave(&m->lock, flags);
	node = rs_node(m, node_id);
	if (!node) {
		ret = -ENOENT;
		goto out;
	}
	c = &m->chunks[gid];
	if (c->active) {
		if (c->node_id == node_id && c->epoch == epoch &&
		    c->base == raddr && c->rkey == rkey && c->nr_ext == chunk_mb)
			ret = 0;
		else
			ret = -EEXIST;
		goto out;
	}
	c->active = true;
	c->node_id = node_id;
	c->epoch = epoch;
	c->gen = 1;
	c->base = raddr;
	c->rkey = rkey;
	c->nr_ext = chunk_mb;
	c->free_ext = chunk_mb;
	c->bm = bm;
	bm = NULL;
	node->free_ext += chunk_mb;
out:
	spin_unlock_irqrestore(&m->lock, flags);
	kfree(bm);
	return ret;
}

int rs_map_chunk_update(u32 gid, u32 epoch, u64 raddr, u32 rkey)
{
	struct rs_map *m = rs_map;
	struct rs_chunk *c;
	unsigned long flags;
	int changed = 0;

	if (!m)
		return -ENODEV;
	if (gid >= RS_MAX_CHUNKS)
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	c = &m->chunks[gid];
	if (!c->active) {
		changed = -ENOENT;
		goto out;
	}
	if (c->rkey != rkey || c->base != raddr || c->epoch != epoch) {
		c->rkey = rkey;
		c->base = raddr;
		c->epoch = epoch;
		c->gen++;
		if (!c->gen)
			c->gen = 1;
		changed = 1;
	}
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return changed;
}

static int rs_activate(struct rs_map *m, u32 slot, struct rs_copies *out)
{
	u32 ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	struct rs_extent *ext = &m->extents[ext_i];
	u8 state = m->sstate[slot];
	int ret;

	if (state & RS_S_BUSY)
		return -EBUSY;
	ret = rs_bind_extent(m, ext);
	if (ret)
		return ret;
	ret = rs_emit_copies(m, ext, slot, rs_bound_mask(ext), out);
	if (ret)
		return ret;
	if (!(state & RS_S_USED)) {
		m->used_slots++;
		ext->used++;
	}
	m->sstate[slot] = (state & (RS_S_C0 | RS_S_C1 | RS_S_SHORT | RS_S_USED)) |
			  RS_S_USED | RS_S_BUSY;
	return 0;
}

int rs_map_set_protection(u32 mode)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	int ret = 0;

	if (mode != RS_PROT_SINGLE && mode != RS_PROT_REPLICA2)
		return -EINVAL;
	if (!m)
		return -ENODEV;
	spin_lock_irqsave(&m->lock, flags);
	if (m->used_slots)
		ret = -EBUSY;
	else
		m->prot = (u8)mode;
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

u32 rs_map_protection(void)
{
	struct rs_map *m = rs_map;

	return m ? m->prot : RS_PROT_SINGLE;
}

int rs_map_prepare_write(u64 off, u32 len, struct rs_copies *out)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	u32 slot;
	int ret;

	if (!m || !out || len != RS_PAGE_SIZE)
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	ret = rs_slot(m, off, len, &slot);
	if (!ret)
		ret = rs_activate(m, slot, out);
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

int rs_map_lookup_read(u64 off, u32 len, struct rs_copies *out)
{
	struct rs_map *m = rs_map;
	u32 slot, ext_i;
	struct rs_extent *ext;
	unsigned long flags;
	u8 state;
	int ret;

	if (!m || !out || len != RS_PAGE_SIZE)
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	ret = rs_slot(m, off, len, &slot);
	if (ret)
		goto out;
	state = m->sstate[slot];
	if (!(state & RS_S_USED) || (state & RS_S_ZERO)) {
		ret = RS_MAP_ZERO;
		goto out;
	}
	if (state & RS_S_LOST) {
		ret = -EIO;
		goto out;
	}
	if (state & RS_S_BUSY) {
		ret = -EBUSY;
		goto out;
	}
	ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	ext = &m->extents[ext_i];
	ret = rs_emit_copies(m, ext, slot, state & (RS_S_C0 | RS_S_C1), out);
	if (ret)
		goto out;
	if (!out->mask) {
		ret = -EIO;
		goto out;
	}
	m->sstate[slot] = state | RS_S_BUSY;
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

int rs_map_discard(u64 off, u32 len)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	u32 slot, n, i;
	int ret;

	if (!m)
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	ret = rs_slot(m, off, len, &slot);
	if (ret)
		goto out;
	n = len >> RS_PAGE_SHIFT;
	for (i = 0; i < n; i++) {
		if (m->sstate[slot + i] & RS_S_BUSY) {
			ret = -EBUSY;
			goto out;
		}
	}
	for (i = 0; i < n; i++) {
		u32 ext_i = (slot + i) >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
		struct rs_extent *ext = &m->extents[ext_i];
		u8 state = m->sstate[slot + i];

		if (!(state & RS_S_USED))
			continue;
		if ((state & RS_S_SHORT) && m->degraded_slots)
			m->degraded_slots--;
		m->sstate[slot + i] = RS_S_ZERO;
		if (m->used_slots)
			m->used_slots--;
		if (ext->used)
			ext->used--;
		if (!ext->used) {
			if (ext->loc0 != RS_LOC_NONE)
				rs_release_loc(m, ext->loc0);
			if (ext->loc1 != RS_LOC_NONE)
				rs_release_loc(m, ext->loc1);
			ext->loc0 = RS_LOC_NONE;
			ext->loc1 = RS_LOC_NONE;
		}
	}
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

void rs_map_finish(u64 off)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	u32 slot;

	if (!m || (off & (RS_PAGE_SIZE - 1)))
		return;
	slot = off >> RS_PAGE_SHIFT;
	if (slot >= m->nr_slots)
		return;
	spin_lock_irqsave(&m->lock, flags);
	m->sstate[slot] &= (u8)~RS_S_BUSY;
	spin_unlock_irqrestore(&m->lock, flags);
}

/* 输入 ok_mask 的 bit0/bit1。有效副本改成其中已绑定的那些，并清 BUSY。 */
void rs_map_finish_write(u64 off, u8 ok_mask)
{
	struct rs_map *m = rs_map;
	struct rs_extent *ext;
	unsigned long flags;
	u32 slot, ext_i;
	u8 state, bound, wanted, valid;
	bool short_copy;

	if (!m || (off & (RS_PAGE_SIZE - 1)))
		return;
	slot = off >> RS_PAGE_SHIFT;
	if (slot >= m->nr_slots)
		return;
	spin_lock_irqsave(&m->lock, flags);
	state = m->sstate[slot];
	if (!(state & RS_S_USED)) {
		m->sstate[slot] = state & (u8)~RS_S_BUSY;
		goto out;
	}
	ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	ext = &m->extents[ext_i];
	bound = rs_bound_mask(ext);
	wanted = rs_wanted_mask(m, bound);
	valid = ok_mask & bound & (RS_S_C0 | RS_S_C1);
	short_copy = valid != wanted;
	if (short_copy && !(state & RS_S_SHORT))
		m->degraded_slots++;
	else if (!short_copy && (state & RS_S_SHORT) && m->degraded_slots)
		m->degraded_slots--;
	m->sstate[slot] = RS_S_USED | valid | (short_copy ? RS_S_SHORT : 0);
out:
	spin_unlock_irqrestore(&m->lock, flags);
}

u32 rs_map_chunk_gen(u32 gid)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	u32 gen = 0;

	if (!m || gid >= RS_MAX_CHUNKS)
		return 0;
	spin_lock_irqsave(&m->lock, flags);
	if (m->chunks[gid].active)
		gen = m->chunks[gid].gen;
	spin_unlock_irqrestore(&m->lock, flags);
	return gen;
}

int rs_map_chunk_node(u32 gid, u16 *node_id)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	int ret = -ENOENT;

	if (!m || gid >= RS_MAX_CHUNKS || !node_id)
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	if (m->chunks[gid].active) {
		*node_id = m->chunks[gid].node_id;
		ret = 0;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

void rs_map_stats(struct rs_map_stats *out)
{
	struct rs_map *m = rs_map;
	u32 i, chunks = 0, nodes = 0, free_ext = 0;
	unsigned long flags;

	memset(out, 0, sizeof(*out));
	if (!m)
		return;
	spin_lock_irqsave(&m->lock, flags);
	out->nr_slots = m->nr_slots;
	out->used_slots = m->used_slots;
	for (i = 0; i < RS_MAX_CHUNKS; i++) {
		if (!m->chunks[i].active)
			continue;
		chunks++;
		free_ext += m->chunks[i].free_ext;
	}
	for (i = 0; i < RS_MAX_NODES; i++) {
		if (m->nodes[i].active)
			nodes++;
	}
	out->nr_chunks = chunks;
	out->nr_nodes = nodes;
	out->free_extents = free_ext + rs_reserve_held();
	out->degraded_slots = m->degraded_slots;
	spin_unlock_irqrestore(&m->lock, flags);
}
