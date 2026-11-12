// SPDX-License-Identifier: GPL-2.0
#include "map.c"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect(int cond, const char *what)
{
	if (cond)
		return;
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

static u32 scripted_i;
static const u32 scripted[] = { 0, 1 };

static u32 scripted_rand(void)
{
	u32 v = scripted[scripted_i % 2];

	scripted_i++;
	return v;
}

static void reset_map(u32 capacity_mb)
{
	rs_map_exit();
	expect(rs_map_init(capacity_mb) == 0, "init");
}

static void test_same_extent_and_discard(void)
{
	struct rs_remote a, b;
	int rc;

	reset_map(2);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 3, 0x100000000ULL, 0xabc, 2) == 0,
	       "chunk");
	rc = rs_map_prepare_write(0, 4096, &a);
	expect(rc == 0, "write page 0");
	rc = rs_map_prepare_write(4096, 4096, &b);
	expect(rc == 0, "write page 1");
	expect(a.rkey == 0xabc && b.rkey == 0xabc, "rkey");
	expect(a.addr + 4096 == b.addr, "adjacent pages share the extent");
	expect(((a.addr - 0x100000000ULL) & ((1ULL << 20) - 1)) == 0, "extent aligned");
	expect(a.addr >= 0x100000000ULL && a.addr < 0x100000000ULL + (2ULL << 20),
	       "inside the chunk");
	rs_map_finish(0);
	rs_map_finish(4096);

	expect(rs_map_lookup_read(0, 4096, &a) == 0, "read page 0");
	rs_map_finish(0);
	expect(rs_map_discard(0, 4096) == 0, "discard one page");
	expect(rs_map_lookup_read(0, 4096, &a) == RS_MAP_ZERO, "discarded page is zero");
	expect(rs_map_lookup_read(4096, 4096, &b) == 0, "sibling page stays");
	rs_map_finish(4096);
	expect(rs_map_discard(4096, 4096) == 0, "discard sibling");
	expect(rs_map_lookup_read(4096, 4096, &b) == RS_MAP_ZERO, "extent freed");
	expect(rs_map_prepare_write(0, 4096, &a) == 0, "reuse after discard");
	rs_map_finish(0);
}

static void test_power_of_two(void)
{
	struct rs_remote remote, again;
	struct rs_map_stats st;
	int on_small = 0;
	int on_large = 0;
	int i;

	scripted_i = 0;
	rs_map_set_random(scripted_rand);
	reset_map(9);
	expect(rs_map_node_add(1) == 0, "node 1");
	expect(rs_map_node_add(2) == 0, "node 2");
	expect(rs_map_chunk_add(1, 0, 1, 0x2000ULL, 11, 1) == 0, "small chunk");
	expect(rs_map_chunk_add(2, 1, 1, 0x80000000ULL, 22, 8) == 0, "large chunk");

	for (i = 0; i < 9; i++) {
		u64 off = (u64)i << 20;

		expect(rs_map_prepare_write(off, 4096, &remote) == 0, "place extent");
		expect(rs_map_prepare_write(off + 4096, 4096, &again) == 0,
		       "second page");
		expect(again.addr == remote.addr + 4096, "same remote extent");
		expect(remote.node_id == 1 || remote.node_id == 2, "known node");
		if (remote.node_id == 1)
			on_small++;
		else
			on_large++;
		rs_map_finish(off);
		rs_map_finish(off + 4096);
	}
	expect(on_large == 8 && on_small == 1, "larger node takes eight extents");

	rs_map_stats(&st);
	expect(st.nr_nodes == 2 && st.nr_chunks == 2, "stats nodes");
	expect(st.used_slots == 18, "two pages in each extent");
	expect(st.free_extents == 0, "backing extents are all in use");
}

static void test_key_update_and_sizes(void)
{
	struct rs_remote remote;
	u32 gen;

	reset_map(1);
	expect(rs_map_node_add(4) == 0, "node");
	expect(rs_map_chunk_add(4, 0, 1, 0x1000, 1, 64) == 0, "64MB");
	expect(rs_map_chunk_add(4, 1, 1, 0x2000, 1, 256) == 0, "256MB");
	expect(rs_map_chunk_add(4, 2, 1, 0x3000, 1, 1024) == 0, "1GB");
	expect(rs_map_chunk_add(4, 3, 1, 0x4000, 1, 1025) == -EINVAL, "reject >1GB");
	expect(rs_map_chunk_add(4, 3, 1, 0x4000, 1, 0) == -EINVAL, "reject 0");
	expect(rs_map_chunk_add(4, 0, 1, 0x1000, 1, 64) == 0, "idempotent add");
	expect(rs_map_chunk_add(4, 0, 1, 0x1000, 2, 64) == -EEXIST, "changed add");

	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "write");
	gen = remote.gen;
	expect(rs_map_chunk_update(0, 1, 0x1000, 1) == 0, "same key");
	expect(rs_map_chunk_gen(0) == gen, "gen stable");
	expect(rs_map_chunk_update(0, 9, 0x5555, 77) == 1, "new key");
	expect(rs_map_chunk_gen(0) != gen, "gen bumped");
	rs_map_finish(0);
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "read new key");
	expect(remote.rkey == 77, "updated rkey");
	expect(remote.addr >= 0x5555 &&
	       ((remote.addr - 0x5555) & ((1ULL << 20) - 1)) == 0,
	       "updated base");
	expect(remote.gen == rs_map_chunk_gen(0), "gen matches");
	rs_map_finish(0);
	expect(rs_map_chunk_update(9, 1, 1, 1) == -ENOENT, "unknown chunk");
	expect(rs_map_prepare_write(0, 512, &remote) == -EINVAL, "short write");
	expect(rs_map_lookup_read(1ULL << 20, 4096, &remote) == -EINVAL, "past end");
}

static u32 zero_rand(void)
{
	return 0;
}

static void test_edges(void)
{
	struct rs_remote remote;
	struct rs_map_stats st;
	u16 node = 99;
	const u64 base = 0x100000000ULL;

	rs_map_exit();
	expect(rs_map_init(0) == -EINVAL, "reject zero capacity");
	expect(rs_map_init((1u << 20) + 1) == -EINVAL, "reject above 1TiB");
	expect(rs_map_node_add(1) == -ENODEV, "node before init");
	expect(rs_map_chunk_add(1, 0, 1, 1, 1, 1) == -ENODEV, "chunk before init");
	expect(rs_map_chunk_update(0, 1, 1, 1) == -ENODEV, "update before init");
	expect(rs_map_prepare_write(0, 4096, &remote) == -EINVAL, "write before init");
	expect(rs_map_lookup_read(0, 4096, &remote) == -EINVAL, "read before init");
	expect(rs_map_discard(0, 4096) == -EINVAL, "discard before init");
	expect(rs_map_init(1) == 0, "init");
	expect(rs_map_init(1) == -EBUSY, "second init");

	reset_map(1);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_prepare_write(0, 4096, &remote) == -ENOSPC, "write with no chunk");
	expect(rs_map_lookup_read(0, 4096, &remote) == RS_MAP_ZERO, "unmapped read");
	rs_map_stats(&st);
	expect(st.nr_slots == 256 && st.used_slots == 0 && st.free_extents == 0,
	       "empty stats");
	expect(st.nr_nodes == 1 && st.nr_chunks == 0, "node without chunk");
	expect(rs_map_chunk_add(9, 0, 1, base, 1, 1) == -ENOENT, "chunk needs a node");
	expect(rs_map_chunk_add(1, 64, 1, base, 1, 1) == -EINVAL, "gid 64");
	expect(rs_map_chunk_add(1, 63, 1, base, 1, 1) == 0, "gid 63");
	expect(rs_map_chunk_node(63, &node) == 0 && node == 1, "chunk node");
	expect(rs_map_chunk_node(0, &node) == -ENOENT, "missing chunk node");
	expect(rs_map_chunk_node(63, NULL) == -EINVAL, "null node out");
	expect(rs_map_node_add(1) == 0, "idempotent node");
	expect(rs_map_lookup_read(0, 4096, &remote) == RS_MAP_ZERO, "read allocates nothing");
	rs_map_stats(&st);
	expect(st.free_extents == 1 && st.used_slots == 0, "read left the extent free");
	expect(rs_map_prepare_write(1, 4096, &remote) == -EINVAL, "unaligned offset");
	expect(rs_map_prepare_write(0, 0, &remote) == -EINVAL, "zero length");
	expect(rs_map_prepare_write(0, 8192, &remote) == -EINVAL, "write is one page");
	expect(rs_map_discard(4096, 100) == -EINVAL, "unaligned discard length");

	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "first page");
	expect(remote.addr == base, "the only extent is index 0");
	expect(rs_map_prepare_write(0, 4096, &remote) == -EBUSY, "busy write");
	expect(rs_map_lookup_read(0, 4096, &remote) == -EBUSY, "busy read");
	expect(rs_map_discard(0, 4096) == -EBUSY, "busy discard");
	rs_map_finish(0);
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "read after finish");
	rs_map_finish(0);
	expect(rs_map_prepare_write(255u * 4096, 4096, &remote) == 0, "last page");
	expect(remote.addr == base + 255u * 4096, "same extent, last page");
	rs_map_finish(255u * 4096);
	expect(rs_map_prepare_write(256ull * 4096, 4096, &remote) == -EINVAL, "past the disk");
	rs_map_stats(&st);
	expect(st.used_slots == 2 && st.free_extents == 0, "one extent holds both pages");

	expect(rs_map_prepare_write(4096, 4096, &remote) == 0, "middle page stays busy");
	expect(rs_map_discard(0, 3 * 4096) == -EBUSY, "one busy page blocks the range");
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "earlier page was not discarded");
	rs_map_finish(0);
	rs_map_finish(4096);
	expect(rs_map_discard(0, 4096) == 0, "discard the first page only");
	rs_map_stats(&st);
	expect(st.used_slots == 2 && st.free_extents == 0, "extent stays while a page remains");
	expect(rs_map_discard(4096, 4096) == 0, "discard the middle page");
	expect(rs_map_discard(255u * 4096, 4096) == 0, "discard the last page");
	expect(rs_map_lookup_read(255u * 4096, 4096, &remote) == RS_MAP_ZERO, "extent returned");
	rs_map_stats(&st);
	expect(st.used_slots == 0 && st.free_extents == 1, "extent is free again");
}

static void test_limits_and_placement(void)
{
	struct rs_remote remote;
	struct rs_map_stats st;
	const u64 base = 0x200000000ULL;
	u32 i;

	reset_map(2);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 3, 1) == 0, "one remote extent");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "consume it");
	rs_map_finish(0);
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == -ENOSPC,
	       "no remote extent for the next logical extent");
	rs_map_stats(&st);
	expect(st.used_slots == 1 && st.free_extents == 0, "first extent stayed allocated");

	reset_map(2);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 3, 2) == 0, "two remote extents");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "first logical extent");
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0, "second logical extent");
	rs_map_finish(0);
	rs_map_finish(1ull << 20);
	expect(rs_map_discard(0, 2u << 20) == 0, "discard both logical extents");
	expect(rs_map_lookup_read(0, 4096, &remote) == RS_MAP_ZERO, "first cleared");
	expect(rs_map_lookup_read(1ull << 20, 4096, &remote) == RS_MAP_ZERO, "second cleared");
	rs_map_stats(&st);
	expect(st.free_extents == 2 && st.used_slots == 0, "both extents returned");

	reset_map(8);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 5, 8) == 0, "eight extents");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "lifo first");
	expect(remote.addr == base + (7ull << 20), "reserve pops the last refilled extent");
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0, "lifo second");
	expect(remote.addr == base + (6ull << 20), "next pop walks backward");
	rs_map_finish(0);
	rs_map_finish(1ull << 20);
	expect(rs_map_discard(0, 4096) == 0, "return the first loc to the reserve");
	expect(rs_map_prepare_write(2ull << 20, 4096, &remote) == 0, "reuse");
	expect(remote.addr == base + (7ull << 20), "released loc is reused first");
	rs_map_finish(2ull << 20);
	rs_map_stats(&st);
	expect(st.free_extents == 6 && st.used_slots == 2, "reserve still holds the rest");

	reset_map(65);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 5, 65) == 0, "65 extents cross a bitmap word");
	for (i = 0; i < 65; i++) {
		u32 group = i / 8;
		u32 within = i % 8;
		u32 ext = group < 8 ? group * 8 + (7 - within) : 64;

		expect(rs_map_prepare_write((u64)i << 20, 4096, &remote) == 0, "claim");
		expect(remote.addr == base + ((u64)ext << 20), "lifo batches of eight");
		rs_map_finish((u64)i << 20);
	}
	rs_map_stats(&st);
	expect(st.used_slots == 65 && st.free_extents == 0, "every remote extent is in use");

	rs_map_set_random(zero_rand);
	reset_map(2);
	expect(rs_map_node_add(1) == 0, "first node");
	expect(rs_map_node_add(2) == 0, "second node");
	expect(rs_map_chunk_add(1, 0, 1, base, 1, 1) == 0, "node 1 chunk");
	expect(rs_map_chunk_add(2, 1, 1, base + (1ull << 20), 2, 1) == 0, "node 2 chunk");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "first pop");
	expect(remote.node_id == 2, "lifo serves the extent claimed after the tie");
	rs_map_finish(0);
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0, "second pop");
	expect(remote.node_id == 1, "the tied pick stays underneath in the reserve");
	rs_map_finish(1ull << 20);

	reset_map(1);
	for (i = 0; i < 16; i++)
		expect(rs_map_node_add((u16)i) == 0, "fill the node table");
	expect(rs_map_node_add(16) == -ENOSPC, "17th node is rejected");
	expect(rs_map_node_add(0) == 0, "an existing node can be added again");

	reset_map(1);
	expect(rs_map_node_add(4) == 0, "node");
	expect(rs_map_chunk_add(4, 0, 1, base, 1, 1) == 0, "chunk");
	rs_map_test_set_gen(0, 0xffffffffu);
	expect(rs_map_chunk_update(0, 1, base, 1) == 0, "same identity does not bump gen");
	expect(rs_map_chunk_gen(0) == 0xffffffffu, "gen stays at the top");
	expect(rs_map_chunk_update(0, 2, base, 1) == 1, "epoch change wraps gen");
	expect(rs_map_chunk_gen(0) == 1, "gen skips zero");
}

int main(void)
{
	test_same_extent_and_discard();
	test_power_of_two();
	test_key_update_and_sizes();
	test_edges();
	test_limits_and_placement();
	rs_map_exit();
	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	printf("map tests ok\n");
	return 0;
}
