// SPDX-License-Identifier: GPL-2.0
/*
 * 两个线程同时给同一页的两份副本登记结果。
 * 结束条件只有一个：恰好一次 PUBLISH，或恰好一次 FAIL。
 * 5 秒出不来就当成死锁。
 */
#define _GNU_SOURCE
#include "map.c"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
	fprintf(stderr, "FAIL hedge race timed out\n");
	_exit(2);
}

struct race {
	atomic_uint word;
	atomic_int publishes;
	atomic_int fails;
	int success[RS_NR_COPIES];
};

struct lane {
	struct race *race;
	u32 which;
};

static void *lane_thread(void *arg)
{
	struct lane *lane = arg;

	for (;;) {
		u32 old, next;
		int act;

		old = atomic_load(&lane->race->word);
		if (old & RS_COPY_FINISH)
			return NULL;
		act = rs_read_arrive(old, lane->which,
				     lane->race->success[lane->which], &next);
		if (!atomic_compare_exchange_weak(&lane->race->word, &old, next))
			continue;
		if (act == RS_ACT_PUBLISH)
			atomic_fetch_add(&lane->race->publishes, 1);
		if (act == RS_ACT_FAIL)
			atomic_fetch_add(&lane->race->fails, 1);
		if (next & RS_COPY_FINISH)
			return NULL;
	}
}

static void run_once(int ok0, int ok1)
{
	struct race race = { 0 };
	struct lane lane0 = { .race = &race, .which = 0 };
	struct lane lane1 = { .race = &race, .which = 1 };
	pthread_t t0, t1;
	u32 word;

	race.success[0] = ok0;
	race.success[1] = ok1;
	word = rs_copy_mark_sent(rs_copy_mark_sent(0, 0), 1);
	atomic_store(&race.word, word);
	if (pthread_create(&t0, NULL, lane_thread, &lane0) ||
	    pthread_create(&t1, NULL, lane_thread, &lane1)) {
		expect(0, "pthread_create");
		return;
	}
	pthread_join(t0, NULL);
	pthread_join(t1, NULL);
	if (ok0 || ok1) {
		expect(atomic_load(&race.publishes) == 1, "one publish");
		expect(atomic_load(&race.fails) == 0, "success is not a fail");
	} else {
		expect(atomic_load(&race.publishes) == 0, "no data to publish");
		expect(atomic_load(&race.fails) == 1, "one fail");
	}
	expect(atomic_load(&race.word) & RS_COPY_FINISH, "word is closed");
}

int main(void)
{
	int i;

	signal(SIGALRM, on_alarm);
	alarm(5);
	for (i = 0; i < 2000; i++) {
		run_once(1, 0);
		run_once(0, 1);
		run_once(1, 1);
		run_once(0, 0);
	}
	alarm(0);
	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	printf("hedge race ok\n");
	return 0;
}
