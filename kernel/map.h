/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_MAP_H
#define _ROSWAP_MAP_H

/*
 * Slot / Extent 映射。Extent 固定 1MB，loc = (chunk_gid << 10) | ext_idx。
 * 读者在 I/O 路径上调用，分配器本身用一把自旋锁挡住 discard。
 */

#ifdef RS_MAP_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <linux/types.h>
#endif

#define RS_MAP_ZERO		1
#define RS_MAX_NODES		256
/*
 * loc 把全局 chunk 号放在高 22 位，extent 下标占低 10 位。
 * 页号是 u32，16TiB 整除时页数是 2^32，装不进页号，所以容量停在 16TiB - 1MiB。
 */
#define RS_GID_BITS		22
#define RS_MAX_GLOBAL_CHUNKS	(1u << RS_GID_BITS)
#define RS_MAX_CAPACITY_MB	((u32)~0U >> 8)
#define RS_EXT_IDX_BITS		10
#define RS_MAX_EXT_PER_CHUNK	(1u << RS_EXT_IDX_BITS)
#define RS_NR_COPIES		2

enum {
	RS_PROT_SINGLE = 0,
	RS_PROT_REPLICA2 = 1,
};

#define RS_S_C0			0x01
#define RS_S_C1			0x02
#define RS_S_SSD		0x04
#define RS_S_LOST		0x08
#define RS_S_BUSY		0x10
#define RS_S_ZERO		0x20
#define RS_S_USED		0x40

struct rs_remote {
	u16 node_id;
	u32 gid;
	u32 gen;
	u32 rkey;
	u64 addr;
};

struct rs_copies {
	u8 mask;
	struct rs_remote r[RS_NR_COPIES];
};

struct rs_map_stats {
	u32 nr_slots;
	u32 used_slots;
	u32 free_extents;
	u32 nr_nodes;
	u32 nr_chunks;
	u32 degraded_slots;
};

int rs_map_init(u32 capacity_mb);
void rs_map_exit(void);
int rs_map_node_add(u16 node_id);
int rs_map_chunk_add(u16 node_id, u32 local_gid, u32 epoch, u64 raddr, u32 rkey,
		     u32 chunk_mb);
/* 按 (node_id, 本地 gid) 换钥匙。0 没变，1 换过，负数为错误。 */
int rs_map_chunk_update(u16 node_id, u32 local_gid, u32 epoch, u64 raddr,
			u32 rkey);
int rs_map_chunk_find(u16 node_id, u32 local_gid, u32 *global_gid);
int rs_map_set_protection(u32 mode);
u32 rs_map_protection(void);
int rs_map_prepare_write(u64 off, u32 len, struct rs_copies *out);
int rs_map_lookup_read(u64 off, u32 len, struct rs_copies *out);
int rs_map_discard(u64 off, u32 len);
void rs_map_finish(u64 off);
void rs_map_finish_write(u64 off, u8 ok_mask);
/* bit0、bit1 已到达，bit4、bit5 成功，bit8、bit9 已发送，bit30 已有人收尾。 */
#define RS_COPY_SENT_SHIFT	8
#define RS_COPY_FINISH		0x40000000u

u32 rs_copy_note(u32 word, u32 which, bool success);
u32 rs_copy_mark_sent(u32 word, u32 which);
u8 rs_copy_sent_mask(u32 word);
bool rs_copy_settled(u32 word, u8 sent_mask);
u8 rs_copy_ok(u32 word);
bool rs_hedge_should_post(u32 word, u32 which, bool overdue);
int rs_copy_flush_which(u32 word);
bool rs_copy_claim_finish(u32 word, u32 *out);

enum {
	RS_ACT_WAIT = 0,
	RS_ACT_PUBLISH = 1,
	RS_ACT_FAIL = 2,
};

int rs_read_arrive(u32 word, u32 which, bool success, u32 *out);
u32 rs_map_chunk_gen(u32 gid);
int rs_map_chunk_node(u32 gid, u16 *node_id);
void rs_map_stats(struct rs_map_stats *out);

#ifdef RS_MAP_TEST
void rs_map_set_random(u32 (*fn)(void));
void rs_map_test_set_gen(u32 global_gid, u32 gen);
void rs_map_test_set_cpu(int cpu);
#endif

#endif
