/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROSWAP_MAP_H
#define _ROSWAP_MAP_H

/*
 * Slot / Extent 映射。Extent 固定 1MB，loc = (chunk_gid << 10) | ext_idx。
 * 读者在 I/O 路径上调用，分配器本身用一把自旋锁挡住 discard。
 */

#ifdef RS_MAP_TEST
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
#define RS_MAX_NODES		16
#define RS_MAX_CHUNKS		64
#define RS_EXT_IDX_BITS		10
#define RS_MAX_EXT_PER_CHUNK	(1u << RS_EXT_IDX_BITS)

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

struct rs_map_stats {
	u32 nr_slots;
	u32 used_slots;
	u32 free_extents;
	u32 nr_nodes;
	u32 nr_chunks;
};

int rs_map_init(u32 capacity_mb);
void rs_map_exit(void);
int rs_map_node_add(u16 node_id);
int rs_map_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 raddr, u32 rkey,
		     u32 chunk_mb);
int rs_map_chunk_update(u32 gid, u32 epoch, u64 raddr, u32 rkey);
int rs_map_prepare_write(u64 off, u32 len, struct rs_remote *out);
int rs_map_lookup_read(u64 off, u32 len, struct rs_remote *out);
int rs_map_discard(u64 off, u32 len);
void rs_map_finish(u64 off);
u32 rs_map_chunk_gen(u32 gid);
int rs_map_chunk_node(u32 gid, u16 *node_id);
void rs_map_stats(struct rs_map_stats *out);

#ifdef RS_MAP_TEST
void rs_map_set_random(u32 (*fn)(void));
void rs_map_test_set_gen(u32 gid, u32 gen);
#endif

#endif
