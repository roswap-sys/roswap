#ifndef RS_AGENT_CM_H
#define RS_AGENT_CM_H

#include <stddef.h>
#include <stdint.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

struct rs_agent;

struct rs_map_result {
  int err;
  uint32_t rkey;
  uint64_t addr;
};

struct rs_agent *rs_agent_open(const char *ip, int port, const char *master_ip,
             const uint8_t uuid[16], int check_uuid);
struct rs_map_result rs_agent_map(struct rs_agent *a, const char *path, size_t len, int numa);
int rs_agent_serve(struct rs_agent *a);
void rs_agent_stop(struct rs_agent *a);
void rs_agent_close(struct rs_agent *a);

#endif
