#define _GNU_SOURCE
#include "cm_linux.h"

#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <fcntl.h>

#include "roswap_proto.h"

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif

struct rs_map {
  struct rs_map *next;
  void *addr;
  size_t len;
  struct ibv_mr *mr;
  int fd;
};

struct rs_child {
  struct rs_child *next;
  struct rdma_cm_id *id;
  struct ibv_cq *cq;
};

struct rs_agent {
  struct rdma_event_channel *ch;
  struct rdma_cm_id *listen;
  struct ibv_pd *pd;
  struct rs_map *maps;
  struct rs_child *children;
  int stop_pipe[2];
  int stopping;
  char master_ip[64];
  uint8_t uuid[16];
  int check_uuid;
  int max_rd_atom;
};

static int rs_mbind(void *addr, size_t len, int node)
{
  unsigned long mask[4];
  long rc;

  if (node < 0 || node >= 256)
    return 0;
  memset(mask, 0, sizeof(mask));
  mask[node / 64] = 1UL << (node % 64);
  rc = syscall(__NR_mbind, addr, len, MPOL_BIND, mask,
         (unsigned long)node + 2, (unsigned long)MPOL_MF_MOVE);
  if (rc != 0) {
    fprintf(stderr, "roswap-agent: mbind node %d: %s\n", node, strerror(errno));
    return -errno;
  }
  return 0;
}

static void rs_touch(void *addr, size_t len)
{
  size_t off;
  volatile char *p = addr;

  for (off = 0; off < len; off += (size_t)1 << 30)
    p[off] = 0;
}

struct rs_agent *rs_agent_open(const char *ip, int port, const char *master_ip,
             const uint8_t uuid[16], int check_uuid)
{
  struct rs_agent *a;
  struct sockaddr_in addr;
  int flags;

  if (!ip || !ip[0] || strcmp(ip, "0.0.0.0") == 0) {
    fprintf(stderr, "roswap-agent: 必须绑定具体的 RoCE 地址，不能使用 0.0.0.0\n");
    return NULL;
  }
  a = calloc(1, sizeof(*a));
  if (!a)
    return NULL;
  if (master_ip)
    snprintf(a->master_ip, sizeof(a->master_ip), "%s", master_ip);
  if (uuid)
    memcpy(a->uuid, uuid, 16);
  a->check_uuid = check_uuid;
  a->stop_pipe[0] = a->stop_pipe[1] = -1;
  if (pipe(a->stop_pipe) != 0)
    goto fail;
  a->ch = rdma_create_event_channel();
  if (!a->ch)
    goto fail;
  flags = fcntl(a->ch->fd, F_GETFL);
  fcntl(a->ch->fd, F_SETFL, flags | O_NONBLOCK);
  if (rdma_create_id(a->ch, &a->listen, NULL, RDMA_PS_TCP) != 0)
    goto fail;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
    goto fail;
  if (rdma_bind_addr(a->listen, (struct sockaddr *)&addr) != 0) {
    fprintf(stderr, "roswap-agent: rdma_bind_addr %s:%d: %s\n", ip, port, strerror(errno));
    goto fail;
  }
  if (!a->listen->verbs) {
    fprintf(stderr, "roswap-agent: 绑定后 verbs 上下文为空，地址可能不是 RoCE 地址\n");
    goto fail;
  }
  if (rdma_listen(a->listen, 128) != 0)
    goto fail;
  a->pd = ibv_alloc_pd(a->listen->verbs);
  if (!a->pd)
    goto fail;
  {
    struct ibv_device_attr attr;
    if (ibv_query_device(a->listen->verbs, &attr) == 0)
      a->max_rd_atom = attr.max_qp_rd_atom;
  }
  if (a->max_rd_atom < 1)
    a->max_rd_atom = 1;
  fprintf(stderr, "roswap-agent: listen %s:%d dev %s max_rd_atom %d\n",
    ip, port, ibv_get_device_name(a->listen->verbs->device), a->max_rd_atom);
  return a;
fail:
  rs_agent_close(a);
  return NULL;
}

struct rs_map_result rs_agent_map(struct rs_agent *a, const char *path, size_t len, int numa)
{
  struct rs_map_result r;
  struct rs_map *m;
  int fd;

  memset(&r, 0, sizeof(r));
  r.err = -EINVAL;
  if (!a || !a->pd || !path || len == 0)
    return r;
  fd = open(path, O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    fprintf(stderr, "roswap-agent: open %s: %s\n", path, strerror(errno));
    r.err = -errno;
    return r;
  }
  if (ftruncate(fd, (off_t)len) != 0) {
    fprintf(stderr, "roswap-agent: ftruncate %s: %s\n", path, strerror(errno));
    r.err = -errno;
    close(fd);
    return r;
  }
  m = calloc(1, sizeof(*m));
  if (!m) {
    close(fd);
    r.err = -ENOMEM;
    return r;
  }
  m->fd = fd;
  m->len = len;
  m->addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m->addr == MAP_FAILED) {
    fprintf(stderr, "roswap-agent: mmap %s: %s\n", path, strerror(errno));
    r.err = -errno;
    free(m);
    close(fd);
    return r;
  }
  if (rs_mbind(m->addr, len, numa) != 0)
    fprintf(stderr, "roswap-agent: 继续使用默认 NUMA 放置\n");
  rs_touch(m->addr, len);
  m->mr = ibv_reg_mr(a->pd, m->addr, len,
         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
  if (!m->mr) {
    fprintf(stderr, "roswap-agent: ibv_reg_mr %s: %s\n", path, strerror(errno));
    munmap(m->addr, len);
    close(fd);
    free(m);
    r.err = -errno;
    return r;
  }
  m->next = a->maps;
  a->maps = m;
  r.err = 0;
  r.addr = (uint64_t)(uintptr_t)m->addr;
  r.rkey = m->mr->rkey;
  fprintf(stderr, "roswap-agent: mr %s addr 0x%llx rkey 0x%x len %zu numa %d\n",
    path, (unsigned long long)r.addr, r.rkey, len, numa);
  return r;
}

static int rs_peer_ok(struct rs_agent *a, struct rdma_cm_id *id)
{
  struct sockaddr *peer;
  char buf[64];

  if (!a->master_ip[0])
    return 1;
  peer = rdma_get_peer_addr(id);
  if (!peer || peer->sa_family != AF_INET)
    return 0;
  if (!inet_ntop(AF_INET, &((struct sockaddr_in *)peer)->sin_addr, buf, sizeof(buf)))
    return 0;
  return strcmp(buf, a->master_ip) == 0;
}

static int rs_pdata_ok(struct rs_agent *a, const void *data, size_t len)
{
  const struct rs_conn_pdata *p;

  if (len < sizeof(*p) || !data)
    return 0;
  p = data;
  if (be32toh(p->magic) != RS_CONN_MAGIC)
    return 0;
  if (be16toh(p->version) != RS_CONN_VERSION)
    return 0;
  if (a->check_uuid && memcmp(p->cluster_uuid, a->uuid, 16) != 0)
    return 0;
  return 1;
}

static void rs_reject(struct rdma_cm_id *id)
{
  rdma_reject(id, NULL, 0);
}

static int rs_accept(struct rs_agent *a, struct rdma_cm_event *ev)
{
  struct rdma_cm_id *id = ev->id;
  struct ibv_qp_init_attr qia;
  struct rdma_conn_param cp;
  struct rs_child *ch;
  struct ibv_cq *cq;
  const struct rs_conn_pdata *p;
  int depth;

  if (!rs_peer_ok(a, id) || !rs_pdata_ok(a, ev->param.conn.private_data,
                 ev->param.conn.private_data_len)) {
    fprintf(stderr, "roswap-agent: 拒绝连接（来源或 private data 不匹配）\n");
    rs_reject(id);
    return 1;
  }
  p = ev->param.conn.private_data;
  cq = ibv_create_cq(id->verbs, 16, NULL, NULL, 0);
  if (!cq) {
    rs_reject(id);
    return 1;
  }
  memset(&qia, 0, sizeof(qia));
  qia.qp_type = IBV_QPT_RC;
  qia.send_cq = cq;
  qia.recv_cq = cq;
  qia.cap.max_send_wr = 16;
  qia.cap.max_recv_wr = 16;
  qia.cap.max_send_sge = 1;
  qia.cap.max_recv_sge = 1;
  if (rdma_create_qp(id, a->pd, &qia) != 0) {
    fprintf(stderr, "roswap-agent: rdma_create_qp: %s\n", strerror(errno));
    ibv_destroy_cq(cq);
    rs_reject(id);
    return 1;
  }
  memset(&cp, 0, sizeof(cp));
  depth = ev->param.conn.initiator_depth;
  if (depth < 1)
    depth = 1;
  if (depth > a->max_rd_atom)
    depth = a->max_rd_atom;
  cp.responder_resources = (uint8_t)depth;
  cp.initiator_depth = 1;
  cp.retry_count = 7;
  cp.rnr_retry_count = 7;
  if (rdma_accept(id, &cp) != 0) {
    fprintf(stderr, "roswap-agent: rdma_accept: %s\n", strerror(errno));
    rdma_destroy_qp(id);
    ibv_destroy_cq(cq);
    return 1;
  }
  ch = calloc(1, sizeof(*ch));
  if (!ch) {
    fprintf(stderr, "roswap-agent: 连接已接受但无法跟踪，进程退出前不会回收\n");
    return 0;
  }
  ch->id = id;
  ch->cq = cq;
  ch->next = a->children;
  a->children = ch;
  fprintf(stderr, "roswap-agent: accept cpu %u node %u epoch %u\n",
    be16toh(p->cpu), be16toh(p->node_id), be32toh(p->node_epoch));
  return 0;
}

static void rs_drop_child(struct rs_agent *a, struct rdma_cm_id *id)
{
  struct rs_child **pp = &a->children;

  while (*pp) {
    if ((*pp)->id == id) {
      struct rs_child *c = *pp;
      *pp = c->next;
      if (c->id && c->id->qp)
        rdma_destroy_qp(c->id);
      if (c->cq)
        ibv_destroy_cq(c->cq);
      if (c->id)
        rdma_destroy_id(c->id);
      free(c);
      return;
    }
    pp = &(*pp)->next;
  }
}

static int rs_handle(struct rs_agent *a, struct rdma_cm_event *ev)
{
  switch (ev->event) {
  case RDMA_CM_EVENT_CONNECT_REQUEST:
    return rs_accept(a, ev);
  case RDMA_CM_EVENT_ESTABLISHED:
    return 0;
  case RDMA_CM_EVENT_DISCONNECTED:
  case RDMA_CM_EVENT_DEVICE_REMOVAL:
    fprintf(stderr, "roswap-agent: disconnect\n");
    return 0;
  default:
    return 0;
  }
}

int rs_agent_serve(struct rs_agent *a)
{
  if (!a || !a->ch)
    return -EINVAL;
  while (!a->stopping) {
    struct pollfd pf[2];
    struct rdma_cm_event *ev;
    int rc;

    pf[0].fd = a->ch->fd;
    pf[0].events = POLLIN;
    pf[1].fd = a->stop_pipe[0];
    pf[1].events = POLLIN;
    rc = poll(pf, 2, -1);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      return -errno;
    }
    if (pf[1].revents)
      break;
    if (!(pf[0].revents & POLLIN))
      continue;
    if (rdma_get_cm_event(a->ch, &ev) != 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      return -errno;
    }
    {
      struct rdma_cm_id *id = ev->id;
      enum rdma_cm_event_type type = ev->event;

      rc = rs_handle(a, ev);
      rdma_ack_cm_event(ev);
      if (rc == 1)
        rdma_destroy_id(id);
      else if (type == RDMA_CM_EVENT_DISCONNECTED ||
         type == RDMA_CM_EVENT_DEVICE_REMOVAL)
        rs_drop_child(a, id);
      else if (rc)
        return rc;
    }
  }
  return 0;
}

void rs_agent_stop(struct rs_agent *a)
{
  char b = 1;

  if (!a || a->stop_pipe[1] < 0)
    return;
  a->stopping = 1;
  if (write(a->stop_pipe[1], &b, 1) < 0)
    fprintf(stderr, "roswap-agent: stop pipe: %s\n", strerror(errno));
}

void rs_agent_close(struct rs_agent *a)
{
  struct rs_map *m;
  struct rs_child *c;

  if (!a)
    return;
  while ((c = a->children) != NULL)
    rs_drop_child(a, c->id);
  if (a->listen) {
    rdma_destroy_id(a->listen);
    a->listen = NULL;
  }
  while ((m = a->maps) != NULL) {
    a->maps = m->next;
    if (m->mr)
      ibv_dereg_mr(m->mr);
    if (m->addr && m->addr != MAP_FAILED)
      munmap(m->addr, m->len);
    if (m->fd >= 0)
      close(m->fd);
    free(m);
  }
  if (a->pd)
    ibv_dealloc_pd(a->pd);
  if (a->ch)
    rdma_destroy_event_channel(a->ch);
  if (a->stop_pipe[0] >= 0)
    close(a->stop_pipe[0]);
  if (a->stop_pipe[1] >= 0)
    close(a->stop_pipe[1]);
  free(a);
}
