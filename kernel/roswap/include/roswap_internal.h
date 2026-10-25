#ifndef _ROSWAP_INTERNAL_H
#define _ROSWAP_INTERNAL_H

#include <linux/atomic.h>
#include <linux/blk-mq.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/percpu-refcount.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "rs_compat.h"
#include "roswap_proto.h"

#define RS_MAX_NODES		256
#define RS_MAX_COPIES		2
#define RS_EXTENT_PAGES		256
#define RS_CHUNK_EXTENTS	1024
#define RS_EXT_LOCKS		4096
#define RS_EXT_RESERVE		8
#define RS_MAX_SEGS_HARD	32
#define RS_LOC_NONE		0xffffffffu
#define RS_GID_MAX		((1u << 22) - 1)

#define RS_S_C0			0x01
#define RS_S_USED		0x40

enum rs_node_state {
	RS_N_EMPTY = 0,
	RS_N_JOINING,
	RS_N_ACTIVE,
	RS_N_ERROR,
};

enum rs_conn_state {
	RS_CONN_INIT = 0,
	RS_CONN_CONNECTING,
	RS_CONN_UP,
	RS_CONN_ERROR,
	RS_CONN_CLOSED,
};

struct rs_extent {
	u32 loc[RS_MAX_COPIES];
};

struct rs_dev;
struct rs_blk_cmd;
struct rs_chunk;

struct rs_conn {
	struct rdma_cm_id	*cm_id;
	struct ib_cq		*cq;
	spinlock_t		sq_lock;
	struct completion	established;
	atomic_t		state;
	u32			sq_depth;
	u32			sq_free;
	u32			unsignaled;
	int			cpu;
	u16			node_id;
	struct rs_conn_pdata	pdata;
};

struct rs_io {
	struct ib_cqe		cqe;
	struct ib_rdma_wr	wr;
	struct ib_sge		sge;
	struct rs_blk_cmd	*cmd;
	struct rs_conn		*conn;
	struct rs_chunk		*chunk;
	void			(*done)(struct rs_io *io, const struct ib_wc *wc);
	void			*priv;
	dma_addr_t		dma;
	pgoff_t			slot;
	u32			node_epoch;
	u32			remote_rkey;
	u64			remote_addr;
	int			opcode;
	u8			signaled;
	u8			is_write;
	u8			mapped;
	u16			chain_len;
	atomic_t		finished;
};

struct rs_blk_cmd {
	struct request		*rq;
	struct rs_dev		*dev;
	atomic_t		arrived;
	atomic_t		wc_status;
	atomic_t		cmd_done;
	int			nios;
	int			single_group;
	struct rs_io		ios[];
};

struct rs_chunk {
	struct rcu_head		rcu;
	struct percpu_ref	ioref;
	struct completion	dead;
	struct list_head	node_link;
	spinlock_t		lock;
	u32			gid;
	u16			node_id;
	u32			node_epoch;
	u32			nr_ext;
	u64			raddr;
	u32			rkey;
	u64			len;
	atomic_t		free_ext;
	DECLARE_BITMAP(ext_used, RS_CHUNK_EXTENTS);
	u32			owner[RS_CHUNK_EXTENTS];
};

struct rs_node {
	u16			id;
	u32			epoch;
	atomic_t		state;
	struct sockaddr_in	addr;
	char			addr_str[64];
	int			nr_conns;
	struct rs_conn		*conns;
	atomic_t		free_ext;
	atomic_t		mapped;
	int			nr_chunks;
	spinlock_t		lock;
	struct list_head	chunks;
	bool			local_only;
};

struct rs_pcpu_resv {
	spinlock_t	lock;
	u8		n[RS_MAX_NODES];
	u32		loc[RS_MAX_NODES][RS_EXT_RESERVE];
};

struct rs_blk {
	struct rs_dev		*dev;
	struct gendisk		*disk;
	struct request_queue	*q;
	struct blk_mq_tag_set	tag_set;
	int			major;
};

struct rs_dev {
	u64			nr_slots;
	u64			nr_extents;
	int			max_segs;
	u8			*sstate;
	struct rs_extent	*extents;
	u32			*ext_live;
	u32			*ext_inflight;
	spinlock_t		*ext_locks;
	struct rs_pcpu_resv __percpu *resv;
	struct rs_blk		*blk;
	atomic_t		openers;
	atomic64_t		read_ok;
	atomic64_t		write_ok;
	atomic64_t		read_err;
	atomic64_t		write_err;
	atomic64_t		discard_pages;
	atomic64_t		zero_read;
	atomic64_t		alloc_fail;
};

extern struct ib_device *rs_ibdev;
extern struct ib_pd *rs_pd;
extern u32 rs_lkey;
extern struct mutex rs_ctl_lock;
extern struct rs_dev *rs_device;
extern struct xarray rs_chunks;

extern int rs_sq_depth;
extern int rs_nr_conns;
extern int rs_ack_timeout;
extern int rs_retry_count;
extern int rs_tos;
extern int rs_max_segs;
extern int rs_queue_depth;
extern int rs_initiator_depth;
extern char rs_src_addr[64];
extern u8 rs_cluster_uuid[16];

void rs_globals_init(void);

int rs_parse_size(const char *s, u64 *out);
int rs_parse_u64(const char *s, u64 *out);
int rs_parse_u32(const char *s, u32 *out);

static inline u32 rs_pack_loc(u32 gid, u32 ext_idx)
{
	return (gid << 10) | (ext_idx & 0x3ffu);
}

static inline void rs_unpack_loc(u32 loc, u32 *gid, u32 *ext_idx)
{
	*gid = loc >> 10;
	*ext_idx = loc & 0x3ffu;
}

const char *rs_node_state_name(int state);

int rs_node_add(u16 id, const char *ip, u16 port, u32 epoch);
int rs_node_add_local(u16 id, u32 epoch);
void rs_node_del(u16 id);
struct rs_node *rs_node_get(u16 id);
struct rs_conn *rs_node_conn_up(struct rs_node *n, int cpu);
void rs_node_note_conn_error(struct rs_conn *c);
void rs_node_shutdown_all(void);

int rs_conn_connect(struct rs_conn *c, struct rs_node *n, int index);
void rs_conn_destroy(struct rs_conn *c);

int rs_conn_post(struct rs_conn *c, struct rs_io **ios, int n, bool selective);
void rs_io_software_fail(struct rs_io *io);

int rs_chunk_add(u16 node_id, u32 gid, u32 epoch, u64 raddr, u32 rkey, u64 len);
void rs_chunk_del(u32 gid);
void rs_chunks_destroy(void);
struct rs_chunk *rs_chunk_lookup(u32 gid);
void rs_chunk_put(struct rs_chunk *c);
int rs_chunk_alloc_locs(struct rs_node *n, int want, u32 *locs, int *got);
void rs_chunk_free_loc(u32 loc);
void rs_chunk_set_owner(struct rs_chunk *c, u32 ext_idx, u32 extent);

int rs_dev_alloc(struct rs_dev **out, u64 bytes);
void rs_dev_free(struct rs_dev *dev);
int rs_dev_create(u64 bytes);
void rs_dev_destroy(void);

void rs_node_mapped_add(u32 loc, int delta);
int rs_write_pin(struct rs_dev *dev, pgoff_t slot, u32 *loc);
int rs_read_pin(struct rs_dev *dev, pgoff_t slot, u32 *loc, bool *zero);
void rs_write_commit(struct rs_dev *dev, pgoff_t slot);
void rs_pin_put(struct rs_dev *dev, pgoff_t slot);
void rs_discard_range(struct rs_dev *dev, pgoff_t first, u32 nr);
void rs_resv_flush(struct rs_dev *dev);
unsigned int rs_resv_cached(struct rs_dev *dev);

blk_status_t rs_io_queue(struct rs_dev *dev, struct request *rq, int cpu);

int rs_blk_attach(struct rs_dev *dev);
void rs_blk_detach(struct rs_dev *dev);

int rs_debugfs_init(void);
void rs_debugfs_exit(void);

int rs_meta_selftest(void);
int rs_rdma_selftest(u32 gid);

#endif
