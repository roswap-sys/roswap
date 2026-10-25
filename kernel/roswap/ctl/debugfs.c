#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "roswap_internal.h"

static struct dentry *rs_dbgdir;

int rs_parse_u64(const char *s, u64 *out)
{
	if (!s || !*s)
		return -EINVAL;
	return kstrtoull(s, 0, out);
}

int rs_parse_u32(const char *s, u32 *out)
{
	if (!s || !*s)
		return -EINVAL;
	return kstrtou32(s, 0, out);
}

int rs_parse_size(const char *s, u64 *out)
{
	char tmp[32];
	size_t n;
	u64 mul = 1, v;
	char suf;

	if (!s || !*s)
		return -EINVAL;
	n = strlen(s);
	if (n >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, s, n + 1);
	suf = tmp[n - 1];
	if (suf == 'T' || suf == 't' || suf == 'G' || suf == 'g' ||
	    suf == 'M' || suf == 'm' || suf == 'K' || suf == 'k') {
		if (suf == 'T' || suf == 't')
			mul = 1ULL << 40;
		else if (suf == 'G' || suf == 'g')
			mul = 1ULL << 30;
		else if (suf == 'M' || suf == 'm')
			mul = 1ULL << 20;
		else
			mul = 1ULL << 10;
		tmp[n - 1] = '\0';
	}
	if (kstrtoull(tmp, 0, &v))
		return -EINVAL;
	if (mul != 1 && v > U64_MAX / mul)
		return -ERANGE;
	*out = v * mul;
	return 0;
}

static int rs_kv(const char *cmd, const char *key, char *out, size_t outsz)
{
	size_t klen = strlen(key);
	const char *p = cmd;

	while ((p = strstr(p, key)) != NULL) {
		if ((p == cmd || p[-1] == ' ') && p[klen] == '=') {
			size_t i = 0;

			p += klen + 1;
			while (p[i] && p[i] != ' ' && p[i] != '\n' && p[i] != '\t' &&
			       i + 1 < outsz) {
				out[i] = p[i];
				i++;
			}
			out[i] = '\0';
			return i ? 0 : -EINVAL;
		}
		p += klen;
	}
	return -ENOENT;
}

static const char rs_usage[] =
	"dev_create size=1G\n"
	"dev_destroy\n"
	"node_add id=1 addr=10.0.0.11:18515 epoch=1\n"
	"chunk_add node=1 gid=1 raddr=0x.. rkey=0x.. len=1G epoch=1\n"
	"resv_flush\n";

static int rs_cmd_dev_create(const char *cmd)
{
	char sz[32];
	u64 bytes;
	int ret;

	if (rs_kv(cmd, "size", sz, sizeof(sz)))
		return -EINVAL;
	ret = rs_parse_size(sz, &bytes);
	if (ret)
		return ret;
	return rs_dev_create(bytes);
}

static int rs_cmd_node_add(const char *cmd)
{
	char idb[16], addr[64], epb[16], ip[64];
	u32 id, epoch = 1;
	u16 port = RS_AGENT_PORT;
	const char *colon;
	size_t n;
	int ret;

	if (rs_kv(cmd, "id", idb, sizeof(idb)) || rs_kv(cmd, "addr", addr, sizeof(addr)))
		return -EINVAL;
	if (rs_parse_u32(idb, &id) || id == 0 || id >= RS_MAX_NODES)
		return -EINVAL;
	if (!rs_kv(cmd, "epoch", epb, sizeof(epb))) {
		if (rs_parse_u32(epb, &epoch))
			return -EINVAL;
	}
	colon = strrchr(addr, ':');
	if (colon) {
		n = colon - addr;
		if (!n || n >= sizeof(ip))
			return -EINVAL;
		memcpy(ip, addr, n);
		ip[n] = '\0';
		if (kstrtou16(colon + 1, 10, &port))
			return -EINVAL;
	} else {
		strlcpy(ip, addr, sizeof(ip));
	}
	ret = rs_node_add(id, ip, port, epoch);
	return ret;
}

static int rs_cmd_chunk_add(const char *cmd)
{
	char nb[16], gb[32], rb[32], kb[32], lb[32], eb[16];
	u32 node, gid, rkey, epoch;
	u64 raddr, len;
	int ret;

	if (rs_kv(cmd, "node", nb, sizeof(nb)) || rs_kv(cmd, "gid", gb, sizeof(gb)) ||
	    rs_kv(cmd, "raddr", rb, sizeof(rb)) || rs_kv(cmd, "rkey", kb, sizeof(kb)) ||
	    rs_kv(cmd, "len", lb, sizeof(lb)))
		return -EINVAL;
	if (rs_parse_u32(nb, &node) || rs_parse_u32(gb, &gid) ||
	    rs_parse_u64(rb, &raddr) || rs_parse_u32(kb, &rkey))
		return -EINVAL;
	ret = rs_parse_size(lb, &len);
	if (ret)
		return ret;
	epoch = 1;
	if (!rs_kv(cmd, "epoch", eb, sizeof(eb))) {
		if (rs_parse_u32(eb, &epoch))
			return -EINVAL;
	} else {
		struct rs_node *n = rs_node_get(node);

		if (n)
			epoch = n->epoch;
	}
	return rs_chunk_add(node, gid, epoch, raddr, rkey, len);
}

static int rs_exec_line(char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '\0' || *line == '#')
		return 0;
	if (!strncmp(line, "dev_create", 10))
		return rs_cmd_dev_create(line);
	if (!strncmp(line, "dev_destroy", 11)) {
		if (!rs_device)
			return -ENOENT;
		if (atomic_read(&rs_device->openers))
			return -EBUSY;
		rs_dev_destroy();
		return 0;
	}
	if (!strncmp(line, "node_add", 8))
		return rs_cmd_node_add(line);
	if (!strncmp(line, "chunk_add", 9))
		return rs_cmd_chunk_add(line);
	if (!strncmp(line, "resv_flush", 10)) {
		if (!rs_device)
			return -ENOENT;
		rs_resv_flush(rs_device);
		return 0;
	}
	pr_info("%s", rs_usage);
	return -EINVAL;
}

static ssize_t rs_ctl_write(struct file *file, const char __user *buf,
			    size_t len, loff_t *ppos)
{
	char *kbuf, *p, *line;
	int ret = 0;

	if (len == 0 || len > 4096)
		return -EINVAL;
	kbuf = kzalloc(len + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, buf, len)) {
		kfree(kbuf);
		return -EFAULT;
	}
	mutex_lock(&rs_ctl_lock);
	p = kbuf;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (!line[0])
			continue;
		ret = rs_exec_line(line);
		if (ret)
			break;
	}
	mutex_unlock(&rs_ctl_lock);
	kfree(kbuf);
	return ret ? ret : (ssize_t)len;
}

static ssize_t rs_ctl_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	return simple_read_from_buffer(buf, len, ppos, rs_usage, strlen(rs_usage));
}

static const struct file_operations rs_ctl_fops = {
	.owner	= THIS_MODULE,
	.read	= rs_ctl_read,
	.write	= rs_ctl_write,
	.llseek	= default_llseek,
};

static int rs_nodes_show(struct seq_file *m, void *v)
{
	int i;

	seq_puts(m, "id state epoch addr conns free mapped chunks\n");
	mutex_lock(&rs_ctl_lock);
	for (i = 1; i < RS_MAX_NODES; i++) {
		struct rs_node *n = rs_node_get(i);
		int up = 0, c;

		if (!n)
			continue;
		for (c = 0; c < n->nr_conns; c++) {
			if (atomic_read(&n->conns[c].state) == RS_CONN_UP)
				up++;
		}
		seq_printf(m, "%u %s %u %s %d %d %d %d\n",
			   n->id, rs_node_state_name(atomic_read(&n->state)),
			   n->epoch, n->addr_str, up,
			   atomic_read(&n->free_ext), atomic_read(&n->mapped),
			   n->nr_chunks);
	}
	mutex_unlock(&rs_ctl_lock);
	return 0;
}

static int rs_chunks_show(struct seq_file *m, void *v)
{
	struct rs_chunk *c;
	unsigned long gid;

	seq_puts(m, "gid node epoch raddr rkey len free\n");
	mutex_lock(&rs_ctl_lock);
	xa_for_each(&rs_chunks, gid, c) {
		seq_printf(m, "%u %u %u 0x%llx 0x%x %llu %d\n",
			   c->gid, c->node_id, c->node_epoch, c->raddr, c->rkey,
			   c->len, atomic_read(&c->free_ext));
	}
	mutex_unlock(&rs_ctl_lock);
	return 0;
}

static int rs_extents_show(struct seq_file *m, void *v)
{
	int i;
	int total_mapped = 0;

	seq_puts(m, "node chunks free mapped\n");
	mutex_lock(&rs_ctl_lock);
	for (i = 1; i < RS_MAX_NODES; i++) {
		struct rs_node *n = rs_node_get(i);
		int mapped;

		if (!n)
			continue;
		mapped = atomic_read(&n->mapped);
		total_mapped += mapped;
		seq_printf(m, "%u %d %d %d\n", n->id, n->nr_chunks,
			   atomic_read(&n->free_ext), mapped);
	}
	seq_printf(m, "TOTAL mapped %d cached %u\n", total_mapped,
		   rs_device ? rs_resv_cached(rs_device) : 0);
	mutex_unlock(&rs_ctl_lock);
	return 0;
}

static int rs_stats_show(struct seq_file *m, void *v)
{
	struct rs_dev *dev = rs_device;

	if (!dev) {
		seq_puts(m, "no device\n");
		return 0;
	}
	seq_printf(m, "read_ok %llu\nwrite_ok %llu\nread_err %llu\nwrite_err %llu\n",
		   atomic64_read(&dev->read_ok), atomic64_read(&dev->write_ok),
		   atomic64_read(&dev->read_err), atomic64_read(&dev->write_err));
	seq_printf(m, "discard_pages %llu\nzero_read %llu\nalloc_fail %llu\n",
		   atomic64_read(&dev->discard_pages), atomic64_read(&dev->zero_read),
		   atomic64_read(&dev->alloc_fail));
	seq_printf(m, "slots %llu extents %llu\n", dev->nr_slots, dev->nr_extents);
	return 0;
}

static int rs_single_open(struct inode *inode, struct file *file,
			  int (*show)(struct seq_file *, void *))
{
	return single_open(file, show, NULL);
}

static int rs_nodes_open(struct inode *inode, struct file *file)
{
	return rs_single_open(inode, file, rs_nodes_show);
}
static int rs_chunks_open(struct inode *inode, struct file *file)
{
	return rs_single_open(inode, file, rs_chunks_show);
}
static int rs_extents_open(struct inode *inode, struct file *file)
{
	return rs_single_open(inode, file, rs_extents_show);
}
static int rs_stats_open(struct inode *inode, struct file *file)
{
	return rs_single_open(inode, file, rs_stats_show);
}

static const struct file_operations rs_nodes_fops = {
	.owner = THIS_MODULE, .open = rs_nodes_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};
static const struct file_operations rs_chunks_fops = {
	.owner = THIS_MODULE, .open = rs_chunks_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};
static const struct file_operations rs_extents_fops = {
	.owner = THIS_MODULE, .open = rs_extents_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};
static const struct file_operations rs_stats_fops = {
	.owner = THIS_MODULE, .open = rs_stats_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

int rs_debugfs_init(void)
{
	rs_dbgdir = debugfs_create_dir("roswap", NULL);
	if (!rs_dbgdir || IS_ERR(rs_dbgdir))
		return -ENODEV;
	debugfs_create_file("ctl", 0600, rs_dbgdir, NULL, &rs_ctl_fops);
	debugfs_create_file("nodes", 0400, rs_dbgdir, NULL, &rs_nodes_fops);
	debugfs_create_file("chunks", 0400, rs_dbgdir, NULL, &rs_chunks_fops);
	debugfs_create_file("extents", 0400, rs_dbgdir, NULL, &rs_extents_fops);
	debugfs_create_file("stats", 0400, rs_dbgdir, NULL, &rs_stats_fops);
	return 0;
}

void rs_debugfs_exit(void)
{
	debugfs_remove_recursive(rs_dbgdir);
	rs_dbgdir = NULL;
}
