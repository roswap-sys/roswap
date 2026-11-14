// SPDX-License-Identifier: GPL-2.0
#include "map.c"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;

static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "FAIL map race timed out\n");
	_exit(2);
}

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

static void commit(u64 off, const struct rs_copies *c)
{
	rs_map_finish_write(off, c->mask);
}

static void test_copy_word(void)
{
	u32 w, again;

	w = rs_copy_note(0, 0, true);
	expect(!rs_copy_settled(w, 0x3), "copy 0 alone is not both");
	expect(rs_copy_settled(w, 0x1), "copy 0 settles a single send");
	expect(rs_copy_ok(w) == 0x1, "copy 0 succeeded");

	w = rs_copy_note(w, 1, false);
	expect(rs_copy_settled(w, 0x3), "both copies arrived");
	expect(rs_copy_ok(w) == 0x1, "the failed copy stays failed");
	again = rs_copy_note(w, 1, true);
	expect(again == w, "a second arrival is ignored");
	expect(rs_copy_ok(again) == 0x1, "a duplicate cannot turn failure into success");

	w = rs_copy_note(0, 1, true);
	expect(!rs_copy_settled(w, 0x3), "copy 1 first still waits");
	expect(rs_copy_settled(w, 0x2), "only the sent copy needs to arrive");
	w = rs_copy_note(w, 0, true);
	expect(rs_copy_settled(w, 0x3) && rs_copy_ok(w) == 0x3, "reverse order commits both");

	w = rs_copy_note(0, 0, false);
	expect(rs_copy_settled(w, 0x1) && rs_copy_ok(w) == 0, "the only send failed");
	expect(!rs_copy_settled(w, 0x3), "one arrival does not cover two sends");

	expect(rs_copy_note(0x15, 2, true) == 0x15, "which 2 is ignored");
	expect(rs_copy_note(0x15, 99, false) == 0x15, "a large which is ignored");
}

static void test_hedge_word(void)
{
	u32 w, out;

	expect(rs_copy_mark_sent(0x15, 2) == 0x15, "mark sent ignores which 2");
	w = rs_copy_mark_sent(0, 0);
	expect(rs_copy_sent_mask(w) == 0x1, "copy 0 is sent");
	w = rs_copy_note(w, 0, true);
	expect(rs_copy_sent_mask(w) == 0x1, "arrival keeps the sent bit");
	expect(rs_copy_ok(w) == 0x1, "arrival records success");
	expect(!rs_hedge_should_post(w, 1, true), "arrived primary is not hedged");
	expect(rs_copy_flush_which(w) == -1, "a single success has nothing to flush");
	expect(rs_copy_claim_finish(w, &out) && (out & RS_COPY_FINISH),
	       "one sent copy can finish");
	expect(!rs_copy_claim_finish(out, &out), "finish is claimed once");

	w = rs_copy_mark_sent(0, 0);
	expect(!rs_hedge_should_post(w, 1, false), "hedge waits for the deadline");
	expect(rs_hedge_should_post(w, 1, true), "overdue primary can hedge");
	expect(!rs_hedge_should_post(w, 0, true), "the sent copy is not the hedge");
	expect(!rs_hedge_should_post(w, 2, true), "which 2 is not a hedge");
	w = rs_copy_mark_sent(w, 1);
	expect(!rs_hedge_should_post(w, 1, true), "hedge is posted once");
	expect(rs_copy_flush_which(w) == -1, "no success yet, do not flush");
	expect(!rs_copy_claim_finish(w, &out), "a pending hedge blocks finish");

	w = rs_copy_note(w, 0, true);
	expect(rs_copy_flush_which(w) == 1, "flush the outstanding hedge");
	expect(!rs_copy_claim_finish(w, &out), "hedge arrival is still required");
	w = rs_copy_note(w, 1, false);
	expect(rs_copy_ok(w) == 0x1, "flush failure keeps the good copy");
	expect(rs_copy_flush_which(w) == -1, "both arrivals clear the flush");
	expect(rs_copy_claim_finish(w, &out), "both arrivals can finish");
	expect(!rs_hedge_should_post(out, 1, true), "a finished word is closed");

	w = rs_copy_mark_sent(rs_copy_mark_sent(0, 0), 1);
	w = rs_copy_note(w, 1, true);
	expect(rs_copy_flush_which(w) == 0, "flush the outstanding primary");
	expect(!rs_copy_claim_finish(0, &out), "nothing sent cannot finish");
	expect(!rs_copy_claim_finish(w, NULL), "claim needs an output");
}

static void test_read_arrive(void)
{
	u32 w, out;
	int act;

	w = rs_copy_mark_sent(rs_copy_mark_sent(0, 0), 1);
	act = rs_read_arrive(w, 0, true, &out);
	expect(act == RS_ACT_PUBLISH && (out & RS_COPY_FINISH),
	       "first success publishes");
	expect(rs_copy_ok(out) == 0x1, "publish records copy 0");
	act = rs_read_arrive(out, 1, true, &out);
	expect(act == RS_ACT_WAIT && rs_copy_ok(out) == 0x1,
	       "a finished word ignores the late copy");

	w = rs_copy_mark_sent(rs_copy_mark_sent(0, 0), 1);
	act = rs_read_arrive(w, 0, false, &out);
	expect(act == RS_ACT_WAIT, "one failure still waits");
	act = rs_read_arrive(out, 1, false, &out);
	expect(act == RS_ACT_FAIL && (out & RS_COPY_FINISH),
	       "both failures finish the read");
	expect(rs_copy_ok(out) == 0, "fail publish has no data");

	w = rs_copy_mark_sent(0, 0);
	act = rs_read_arrive(w, 0, true, &out);
	expect(act == RS_ACT_PUBLISH, "the only copy publishes");
	act = rs_read_arrive(w, 0, true, &out);
	expect(act == RS_ACT_PUBLISH, "same input publishes again");
	act = rs_read_arrive(out, 0, false, &out);
	expect(act == RS_ACT_WAIT, "duplicate arrival does not fail a publish");
	expect(rs_read_arrive(w, 2, true, &out) == RS_ACT_WAIT, "which 2 is ignored");
	expect(out == w, "ignored which leaves the word");
}

static void test_same_extent_and_discard(void)
{
	struct rs_copies a, b;
	int rc;

	reset_map(2);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 3, 0x100000000ULL, 0xabc, 2) == 0,
	       "chunk");
	rc = rs_map_prepare_write(0, 4096, &a);
	expect(rc == 0, "write page 0");
	rc = rs_map_prepare_write(4096, 4096, &b);
	expect(rc == 0, "write page 1");
	expect(a.r[0].rkey == 0xabc && b.r[0].rkey == 0xabc, "rkey");
	expect(a.r[0].addr + 4096 == b.r[0].addr, "adjacent pages share the extent");
	expect(((a.r[0].addr - 0x100000000ULL) & ((1ULL << 20) - 1)) == 0, "extent aligned");
	expect(a.r[0].addr >= 0x100000000ULL && a.r[0].addr < 0x100000000ULL + (2ULL << 20),
	       "inside the chunk");
	commit(0, &a);
	commit(4096, &b);

	expect(rs_map_lookup_read(0, 4096, &a) == 0, "read page 0");
	rs_map_finish(0);
	expect(rs_map_discard(0, 4096) == 0, "discard one page");
	expect(rs_map_lookup_read(0, 4096, &a) == RS_MAP_ZERO, "discarded page is zero");
	expect(rs_map_lookup_read(4096, 4096, &b) == 0, "sibling page stays");
	rs_map_finish(4096);
	expect(rs_map_discard(4096, 4096) == 0, "discard sibling");
	expect(rs_map_lookup_read(4096, 4096, &b) == RS_MAP_ZERO, "extent freed");
	expect(rs_map_prepare_write(0, 4096, &a) == 0, "reuse after discard");
	commit(0, &a);
}

static void test_power_of_two(void)
{
	struct rs_copies remote, again;
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
		expect(again.r[0].addr == remote.r[0].addr + 4096, "same remote extent");
		expect(remote.mask == 0x1, "single mode places one copy");
		expect(remote.r[0].node_id == 1 || remote.r[0].node_id == 2, "known node");
		if (remote.r[0].node_id == 1)
			on_small++;
		else
			on_large++;
		commit(off, &remote);
		commit(off + 4096, &again);
	}
	expect(on_large == 8 && on_small == 1, "larger node takes eight extents");

	rs_map_stats(&st);
	expect(st.nr_nodes == 2 && st.nr_chunks == 2, "stats nodes");
	expect(st.used_slots == 18, "two pages in each extent");
	expect(st.free_extents == 0, "backing extents are all in use");
	expect(st.degraded_slots == 0, "single mode has nothing to rebuild");
}

static void test_key_update_and_sizes(void)
{
	struct rs_copies remote;
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
	gen = remote.r[0].gen;
	expect(rs_map_chunk_update(4, 0, 1, 0x1000, 1) == 0, "same key");
	expect(rs_map_chunk_gen(0) == gen, "gen stable");
	expect(rs_map_chunk_update(4, 0, 9, 0x5555, 77) == 1, "new key");
	expect(rs_map_chunk_gen(0) != gen, "gen bumped");
	commit(0, &remote);
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "read new key");
	expect(remote.r[0].rkey == 77, "updated rkey");
	expect(remote.r[0].addr >= 0x5555 &&
	       ((remote.r[0].addr - 0x5555) & ((1ULL << 20) - 1)) == 0,
	       "updated base");
	expect(remote.r[0].gen == rs_map_chunk_gen(0), "gen matches");
	rs_map_finish(0);
	expect(rs_map_chunk_update(4, 9, 1, 1, 1) == -ENOENT, "unknown chunk");
	expect(rs_map_prepare_write(0, 512, &remote) == -EINVAL, "short write");
	expect(rs_map_lookup_read(1ULL << 20, 4096, &remote) == -EINVAL, "past end");
}

static u32 zero_rand(void)
{
	return 0;
}

static void test_edges(void)
{
	struct rs_copies remote;
	struct rs_map_stats st;
	u16 node = 99;
	const u64 base = 0x100000000ULL;

	rs_map_exit();
	expect(rs_map_init(0) == -EINVAL, "reject zero capacity");
	expect(rs_capacity_ok(0) == -EINVAL, "reject zero capacity");
	expect(rs_capacity_ok((16u << 20) + 1) == -EINVAL, "reject above 16TiB");
	expect(rs_capacity_ok(16u << 20) == -EINVAL, "16TiB does not fit in a page number");
	expect(rs_capacity_ok(RS_MAX_CAPACITY_MB) == 0, "largest fitting capacity");
	expect(rs_capacity_ok(1u << 20) == 0, "1TiB fits");
	expect(rs_map_node_add(1) == -ENODEV, "node before init");
	expect(rs_map_chunk_add(1, 0, 1, 1, 1, 1) == -ENODEV, "chunk before init");
	expect(rs_map_chunk_update(0, 0, 1, 1, 1) == -ENODEV, "update before init");
	expect(rs_map_prepare_write(0, 4096, &remote) == -EINVAL, "write before init");
	expect(rs_map_lookup_read(0, 4096, &remote) == -EINVAL, "read before init");
	expect(rs_map_discard(0, 4096) == -EINVAL, "discard before init");
	expect(rs_map_set_protection(RS_PROT_SINGLE) == -ENODEV, "protection before init");
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
	expect(st.degraded_slots == 0, "nothing degraded yet");
	expect(rs_map_chunk_add(9, 0, 1, base, 1, 1) == -ENOENT, "chunk needs a node");
	expect(rs_map_chunk_add(1, 64, 1, base, 1, 1) == 0, "local gid 64");
	{
		u32 global = 0;

		expect(rs_map_chunk_find(1, 64, &global) == 0, "find local gid 64");
		expect(rs_map_chunk_node(global, &node) == 0 && node == 1, "chunk node");
	}
	expect(rs_map_chunk_node(99, &node) == -ENOENT, "missing chunk node");
	expect(rs_map_chunk_node(63, NULL) == -EINVAL, "null node out");
	expect(rs_map_node_add(1) == 0, "idempotent node");
	expect(rs_map_lookup_read(0, 4096, &remote) == RS_MAP_ZERO, "read allocates nothing");
	rs_map_stats(&st);
	expect(st.free_extents == 1 && st.used_slots == 0, "read left the extent free");
	expect(rs_map_prepare_write(1, 4096, &remote) == -EINVAL, "unaligned offset");
	expect(rs_map_prepare_write(0, 0, &remote) == -EINVAL, "zero length");
	expect(rs_map_prepare_write(0, 8192, &remote) == -EINVAL, "write is one page");
	expect(rs_map_prepare_write(0, 4096, NULL) == -EINVAL, "null write out");
	expect(rs_map_lookup_read(0, 4096, NULL) == -EINVAL, "null read out");
	expect(rs_map_discard(4096, 100) == -EINVAL, "unaligned discard length");

	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "first page");
	expect(remote.r[0].addr == base, "the only extent is index 0");
	expect(rs_map_prepare_write(0, 4096, &remote) == -EBUSY, "busy write");
	expect(rs_map_lookup_read(0, 4096, &remote) == -EBUSY, "busy read");
	expect(rs_map_discard(0, 4096) == -EBUSY, "busy discard");
	commit(0, &remote);
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "read after finish");
	rs_map_finish(0);
	expect(rs_map_prepare_write(255u * 4096, 4096, &remote) == 0, "last page");
	expect(remote.r[0].addr == base + 255u * 4096, "same extent, last page");
	commit(255u * 4096, &remote);
	expect(rs_map_prepare_write(256ull * 4096, 4096, &remote) == -EINVAL, "past the disk");
	rs_map_stats(&st);
	expect(st.used_slots == 2 && st.free_extents == 0, "one extent holds both pages");

	expect(rs_map_prepare_write(4096, 4096, &remote) == 0, "middle page stays busy");
	expect(rs_map_discard(0, 3 * 4096) == -EBUSY, "one busy page blocks the range");
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "earlier page was not discarded");
	rs_map_finish(0);
	rs_map_finish_write(4096, 0x1);
	expect(rs_map_discard(0, 4096) == 0, "discard the first page only");
	rs_map_stats(&st);
	expect(st.used_slots == 2 && st.free_extents == 0, "extent stays while a page remains");
	expect(rs_map_discard(4096, 4096) == 0, "discard the middle page");
	expect(rs_map_discard(255u * 4096, 4096) == 0, "discard the last page");
	expect(rs_map_lookup_read(255u * 4096, 4096, &remote) == RS_MAP_ZERO, "extent returned");
	rs_map_stats(&st);
	expect(st.used_slots == 0 && st.free_extents == 1, "extent is free again");
	expect(st.degraded_slots == 0, "discard drops the degraded count");
}

static void test_limits_and_placement(void)
{
	struct rs_copies remote;
	struct rs_map_stats st;
	const u64 base = 0x200000000ULL;
	u32 i;

	reset_map(2);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 3, 1) == 0, "one remote extent");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "consume it");
	commit(0, &remote);
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == -ENOSPC,
	       "no remote extent for the next logical extent");
	rs_map_stats(&st);
	expect(st.used_slots == 1 && st.free_extents == 0, "first extent stayed allocated");

	reset_map(2);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 3, 2) == 0, "two remote extents");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "first logical extent");
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0, "second logical extent");
	commit(0, &remote);
	commit(1ull << 20, &remote);
	expect(rs_map_discard(0, 2u << 20) == 0, "discard both logical extents");
	expect(rs_map_lookup_read(0, 4096, &remote) == RS_MAP_ZERO, "first cleared");
	expect(rs_map_lookup_read(1ull << 20, 4096, &remote) == RS_MAP_ZERO, "second cleared");
	rs_map_stats(&st);
	expect(st.free_extents == 2 && st.used_slots == 0, "both extents returned");

	reset_map(8);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 5, 8) == 0, "eight extents");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "lifo first");
	expect(remote.r[0].addr == base + (7ull << 20), "reserve pops the last refilled extent");
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0, "lifo second");
	expect(remote.r[0].addr == base + (6ull << 20), "next pop walks backward");
	commit(0, &remote);
	commit(1ull << 20, &remote);
	expect(rs_map_discard(0, 4096) == 0, "return the first loc to the reserve");
	expect(rs_map_prepare_write(2ull << 20, 4096, &remote) == 0, "reuse");
	expect(remote.r[0].addr == base + (7ull << 20), "released loc is reused first");
	commit(2ull << 20, &remote);
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
		expect(remote.r[0].addr == base + ((u64)ext << 20), "lifo batches of eight");
		commit((u64)i << 20, &remote);
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
	expect(remote.r[0].node_id == 2, "lifo serves the extent claimed after the tie");
	commit(0, &remote);
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0, "second pop");
	expect(remote.r[0].node_id == 1, "the tied pick stays underneath in the reserve");
	commit(1ull << 20, &remote);

	reset_map(1);
	for (i = 0; i < RS_MAX_NODES; i++)
		expect(rs_map_node_add((u16)i) == 0, "fill the node table");
	expect(rs_map_node_add(RS_MAX_NODES) == -ENOSPC, "one past the node table");
	expect(rs_map_node_add(0) == 0, "an existing node can be added again");

	reset_map(1);
	expect(rs_map_node_add(4) == 0, "node");
	expect(rs_map_chunk_add(4, 0, 1, base, 1, 1) == 0, "chunk");
	rs_map_test_set_gen(0, 0xffffffffu);
	expect(rs_map_chunk_update(4, 0, 1, base, 1) == 0, "same identity does not bump gen");
	expect(rs_map_chunk_gen(0) == 0xffffffffu, "gen stays at the top");
	expect(rs_map_chunk_update(4, 0, 2, base, 1) == 1, "epoch change wraps gen");
	expect(rs_map_chunk_gen(0) == 1, "gen skips zero");
}

static void test_replica2(void)
{
	struct rs_copies a, b, rd;
	struct rs_map_stats st;
	const u64 base0 = 0x300000000ULL;
	const u64 base1 = 0x400000000ULL;

	expect(rs_map_set_protection(9) == -EINVAL, "unknown protection");

	reset_map(4);
	expect(rs_map_set_protection(RS_PROT_REPLICA2) == 0, "enable replica2");
	expect(rs_map_protection() == RS_PROT_REPLICA2, "mode sticks");
	expect(rs_map_node_add(1) == 0, "only node");
	expect(rs_map_chunk_add(1, 0, 1, base0, 7, 2) == 0, "one node, two extents");
	expect(rs_map_chunk_add(1, 1, 1, base0 + (8ull << 20), 8, 2) == 0,
	       "second chunk on the same node");
	expect(rs_map_prepare_write(0, 4096, &a) == 0, "degrade to one copy");
	expect(a.mask == 0x1, "same node cannot hold both copies");
	commit(0, &a);
	rs_map_stats(&st);
	expect(st.degraded_slots == 1, "a single replica2 copy waits for rebuild");
	expect(rs_map_set_protection(RS_PROT_SINGLE) == -EBUSY, "mode is frozen after a write");
	expect(rs_map_protection() == RS_PROT_REPLICA2, "rejected switch leaves replica2");

	reset_map(2);
	expect(rs_map_set_protection(RS_PROT_REPLICA2) == 0, "replica2 again");
	expect(rs_map_node_add(1) == 0, "node 1");
	expect(rs_map_node_add(2) == 0, "node 2");
	expect(rs_map_chunk_add(1, 0, 1, base0, 11, 1) == 0, "node 1 extent");
	expect(rs_map_chunk_add(2, 1, 1, base1, 22, 1) == 0, "node 2 extent");
	rs_map_stats(&st);
	expect(st.free_extents == 2, "two remote extents before placement");
	expect(rs_map_prepare_write(0, 4096, &a) == 0, "place a pair");
	expect(a.mask == 0x3, "both copies are bound");
	expect(a.r[0].node_id != a.r[1].node_id, "copies land on different nodes");
	expect(rs_map_prepare_write(4096, 4096, &b) == 0, "sibling page");
	expect(b.mask == 0x3, "sibling keeps both copies");
	expect(b.r[0].addr == a.r[0].addr + 4096, "copy 0 stays in its extent");
	expect(b.r[1].addr == a.r[1].addr + 4096, "copy 1 stays in its extent");
	rs_map_stats(&st);
	expect(st.free_extents == 0, "the pair consumed both remote extents");
	expect(rs_map_prepare_write(1ull << 20, 4096, &b) == -ENOSPC,
	       "no pair left for the next logical extent");
	commit(0, &a);
	commit(4096, &b);
	expect(rs_map_discard(0, 8192) == 0, "discard the pair");
	rs_map_stats(&st);
	expect(st.free_extents == 2 && st.used_slots == 0, "both copies came back");
	expect(st.degraded_slots == 0, "a full pair is not degraded");

	reset_map(4);
	expect(rs_map_set_protection(RS_PROT_REPLICA2) == 0, "replica2 for degrade");
	expect(rs_map_node_add(1) == 0, "node 1");
	expect(rs_map_node_add(2) == 0, "node 2");
	expect(rs_map_chunk_add(1, 0, 1, base0, 11, 2) == 0, "node 1 chunk");
	expect(rs_map_chunk_add(2, 1, 1, base1, 22, 2) == 0, "node 2 chunk");
	expect(rs_map_prepare_write(0, 4096, &a) == 0, "write both");
	expect(a.mask == 0x3, "two live nodes bind two copies");
	rs_map_finish_write(0, 0x2);
	rs_map_stats(&st);
	expect(st.degraded_slots == 1, "one successful copy is degraded");
	expect(rs_map_lookup_read(0, 4096, &rd) == 0, "read the surviving copy");
	expect(rd.mask == 0x2, "the failed copy is not readable");
	expect(rd.r[1].node_id == a.r[1].node_id, "read returns copy 1");
	expect(rd.r[1].addr == a.r[1].addr, "read returns that page");
	rs_map_finish(0);

	expect(rs_map_prepare_write(0, 4096, &a) == 0, "rewrite both");
	rs_map_finish_write(0, 0x3);
	rs_map_stats(&st);
	expect(st.degraded_slots == 0, "both copies clear the degraded count");
	expect(rs_map_lookup_read(0, 4096, &rd) == 0, "both copies are readable");
	expect(rd.mask == 0x3, "read mask is both");
	rs_map_finish(0);

	expect(rs_map_prepare_write(0, 4096, &a) == 0, "rewrite nothing");
	rs_map_finish_write(0, 0);
	expect(rs_map_lookup_read(0, 4096, &rd) == -EIO, "no surviving copy");
	rs_map_stats(&st);
	expect(st.degraded_slots == 1, "a lost page stays degraded");
	expect(rs_map_discard(0, 4096) == 0, "discard the lost page");
	rs_map_stats(&st);
	expect(st.degraded_slots == 0, "discard forgets the lost page");
	expect(rs_map_lookup_read(0, 4096, &rd) == RS_MAP_ZERO, "discarded page is zero");

	rs_map_finish_write(1, 0x3);
	rs_map_finish_write((u64)1 << 40, 0x3);
	rs_map_stats(&st);
	expect(st.used_slots == 0 && st.degraded_slots == 0, "a bad offset changes nothing");
}

static void test_same_local_gid(void)
{
	struct rs_copies remote;
	u32 g0 = 0, g1 = 0, gen0;

	reset_map(2);
	expect(rs_map_node_add(1) == 0 && rs_map_node_add(2) == 0, "two nodes");
	expect(rs_map_chunk_add(1, 0, 1, 0x1000, 11, 1) == 0, "node 1 local 0");
	expect(rs_map_chunk_add(2, 0, 1, 0x8000, 22, 1) == 0, "node 2 local 0");
	expect(rs_map_chunk_find(1, 0, &g0) == 0, "find node 1");
	expect(rs_map_chunk_find(2, 0, &g1) == 0, "find node 2");
	expect(g0 != g1, "the same local gid gets two global ids");
	gen0 = rs_map_chunk_gen(g0);
	expect(rs_map_chunk_update(2, 0, 9, 0x9000, 77) == 1, "update node 2");
	expect(rs_map_chunk_gen(g0) == gen0, "node 1 keeps its generation");
	expect(rs_map_chunk_gen(g1) != gen0, "node 2 generation moved");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "place");
	commit(0, &remote);
	expect(rs_map_lookup_read(0, 4096, &remote) == 0, "read");
	if (remote.r[0].node_id == 2) {
		expect(remote.r[0].rkey == 77, "updated rkey is node 2");
		expect(remote.r[0].addr == 0x9000, "updated base is node 2");
	} else {
		expect(remote.r[0].rkey == 11, "node 1 rkey was not rewritten");
		expect(remote.r[0].addr == 0x1000, "node 1 base was not rewritten");
	}
	rs_map_finish(0);
	expect(rs_map_chunk_find(3, 0, &g0) == -ENOENT, "missing node");
	expect(rs_map_chunk_find(1, 0, NULL) == -EINVAL, "null global out");
}

static void test_steal_reserve(void)
{
	struct rs_copies remote;
	const u64 base = 0x500000000ULL;

	rs_map_test_set_cpu(0);
	reset_map(16);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, base, 5, 8) == 0, "eight extents");
	expect(rs_map_prepare_write(0, 4096, &remote) == 0, "cpu 0 takes one");
	rs_map_test_set_cpu(1);
	expect(rs_map_prepare_write(1ull << 20, 4096, &remote) == 0,
	       "cpu 1 steals a reserved extent");
	expect(remote.r[0].addr != base + (7ull << 20), "the stolen extent is not cpu 0's");
	commit(0, &remote);
	commit(1ull << 20, &remote);
	rs_map_test_set_cpu(0);
}

struct rs_race_job {
	int cpu;
	u32 base;
	u32 n;
	int fail;
};

static void *rs_race_worker(void *arg)
{
	struct rs_race_job *job = arg;
	u32 i;

	rs_map_test_set_cpu(job->cpu);
	for (i = 0; i < job->n; i++) {
		struct rs_copies remote;
		u64 off = ((u64)job->base + i) << 20;

		if (rs_map_prepare_write(off, 4096, &remote))
			job->fail = 1;
		else
			rs_map_finish_write(off, remote.mask);
		if (rs_map_lookup_read(off, 4096, &remote))
			job->fail = 1;
		else
			rs_map_finish(off);
		if (rs_map_discard(off, 4096))
			job->fail = 1;
	}
	return NULL;
}

static void test_concurrent_map(void)
{
	struct rs_race_job jobs[4];
	pthread_t threads[4];
	struct rs_map_stats st;
	int i;

	if (signal(SIGALRM, on_alarm) == SIG_ERR)
		expect(0, "alarm");
	reset_map(32);
	expect(rs_map_node_add(1) == 0, "node");
	expect(rs_map_chunk_add(1, 0, 1, 0x600000000ULL, 3, 32) == 0, "chunk");
	alarm(5);
	for (i = 0; i < 4; i++) {
		jobs[i].cpu = i;
		jobs[i].base = (u32)i * 8;
		jobs[i].n = 8;
		jobs[i].fail = 0;
		expect(pthread_create(&threads[i], NULL, rs_race_worker, &jobs[i]) == 0,
		       "start map racer");
	}
	for (i = 0; i < 4; i++) {
		expect(pthread_join(threads[i], NULL) == 0, "join map racer");
		expect(!jobs[i].fail, "racer io");
	}
	alarm(0);
	rs_map_stats(&st);
	expect(st.used_slots == 0 && st.free_extents == 32, "racers returned every extent");
	rs_map_test_set_cpu(0);
}

int main(void)
{
	test_copy_word();
	test_hedge_word();
	test_read_arrive();
	test_same_extent_and_discard();
	test_power_of_two();
	test_key_update_and_sizes();
	test_edges();
	test_limits_and_placement();
	test_replica2();
	test_same_local_gid();
	test_steal_reserve();
	test_concurrent_map();
	rs_map_exit();
	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	printf("map tests ok\n");
	return 0;
}
