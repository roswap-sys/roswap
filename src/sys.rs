// SPDX-License-Identifier: GPL-2.0
#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_uint, c_void};

pub const AF_INET: u16 = 2;
pub const PROT_READ: c_int = 0x1;
pub const PROT_WRITE: c_int = 0x2;
pub const MAP_SHARED: c_int = 0x01;
pub const MAP_POPULATE: c_int = 0x08000;
pub const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
pub const MADV_HUGEPAGE: c_int = 14;
pub const MCL_CURRENT: c_int = 1;
pub const MCL_FUTURE: c_int = 2;
pub const POLLIN: i16 = 0x0001;

pub const IBV_ACCESS_LOCAL_WRITE: c_int = 1;
pub const IBV_ACCESS_REMOTE_WRITE: c_int = 1 << 1;
pub const IBV_ACCESS_REMOTE_READ: c_int = 1 << 2;

pub const IBV_QPT_RC: c_uint = 2;
pub const IBV_WR_RDMA_WRITE: c_uint = 0;
pub const IBV_WR_RDMA_READ: c_uint = 4;
pub const IBV_SEND_SIGNALED: c_uint = 1 << 1;
pub const IBV_WC_SUCCESS: c_uint = 0;

pub const RDMA_PS_TCP: c_uint = 0x0106;

pub const RDMA_CM_EVENT_ADDR_RESOLVED: c_uint = 0;
pub const RDMA_CM_EVENT_ADDR_ERROR: c_uint = 1;
pub const RDMA_CM_EVENT_ROUTE_RESOLVED: c_uint = 2;
pub const RDMA_CM_EVENT_ROUTE_ERROR: c_uint = 3;
pub const RDMA_CM_EVENT_CONNECT_REQUEST: c_uint = 4;
pub const RDMA_CM_EVENT_CONNECT_RESPONSE: c_uint = 5;
pub const RDMA_CM_EVENT_CONNECT_ERROR: c_uint = 6;
pub const RDMA_CM_EVENT_UNREACHABLE: c_uint = 7;
pub const RDMA_CM_EVENT_REJECTED: c_uint = 8;
pub const RDMA_CM_EVENT_ESTABLISHED: c_uint = 9;
pub const RDMA_CM_EVENT_DISCONNECTED: c_uint = 10;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct sockaddr {
    pub sa_family: u16,
    pub sa_data: [c_char; 14],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct in_addr {
    pub s_addr: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct sockaddr_in {
    pub sin_family: u16,
    pub sin_port: u16,
    pub sin_addr: in_addr,
    pub sin_zero: [u8; 8],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct sockaddr_storage {
    pub ss_family: u16,
    pub __ss_padding: [u8; 118],
    pub __ss_align: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct pollfd {
    pub fd: c_int,
    pub events: i16,
    pub revents: i16,
}

#[repr(C)]
pub struct ibv_device {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ibv_comp_channel {
    pub context: *mut ibv_context,
    pub fd: c_int,
    pub refcnt: c_int,
}

#[repr(C)]
pub struct ibv_cq {
    pub context: *mut ibv_context,
    pub channel: *mut ibv_comp_channel,
    pub cq_context: *mut c_void,
    pub handle: u32,
    pub cqe: c_int,
}

#[repr(C)]
pub struct ibv_srq {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ibv_ah {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ibv_mw {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ibv_qp {
    pub context: *mut ibv_context,
    pub qp_context: *mut c_void,
    pub pd: *mut ibv_pd,
    pub send_cq: *mut ibv_cq,
    pub recv_cq: *mut ibv_cq,
    pub srq: *mut ibv_srq,
    pub handle: u32,
    pub qp_num: u32,
    pub state: c_uint,
    pub qp_type: c_uint,
}

#[repr(C)]
pub struct ibv_pd {
    pub context: *mut ibv_context,
    pub handle: u32,
}

#[repr(C)]
pub struct ibv_mr {
    pub context: *mut ibv_context,
    pub pd: *mut ibv_pd,
    pub addr: *mut c_void,
    pub length: usize,
    pub handle: u32,
    pub lkey: u32,
    pub rkey: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ibv_wc {
    pub wr_id: u64,
    pub status: c_uint,
    pub opcode: c_uint,
    pub vendor_err: u32,
    pub byte_len: u32,
    pub imm_data_or_inv_rkey: u32,
    pub qp_num: u32,
    pub src_qp: u32,
    pub wc_flags: c_uint,
    pub pkey_index: u16,
    pub slid: u16,
    pub sl: u8,
    pub dlid_path_bits: u8,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ibv_sge {
    pub addr: u64,
    pub length: u32,
    pub lkey: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ibv_send_wr_rdma {
    pub remote_addr: u64,
    pub rkey: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ibv_send_wr_atomic {
    pub remote_addr: u64,
    pub compare_add: u64,
    pub swap: u64,
    pub rkey: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union ibv_send_wr_wr {
    pub rdma: ibv_send_wr_rdma,
    pub atomic: ibv_send_wr_atomic,
}

#[repr(C)]
pub struct ibv_send_wr {
    pub wr_id: u64,
    pub next: *mut ibv_send_wr,
    pub sg_list: *mut ibv_sge,
    pub num_sge: c_int,
    pub opcode: c_uint,
    pub send_flags: c_uint,
    pub imm_data: u32,
    pub wr: ibv_send_wr_wr,
    pub qp_type_xrc_remote_srqn: u32,
    pub bind_mw_or_tso: [u64; 5],
}

#[repr(C)]
pub struct ibv_recv_wr {
    pub wr_id: u64,
    pub next: *mut ibv_recv_wr,
    pub sg_list: *mut ibv_sge,
    pub num_sge: c_int,
}

#[repr(C)]
pub struct ibv_context_ops {
    pub _compat_query_device: *const c_void,
    pub _compat_query_port: *const c_void,
    pub _compat_alloc_pd: *const c_void,
    pub _compat_dealloc_pd: *const c_void,
    pub _compat_reg_mr: *const c_void,
    pub _compat_rereg_mr: *const c_void,
    pub _compat_dereg_mr: *const c_void,
    pub alloc_mw: *const c_void,
    pub bind_mw: *const c_void,
    pub dealloc_mw: *const c_void,
    pub _compat_create_cq: *const c_void,
    pub poll_cq: Option<unsafe extern "C" fn(*mut ibv_cq, c_int, *mut ibv_wc) -> c_int>,
    pub req_notify_cq: Option<unsafe extern "C" fn(*mut ibv_cq, c_int) -> c_int>,
    pub _compat_cq_event: *const c_void,
    pub _compat_resize_cq: *const c_void,
    pub _compat_destroy_cq: *const c_void,
    pub _compat_create_srq: *const c_void,
    pub _compat_modify_srq: *const c_void,
    pub _compat_query_srq: *const c_void,
    pub _compat_destroy_srq: *const c_void,
    pub post_srq_recv: *const c_void,
    pub _compat_create_qp: *const c_void,
    pub _compat_query_qp: *const c_void,
    pub _compat_modify_qp: *const c_void,
    pub _compat_destroy_qp: *const c_void,
    pub post_send:
        Option<unsafe extern "C" fn(*mut ibv_qp, *mut ibv_send_wr, *mut *mut ibv_send_wr) -> c_int>,
    pub post_recv:
        Option<unsafe extern "C" fn(*mut ibv_qp, *mut ibv_recv_wr, *mut *mut ibv_recv_wr) -> c_int>,
    pub _compat_create_ah: *const c_void,
    pub _compat_destroy_ah: *const c_void,
    pub _compat_attach_mcast: *const c_void,
    pub _compat_detach_mcast: *const c_void,
    pub _compat_async_event: *const c_void,
}

#[repr(C)]
pub struct ibv_context {
    pub device: *mut ibv_device,
    pub ops: ibv_context_ops,
    pub cmd_fd: c_int,
    pub async_fd: c_int,
    pub num_comp_vectors: c_int,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ibv_qp_cap {
    pub max_send_wr: u32,
    pub max_recv_wr: u32,
    pub max_send_sge: u32,
    pub max_recv_sge: u32,
    pub max_inline_data: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ibv_qp_init_attr {
    pub qp_context: *mut c_void,
    pub send_cq: *mut ibv_cq,
    pub recv_cq: *mut ibv_cq,
    pub srq: *mut ibv_srq,
    pub cap: ibv_qp_cap,
    pub qp_type: c_uint,
    pub sq_sig_all: c_int,
}

#[repr(C)]
pub struct rdma_event_channel {
    pub fd: c_int,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct rdma_ib_addr {
    pub sgid: [u8; 16],
    pub dgid: [u8; 16],
    pub pkey: u16,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct rdma_addr {
    pub src_storage: sockaddr_storage,
    pub dst_storage: sockaddr_storage,
    pub ibaddr: rdma_ib_addr,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct rdma_route {
    pub addr: rdma_addr,
    pub path_rec: *mut c_void,
    pub num_paths: c_int,
}

#[repr(C)]
pub struct rdma_cm_id {
    pub verbs: *mut ibv_context,
    pub channel: *mut rdma_event_channel,
    pub context: *mut c_void,
    pub qp: *mut ibv_qp,
    pub route: rdma_route,
    pub ps: c_uint,
    pub port_num: u8,
    pub event: *mut rdma_cm_event,
    pub send_cq_channel: *mut ibv_comp_channel,
    pub send_cq: *mut ibv_cq,
    pub recv_cq_channel: *mut ibv_comp_channel,
    pub recv_cq: *mut ibv_cq,
    pub srq: *mut ibv_srq,
    pub pd: *mut ibv_pd,
    pub qp_type: c_uint,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct rdma_conn_param {
    pub private_data: *const c_void,
    pub private_data_len: u8,
    pub responder_resources: u8,
    pub initiator_depth: u8,
    pub flow_control: u8,
    pub retry_count: u8,
    pub rnr_retry_count: u8,
    pub srq: u8,
    pub qp_num: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct rdma_cm_event {
    pub id: *mut rdma_cm_id,
    pub listen_id: *mut rdma_cm_id,
    pub event: c_uint,
    pub status: c_int,
    pub conn: rdma_conn_param,
}

#[inline]
pub unsafe fn ibv_post_send(
    qp: *mut ibv_qp,
    wr: *mut ibv_send_wr,
    bad_wr: *mut *mut ibv_send_wr,
) -> c_int {
    match (*(*qp).context).ops.post_send {
        Some(f) => f(qp, wr, bad_wr),
        None => -1,
    }
}

#[inline]
pub unsafe fn ibv_poll_cq(cq: *mut ibv_cq, num_entries: c_int, wc: *mut ibv_wc) -> c_int {
    match (*(*cq).context).ops.poll_cq {
        Some(f) => f(cq, num_entries, wc),
        None => -1,
    }
}

extern "C" {
    pub fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    pub fn munmap(addr: *mut c_void, length: usize) -> c_int;
    pub fn madvise(addr: *mut c_void, length: usize, advice: c_int) -> c_int;
    pub fn mlockall(flags: c_int) -> c_int;
    pub fn posix_memalign(memptr: *mut *mut c_void, alignment: usize, size: usize) -> c_int;
    pub fn free(ptr: *mut c_void);
    pub fn poll(fds: *mut pollfd, nfds: u64, timeout: c_int) -> c_int;

    pub fn ibv_get_device_list(num_devices: *mut c_int) -> *mut *mut ibv_device;
    pub fn ibv_free_device_list(list: *mut *mut ibv_device);
    pub fn ibv_get_device_name(device: *mut ibv_device) -> *const c_char;
    pub fn ibv_open_device(device: *mut ibv_device) -> *mut ibv_context;
    pub fn ibv_close_device(context: *mut ibv_context) -> c_int;
    pub fn ibv_alloc_pd(context: *mut ibv_context) -> *mut ibv_pd;
    pub fn ibv_dealloc_pd(pd: *mut ibv_pd) -> c_int;
    pub fn ibv_reg_mr(
        pd: *mut ibv_pd,
        addr: *mut c_void,
        length: usize,
        access: c_int,
    ) -> *mut ibv_mr;
    pub fn ibv_dereg_mr(mr: *mut ibv_mr) -> c_int;
    pub fn ibv_create_cq(
        context: *mut ibv_context,
        cqe: c_int,
        cq_context: *mut c_void,
        channel: *mut ibv_comp_channel,
        comp_vector: c_int,
    ) -> *mut ibv_cq;
    pub fn ibv_destroy_cq(cq: *mut ibv_cq) -> c_int;

    pub fn rdma_create_event_channel() -> *mut rdma_event_channel;
    pub fn rdma_destroy_event_channel(channel: *mut rdma_event_channel);
    pub fn rdma_get_devices(num_devices: *mut c_int) -> *mut *mut ibv_context;
    pub fn rdma_free_devices(list: *mut *mut ibv_context);
    pub fn rdma_create_id(
        channel: *mut rdma_event_channel,
        id: *mut *mut rdma_cm_id,
        context: *mut c_void,
        ps: c_uint,
    ) -> c_int;
    pub fn rdma_destroy_id(id: *mut rdma_cm_id) -> c_int;
    pub fn rdma_bind_addr(id: *mut rdma_cm_id, addr: *mut sockaddr) -> c_int;
    pub fn rdma_resolve_addr(
        id: *mut rdma_cm_id,
        src_addr: *mut sockaddr,
        dst_addr: *mut sockaddr,
        timeout_ms: c_int,
    ) -> c_int;
    pub fn rdma_resolve_route(id: *mut rdma_cm_id, timeout_ms: c_int) -> c_int;
    pub fn rdma_listen(id: *mut rdma_cm_id, backlog: c_int) -> c_int;
    pub fn rdma_create_qp(
        id: *mut rdma_cm_id,
        pd: *mut ibv_pd,
        qp_init_attr: *mut ibv_qp_init_attr,
    ) -> c_int;
    pub fn rdma_destroy_qp(id: *mut rdma_cm_id);
    pub fn rdma_connect(id: *mut rdma_cm_id, conn_param: *mut rdma_conn_param) -> c_int;
    pub fn rdma_accept(id: *mut rdma_cm_id, conn_param: *mut rdma_conn_param) -> c_int;
    pub fn rdma_reject(
        id: *mut rdma_cm_id,
        private_data: *const c_void,
        private_data_len: u8,
    ) -> c_int;
    pub fn rdma_disconnect(id: *mut rdma_cm_id) -> c_int;
    pub fn rdma_get_cm_event(
        channel: *mut rdma_event_channel,
        event: *mut *mut rdma_cm_event,
    ) -> c_int;
    pub fn rdma_ack_cm_event(event: *mut rdma_cm_event) -> c_int;
}
