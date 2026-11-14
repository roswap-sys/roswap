/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_MAP_USER_H
#define _ROSWAP_MAP_USER_H

/* 让 map.c 在用户态被直接编进分配器测试，不拉内核头。 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long xa_mark_t;

/*
 * 故意不提供 spin_lock / spin_unlock。完成回调在硬中断里，
 * 进程上下文必须关中断，map.c 里写了裸 spin_lock 就让 map-test 编不过。
 * irqsave 版本在用户态是互斥锁，多线程测试才能抓住死锁。
 */
typedef struct {
	pthread_mutex_t mu;
} spinlock_t;

#define spin_lock_init(l) pthread_mutex_init(&(l)->mu, NULL)
#define spin_lock_irqsave(l, f)                                                \
	do {                                                                   \
		(f) = 0;                                                       \
		pthread_mutex_lock(&(l)->mu);                                  \
	} while (0)
#define spin_unlock_irqrestore(l, f)                                           \
	do {                                                                   \
		(void)(f);                                                     \
		pthread_mutex_unlock(&(l)->mu);                                \
	} while (0)
#define lockdep_set_novalidate_class(l) ((void)(l))

#define GFP_KERNEL 0
#define kvzalloc(sz, gfp) calloc(1, (sz))
#define kvcalloc(n, sz, gfp) calloc((n), (sz))
#define kvfree(p) free(p)
#define kzalloc(sz, gfp) calloc(1, (sz))
#define kmalloc(sz, gfp) malloc(sz)
#define kfree(p) free(p)
#define pr_err(fmt, ...) fprintf(stderr, "RoSwap: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) ((void)0)
#define pr_warn(fmt, ...) ((void)0)
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) ((*(volatile __typeof__(x) *)&(x)) = (v))
#define cmpxchg(ptr, old, new) __sync_val_compare_and_swap((ptr), (old), (new))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ULONG_MAX (~0UL)
#define U32_MAX ((uint32_t)~0U)
#define __rcu

#define container_of(ptr, type, member)                                        \
	((type *)((char *)(ptr) - offsetof(type, member)))

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *n, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = n;
	n->next = next;
	n->prev = prev;
	prev->next = n;
}

static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
	__list_add(n, head->prev, head);
}

static inline void list_del(struct list_head *n)
{
	n->next->prev = n->prev;
	n->prev->next = n->next;
	INIT_LIST_HEAD(n);
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each_entry(pos, head, member)                                 \
	for (pos = list_entry((head)->next, __typeof__(*pos), member);         \
	     &pos->member != (head);                                           \
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))

typedef struct {
	int v;
} atomic_t;

#define atomic_set(a, val) ((a)->v = (val))
#define atomic_read(a) ((a)->v)
#define atomic_inc(a) ((void)__sync_add_and_fetch(&(a)->v, 1))
#define atomic_dec(a) ((void)__sync_sub_and_fetch(&(a)->v, 1))
#define atomic_add(i, a) ((void)__sync_add_and_fetch(&(a)->v, (i)))
#define atomic_dec_return(a) __sync_sub_and_fetch(&(a)->v, 1)

#define rs_lock_nested(l) pthread_mutex_lock(&(l)->mu)
#define rs_unlock_nested(l) pthread_mutex_unlock(&(l)->mu)
#define spin_trylock_irqsave(l, f)                                             \
	({                                                                     \
		(f) = 0;                                                       \
		pthread_mutex_trylock(&(l)->mu) == 0;                          \
	})

struct percpu_counter {
	atomic_t n;
};

static inline int percpu_counter_init(struct percpu_counter *fbc, int64_t amount,
				       int gfp)
{
	(void)gfp;
	atomic_set(&fbc->n, (int)amount);
	return 0;
}

static inline void percpu_counter_destroy(struct percpu_counter *fbc)
{
	(void)fbc;
}

static inline void percpu_counter_inc(struct percpu_counter *fbc)
{
	__sync_add_and_fetch(&fbc->n.v, 1);
}

static inline void percpu_counter_dec(struct percpu_counter *fbc)
{
	__sync_sub_and_fetch(&fbc->n.v, 1);
}

static inline int64_t percpu_counter_sum(struct percpu_counter *fbc)
{
	return atomic_read(&fbc->n);
}

struct xa_limit {
	uint32_t min;
	uint32_t max;
};

#define XA_LIMIT(_min, _max) ((struct xa_limit){ (_min), (_max) })
#define XA_PRESENT 0

struct xa_node {
	unsigned long index;
	void *entry;
	struct xa_node *next;
};

struct xarray {
	struct xa_node *head;
};

static inline void xa_init(struct xarray *xa)
{
	xa->head = NULL;
}

static inline void *xa_load(struct xarray *xa, unsigned long index)
{
	struct xa_node *n;

	for (n = xa->head; n; n = n->next) {
		if (n->index == index)
			return n->entry;
	}
	return NULL;
}

static inline int xa_insert(struct xarray *xa, unsigned long index, void *entry,
			    int gfp)
{
	struct xa_node *n, **pp;

	(void)gfp;
	if (!entry)
		return -EINVAL;
	for (pp = &xa->head; *pp; pp = &(*pp)->next) {
		if ((*pp)->index == index)
			return -EBUSY;
		if ((*pp)->index > index)
			break;
	}
	n = calloc(1, sizeof(*n));
	if (!n)
		return -ENOMEM;
	n->index = index;
	n->entry = entry;
	n->next = *pp;
	*pp = n;
	return 0;
}

static inline void *xa_erase(struct xarray *xa, unsigned long index)
{
	struct xa_node **pp, *n;
	void *entry;

	for (pp = &xa->head; *pp; pp = &(*pp)->next) {
		if ((*pp)->index != index)
			continue;
		n = *pp;
		*pp = n->next;
		entry = n->entry;
		free(n);
		return entry;
	}
	return NULL;
}

static inline void *xa_find(struct xarray *xa, unsigned long *index,
			    unsigned long max, xa_mark_t filter)
{
	struct xa_node *n;

	(void)filter;
	for (n = xa->head; n; n = n->next) {
		if (n->index < *index)
			continue;
		if (n->index > max)
			return NULL;
		*index = n->index;
		return n->entry;
	}
	return NULL;
}

static inline void *xa_find_after(struct xarray *xa, unsigned long *index,
				  unsigned long max, xa_mark_t filter)
{
	if (*index == ULONG_MAX)
		return NULL;
	(*index)++;
	return xa_find(xa, index, max, filter);
}

static inline int xa_alloc(struct xarray *xa, uint32_t *id, void *entry,
			   struct xa_limit limit, int gfp)
{
	uint32_t i;

	for (i = limit.min; i <= limit.max; i++) {
		if (xa_load(xa, i))
			continue;
		if (xa_insert(xa, i, entry, gfp))
			return -ENOMEM;
		*id = i;
		return 0;
	}
	return -EBUSY;
}

static inline void xa_destroy(struct xarray *xa)
{
	struct xa_node *n;

	while ((n = xa->head)) {
		xa->head = n->next;
		free(n);
	}
}

#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define rcu_dereference(p) READ_ONCE(p)
#define rcu_assign_pointer(p, v) WRITE_ONCE((p), (v))
#define synchronize_rcu() ((void)0)
#define rcu_barrier() ((void)0)

struct rcu_head {
	struct rcu_head *next;
};

static struct rcu_head *rs_test_deferred;

static inline void rs_test_defer(struct rcu_head *head)
{
	head->next = rs_test_deferred;
	rs_test_deferred = head;
}

#define kfree_rcu(p, member) rs_test_defer(&(p)->member)

static inline void rs_test_reap_deferred(void)
{
	struct rcu_head *head;

	while ((head = rs_test_deferred)) {
		rs_test_deferred = head->next;
		free(head);
	}
}

#endif
