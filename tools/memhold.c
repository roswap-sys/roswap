// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096

enum {
	POLL_TIMEOUT = 0,
	POLL_QUIT = 1,
	POLL_CMD = 2,
};

static void *mem;
static size_t mem_bytes;
static int stdin_alive = 1;
static int interactive;
static int use_color;
static int watch;
static int interval_ms = 2000;
static int have_last;
static unsigned long last_rss_kb;
static unsigned long last_swap_kb;
static int last_ok = -1;
static size_t last_bytes;
static char line[128];
static size_t line_len;
static uint64_t lat_bucket[64];
static uint64_t lat_samples;
static uint64_t lat_max_ns;

static void prompt(void)
{
	if (!interactive || !stdin_alive)
		return;
	fputs("memhold> ", stdout);
	fflush(stdout);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s [-m 大小] [-i 秒]\n"
		"\n"
		"持续占用一块匿名内存。每一页写入该页偏移，并定期读回校验。\n"
		"申请量大于 MemoryMax 时，读页会把数据从 swap 换入。\n"
		"\n"
		"  -m    起始大小，默认 40 MiB。例如 40、512k、1g\n"
		"  -i    检查间隔，默认 2 秒。0 表示只在输入命令时检查\n"
		"\n"
		"运行后输入 help 查看命令。\n",
		prog);
}

static void help(void)
{
	fputs("命令:\n"
	      "  64          申请调整到 64 MiB\n"
	      "  +16         增加 16 MiB\n"
	      "  -8          减少 8 MiB\n"
	      "  回车        立刻校验并显示状态\n"
	      "  watch       每个间隔都打印；再输入一次关闭\n"
	      "  quiet       只在数值变化时打印\n"
	      "  interval 2  修改检查间隔（秒），0 为手动\n"
	      "  help        显示本帮助\n"
	      "  quit        释放内存并退出\n"
	      "\n"
	      "大小可以写 512k、64m、1g。set / add / sub 以及 设置 / 增加 / 减少 同样可用。\n",
	      stdout);
}

static void fmt_size(char *dst, size_t n, unsigned long long bytes)
{
	static const struct {
		unsigned long long unit;
		const char *name;
	} units[] = {
		{ 1ull << 30, "GiB" },
		{ 1ull << 20, "MiB" },
		{ 1ull << 10, "KiB" },
	};
	int i;

	if (bytes < 1024) {
		snprintf(dst, n, "%llu B", bytes);
		return;
	}
	for (i = 0; i < 3; i++) {
		double v;

		if (bytes < units[i].unit)
			continue;
		if (bytes % units[i].unit == 0) {
			snprintf(dst, n, "%llu %s", bytes / units[i].unit, units[i].name);
			return;
		}
		v = (double)bytes / (double)units[i].unit;
		snprintf(dst, n, v < 10 ? "%.2f %s" : "%.1f %s", v, units[i].name);
		return;
	}
}

static int read_vm(unsigned long *rss_kb, unsigned long *swap_kb)
{
	FILE *f;
	char buf[256];

	*rss_kb = 0;
	*swap_kb = 0;
	f = fopen("/proc/self/status", "r");
	if (!f)
		return -1;
	while (fgets(buf, sizeof(buf), f)) {
		sscanf(buf, "VmRSS: %lu", rss_kb);
		sscanf(buf, "VmSwap: %lu", swap_kb);
	}
	fclose(f);
	return 0;
}

static const char *verdict(int ok)
{
	if (!use_color)
		return ok ? "校验通过" : "校验失败";
	return ok ? "\033[32m校验通过\033[0m" : "\033[31m校验失败\033[0m";
}

static void lat_reset(void)
{
	memset(lat_bucket, 0, sizeof(lat_bucket));
	lat_samples = 0;
	lat_max_ns = 0;
}

static void lat_add(uint64_t ns)
{
	unsigned b = 0;
	uint64_t v = ns;

	if (ns > lat_max_ns)
		lat_max_ns = ns;
	while (v > 1 && b < 63) {
		v >>= 1;
		b++;
	}
	lat_bucket[b]++;
	lat_samples++;
}

/* ppt 是千分位：500、990、999。返回落桶的上界。 */
static uint64_t lat_pct(unsigned ppt)
{
	uint64_t need, acc = 0;
	unsigned b;

	if (!lat_samples)
		return 0;
	need = (lat_samples * ppt + 999) / 1000;
	if (!need)
		need = 1;
	for (b = 0; b < 64; b++) {
		uint64_t edge;

		acc += lat_bucket[b];
		if (acc < need)
			continue;
		edge = b == 63 ? lat_max_ns : (1ull << (b + 1));
		return edge > lat_max_ns ? lat_max_ns : edge;
	}
	return lat_max_ns;
}

static void fmt_ns(char *dst, size_t n, uint64_t ns)
{
	if (ns < 1000)
		snprintf(dst, n, "%lluns", (unsigned long long)ns);
	else if (ns < 1000000ull)
		snprintf(dst, n, "%lluus", (unsigned long long)((ns + 500) / 1000));
	else if (ns < 1000000000ull)
		snprintf(dst, n, "%llums", (unsigned long long)((ns + 500000) / 1000000));
	else
		snprintf(dst, n, "%llus", (unsigned long long)((ns + 500000000ull) / 1000000000ull));
}

static void print_status(int ok, size_t bad_off)
{
	char hold[32], rss[32], sw[32], off[32];
	char p50[16], p99[16], p999[16], pmax[16];
	unsigned long rss_kb, swap_kb;

	read_vm(&rss_kb, &swap_kb);
	fmt_size(hold, sizeof(hold), mem_bytes);
	fmt_size(rss, sizeof(rss), (unsigned long long)rss_kb << 10);
	fmt_size(sw, sizeof(sw), (unsigned long long)swap_kb << 10);
	if (ok) {
		printf("申请 %s | 物理 %s | 交换 %s | %s\n",
		       hold, rss, sw, verdict(1));
	} else {
		fmt_size(off, sizeof(off), bad_off);
		printf("申请 %s | 物理 %s | 交换 %s | %s，偏移 %s\n",
		       hold, rss, sw, verdict(0), off);
	}
	if (lat_samples) {
		fmt_ns(p50, sizeof(p50), lat_pct(500));
		fmt_ns(p99, sizeof(p99), lat_pct(990));
		fmt_ns(p999, sizeof(p999), lat_pct(999));
		fmt_ns(pmax, sizeof(pmax), lat_max_ns);
		printf("换入 p50 %s | p99 %s | p999 %s | max %s\n",
		       p50, p99, p999, pmax);
	}
	fflush(stdout);
	last_rss_kb = rss_kb;
	last_swap_kb = swap_kb;
	last_ok = ok;
	last_bytes = mem_bytes;
	have_last = 1;
}

static int status_changed(int ok)
{
	unsigned long rss_kb, swap_kb;

	if (watch || !have_last)
		return 1;
	if (read_vm(&rss_kb, &swap_kb) < 0)
		return 1;
	return rss_kb != last_rss_kb || swap_kb != last_swap_kb ||
	       ok != last_ok || mem_bytes != last_bytes;
}

static int resize(size_t bytes)
{
	void *p;

	bytes &= ~(size_t)(PAGE - 1);
	if (bytes == mem_bytes)
		return 0;
	if (bytes == 0) {
		if (mem && munmap(mem, mem_bytes) < 0)
			return -1;
		mem = NULL;
		mem_bytes = 0;
		return 0;
	}
	if (!mem)
		p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	else
		p = mremap(mem, mem_bytes, bytes, MREMAP_MAYMOVE);
	if (p == MAP_FAILED)
		return -1;
	mem = p;
	mem_bytes = bytes;
	return 0;
}

static void stamp(void)
{
	uint8_t *b = mem;
	size_t i;

	for (i = 0; i < mem_bytes; i += PAGE) {
		uint64_t v = i;

		memcpy(b + i, &v, sizeof(v));
	}
}

static uint64_t elapsed_ns(const struct timespec *a, const struct timespec *b)
{
	return (uint64_t)(b->tv_sec - a->tv_sec) * 1000000000ull +
	       (uint64_t)(b->tv_nsec - a->tv_nsec);
}

static int verify(size_t *bad_off)
{
	uint8_t *b = mem;
	size_t i;

	lat_reset();
	for (i = 0; i < mem_bytes; i += PAGE) {
		struct timespec t0, t1;
		uint64_t v;
		int timed = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;

		memcpy(&v, b + i, sizeof(v));
		if (timed && clock_gettime(CLOCK_MONOTONIC, &t1) == 0)
			lat_add(elapsed_ns(&t0, &t1));
		if (v != i) {
			*bad_off = i;
			return 0;
		}
	}
	return 1;
}

static int parse_size(const char *s, size_t *out)
{
	char *end;
	unsigned long long n;
	unsigned shift = 20;

	errno = 0;
	n = strtoull(s, &end, 10);
	if (errno || end == s)
		return -1;
	switch (*end) {
	case '\0':
		break;
	case 'k':
	case 'K':
		shift = 10;
		end++;
		break;
	case 'm':
	case 'M':
		shift = 20;
		end++;
		break;
	case 'g':
	case 'G':
		shift = 30;
		end++;
		break;
	default:
		return -1;
	}
	if (*end != '\0')
		return -1;
	if (n > (unsigned long long)SIZE_MAX >> shift)
		return -1;
	*out = (size_t)n << shift;
	return 0;
}

static void say_resize(size_t old)
{
	char now[32], delta[32];

	fmt_size(now, sizeof(now), mem_bytes);
	if (mem_bytes == old) {
		printf("大小没有变化，仍是 %s。\n", now);
		return;
	}
	fmt_size(delta, sizeof(delta),
		 mem_bytes > old ? mem_bytes - old : old - mem_bytes);
	printf("已调整为 %s（%s %s）。\n", now,
	       mem_bytes > old ? "增加" : "减少", delta);
	fflush(stdout);
}

static int apply_size(size_t bytes, int announce)
{
	size_t old = mem_bytes;

	if (resize(bytes) < 0) {
		fprintf(stderr, "调整失败: %s\n", strerror(errno));
		prompt();
		return -1;
	}
	stamp();
	if (announce)
		say_resize(old);
	print_status(1, 0);
	prompt();
	return 0;
}

static int do_status(void)
{
	size_t bad = 0;
	int ok = verify(&bad);

	print_status(ok, bad);
	if (!ok)
		stamp();
	prompt();
	return 0;
}

static void lower_copy(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i + 1 < n && src[i]; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = '\0';
}

static int same(const char *cmd, const char *a, const char *b)
{
	return !strcmp(cmd, a) || (b && !strcmp(cmd, b));
}

static int handle_line(char *s)
{
	char raw[16], cmd[16], arg[32];
	size_t n;
	int nr;
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '\0')
		return do_status();
	if (*s == '+' || (*s == '-' && isdigit((unsigned char)s[1]))) {
		if (parse_size(s + 1, &n) < 0 || n == 0) {
			fprintf(stderr, "大小无效。例如 +16 或 -8。\n");
			prompt();
			return 0;
		}
		if (*s == '+') {
			if (n > SIZE_MAX - mem_bytes) {
				fprintf(stderr, "大小超出范围。\n");
				prompt();
				return 0;
			}
			apply_size(mem_bytes + n, 1);
		} else {
			apply_size(n >= mem_bytes ? 0 : mem_bytes - n, 1);
		}
		return 0;
	}
	if (parse_size(s, &n) == 0) {
		if (n == 0) {
			fprintf(stderr, "大小要大于 0。输入 quit 退出。\n");
			prompt();
			return 0;
		}
		apply_size(n, 1);
		return 0;
	}

	nr = sscanf(s, "%15s %31s", raw, arg);
	if (nr < 1)
		return 0;
	lower_copy(cmd, raw, sizeof(cmd));
	if (same(cmd, "quit", "q") || !strcmp(raw, "退出"))
		return 1;
	if (same(cmd, "help", "h") || !strcmp(cmd, "?") || !strcmp(raw, "帮助")) {
		help();
		prompt();
		return 0;
	}
	if (same(cmd, "status", "s") || !strcmp(raw, "状态"))
		return do_status();
	if (same(cmd, "watch", NULL) || !strcmp(raw, "监控")) {
		if (nr >= 2 && (!strcmp(arg, "on") || !strcmp(arg, "开")))
			watch = 1;
		else if (nr >= 2 && (!strcmp(arg, "off") || !strcmp(arg, "关")))
			watch = 0;
		else
			watch = !watch;
		printf("%s\n", watch ? "每个间隔都会打印状态。" : "只在数值变化时打印。");
		fflush(stdout);
		prompt();
		return 0;
	}
	if (same(cmd, "quiet", NULL) || !strcmp(raw, "安静")) {
		watch = 0;
		puts("只在数值变化时打印。");
		prompt();
		return 0;
	}
	if (same(cmd, "interval", NULL) || !strcmp(raw, "间隔")) {
		double sec;

		if (nr < 2) {
			fprintf(stderr, "用法: interval 2\n");
			prompt();
			return 0;
		}
		errno = 0;
		sec = strtod(arg, &end);
		if (errno || end == arg || *end != '\0' || sec < 0) {
			fprintf(stderr, "间隔无效。\n");
			prompt();
			return 0;
		}
		interval_ms = (int)(sec * 1000);
		if (interval_ms == 0)
			puts("已改为手动检查，输入回车查看状态。");
		else
			printf("检查间隔已改为 %.1f 秒。\n", sec);
		fflush(stdout);
		prompt();
		return 0;
	}
	if (nr < 2 || parse_size(arg, &n) < 0) {
		fprintf(stderr, "无法识别。输入 help 查看命令。\n");
		prompt();
		return 0;
	}
	if (same(cmd, "set", NULL) || !strcmp(raw, "设置")) {
		apply_size(n, 1);
	} else if (same(cmd, "add", NULL) || !strcmp(raw, "增加")) {
		if (n > SIZE_MAX - mem_bytes) {
			fprintf(stderr, "大小超出范围。\n");
			prompt();
			return 0;
		}
		apply_size(mem_bytes + n, 1);
	} else if (same(cmd, "sub", NULL) || !strcmp(raw, "减少")) {
		apply_size(n >= mem_bytes ? 0 : mem_bytes - n, 1);
	} else {
		fprintf(stderr, "无法识别。输入 help 查看命令。\n");
		prompt();
	}
	return 0;
}

static int poll_commands(int timeout_ms)
{
	struct pollfd pfd;
	char tmp[256];
	ssize_t n, i;
	int rc;
	int action = POLL_TIMEOUT;

	if (!stdin_alive) {
		if (timeout_ms > 0)
			(void)poll(NULL, 0, timeout_ms);
		return POLL_TIMEOUT;
	}

	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;
	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0) {
		if (errno == EINTR)
			return POLL_TIMEOUT;
		stdin_alive = 0;
		return POLL_TIMEOUT;
	}
	if (rc == 0)
		return POLL_TIMEOUT;
	if (pfd.revents & (POLLERR | POLLNVAL)) {
		stdin_alive = 0;
		return POLL_TIMEOUT;
	}
	if (!(pfd.revents & (POLLIN | POLLHUP)))
		return POLL_TIMEOUT;

	n = read(STDIN_FILENO, tmp, sizeof(tmp));
	if (n <= 0) {
		stdin_alive = 0;
		return POLL_TIMEOUT;
	}
	for (i = 0; i < n; i++) {
		if (tmp[i] == '\r')
			continue;
		if (tmp[i] != '\n') {
			if (line_len + 1 < sizeof(line))
				line[line_len++] = tmp[i];
			action = POLL_CMD;
			continue;
		}
		line[line_len] = '\0';
		line_len = 0;
		if (handle_line(line))
			return POLL_QUIT;
		action = POLL_CMD;
	}
	return action;
}

int main(int argc, char **argv)
{
	size_t initial = (size_t)40 << 20;
	int opt;

	interactive = isatty(STDIN_FILENO);
	use_color = isatty(STDOUT_FILENO);

	while ((opt = getopt(argc, argv, "m:i:h")) != -1) {
		char *end;

		switch (opt) {
		case 'm':
			if (parse_size(optarg, &initial) < 0 || initial == 0) {
				usage(argv[0]);
				return 2;
			}
			break;
		case 'i':
			errno = 0;
			interval_ms = (int)(strtod(optarg, &end) * 1000);
			if (errno || end == optarg || *end != '\0' || interval_ms < 0) {
				usage(argv[0]);
				return 2;
			}
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (interactive) {
		puts("memhold：持续占用内存，按页写入偏移并校验。");
		puts("输入 help 查看命令，直接回车查看状态。");
		putchar('\n');
	}

	if (apply_size(initial, 0) < 0)
		return 1;

	for (;;) {
		size_t bad = 0;
		int ok;
		int ev;

		if (!stdin_alive && interval_ms <= 0) {
			fprintf(stderr, "标准输入已关闭，退出。\n");
			break;
		}
		ev = poll_commands(interval_ms > 0 ? interval_ms : -1);
		if (ev == POLL_QUIT)
			break;
		if (ev == POLL_CMD || interval_ms <= 0)
			continue;
		ok = verify(&bad);
		if (status_changed(ok)) {
			print_status(ok, bad);
			prompt();
		}
		stamp();
	}

	resize(0);
	if (interactive)
		puts("已释放内存。");
	return 0;
}
