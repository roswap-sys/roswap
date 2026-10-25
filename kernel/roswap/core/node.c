#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/inet.h>
#include <net/net_namespace.h>

#include "roswap_internal.h"

static struct rs_node __rcu *rs_nodes[RS_MAX_NODES];

const char *rs_node_state_name(int state)
{
	switch (state) {
	case RS_N_JOINING:
		return "JOINING";
	case RS_N_ACTIVE:
		return "ACTIVE";
	case RS_N_ERROR:
		return "ERROR";
	default:
		return "EMPTY";
	}
}

struct rs_node *rs_node_get(u16 id)
{
	struct rs_node *n;

	if (id == 0 || id >= RS_MAX_NODES)
		return NULL;
	rcu_read_lock();
	n = rcu_dereference(rs_nodes[id]);
	rcu_read_unlock();
	return n;
}

struct rs_conn *rs_node_conn_up(struct rs_node *n, int cpu)
{
	int i, idx;

	if (!n || n->nr_conns <= 0 || !n->conns)
		return NULL;
	idx = cpu % n->nr_conns;
	if (atomic_read(&n->conns[idx].state) == RS_CONN_UP)
		return &n->conns[idx];
	for (i = 0; i < n->nr_conns; i++) {
		if (atomic_read(&n->conns[i].state) == RS_CONN_UP)
			return &n->conns[i];
	}
	return NULL;
}

void rs_node_note_conn_error(struct rs_conn *c)
{
	struct rs_node *n;
	int i, up = 0;

	if (!c)
		return;
	atomic_set(&c->state, RS_CONN_ERROR);
	n = rs_node_get(c->node_id);
	if (!n || n->local_only)
		return;
	for (i = 0; i < n->nr_conns; i++) {
		if (atomic_read(&n->conns[i].state) == RS_CONN_UP)
			up++;
	}
	if (!up && atomic_read(&n->state) == RS_N_ACTIVE) {
		atomic_set(&n->state, RS_N_ERROR);
		pr_err("node %u has no live QP, marked ERROR\n", n->id);
	}
}

static void rs_node_free(struct rs_node *n)
{
	int i;

	if (!n)
		return;
	for (i = 0; i < n->nr_conns; i++)
		rs_conn_destroy(&n->conns[i]);
	kfree(n->conns);
	kfree(n);
}

int rs_node_add(u16 id, const char *ip, u16 port, u32 epoch)
{
	struct rs_node *n;
	int nr, i, ret;

	if (id == 0 || id >= RS_MAX_NODES || !ip || !ip[0])
		return -EINVAL;
	if (rs_node_get(id))
		return -EEXIST;
	if (!rs_ibdev || !rs_pd)
		return -ENODEV;

	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;
	n->id = id;
	n->epoch = epoch;
	atomic_set(&n->state, RS_N_JOINING);
	spin_lock_init(&n->lock);
	INIT_LIST_HEAD(&n->chunks);
	n->addr.sin_family = AF_INET;
	n->addr.sin_port = htons(port);
	if (!in4_pton(ip, -1, (u8 *)&n->addr.sin_addr.s_addr, -1, NULL)) {
		kfree(n);
		return -EINVAL;
	}
	scnprintf(n->addr_str, sizeof(n->addr_str), "%s:%u", ip, port);

	nr = rs_nr_conns > 0 ? rs_nr_conns : num_online_cpus();
	if (nr > num_online_cpus())
		nr = num_online_cpus();
	if (nr < 1)
		nr = 1;
	n->conns = kcalloc(nr, sizeof(*n->conns), GFP_KERNEL);
	if (!n->conns) {
		kfree(n);
		return -ENOMEM;
	}
	n->nr_conns = nr;

	for (i = 0; i < nr; i++) {
		ret = rs_conn_connect(&n->conns[i], n, i);
		if (ret) {
			pr_err("node %u conn %d failed: %d\n", id, i, ret);
			while (--i >= 0)
				rs_conn_destroy(&n->conns[i]);
			kfree(n->conns);
			kfree(n);
			return ret;
		}
	}
	atomic_set(&n->state, RS_N_ACTIVE);
	rcu_assign_pointer(rs_nodes[id], n);
	pr_info("node %u ACTIVE epoch %u %s conns %d\n", id, epoch, n->addr_str, nr);
	return 0;
}

int rs_node_add_local(u16 id, u32 epoch)
{
	struct rs_node *n;

	if (id == 0 || id >= RS_MAX_NODES)
		return -EINVAL;
	if (rs_node_get(id))
		return -EEXIST;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;
	n->id = id;
	n->epoch = epoch;
	n->local_only = true;
	atomic_set(&n->state, RS_N_ACTIVE);
	spin_lock_init(&n->lock);
	INIT_LIST_HEAD(&n->chunks);
	scnprintf(n->addr_str, sizeof(n->addr_str), "local");
	rcu_assign_pointer(rs_nodes[id], n);
	return 0;
}

void rs_node_del(u16 id)
{
	struct rs_node *n;

	if (id == 0 || id >= RS_MAX_NODES)
		return;
	n = rcu_dereference_protected(rs_nodes[id], lockdep_is_held(&rs_ctl_lock));
	if (!n)
		return;
	rcu_assign_pointer(rs_nodes[id], NULL);
	synchronize_rcu();
	rs_node_free(n);
}

void rs_node_shutdown_all(void)
{
	int i;

	for (i = 1; i < RS_MAX_NODES; i++)
		rs_node_del(i);
}
