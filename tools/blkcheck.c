// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096
#define MAGIC 0x525357504154544eULL

enum {
	MODE_SMOKE = 0,
	MODE_VERIFY,
	MODE_ZERO,
	MODE_DISCARD,
	MODE_ENOSPC,
	MODE_STRESS,
};

static int fail(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return 1;
}

static int xfer(int fd, void *buf, size_t len, off_t off, int write)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = write ? pwrite(fd, p, len, off) : pread(fd, p, len, off);

		if (n < 0)
			return -1;
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static void fill_page(uint8_t *buf, off_t off, uint32_t seed)
{
	uint64_t magic = MAGIC;
	uint64_t at = (uint64_t)off;

	memset(buf, (uint8_t)seed, PAGE);
	memcpy(buf, &magic, sizeof(magic));
	memcpy(buf + 8, &at, sizeof(at));
	memcpy(buf + 16, &seed, sizeof(seed));
}

static int page_ok(const uint8_t *buf, off_t off, uint32_t seed)
{
	uint8_t expect[PAGE];

	fill_page(expect, off, seed);
	return memcmp(buf, expect, PAGE) == 0;
}

static int check_page(int fd, uint8_t *buf, uint8_t *got, off_t off, uint32_t seed)
{
	fill_page(buf, off, seed);
	if (xfer(fd, buf, PAGE, off, 1) < 0)
		return fail("pwrite");
	memset(got, 0, PAGE);
	if (xfer(fd, got, PAGE, off, 0) < 0)
		return fail("pread");
	if (!page_ok(got, off, seed)) {
		fprintf(stderr, "mismatch at offset %lld\n", (long long)off);
		return 1;
	}
	return 0;
}

static int read_sysfs_ull(const char *path, unsigned long long *out)
{
	FILE *f;
	int n;

	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fscanf(f, "%llu", out);
	fclose(f);
	if (n != 1) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int discard_range(int fd, uint64_t start, uint64_t len)
{
	uint64_t range[2] = { start, len };

	if (ioctl(fd, BLKDISCARD, range) < 0)
		return fail("BLKDISCARD");
	return 0;
}

static int expect_zero(int fd, uint8_t *got, off_t off)
{
	size_t i;

	memset(got, 0x5a, PAGE);
	if (xfer(fd, got, PAGE, off, 0) < 0)
		return fail("pread zero");
	for (i = 0; i < PAGE; i++) {
		if (got[i] == 0)
			continue;
		fprintf(stderr, "offset %lld byte %zu is %#x, want 0\n",
			(long long)off, i, got[i]);
		return 1;
	}
	return 0;
}

static int smoke(int fd, const char *path, unsigned long long nbytes)
{
	const char *name = strrchr(path, '/');
	char sys[128];
	unsigned long long logical, physical, rotational, gran;
	uint8_t *buf, *got;
	int ss, pss, ret;
	off_t mid, end;

	name = name ? name + 1 : path;
	if (ioctl(fd, BLKSSZGET, &ss) < 0 || ioctl(fd, BLKPBSZGET, &pss) < 0)
		return fail("ioctl");
	if (ss != 4096 || pss != 4096) {
		fprintf(stderr, "block size logical %d physical %d\n", ss, pss);
		return 1;
	}
	if (nbytes < 2 * 1024 * 1024 || nbytes % 4096) {
		fprintf(stderr, "unexpected size %llu\n", nbytes);
		return 1;
	}
	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/logical_block_size", name);
	if (read_sysfs_ull(sys, &logical) < 0)
		return fail(sys);
	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/physical_block_size", name);
	if (read_sysfs_ull(sys, &physical) < 0)
		return fail(sys);
	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/rotational", name);
	if (read_sysfs_ull(sys, &rotational) < 0)
		return fail(sys);
	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/discard_granularity", name);
	if (read_sysfs_ull(sys, &gran) < 0)
		return fail(sys);
	if (logical != 4096 || physical != 4096 || rotational != 0 || gran != (1U << 20)) {
		fprintf(stderr,
			"queue logical %llu physical %llu rotational %llu discard %llu\n",
			logical, physical, rotational, gran);
		return 1;
	}
	if (posix_memalign((void **)&buf, PAGE, PAGE) ||
	    posix_memalign((void **)&got, PAGE, PAGE))
		return fail("posix_memalign");
	end = nbytes - PAGE;
	mid = (nbytes / 2) & ~(off_t)4095;
	ret = check_page(fd, buf, got, 0, 0x10);
	if (!ret)
		ret = check_page(fd, buf, got, mid, 0x20);
	if (!ret)
		ret = check_page(fd, buf, got, end, 0x30);
	if (!ret && pwrite(fd, buf, PAGE, nbytes) >= 0) {
		fprintf(stderr, "write past end succeeded\n");
		ret = 1;
	} else if (!ret) {
		errno = 0;
	}
	free(buf);
	free(got);
	if (!ret)
		printf("ok %s %llu bytes\n", path, nbytes);
	return ret;
}

static int verify_span(int fd, unsigned long long nbytes, unsigned size_mb)
{
	uint8_t *buf, *got;
	unsigned long long limit, mb;
	int ret = 0;

	limit = nbytes >> 20;
	if (size_mb < limit)
		limit = size_mb;
	if (limit < 2) {
		fprintf(stderr, "device is too small to cross extents\n");
		return 1;
	}
	if (posix_memalign((void **)&buf, PAGE, PAGE) ||
	    posix_memalign((void **)&got, PAGE, PAGE))
		return fail("posix_memalign");
	for (mb = 0; mb < limit && !ret; mb++) {
		off_t base = (off_t)mb << 20;

		ret = check_page(fd, buf, got, base, 0x40 + (uint32_t)mb);
		if (!ret)
			ret = check_page(fd, buf, got, base + 128 * PAGE, 0x80);
		if (!ret)
			ret = check_page(fd, buf, got, base + 255 * PAGE, 0xc0);
	}
	free(buf);
	free(got);
	if (!ret)
		printf("verify ok %llu MiB\n", limit);
	return ret;
}

static int zero_at(int fd, unsigned long long nbytes, unsigned size_mb)
{
	uint8_t *got;
	off_t off = (off_t)size_mb << 20;
	int ret;

	if ((unsigned long long)off + PAGE > nbytes) {
		fprintf(stderr, "zero offset is past the device\n");
		return 1;
	}
	if (posix_memalign((void **)&got, PAGE, PAGE))
		return fail("posix_memalign");
	ret = expect_zero(fd, got, off);
	free(got);
	if (!ret)
		printf("zero ok at %u MiB\n", size_mb);
	return ret;
}

static int discard_check(int fd, unsigned long long nbytes)
{
	uint8_t *buf, *got;
	int ret;

	if (nbytes < 2ULL << 20)
		return 1;
	if (posix_memalign((void **)&buf, PAGE, PAGE) ||
	    posix_memalign((void **)&got, PAGE, PAGE))
		return fail("posix_memalign");
	ret = check_page(fd, buf, got, 0, 0x11);
	if (!ret)
		ret = check_page(fd, buf, got, 1 << 20, 0x22);
	if (!ret)
		ret = discard_range(fd, 0, 2ULL << 20);
	if (!ret)
		ret = expect_zero(fd, got, 0);
	if (!ret)
		ret = expect_zero(fd, got, 1 << 20);
	free(buf);
	free(got);
	if (!ret)
		printf("discard ok\n");
	return ret;
}

static int enospc_check(int fd, unsigned long long nbytes)
{
	uint8_t *buf, *got;
	off_t off = 0;
	int ret = 0;
	int saw = 0;

	if (posix_memalign((void **)&buf, PAGE, PAGE) ||
	    posix_memalign((void **)&got, PAGE, PAGE))
		return fail("posix_memalign");
	fill_page(buf, 0, 0x33);
	while ((unsigned long long)off + PAGE <= nbytes) {
		fill_page(buf, off, 0x33);
		if (pwrite(fd, buf, PAGE, off) == PAGE) {
			off += PAGE;
			continue;
		}
		if (errno == ENOSPC) {
			saw = 1;
			break;
		}
		ret = fail("pwrite until full");
		break;
	}
	if (!ret && !saw) {
		fprintf(stderr, "disk accepted every page; backing is not smaller than capacity\n");
		ret = 1;
	}
	if (!ret)
		ret = discard_range(fd, 0, 1ULL << 20);
	if (!ret)
		ret = check_page(fd, buf, got, 0, 0x44);
	free(buf);
	free(got);
	if (!ret)
		printf("enospc ok after %lld bytes\n", (long long)off);
	return ret;
}

struct stress {
	int fd;
	int id;
	int threads;
	unsigned pages;
	int seconds;
	int fail;
};

static void *stress_thread(void *arg)
{
	struct stress *job = arg;
	uint8_t *buf, *got;
	struct timespec start, now;
	unsigned n = 0;

	if (posix_memalign((void **)&buf, PAGE, PAGE) ||
	    posix_memalign((void **)&got, PAGE, PAGE)) {
		job->fail = 1;
		return NULL;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		off_t off;
		uint32_t seed = 0x1000u + (uint32_t)job->id + n;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= job->seconds)
			break;
		off = (off_t)((n * (unsigned)job->threads + (unsigned)job->id) % job->pages) * PAGE;
		if ((n & 1) == 0) {
			fill_page(buf, off, seed);
			if (xfer(job->fd, buf, PAGE, off, 1) < 0) {
				job->fail = 1;
				break;
			}
		}
		memset(got, 0, PAGE);
		if (xfer(job->fd, got, PAGE, off, 0) < 0) {
			job->fail = 1;
			break;
		}
		if ((n & 1) == 0 && !page_ok(got, off, seed)) {
			fprintf(stderr, "stress mismatch thread %d off %lld\n",
				job->id, (long long)off);
			job->fail = 1;
			break;
		}
		n++;
	}
	free(buf);
	free(got);
	return NULL;
}

static int stress(int fd, unsigned long long nbytes, int threads, unsigned size_mb, int seconds)
{
	struct stress *jobs;
	pthread_t *threads_id;
	unsigned long long pages;
	int i, ret = 0;

	if (threads < 1)
		threads = 1;
	if (seconds < 1)
		seconds = 1;
	pages = nbytes / PAGE;
	if (((unsigned long long)size_mb << 20) / PAGE < pages)
		pages = ((unsigned long long)size_mb << 20) / PAGE;
	if (pages < (unsigned)threads)
		pages = (unsigned)threads;
	jobs = calloc((size_t)threads, sizeof(*jobs));
	threads_id = calloc((size_t)threads, sizeof(*threads_id));
	if (!jobs || !threads_id)
		return fail("calloc");
	for (i = 0; i < threads; i++) {
		jobs[i].fd = fd;
		jobs[i].id = i;
		jobs[i].threads = threads;
		jobs[i].pages = (unsigned)pages;
		jobs[i].seconds = seconds;
		if (pthread_create(&threads_id[i], NULL, stress_thread, &jobs[i])) {
			ret = fail("pthread_create");
			threads = i;
			break;
		}
	}
	for (i = 0; i < threads; i++) {
		pthread_join(threads_id[i], NULL);
		if (jobs[i].fail)
			ret = 1;
	}
	free(jobs);
	free(threads_id);
	if (!ret)
		printf("stress ok\n");
	return ret;
}

static int mode_of(const char *name)
{
	if (!strcmp(name, "smoke"))
		return MODE_SMOKE;
	if (!strcmp(name, "verify"))
		return MODE_VERIFY;
	if (!strcmp(name, "zero"))
		return MODE_ZERO;
	if (!strcmp(name, "discard"))
		return MODE_DISCARD;
	if (!strcmp(name, "enospc"))
		return MODE_ENOSPC;
	if (!strcmp(name, "stress"))
		return MODE_STRESS;
	return -1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [file] [--mode smoke|verify|zero|discard|enospc|stress] [-t n] [-s MiB] [-d sec]\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *path = "/dev/roswap0";
	const char *mode_name = "smoke";
	unsigned size_mb = 80;
	int threads = 4;
	int seconds = 2;
	int mode, fd, ret;
	unsigned long long nbytes;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			mode_name = argv[++i];
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			threads = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			size_mb = (unsigned)strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		} else if (argv[i][0] != '-') {
			path = argv[i];
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	mode = mode_of(mode_name);
	if (mode < 0) {
		usage(argv[0]);
		return 2;
	}
	fd = open(path, O_RDWR | O_DIRECT);
	if (fd < 0)
		return fail(path);
	if (ioctl(fd, BLKGETSIZE64, &nbytes) < 0) {
		ret = fail("BLKGETSIZE64");
		goto out;
	}
	switch (mode) {
	case MODE_SMOKE:
		ret = smoke(fd, path, nbytes);
		break;
	case MODE_VERIFY:
		ret = verify_span(fd, nbytes, size_mb);
		break;
	case MODE_ZERO:
		ret = zero_at(fd, nbytes, size_mb);
		break;
	case MODE_DISCARD:
		ret = discard_check(fd, nbytes);
		break;
	case MODE_ENOSPC:
		ret = enospc_check(fd, nbytes);
		break;
	default:
		ret = stress(fd, nbytes, threads, size_mb, seconds);
		break;
	}
out:
	close(fd);
	return ret;
}
