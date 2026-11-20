// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

static uint64_t nsec(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

static uint64_t percentile(const uint64_t *samples, size_t n, unsigned numer,
			   unsigned denom)
{
	size_t idx = (n - 1) * numer / denom;

	return samples[idx];
}

int main(int argc, char **argv)
{
	size_t samples = 10000;
	size_t i;
	off_t bytes, pages;
	void *buf = NULL;
	uint64_t *lat;
	int fd, rc = 1;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <file> [samples]\n", argv[0]);
		return 2;
	}
	if (argc == 3) {
		samples = strtoul(argv[2], NULL, 10);
		if (samples < 1)
			return 2;
	}
	fd = open(argv[1], O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	bytes = lseek(fd, 0, SEEK_END);
	if (bytes < PAGE) {
		fprintf(stderr, "%s is smaller than 4KiB\n", argv[1]);
		goto out;
	}
	pages = bytes / PAGE;
	if (posix_memalign(&buf, PAGE, PAGE))
		goto out;
	lat = calloc(samples, sizeof(*lat));
	if (!lat)
		goto out;
	for (i = 0; i < samples; i++) {
		off_t off = (off_t)(((uint64_t)rand() % (uint64_t)pages) * PAGE);
		uint64_t t0 = nsec();

		if (pread(fd, buf, PAGE, off) != PAGE) {
			perror("pread");
			free(lat);
			goto out;
		}
		lat[i] = nsec() - t0;
	}
	qsort(lat, samples, sizeof(*lat), cmp_u64);
	printf("samples %zu p50 %llu p95 %llu p99 %llu p999 %llu\n", samples,
	       (unsigned long long)percentile(lat, samples, 50, 100),
	       (unsigned long long)percentile(lat, samples, 95, 100),
	       (unsigned long long)percentile(lat, samples, 99, 100),
	       (unsigned long long)percentile(lat, samples, 999, 1000));
	free(lat);
	rc = 0;
out:
	free(buf);
	close(fd);
	return rc;
}
