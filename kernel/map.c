// SPDX-License-Identifier: GPL-2.0
#ifdef RS_MAP_TEST
#include "map_user.h"
#else
#define pr_fmt(fmt) "RoSwap: " fmt
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/percpu_counter.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

#endif

#include "map.h"

/*
 * 锁顺序：extent 哈希锁（一次只拿一把）> prot_lock
 * > 本 CPU 预留锁 > 节点锁。查找和完成只 cmpxchg 页状态，不拿这些锁。
 * I/O 完成回调跑在软中断里，所以这里的锁都是关中断的自旋锁。
 */

#define RS_PAGE_SHIFT		12
#define RS_PAGE_SIZE		(1u << RS_PAGE_SHIFT)
#define RS_EXTENT_SHIFT		20
#define RS_EXTENT_PAGES		(1u << (RS_EXTENT_SHIFT - RS_PAGE_SHIFT))
#define RS_LOC_NONE		0xffffffffu
#define RS_CPU_RESERVE		8
#define RS_EXT_LOCKS		4096
#define RS_DISCARD_STACK	64

/* rcu 必须是第一个成员，用户态测试按这个地址释放。 */
struct rs_chunk_key {
	struct rcu_head rcu;
	u32 epoch;
	u32 gen;
	u32 rkey;
	u64 base;
};

struct rs_node {
	u16 id;
	u32 weight;
	u32 rtt_us;
	atomic_t free_ext;
	spinlock_t lock;
	struct list_head free_chunks;
	struct xarray by_local;
};

struct rs_chunk {
	u16 node_id;
	u32 local_gid;
	u32 global_gid;
	u32 nr_ext;
	atomic_t free_ext;
	unsigned long *bm;
	struct rs_chunk_key __rcu *key;
	struct list_head free_link;
	struct rs_node *node;
};

struct rs_extent {
	u32 loc0;
	u32 loc1;
	atomic_t used;
};

struct rs_reserve {
	spinlock_t lock;
	u32 loc[RS_CPU_RESERVE];
	u8 n;
};

struct rs_map {
	u64 nr_slots;
	u32 nr_extents;
	struct percpu_counter used_slots;
	struct percpu_counter degraded_slots;
	spinlock_t prot_lock;
	u8 prot;
	u8 frozen;
	int binding;
	bool counters;
	u8 *sstate;
	struct rs_extent *extents;
	struct xarray chunks;
	struct xarray nodes;
};

/* 这一位只在 map.c 里用：该页已经算进 degraded_slots。 */
#define RS_S_SHORT		0x80

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

u32 rs_copy_mark_sent(u32 word, u32 which)
{
	if (which >= RS_NR_COPIES)
		return word;
	return word | (1u << (which + RS_COPY_SENT_SHIFT));
}

u8 rs_copy_sent_mask(u32 word)
{
	return (u8)((word >> RS_COPY_SENT_SHIFT) & ((1u << RS_NR_COPIES) - 1));
}

bool rs_hedge_should_post(u32 word, u32 which, bool overdue)
{
	u8 sent;

	if (!overdue || which >= RS_NR_COPIES || (word & RS_COPY_FINISH))
		return false;
	sent = rs_copy_sent_mask(word);
	if (!sent || (sent & (1u << which)) || (word & sent) == sent)
		return false;
	return true;
}

int rs_copy_flush_which(u32 word)
{
	u8 sent = rs_copy_sent_mask(word);
	u8 pending = sent & (u8)~(word & ((1u << RS_NR_COPIES) - 1));
	u32 i;

	if (!rs_copy_ok(word) || !pending)
		return -1;
	for (i = 0; i < RS_NR_COPIES; i++) {
		if (pending & (1u << i))
			return (int)i;
	}
	return -1;
}

bool rs_copy_claim_finish(u32 word, u32 *out)
{
	u8 sent = rs_copy_sent_mask(word);

	if (!out || !sent || (word & RS_COPY_FINISH))
		return false;
	if ((word & sent) != sent)
		return false;
	*out = word | RS_COPY_FINISH;
	return true;
}

int rs_read_arrive(u32 word, u32 which, bool success, u32 *out)
{
	u32 next;
	u8 old_ok;
	u8 sent;

	if (!out)
		return RS_ACT_WAIT;
	if (which >= RS_NR_COPIES || (word & RS_COPY_FINISH)) {
		*out = word;
		return RS_ACT_WAIT;
	}
	old_ok = rs_copy_ok(word);
	next = rs_copy_note(word, which, success);
	if (next == word) {
		*out = word;
		return RS_ACT_WAIT;
	}
	if (success && !old_ok) {
		*out = next | RS_COPY_FINISH;
		return RS_ACT_PUBLISH;
	}
	sent = rs_copy_sent_mask(next);
	if (sent && !rs_copy_ok(next) && rs_copy_settled(next, sent)) {
		*out = next | RS_COPY_FINISH;
		return RS_ACT_FAIL;
	}
	*out = next;
	return RS_ACT_WAIT;
}

static struct rs_map *rs_map;
static spinlock_t rs_ext_locks[RS_EXT_LOCKS];
static bool rs_ext_locks_ready;
static bool rs_reserve_ready;

#ifdef RS_MAP_TEST
#define RS_TEST_CPUS		4

static struct rs_reserve rs_reserves[RS_TEST_CPUS];
static __thread int rs_test_cpu;
static u32 (*rs_random_fn)(void);

void rs_map_set_random(u32 (*fn)(void))
{
	rs_random_fn = fn;
}

void rs_map_test_set_cpu(int cpu)
{
	if (cpu >= 0 && cpu < RS_TEST_CPUS)
		rs_test_cpu = cpu;
}

static struct rs_reserve *rs_reserve_pin(void)
{
	return &rs_reserves[rs_test_cpu];
}

static void rs_reserve_unpin(void)
{
}

static int rs_reserve_me(void)
{
	return rs_test_cpu;
}

static int rs_reserve_nr(void)
{
	return RS_TEST_CPUS;
}

static struct rs_reserve *rs_reserve_at(int i)
{
	return &rs_reserves[i];
}

static u32 rs_random(void)
{
	if (rs_random_fn)
		return rs_random_fn();
	return (u32)rand();
}
#else
static DEFINE_PER_CPU(struct rs_reserve, rs_reserve);

static void rs_reserve_unpin(void)
{
	put_cpu();
}

static int rs_reserve_me(void)
{
	return smp_processor_id();
}

static int rs_reserve_nr(void)
{
	return nr_cpu_ids;
}

static struct rs_reserve *rs_reserve_at(int cpu)
{
	return per_cpu_ptr(&rs_reserve, cpu);
}

static u32 rs_random(void)
{
	return get_random_u32();
}
#endif

static struct rs_reserve *rs_reserve_pin_cpu(void)
{
#ifdef RS_MAP_TEST
	return rs_reserve_pin();
#else
	get_cpu();
	return this_cpu_ptr(&rs_reserve);
#endif
}

static void rs_locks_once(void)
{
	int i;

	if (!rs_ext_locks_ready) {
		for (i = 0; i < RS_EXT_LOCKS; i++)
			spin_lock_init(&rs_ext_locks[i]);
		rs_ext_locks_ready = true;
	}
	if (rs_reserve_ready)
		return;
#ifdef RS_MAP_TEST
	for (i = 0; i < RS_TEST_CPUS; i++)
		spin_lock_init(&rs_reserves[i].lock);
#else
	for_each_possible_cpu(i)
		spin_lock_init(&per_cpu(rs_reserve, i).lock);
#endif
	rs_reserve_ready = true;
}

static void rs_reserve_reset(void)
{
	int i;

	rs_locks_once();
#ifdef RS_MAP_TEST
	for (i = 0; i < RS_TEST_CPUS; i++)
		rs_reserves[i].n = 0;
	rs_test_cpu = 0;
#else
	for_each_possible_cpu(i)
		per_cpu(rs_reserve, i).n = 0;
#endif
}

static u32 rs_reserve_held(void)
{
	u32 n = 0;
	int i;

	for (i = 0; i < rs_reserve_nr(); i++)
		n += READ_ONCE(rs_reserve_at(i)->n);
	return n;
}

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

static u32 rs_score(const struct rs_node *n)
{
	u32 rtt = n->rtt_us ? n->rtt_us : 1;
	u32 free_ext = (u32)atomic_read(&n->free_ext);

	return free_ext * n->weight / rtt;
}

static struct rs_node *rs_node_nth(struct rs_map *m, u16 exclude, bool use_exclude,
				   u32 nth)
{
	struct rs_node *n;
	unsigned long idx = 0;
	u32 seen = 0;

	n = xa_find(&m->nodes, &idx, ULONG_MAX, XA_PRESENT);
	while (n) {
		if (atomic_read(&n->free_ext) > 0 &&
		    !(use_exclude && n->id == exclude)) {
			if (seen == nth)
				return n;
			seen++;
		}
		n = xa_find_after(&m->nodes, &idx, ULONG_MAX, XA_PRESENT);
	}
	return NULL;
}

/* 候选按节点号排序。随机数的消耗和原来的两次取模一致，这样放置测试不变。 */
static struct rs_node *rs_pick_node(struct rs_map *m, u16 exclude, bool use_exclude)
{
	struct rs_node *a_node, *b_node;
	u32 nr = 0;
	u32 a, b;
	unsigned long idx = 0;
	struct rs_node *n;

	rcu_read_lock();
	n = xa_find(&m->nodes, &idx, ULONG_MAX, XA_PRESENT);
	while (n) {
		if (atomic_read(&n->free_ext) > 0 &&
		    !(use_exclude && n->id == exclude))
			nr++;
		n = xa_find_after(&m->nodes, &idx, ULONG_MAX, XA_PRESENT);
	}
	if (!nr) {
		rcu_read_unlock();
		return NULL;
	}
	if (nr == 1) {
		a_node = rs_node_nth(m, exclude, use_exclude, 0);
		rcu_read_unlock();
		return a_node;
	}
	a = rs_random() % nr;
	b = rs_random() % nr;
	a_node = rs_node_nth(m, exclude, use_exclude, a);
	b_node = rs_node_nth(m, exclude, use_exclude, b);
	rcu_read_unlock();
	if (!a_node)
		return b_node;
	if (!b_node)
		return a_node;
	if (use_exclude)
		return rs_score(b_node) > rs_score(a_node) ? b_node : a_node;
	return rs_score(a_node) >= rs_score(b_node) ? a_node : b_node;
}

static int rs_claim(struct rs_node *node, u32 *loc)
{
	struct rs_chunk *c;
	unsigned long flags;
	int ret = -ENOSPC;

	spin_lock_irqsave(&node->lock, flags);
	list_for_each_entry(c, &node->free_chunks, free_link) {
		u32 ext;

		for (ext = 0; ext < c->nr_ext; ext++) {
			if (!(ext % BITS_PER_LONG) &&
			    c->bm[ext / BITS_PER_LONG] == ~0UL) {
				ext += BITS_PER_LONG - 1;
				continue;
			}
			if (rs_bit(c->bm, ext))
				continue;
			rs_bit_set(c->bm, ext);
			if (atomic_dec_return(&c->free_ext) == 0)
				list_del(&c->free_link);
			atomic_dec(&node->free_ext);
			*loc = (c->global_gid << RS_EXT_IDX_BITS) | ext;
			ret = 0;
			goto out;
		}
	}
out:
	spin_unlock_irqrestore(&node->lock, flags);
	return ret;
}

static u16 rs_loc_node(struct rs_map *m, u32 loc)
{
	struct rs_chunk *c;
	u16 id;

	rcu_read_lock();
	c = xa_load(&m->chunks, loc >> RS_EXT_IDX_BITS);
	id = c ? c->node_id : (u16)RS_MAX_NODES;
	rcu_read_unlock();
	return id;
}

static void rs_release_bitmap(struct rs_map *m, u32 loc)
{
	struct rs_chunk *c;
	struct rs_node *node;
	unsigned long flags;
	u32 gid = loc >> RS_EXT_IDX_BITS;
	u32 ext = loc & (RS_MAX_EXT_PER_CHUNK - 1);
	int was;

	rcu_read_lock();
	c = xa_load(&m->chunks, gid);
	rcu_read_unlock();
	if (!c || ext >= c->nr_ext)
		return;
	node = c->node;
	spin_lock_irqsave(&node->lock, flags);
	if (rs_bit(c->bm, ext)) {
		rs_bit_clear(c->bm, ext);
		was = atomic_read(&c->free_ext);
		atomic_inc(&c->free_ext);
		if (!was)
			list_add_tail(&c->free_link, &node->free_chunks);
		atomic_inc(&node->free_ext);
	}
	spin_unlock_irqrestore(&node->lock, flags);
}

static void rs_release_loc(struct rs_map *m, u32 loc)
{
	struct rs_reserve *r = rs_reserve_pin_cpu();
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	if (r->n < RS_CPU_RESERVE) {
		r->loc[r->n++] = loc;
		spin_unlock_irqrestore(&r->lock, flags);
		rs_reserve_unpin();
		return;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	rs_reserve_unpin();
	rs_release_bitmap(m, loc);
}

/* 从别的 CPU 预留里拿走一份。exclude 为 RS_MAX_NODES 时不限制节点。调用方已 pin。 */
static int rs_steal_loc(struct rs_map *m, u16 exclude, u32 *loc)
{
	int me = rs_reserve_me();
	int i;

	for (i = 0; i < rs_reserve_nr(); i++) {
		struct rs_reserve *other;
		unsigned long flags;
		u32 stolen;
		u8 j;

		if (i == me)
			continue;
		other = rs_reserve_at(i);
		if (!spin_trylock_irqsave(&other->lock, flags))
			continue;
		stolen = RS_LOC_NONE;
		for (j = 0; j < other->n; j++) {
			if (exclude != RS_MAX_NODES &&
			    rs_loc_node(m, other->loc[j]) == exclude)
				continue;
			stolen = other->loc[j];
			other->loc[j] = other->loc[--other->n];
			break;
		}
		spin_unlock_irqrestore(&other->lock, flags);
		if (stolen != RS_LOC_NONE) {
			*loc = stolen;
			return 0;
		}
	}
	return -ENOSPC;
}

static int rs_pop_loc(struct rs_map *m, u32 *loc)
{
	struct rs_reserve *r = rs_reserve_pin_cpu();
	unsigned long flags;
	u32 tmp[RS_CPU_RESERVE];
	u32 extra[RS_CPU_RESERVE];
	int n = 0, extra_n = 0, i;
	int ret = -ENOSPC;

	spin_lock_irqsave(&r->lock, flags);
	if (r->n) {
		*loc = r->loc[--r->n];
		spin_unlock_irqrestore(&r->lock, flags);
		rs_reserve_unpin();
		return 0;
	}
	spin_unlock_irqrestore(&r->lock, flags);

	for (i = 0; i < RS_CPU_RESERVE; i++) {
		struct rs_node *node = rs_pick_node(m, 0, false);
		u32 one;

		if (!node || rs_claim(node, &one))
			break;
		tmp[n++] = one;
	}
	if (!n) {
		ret = rs_steal_loc(m, RS_MAX_NODES, loc);
		rs_reserve_unpin();
		return ret;
	}
	spin_lock_irqsave(&r->lock, flags);
	for (i = 0; i < n; i++) {
		if (r->n < RS_CPU_RESERVE)
			r->loc[r->n++] = tmp[i];
		else
			extra[extra_n++] = tmp[i];
	}
	if (r->n) {
		*loc = r->loc[--r->n];
		ret = 0;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	rs_reserve_unpin();
	for (i = 0; i < extra_n; i++)
		rs_release_bitmap(m, extra[i]);
	return ret;
}

static int rs_claim_other(struct rs_map *m, u16 exclude, u32 *loc)
{
	struct rs_node *node = rs_pick_node(m, exclude, true);

	if (!node)
		return -ENOSPC;
	return rs_claim(node, loc);
}

static int rs_take_other(struct rs_map *m, u16 exclude, u32 *loc)
{
	struct rs_reserve *r = rs_reserve_pin_cpu();
	unsigned long flags;
	u8 i;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	for (i = 0; i < r->n; i++) {
		if (rs_loc_node(m, r->loc[i]) == exclude)
			continue;
		*loc = r->loc[i];
		r->loc[i] = r->loc[--r->n];
		spin_unlock_irqrestore(&r->lock, flags);
		rs_reserve_unpin();
		return 0;
	}
	spin_unlock_irqrestore(&r->lock, flags);
	ret = rs_claim_other(m, exclude, loc);
	if (ret)
		ret = rs_steal_loc(m, exclude, loc);
	rs_reserve_unpin();
	return ret;
}

static int rs_bind_extent(struct rs_map *m, struct rs_extent *ext, u8 prot)
{
	u32 loc;
	u16 node;
	int ret;

	if (READ_ONCE(ext->loc0) == RS_LOC_NONE) {
		ret = rs_pop_loc(m, &loc);
		if (ret)
			return ret;
		if (cmpxchg(&ext->loc0, RS_LOC_NONE, loc) != RS_LOC_NONE)
			rs_release_loc(m, loc);
	}
	if (prot != RS_PROT_REPLICA2 || READ_ONCE(ext->loc1) != RS_LOC_NONE)
		return 0;
	node = rs_loc_node(m, READ_ONCE(ext->loc0));
	if (rs_take_other(m, node, &loc))
		return 0;
	if (cmpxchg(&ext->loc1, RS_LOC_NONE, loc) != RS_LOC_NONE)
		rs_release_loc(m, loc);
	return 0;
}

static int rs_slot(struct rs_map *m, u64 off, u32 len, u32 *slot)
{
	u64 end, idx;

	if (!len || (off & (RS_PAGE_SIZE - 1)) || (len & (RS_PAGE_SIZE - 1)))
		return -EINVAL;
	idx = off >> RS_PAGE_SHIFT;
	if (idx > U32_MAX)
		return -EINVAL;
	end = off + (u64)len;
	if (end < off || end > m->nr_slots * RS_PAGE_SIZE)
		return -EINVAL;
	*slot = (u32)idx;
	return 0;
}

static int rs_fill(struct rs_map *m, u32 loc, u32 slot, struct rs_remote *out)
{
	u32 gid = loc >> RS_EXT_IDX_BITS;
	u32 ext = loc & (RS_MAX_EXT_PER_CHUNK - 1);
	u32 pg = slot & (RS_EXTENT_PAGES - 1);
	struct rs_chunk *c;
	struct rs_chunk_key *key;
	u64 base;
	u32 rkey, gen;
	u16 node_id;

	rcu_read_lock();
	c = xa_load(&m->chunks, gid);
	if (!c || ext >= c->nr_ext) {
		rcu_read_unlock();
		return -EIO;
	}
	key = rcu_dereference(c->key);
	if (!key) {
		rcu_read_unlock();
		return -EIO;
	}
	node_id = c->node_id;
	gen = key->gen;
	rkey = key->rkey;
	base = key->base;
	rcu_read_unlock();
	out->node_id = node_id;
	out->gid = gid;
	out->gen = gen;
	out->rkey = rkey;
	out->addr = base + ((u64)ext << RS_EXTENT_SHIFT) + ((u64)pg << RS_PAGE_SHIFT);
	return 0;
}

static u8 rs_bound_mask(const struct rs_extent *ext)
{
	u8 bound = 0;

	if (READ_ONCE(ext->loc0) != RS_LOC_NONE)
		bound |= RS_S_C0;
	if (READ_ONCE(ext->loc1) != RS_LOC_NONE)
		bound |= RS_S_C1;
	return bound;
}

static u8 rs_wanted_mask(const struct rs_map *m, u8 bound)
{
	if (READ_ONCE(m->prot) == RS_PROT_REPLICA2)
		return RS_S_C0 | RS_S_C1;
	return bound & RS_S_C0;
}

static int rs_emit_copies(struct rs_map *m, struct rs_extent *ext, u32 slot,
			  u8 bits, struct rs_copies *out)
{
	u32 locs[RS_NR_COPIES];
	u32 i;
	u8 mask = 0;

	locs[0] = READ_ONCE(ext->loc0);
	locs[1] = READ_ONCE(ext->loc1);
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

static void rs_unbind_extent(struct rs_map *m, struct rs_extent *ext)
{
	u32 loc0 = READ_ONCE(ext->loc0);
	u32 loc1 = READ_ONCE(ext->loc1);

	WRITE_ONCE(ext->loc0, RS_LOC_NONE);
	WRITE_ONCE(ext->loc1, RS_LOC_NONE);
	if (loc0 != RS_LOC_NONE)
		rs_release_loc(m, loc0);
	if (loc1 != RS_LOC_NONE)
		rs_release_loc(m, loc1);
}

static int rs_activate_locked(struct rs_map *m, u32 slot, struct rs_copies *out)
{
	u32 ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	struct rs_extent *ext = &m->extents[ext_i];
	unsigned long flags;
	u8 state, neu, prot;
	int ret;

	state = READ_ONCE(m->sstate[slot]);
	for (;;) {
		if (state & RS_S_BUSY)
			return -EBUSY;
		neu = (state & (RS_S_C0 | RS_S_C1 | RS_S_SHORT | RS_S_USED)) |
		      RS_S_USED | RS_S_BUSY;
		if (cmpxchg(&m->sstate[slot], state, neu) == state)
			break;
		state = READ_ONCE(m->sstate[slot]);
	}
	if (!(state & RS_S_USED)) {
		spin_lock_irqsave(&m->prot_lock, flags);
		m->binding++;
		prot = m->prot;
		spin_unlock_irqrestore(&m->prot_lock, flags);
		ret = rs_bind_extent(m, ext, prot);
		if (ret) {
			spin_lock_irqsave(&m->prot_lock, flags);
			m->binding--;
			spin_unlock_irqrestore(&m->prot_lock, flags);
			WRITE_ONCE(m->sstate[slot], state);
			return ret;
		}
		percpu_counter_inc(&m->used_slots);
		atomic_inc(&ext->used);
		spin_lock_irqsave(&m->prot_lock, flags);
		m->binding--;
		m->frozen = 1;
		spin_unlock_irqrestore(&m->prot_lock, flags);
	}
	ret = rs_emit_copies(m, ext, slot, rs_bound_mask(ext), out);
	if (ret) {
		WRITE_ONCE(m->sstate[slot], state);
		if (!(state & RS_S_USED) && atomic_dec_return(&ext->used) == 0) {
			percpu_counter_dec(&m->used_slots);
			rs_unbind_extent(m, ext);
		}
	}
	return ret;
}

static int rs_capacity_ok(u32 capacity_mb)
{
	u64 slots;

	if (!capacity_mb || capacity_mb > RS_MAX_CAPACITY_MB)
		return -EINVAL;
	slots = (u64)capacity_mb << (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	if (slots > SIZE_MAX)
		return -EINVAL;
	return 0;
}

int rs_map_init(u32 capacity_mb)
{
	struct rs_map *m;
	u32 i;
	int ret;

	if (rs_map)
		return -EBUSY;
	ret = rs_capacity_ok(capacity_mb);
	if (ret)
		return ret;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->nr_extents = capacity_mb;
	m->nr_slots = (u64)capacity_mb << (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	m->sstate = kvzalloc((size_t)m->nr_slots, GFP_KERNEL);
	m->extents = kvcalloc(m->nr_extents, sizeof(*m->extents), GFP_KERNEL);
	if (!m->sstate || !m->extents) {
		kvfree(m->sstate);
		kvfree(m->extents);
		kfree(m);
		return -ENOMEM;
	}
	for (i = 0; i < m->nr_extents; i++) {
		m->extents[i].loc0 = RS_LOC_NONE;
		m->extents[i].loc1 = RS_LOC_NONE;
		atomic_set(&m->extents[i].used, 0);
	}
	if (percpu_counter_init(&m->used_slots, 0, GFP_KERNEL)) {
		kvfree(m->sstate);
		kvfree(m->extents);
		kfree(m);
		return -ENOMEM;
	}
	if (percpu_counter_init(&m->degraded_slots, 0, GFP_KERNEL)) {
		percpu_counter_destroy(&m->used_slots);
		kvfree(m->sstate);
		kvfree(m->extents);
		kfree(m);
		return -ENOMEM;
	}
	m->counters = true;
	spin_lock_init(&m->prot_lock);
	/*
	 * xa_alloc 只在标了 XA_FLAGS_ALLOC 的数组上合法。5.10 否则会
	 * WARN 并返回 -EINVAL，chunk 加不进去。
	 */
#ifdef RS_MAP_TEST
	xa_init(&m->chunks);
#else
	xa_init_flags(&m->chunks, XA_FLAGS_ALLOC);
#endif
	xa_init(&m->nodes);
	rs_reserve_reset();
	rs_map = m;
	return 0;
}

static void rs_chunk_free(struct rs_chunk *c)
{
	if (!c)
		return;
	kfree(c->bm);
	kfree(rcu_dereference(c->key));
	kfree(c);
}

void rs_map_exit(void)
{
	struct rs_map *m = rs_map;
	unsigned long idx;

	if (!m)
		return;
	idx = 0;
	for (;;) {
		struct rs_chunk *c = xa_find(&m->chunks, &idx, ULONG_MAX, XA_PRESENT);
		unsigned long cur;

		if (!c)
			break;
		cur = idx;
		if (idx < ULONG_MAX)
			idx++;
		xa_erase(&m->chunks, cur);
		rs_chunk_free(c);
		if (cur == ULONG_MAX)
			break;
	}
	idx = 0;
	for (;;) {
		struct rs_node *node = xa_find(&m->nodes, &idx, ULONG_MAX, XA_PRESENT);
		unsigned long cur;

		if (!node)
			break;
		cur = idx;
		if (idx < ULONG_MAX)
			idx++;
		xa_erase(&m->nodes, cur);
		xa_destroy(&node->by_local);
		kfree(node);
		if (cur == ULONG_MAX)
			break;
	}
	xa_destroy(&m->chunks);
	xa_destroy(&m->nodes);
	percpu_counter_destroy(&m->used_slots);
	percpu_counter_destroy(&m->degraded_slots);
	kvfree(m->sstate);
	kvfree(m->extents);
	kfree(m);
	rs_map = NULL;
	rs_reserve_reset();
#ifdef RS_MAP_TEST
	rs_test_reap_deferred();
#else
	rcu_barrier();
#endif
}

int rs_map_node_add(u16 node_id)
{
	struct rs_map *m = rs_map;
	struct rs_node *node, *old;
	unsigned long idx = 0;
	u32 nr = 0;

	if (!m)
		return -ENODEV;
	if (xa_load(&m->nodes, node_id))
		return 0;
	while (xa_find(&m->nodes, &idx, ULONG_MAX, XA_PRESENT)) {
		nr++;
		if (idx == ULONG_MAX)
			break;
		idx++;
	}
	if (nr >= RS_MAX_NODES)
		return -ENOSPC;
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;
	node->id = node_id;
	node->weight = 1;
	node->rtt_us = 1;
	atomic_set(&node->free_ext, 0);
	spin_lock_init(&node->lock);
	INIT_LIST_HEAD(&node->free_chunks);
	xa_init(&node->by_local);
	if (xa_insert(&m->nodes, node_id, node, GFP_KERNEL)) {
		old = xa_load(&m->nodes, node_id);
		xa_destroy(&node->by_local);
		kfree(node);
		return old ? 0 : -ENOMEM;
	}
	return 0;
}

static unsigned long *rs_bitmap(u32 nr_ext)
{
	size_t bytes = ((nr_ext + BITS_PER_LONG - 1) / BITS_PER_LONG) *
		       sizeof(unsigned long);

	return kzalloc(bytes, GFP_KERNEL);
}

static int rs_chunk_same(struct rs_chunk *c, u32 epoch, u64 raddr, u32 rkey,
			 u32 chunk_mb)
{
	struct rs_chunk_key *key = READ_ONCE(c->key);

	if (!key)
		return 0;
	return c->nr_ext == chunk_mb && key->epoch == epoch && key->base == raddr &&
	       key->rkey == rkey;
}

int rs_map_chunk_add(u16 node_id, u32 local_gid, u32 epoch, u64 raddr, u32 rkey,
		     u32 chunk_mb)
{
	struct rs_map *m = rs_map;
	struct rs_node *node;
	struct rs_chunk *c, *existing;
	struct rs_chunk_key *key;
	unsigned long flags;
	u32 global = 0;
	int ret;

	if (!m)
		return -ENODEV;
	if (!chunk_mb || chunk_mb > RS_MAX_EXT_PER_CHUNK)
		return -EINVAL;
	rcu_read_lock();
	node = xa_load(&m->nodes, node_id);
	existing = node ? xa_load(&node->by_local, local_gid) : NULL;
	if (existing) {
		ret = rs_chunk_same(existing, epoch, raddr, rkey, chunk_mb) ? 0 : -EEXIST;
		rcu_read_unlock();
		return ret;
	}
	rcu_read_unlock();
	if (!node)
		return -ENOENT;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	key = kzalloc(sizeof(*key), GFP_KERNEL);
	if (!c || !key) {
		kfree(key);
		kfree(c);
		return -ENOMEM;
	}
	c->bm = rs_bitmap(chunk_mb);
	if (!c->bm) {
		kfree(key);
		kfree(c);
		return -ENOMEM;
	}
	c->node_id = node_id;
	c->local_gid = local_gid;
	c->nr_ext = chunk_mb;
	c->node = node;
	atomic_set(&c->free_ext, 0);
	INIT_LIST_HEAD(&c->free_link);
	key->epoch = epoch;
	key->gen = 1;
	key->rkey = rkey;
	key->base = raddr;
	rcu_assign_pointer(c->key, key);
	ret = xa_alloc(&m->chunks, &global, c,
		       XA_LIMIT(0, RS_MAX_GLOBAL_CHUNKS - 1), GFP_KERNEL);
	if (ret) {
		rs_chunk_free(c);
		return ret;
	}
	c->global_gid = global;
	ret = xa_insert(&node->by_local, local_gid, c, GFP_KERNEL);
	if (ret) {
		existing = xa_load(&node->by_local, local_gid);
		xa_erase(&m->chunks, global);
		if (existing && rs_chunk_same(existing, epoch, raddr, rkey, chunk_mb))
			ret = 0;
		else if (existing)
			ret = -EEXIST;
		rs_chunk_free(c);
		return ret;
	}
	spin_lock_irqsave(&node->lock, flags);
	atomic_set(&c->free_ext, (int)chunk_mb);
	list_add_tail(&c->free_link, &node->free_chunks);
	atomic_add((int)chunk_mb, &node->free_ext);
	spin_unlock_irqrestore(&node->lock, flags);
	return 0;
}

int rs_map_chunk_update(u16 node_id, u32 local_gid, u32 epoch, u64 raddr, u32 rkey)
{
	struct rs_map *m = rs_map;
	struct rs_node *node;
	struct rs_chunk *c;
	struct rs_chunk_key *old, *neu;
	u32 gen;

	if (!m)
		return -ENODEV;
	rcu_read_lock();
	node = xa_load(&m->nodes, node_id);
	c = node ? xa_load(&node->by_local, local_gid) : NULL;
	old = c ? rcu_dereference(c->key) : NULL;
	if (!old) {
		rcu_read_unlock();
		return -ENOENT;
	}
	if (old->rkey == rkey && old->base == raddr && old->epoch == epoch) {
		rcu_read_unlock();
		return 0;
	}
	gen = old->gen + 1;
	if (!gen)
		gen = 1;
	rcu_read_unlock();
	neu = kmalloc(sizeof(*neu), GFP_KERNEL);
	if (!neu)
		return -ENOMEM;
	neu->epoch = epoch;
	neu->gen = gen;
	neu->rkey = rkey;
	neu->base = raddr;
	rcu_read_lock();
	old = rcu_dereference(c->key);
	if (!old || old->gen + 1 == gen || (old->gen == 0xffffffffu && gen == 1)) {
		if (old && old->rkey == rkey && old->base == raddr && old->epoch == epoch) {
			rcu_read_unlock();
			kfree(neu);
			return 0;
		}
		rcu_assign_pointer(c->key, neu);
		rcu_read_unlock();
		if (old)
			kfree_rcu(old, rcu);
		return 1;
	}
	rcu_read_unlock();
	kfree(neu);
	return rs_map_chunk_update(node_id, local_gid, epoch, raddr, rkey);
}

int rs_map_chunk_find(u16 node_id, u32 local_gid, u32 *global_gid)
{
	struct rs_map *m = rs_map;
	struct rs_node *node;
	struct rs_chunk *c;

	if (!m || !global_gid)
		return -EINVAL;
	rcu_read_lock();
	node = xa_load(&m->nodes, node_id);
	c = node ? xa_load(&node->by_local, local_gid) : NULL;
	if (!c) {
		rcu_read_unlock();
		return -ENOENT;
	}
	*global_gid = c->global_gid;
	rcu_read_unlock();
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
	spin_lock_irqsave(&m->prot_lock, flags);
	if (m->frozen || m->binding || percpu_counter_sum(&m->used_slots))
		ret = -EBUSY;
	else
		m->prot = (u8)mode;
	spin_unlock_irqrestore(&m->prot_lock, flags);
	return ret;
}

u32 rs_map_protection(void)
{
	struct rs_map *m = rs_map;

	return m ? READ_ONCE(m->prot) : RS_PROT_SINGLE;
}

int rs_map_prepare_write(u64 off, u32 len, struct rs_copies *out)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	u32 slot, ext_i;
	int ret;

	if (!m || !out || len != RS_PAGE_SIZE)
		return -EINVAL;
	ret = rs_slot(m, off, len, &slot);
	if (ret)
		return ret;
	ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	spin_lock_irqsave(&rs_ext_locks[ext_i & (RS_EXT_LOCKS - 1)], flags);
	ret = rs_activate_locked(m, slot, out);
	spin_unlock_irqrestore(&rs_ext_locks[ext_i & (RS_EXT_LOCKS - 1)], flags);
	return ret;
}

int rs_map_lookup_read(u64 off, u32 len, struct rs_copies *out)
{
	struct rs_map *m = rs_map;
	struct rs_extent *ext;
	u32 slot, ext_i;
	u8 state;
	int ret;

	if (!m || !out || len != RS_PAGE_SIZE)
		return -EINVAL;
	ret = rs_slot(m, off, len, &slot);
	if (ret)
		return ret;
	do {
		state = READ_ONCE(m->sstate[slot]);
		if (!(state & RS_S_USED) || (state & RS_S_ZERO))
			return RS_MAP_ZERO;
		if (state & RS_S_LOST)
			return -EIO;
		if (state & RS_S_BUSY)
			return -EBUSY;
	} while (cmpxchg(&m->sstate[slot], state, (u8)(state | RS_S_BUSY)) != state);
	ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
	ext = &m->extents[ext_i];
	ret = rs_emit_copies(m, ext, slot, state & (RS_S_C0 | RS_S_C1), out);
	if (ret || !out->mask) {
		rs_map_finish(off);
		return ret ? ret : -EIO;
	}
	return 0;
}

static void rs_account_discard(struct rs_map *m, struct rs_extent *ext, u8 state)
{
	if ((state & RS_S_SHORT) && percpu_counter_sum(&m->degraded_slots) > 0)
		percpu_counter_dec(&m->degraded_slots);
	if (percpu_counter_sum(&m->used_slots) > 0)
		percpu_counter_dec(&m->used_slots);
	if (atomic_dec_return(&ext->used) == 0)
		rs_unbind_extent(m, ext);
}

int rs_map_discard(u64 off, u32 len)
{
	struct rs_map *m = rs_map;
	unsigned long flags;
	u8 saved[RS_DISCARD_STACK];
	u32 slot, n;
	int ret;

	if (!m)
		return -EINVAL;
	ret = rs_slot(m, off, len, &slot);
	if (ret)
		return ret;
	n = len >> RS_PAGE_SHIFT;
	while (n) {
		u32 ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
		u32 room = RS_EXTENT_PAGES - (slot & (RS_EXTENT_PAGES - 1));
		u32 chunk = n < room ? n : room;
		u32 base = slot;
		u32 i = 0;
		int lock = ext_i & (RS_EXT_LOCKS - 1);

		spin_lock_irqsave(&rs_ext_locks[lock], flags);
		for (i = 0; i < chunk; i++) {
			if (READ_ONCE(m->sstate[base + i]) & RS_S_BUSY) {
				spin_unlock_irqrestore(&rs_ext_locks[lock], flags);
				return -EBUSY;
			}
		}
		for (i = 0; i < chunk; ) {
			u32 win = chunk - i;
			u32 j;

			if (win > RS_DISCARD_STACK)
				win = RS_DISCARD_STACK;
			for (j = 0; j < win; j++)
				saved[j] = READ_ONCE(m->sstate[base + i + j]);
			for (j = 0; j < win; j++) {
				if (cmpxchg(&m->sstate[base + i + j], saved[j],
					    (u8)RS_S_ZERO) == saved[j])
					continue;
				while (j--)
					cmpxchg(&m->sstate[base + i + j],
						(u8)RS_S_ZERO, saved[j]);
				spin_unlock_irqrestore(&rs_ext_locks[lock], flags);
				return -EBUSY;
			}
			for (j = 0; j < win; j++) {
				if (!(saved[j] & RS_S_USED))
					continue;
				rs_account_discard(m, &m->extents[ext_i], saved[j]);
			}
			i += win;
		}
		spin_unlock_irqrestore(&rs_ext_locks[lock], flags);
		slot += chunk;
		n -= chunk;
	}
	return 0;
}

void rs_map_finish(u64 off)
{
	struct rs_map *m = rs_map;
	u32 slot;
	u8 state;

	if (!m || (off & (RS_PAGE_SIZE - 1)))
		return;
	if ((off >> RS_PAGE_SHIFT) >= m->nr_slots)
		return;
	slot = (u32)(off >> RS_PAGE_SHIFT);
	do {
		state = READ_ONCE(m->sstate[slot]);
		if (!(state & RS_S_BUSY))
			return;
	} while (cmpxchg(&m->sstate[slot], state, (u8)(state & ~RS_S_BUSY)) != state);
}

void rs_map_finish_write(u64 off, u8 ok_mask)
{
	struct rs_map *m = rs_map;
	struct rs_extent *ext;
	u32 slot, ext_i;
	u8 state, neu, bound, wanted, valid;
	bool short_old, short_new;

	if (!m || (off & (RS_PAGE_SIZE - 1)))
		return;
	if ((off >> RS_PAGE_SHIFT) >= m->nr_slots)
		return;
	slot = (u32)(off >> RS_PAGE_SHIFT);
	do {
		state = READ_ONCE(m->sstate[slot]);
		short_old = false;
		short_new = false;
		if (!(state & RS_S_USED)) {
			neu = state & (u8)~RS_S_BUSY;
		} else {
			ext_i = slot >> (RS_EXTENT_SHIFT - RS_PAGE_SHIFT);
			ext = &m->extents[ext_i];
			bound = rs_bound_mask(ext);
			wanted = rs_wanted_mask(m, bound);
			valid = ok_mask & bound & (RS_S_C0 | RS_S_C1);
			short_old = state & RS_S_SHORT;
			short_new = valid != wanted;
			neu = RS_S_USED | valid | (short_new ? RS_S_SHORT : 0);
		}
	} while (cmpxchg(&m->sstate[slot], state, neu) != state);
	if ((state & RS_S_USED) && short_new && !short_old)
		percpu_counter_inc(&m->degraded_slots);
	else if ((state & RS_S_USED) && !short_new && short_old &&
		 percpu_counter_sum(&m->degraded_slots) > 0)
		percpu_counter_dec(&m->degraded_slots);
}

u32 rs_map_chunk_gen(u32 gid)
{
	struct rs_map *m = rs_map;
	struct rs_chunk *c;
	struct rs_chunk_key *key;
	u32 gen = 0;

	if (!m)
		return 0;
	rcu_read_lock();
	c = xa_load(&m->chunks, gid);
	key = c ? rcu_dereference(c->key) : NULL;
	if (key)
		gen = key->gen;
	rcu_read_unlock();
	return gen;
}

int rs_map_chunk_node(u32 gid, u16 *node_id)
{
	struct rs_map *m = rs_map;
	struct rs_chunk *c;

	if (!m || !node_id)
		return -EINVAL;
	rcu_read_lock();
	c = xa_load(&m->chunks, gid);
	if (!c) {
		rcu_read_unlock();
		return -ENOENT;
	}
	*node_id = c->node_id;
	rcu_read_unlock();
	return 0;
}

void rs_map_stats(struct rs_map_stats *out)
{
	struct rs_map *m = rs_map;
	unsigned long idx = 0;
	u32 chunks = 0, nodes = 0, free_ext = 0;

	memset(out, 0, sizeof(*out));
	if (!m)
		return;
	rcu_read_lock();
	for (;;) {
		struct rs_chunk *c = xa_find(&m->chunks, &idx, ULONG_MAX, XA_PRESENT);

		if (!c)
			break;
		chunks++;
		free_ext += (u32)atomic_read(&c->free_ext);
		if (idx == ULONG_MAX)
			break;
		idx++;
	}
	idx = 0;
	for (;;) {
		struct rs_node *node = xa_find(&m->nodes, &idx, ULONG_MAX, XA_PRESENT);

		if (!node)
			break;
		nodes++;
		if (idx == ULONG_MAX)
			break;
		idx++;
	}
	rcu_read_unlock();
	out->nr_slots = m->nr_slots > U32_MAX ? U32_MAX : (u32)m->nr_slots;
	out->used_slots = (u32)percpu_counter_sum(&m->used_slots);
	out->nr_chunks = chunks;
	out->nr_nodes = nodes;
	out->free_extents = free_ext + rs_reserve_held();
	out->degraded_slots = (u32)percpu_counter_sum(&m->degraded_slots);
}

#ifdef RS_MAP_TEST
void rs_map_test_set_gen(u32 global_gid, u32 gen)
{
	struct rs_chunk *c;
	struct rs_chunk_key *key;

	if (!rs_map)
		return;
	c = xa_load(&rs_map->chunks, global_gid);
	key = c ? rcu_dereference(c->key) : NULL;
	if (key)
		key->gen = gen;
}
#endif
