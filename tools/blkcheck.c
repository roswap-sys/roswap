// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

static void fill(uint8_t *buf, size_t len, uint32_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(seed + i);
}

static int check_region(int fd, uint8_t *buf, uint8_t *got, off_t off,
			size_t len, uint32_t seed)
{
	fill(buf, len, seed);
	if (xfer(fd, buf, len, off, 1) < 0)
		return fail("pwrite");
	memset(got, 0, len);
	if (xfer(fd, got, len, off, 0) < 0)
		return fail("pread");
	if (memcmp(buf, got, len) != 0) {
		fprintf(stderr, "mismatch at offset %lld length %zu\n",
			(long long)off, len);
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

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/roswap0";
	const char *name = strrchr(path, '/');
	char sys[128];
	unsigned long long logical, physical, rotational, nbytes;
	uint8_t *buf, *got;
	int fd, ret, ss, pss;
	off_t mid, end;

	name = name ? name + 1 : path;
	fd = open(path, O_RDWR | O_DIRECT);
	if (fd < 0)
		return fail(path);

	if (ioctl(fd, BLKSSZGET, &ss) < 0 || ioctl(fd, BLKPBSZGET, &pss) < 0 ||
	    ioctl(fd, BLKGETSIZE64, &nbytes) < 0) {
		ret = fail("ioctl");
		goto out;
	}
	if (ss != 4096 || pss != 4096) {
		fprintf(stderr, "block size logical %d physical %d\n", ss, pss);
		ret = 1;
		goto out;
	}
	if (nbytes < 2 * 1024 * 1024 || nbytes % 4096) {
		fprintf(stderr, "unexpected size %llu\n", nbytes);
		ret = 1;
		goto out;
	}

	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/logical_block_size", name);
	if (read_sysfs_ull(sys, &logical) < 0) {
		ret = fail(sys);
		goto out;
	}
	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/physical_block_size", name);
	if (read_sysfs_ull(sys, &physical) < 0) {
		ret = fail(sys);
		goto out;
	}
	snprintf(sys, sizeof(sys), "/sys/block/%s/queue/rotational", name);
	if (read_sysfs_ull(sys, &rotational) < 0) {
		ret = fail(sys);
		goto out;
	}
	if (logical != 4096 || physical != 4096 || rotational != 0) {
		fprintf(stderr, "queue logical %llu physical %llu rotational %llu\n",
			logical, physical, rotational);
		ret = 1;
		goto out;
	}

	if (posix_memalign((void **)&buf, 4096, 32 * 1024) ||
	    posix_memalign((void **)&got, 4096, 32 * 1024)) {
		ret = fail("posix_memalign");
		goto out;
	}

	end = nbytes - 4096;
	mid = (nbytes / 2) & ~(off_t)4095;
	ret = check_region(fd, buf, got, 0, 4096, 0x10);
	if (!ret)
		ret = check_region(fd, buf, got, mid, 32 * 1024, 0x20);
	if (!ret)
		ret = check_region(fd, buf, got, 0, 4096, 0x10);
	if (!ret)
		ret = check_region(fd, buf, got, end, 4096, 0x30);
	if (!ret && pwrite(fd, buf, 4096, nbytes) >= 0) {
		fprintf(stderr, "write past end succeeded\n");
		ret = 1;
	} else if (!ret) {
		errno = 0;
	}

	free(buf);
	free(got);
	if (!ret)
		printf("ok %s %llu bytes\n", path, nbytes);
out:
	close(fd);
	return ret;
}
