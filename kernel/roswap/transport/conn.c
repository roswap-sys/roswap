#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/inet.h>
#include <net/net_namespace.h>

#include "roswap_internal.h"

static int rs_conn_setup_qp(struct rs_conn *c)
{
	struct rdma_cm_id *id = c->cm_id;
	struct ib_qp_init_attr qp;
	int vectors, ret;

	vectors = id->device->num_comp_vectors;
	if (vectors < 1)
		vectors = 1;
#if RS_HAVE_IB_ALLOC_CQ
	c->cq = ib_alloc_cq(id->device, c, c->sq_depth, c->cpu % vectors,
			    IB_POLL_SOFTIRQ);
#else
	{
		struct ib_cq_init_attr cqa = {
			.cqe = c->sq_depth,
			.comp_vector = c->cpu % vectors,
		};
		c->cq = ib_create_cq(id->device, NULL, NULL, c, &cqa);
	}
#endif
	if (IS_ERR(c->cq)) {
		ret = PTR_ERR(c->cq);
		c->cq = NULL;
		return ret;
	}

	memset(&qp, 0, sizeof(qp));
	qp.cap.max_send_wr = c->sq_depth;
	qp.cap.max_recv_wr = 1;
	qp.cap.max_send_sge = 1;
	qp.cap.max_recv_sge = 1;
	qp.sq_sig_type = IB_SIGNAL_REQ_WR;
	qp.qp_type = IB_QPT_RC;
	qp.send_cq = c->cq;
	qp.recv_cq = c->cq;
	ret = rdma_create_qp(id, rs_pd, &qp);
	if (ret)
		pr_err("rdma_create_qp cpu %d: %d\n", c->cpu, ret);
	return ret;
}

static int rs_cm_handler(struct rdma_cm_id *id, struct rdma_cm_event *ev)
{
	struct rs_conn *c = id->context;
	struct rdma_conn_param param;
	int depth, ret;

	switch (ev->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		ret = rdma_resolve_route(id, 2000);
		if (ret) {
			atomic_set(&c->state, RS_CONN_ERROR);
			complete(&c->established);
		}
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ret = rs_conn_setup_qp(c);
		if (ret) {
			atomic_set(&c->state, RS_CONN_ERROR);
			complete(&c->established);
			break;
		}
		depth = rs_initiator_depth > 0 ? rs_initiator_depth : 1;
		memset(&param, 0, sizeof(param));
		param.private_data = &c->pdata;
		param.private_data_len = sizeof(c->pdata);
		param.responder_resources = depth;
		param.initiator_depth = depth;
		param.retry_count = rs_retry_count;
		param.rnr_retry_count = 7;
		param.flow_control = 1;
#if RS_HAVE_RDMA_SET_ACK_TIMEOUT
		rdma_set_ack_timeout(id, rs_ack_timeout);
#endif
#if RS_HAVE_RDMA_CONNECT_LOCKED
		ret = rdma_connect_locked(id, &param);
#else
		ret = rdma_connect(id, &param);
#endif
		if (ret) {
			pr_err("rdma_connect cpu %d: %d\n", c->cpu, ret);
			atomic_set(&c->state, RS_CONN_ERROR);
			complete(&c->established);
		}
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		atomic_set(&c->state, RS_CONN_UP);
		complete(&c->established);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_REJECTED:
		pr_err("cm event %d status %d cpu %d\n", ev->event, ev->status, c->cpu);
		atomic_set(&c->state, RS_CONN_ERROR);
		complete(&c->established);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		atomic_set(&c->state, RS_CONN_ERROR);
		break;
	default:
		break;
	}
	return 0;
}

static void rs_fill_pdata(struct rs_conn *c, struct rs_node *n)
{
	struct rs_conn_pdata *p = &c->pdata;

	memset(p, 0, sizeof(*p));
	p->magic = cpu_to_be32(RS_CONN_MAGIC);
	p->version = cpu_to_be16(RS_CONN_VERSION);
	p->purpose = RS_QP_DATA;
	p->cpu = cpu_to_be16(c->cpu);
	p->node_id = cpu_to_be16(n->id);
	p->node_epoch = cpu_to_be32(n->epoch);
	memcpy(p->cluster_uuid, rs_cluster_uuid, sizeof(p->cluster_uuid));
}

int rs_conn_connect(struct rs_conn *c, struct rs_node *n, int index)
{
	struct sockaddr_in src;
	struct sockaddr *srcp = NULL;
	int ret;

	memset(c, 0, sizeof(*c));
	c->cpu = index;
	c->node_id = n->id;
	c->sq_depth = rs_sq_depth;
	c->sq_free = rs_sq_depth;
	spin_lock_init(&c->sq_lock);
	init_completion(&c->established);
	atomic_set(&c->state, RS_CONN_CONNECTING);
	rs_fill_pdata(c, n);

#if RS_RDMA_CREATE_ID_NET
	ret = rdma_create_id(&init_net, &c->cm_id, rs_cm_handler, c,
			     RDMA_PS_TCP, IB_QPT_RC);
	if (ret) {
		c->cm_id = NULL;
		return ret;
	}
#else
	c->cm_id = rdma_create_id(rs_cm_handler, c, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(c->cm_id)) {
		ret = PTR_ERR(c->cm_id);
		c->cm_id = NULL;
		return ret;
	}
#endif
	rdma_set_service_type(c->cm_id, rs_tos);
	if (rs_src_addr[0]) {
		memset(&src, 0, sizeof(src));
		src.sin_family = AF_INET;
		if (!in4_pton(rs_src_addr, -1, (u8 *)&src.sin_addr.s_addr, -1, NULL)) {
			rs_conn_destroy(c);
			return -EINVAL;
		}
		srcp = (struct sockaddr *)&src;
	}
	ret = rdma_resolve_addr(c->cm_id, srcp, (struct sockaddr *)&n->addr, 2000);
	if (ret) {
		rs_conn_destroy(c);
		return ret;
	}
	if (!wait_for_completion_timeout(&c->established, 30 * HZ)) {
		pr_err("connect timeout node %u cpu %d\n", n->id, index);
		rs_conn_destroy(c);
		return -ETIMEDOUT;
	}
	if (atomic_read(&c->state) != RS_CONN_UP) {
		rs_conn_destroy(c);
		return -ECONNREFUSED;
	}
	return 0;
}

void rs_conn_destroy(struct rs_conn *c)
{
	if (!c || !c->cm_id)
		return;
	atomic_set(&c->state, RS_CONN_CLOSED);
	if (c->cm_id->qp)
		rdma_destroy_qp(c->cm_id);
	if (c->cq) {
#if RS_HAVE_IB_ALLOC_CQ
		ib_free_cq(c->cq);
#else
		ib_destroy_cq(c->cq);
#endif
	}
	c->cq = NULL;
	rdma_destroy_id(c->cm_id);
	c->cm_id = NULL;
}
