/* 内核与 roswap-agent 共享的连接 private data。布局必须保持 48 字节。 */
#ifndef _UAPI_ROSWAP_PROTO_H
#define _UAPI_ROSWAP_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
#endif

#define RS_CONN_MAGIC		0x52535750u /* 'RSWP' */
#define RS_CONN_VERSION		1
#define RS_QP_DATA		1 /* M1：每 CPU 一条数据 QP。FAULT/BULK 在 W5 拆分。 */
#define RS_AGENT_PORT		18515

struct rs_conn_pdata {
	__be32	magic;
	__be16	version;
	__u8	purpose;
	__u8	rsvd;
	__be16	cpu;
	__be16	node_id;
	__be32	node_epoch;
	__u8	cluster_uuid[16];
	__be64	master_boot_id;
	__be32	rsvd2[2];
} __packed;

#endif
