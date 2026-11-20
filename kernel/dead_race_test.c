// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include "map.c"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int failures;

static void expect(int cond, const char *what)
{
	if (cond)
		return;
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "FAIL dead race timed out\n");
	_exit(2);
}

struct io_job {
	u32 base;
	u32 n;
	int cpu;
	int fail;
};

static void *io_thread(void *arg)
{
	struct io_job *job = arg;
	u32 i;

	rs_map_test_set_cpu(job->cpu);
	for (i = 0; i < job->n; i++) {
		struct rs_copies remote;
		u64 off = ((u64)job->base + i) << 12;

		if (rs_map_prepare_write(off, 4096, &remote)) {
			job->fail++;
			continue;
		}
		rs_map_finish_write(off, 0x3);
	}
	return NULL;
}

static void *flip_thread(void *arg)
{
	u32 i;

	(void)arg;
	for (i = 0; i < 200; i++) {
		rs_map_node_set_state(2, (i & 1) ? RS_NODE_SUSPECT : RS_NODE_UP);
		usleep(100);
	}
	rs_map_node_kill(2, NULL);
	return NULL;
}

static void check_dead_invariant(u16 dead)
{
	struct rs_map_stats st;
	u32 lost = 0, degraded = 0, slot;

	rs_map_stats(&st);
	for (slot = 0; slot < 64; slot++) {
		struct rs_copies remote;
		u64 off = (u64)slot << 12;
		int ret = rs_map_lookup_read(off, 4096, &remote);
		u32 c;

		if (ret == RS_MAP_ZERO)
			continue;
		if (ret == -EIO) {
			lost++;
			continue;
		}
		if (ret)
			continue;
		for (c = 0; c < RS_NR_COPIES; c++) {
			if (!(remote.mask & (1u << c)))
				continue;
			if (remote.r[c].node_id == dead)
				expect(0, "a dead node is still readable");
		}
		if (remote.mask && remote.mask != 0x3)
			degraded++;
		rs_map_finish(off);
	}
	expect(st.lost_slots == lost, "lost counter matches the scan");
	expect(st.degraded_slots == degraded, "degraded counter matches the scan");
}

static void test_kill_versus_commit(void)
{
	struct io_job jobs[2] = {
		{ .base = 0, .n = 32, .cpu = 0 },
		{ .base = 32, .n = 32, .cpu = 1 },
	};
	pthread_t io[2];
	struct rs_copies remote;
	int i;

	expect(rs_map_init(1) == 0, "init");
	expect(rs_map_set_protection(RS_PROT_REPLICA2) == 0, "replica2");
	expect(rs_map_node_add(1) == 0 && rs_map_node_add(2) == 0, "nodes");
	expect(rs_map_chunk_add(1, 0, 1, 0x100000000ULL, 1, 8) == 0, "node 1");
	expect(rs_map_chunk_add(2, 0, 1, 0x200000000ULL, 2, 8) == 0, "node 2");
	alarm(3);
	for (i = 0; i < 2; i++)
		expect(pthread_create(&io[i], NULL, io_thread, &jobs[i]) == 0, "io");
	usleep(1000);
	expect(rs_map_node_kill(2, NULL) == 0, "kill during commit");
	for (i = 0; i < 2; i++)
		expect(pthread_join(io[i], NULL) == 0, "join io");
	alarm(0);
	check_dead_invariant(2);
	if (rs_map_prepare_write(200ull << 12, 4096, &remote) == 0) {
		u32 c;

		for (c = 0; c < RS_NR_COPIES; c++) {
			if (remote.mask & (1u << c))
				expect(remote.r[c].node_id != 2, "new io skips the dead node");
		}
		rs_map_finish_write(200ull << 12, remote.mask);
	}
	rs_map_exit();
}

static void test_flip_versus_place(void)
{
	struct io_job job = { .base = 0, .n = 16, .cpu = 0 };
	pthread_t io, flip;
	struct rs_copies remote;

	expect(rs_map_init(1) == 0, "init");
	expect(rs_map_node_add(1) == 0 && rs_map_node_add(2) == 0, "nodes");
	expect(rs_map_chunk_add(1, 0, 1, 0x100000000ULL, 1, 4) == 0, "node 1");
	expect(rs_map_chunk_add(2, 0, 1, 0x200000000ULL, 2, 4) == 0, "node 2");
	alarm(3);
	expect(pthread_create(&io, NULL, io_thread, &job) == 0, "io");
	expect(pthread_create(&flip, NULL, flip_thread, NULL) == 0, "flip");
	expect(pthread_join(io, NULL) == 0, "join io");
	expect(pthread_join(flip, NULL) == 0, "join flip");
	alarm(0);
	expect(rs_map_node_state(2) == RS_NODE_DEAD, "flip ended dead");
	if (rs_map_prepare_write(100ull << 12, 4096, &remote) == 0) {
		u32 c;

		for (c = 0; c < RS_NR_COPIES; c++) {
			if (remote.mask & (1u << c))
				expect(remote.r[c].node_id != 2, "placement skipped dead");
		}
		rs_map_finish_write(100ull << 12, remote.mask);
	}
	check_dead_invariant(2);
	rs_map_exit();
}

int main(void)
{
	if (signal(SIGALRM, on_alarm) == SIG_ERR)
		return 2;
	test_kill_versus_commit();
	test_flip_versus_place();
	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	printf("dead race ok\n");
	return 0;
}
