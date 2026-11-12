/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_MAP_USER_H
#define _ROSWAP_MAP_USER_H

/* 让 map.c 在用户态被直接编进分配器测试，不拉内核头。 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int unused; } spinlock_t;

#define spin_lock_init(l)	((void)(l))
#define spin_lock(l)		((void)(l))
#define spin_unlock(l)		((void)(l))
#define GFP_KERNEL		0
#define kvzalloc(sz, gfp)	calloc(1, (sz))
#define kvfree(p)		free(p)
#define kzalloc(sz, gfp)	calloc(1, (sz))
#define kfree(p)		free(p)
#define pr_err(fmt, ...)	fprintf(stderr, "RoSwap: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)	((void)0)
#define BITS_PER_LONG		(sizeof(unsigned long) * 8)
#define READ_ONCE(x)		(*(volatile __typeof__(x) *)&(x))
#define cmpxchg(ptr, old, new)	__sync_val_compare_and_swap((ptr), (old), (new))

#endif
