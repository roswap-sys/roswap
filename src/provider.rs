// SPDX-License-Identifier: GPL-2.0
use crate::chunk::{Chunk, HeartbeatPage};
use crate::rdma::{
    sockaddr_from_v4, ConnPrivateData, PassiveConnection, QpPurpose, RdmaDeviceContext,
};
use crate::sys;
use log::{debug, info, warn};
use std::collections::HashMap;
use std::fs;
use std::io;
use std::net::SocketAddrV4;
use std::path::PathBuf;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct ProviderConfig {
    pub device_name: Option<String>,
    pub listen_addr: SocketAddrV4,
    pub mem_dir: PathBuf,
    pub chunk_size_mb: usize,
    pub num_chunks: usize,
    pub node_epoch: u32,
}

impl Default for ProviderConfig {
    fn default() -> Self {
        Self {
            device_name: None,
            listen_addr: "0.0.0.0:18515".parse().unwrap(),
            mem_dir: PathBuf::from("/dev/shm/roswap"),
            chunk_size_mb: 64,
            num_chunks: 1,
            node_epoch: 1,
        }
    }
}

pub struct ProviderRuntime {
    pub config: ProviderConfig,
    pub rdma: RdmaDeviceContext,
    pub heartbeat: Arc<HeartbeatPage>,
    pub chunks: Vec<Chunk>,
    channel: NonNull<sys::rdma_event_channel>,
    listen_id: NonNull<sys::rdma_cm_id>,
    conns: HashMap<usize, PassiveConnection>,
}

unsafe impl Send for ProviderRuntime {}

impl ProviderRuntime {
    pub fn init(config: ProviderConfig) -> io::Result<Self> {
        unsafe {
            let _ = sys::mlockall(sys::MCL_CURRENT | sys::MCL_FUTURE);
        }

        let rdma = RdmaDeviceContext::open(config.device_name.as_deref())?;
        let boot_id_lo = read_boot_id_lo();
        let heartbeat = Arc::new(HeartbeatPage::new(
            rdma.pd(),
            config.node_epoch,
            boot_id_lo,
        )?);

        let chunk_bytes = config
            .chunk_size_mb
            .checked_mul(1 << 20)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "chunk size overflow"))?;

        let mut chunks = Vec::with_capacity(config.num_chunks);
        for gid in 0..(config.num_chunks as u32) {
            let chunk = Chunk::create(
                rdma.pd(),
                &config.mem_dir,
                gid,
                config.node_epoch,
                chunk_bytes,
            )?;
            chunks.push(chunk);
        }

        let raw_ch = unsafe { sys::rdma_create_event_channel() };
        let channel = NonNull::new(raw_ch).ok_or_else(io::Error::last_os_error)?;

        let mut raw_listen_id: *mut sys::rdma_cm_id = std::ptr::null_mut();
        let rc = unsafe {
            sys::rdma_create_id(
                channel.as_ptr(),
                &mut raw_listen_id,
                std::ptr::null_mut(),
                sys::RDMA_PS_TCP,
            )
        };
        if rc != 0 || raw_listen_id.is_null() {
            let err = io::Error::last_os_error();
            unsafe { sys::rdma_destroy_event_channel(channel.as_ptr()) };
            return Err(err);
        }
        let listen_id = unsafe { NonNull::new_unchecked(raw_listen_id) };

        let mut sin = sockaddr_from_v4(config.listen_addr);
        let rc = unsafe {
            sys::rdma_bind_addr(
                listen_id.as_ptr(),
                &mut sin as *mut sys::sockaddr_in as *mut sys::sockaddr,
            )
        };
        if rc != 0 {
            let err = io::Error::last_os_error();
            unsafe {
                let _ = sys::rdma_destroy_id(listen_id.as_ptr());
                sys::rdma_destroy_event_channel(channel.as_ptr());
            }
            return Err(err);
        }

        let rc = unsafe { sys::rdma_listen(listen_id.as_ptr(), 128) };
        if rc != 0 {
            let err = io::Error::last_os_error();
            unsafe {
                let _ = sys::rdma_destroy_id(listen_id.as_ptr());
                sys::rdma_destroy_event_channel(channel.as_ptr());
            }
            return Err(err);
        }

        Ok(Self {
            config,
            rdma,
            heartbeat,
            chunks,
            channel,
            listen_id,
            conns: HashMap::new(),
        })
    }

    pub fn log_summary(&self) {
        info!(
            "provider initialized device={} listen={} epoch={}",
            self.rdma.dev_name, self.config.listen_addr, self.config.node_epoch
        );
        info!(
            "heartbeat registered raddr=0x{:016x} rkey=0x{:08x} lkey=0x{:08x}",
            self.heartbeat.addr(),
            self.heartbeat.rkey(),
            self.heartbeat.lkey()
        );
        for c in &self.chunks {
            info!(
                "chunk registered gid={} raddr=0x{:016x} len_mb={} rkey=0x{:08x} lkey=0x{:08x} file={}",
                c.gid,
                c.addr(),
                c.len() >> 20,
                c.rkey(),
                c.lkey(),
                c.file_path.display()
            );
        }
    }

    pub fn active_connection_count(&self) -> usize {
        self.conns.len()
    }

    pub fn poll_and_handle_once(&mut self, timeout_ms: i32) -> io::Result<bool> {
        let fd = unsafe { (*self.channel.as_ptr()).fd };
        let mut pfd = sys::pollfd {
            fd,
            events: sys::POLLIN,
            revents: 0,
        };
        let n = unsafe { sys::poll(&mut pfd, 1, timeout_ms) };
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.kind() == io::ErrorKind::Interrupted {
                return Ok(false);
            }
            return Err(err);
        }
        if n == 0 || (pfd.revents & sys::POLLIN) == 0 {
            return Ok(false);
        }

        let mut raw_ev: *mut sys::rdma_cm_event = std::ptr::null_mut();
        let rc = unsafe { sys::rdma_get_cm_event(self.channel.as_ptr(), &mut raw_ev) };
        if rc != 0 || raw_ev.is_null() {
            return Err(io::Error::last_os_error());
        }

        let ev = unsafe { *raw_ev };
        unsafe {
            let _ = sys::rdma_ack_cm_event(raw_ev);
        }

        self.handle_cm_event(ev)?;
        Ok(true)
    }

    fn handle_cm_event(&mut self, ev: sys::rdma_cm_event) -> io::Result<()> {
        match ev.event {
            sys::RDMA_CM_EVENT_CONNECT_REQUEST => {
                let client_id = match NonNull::new(ev.id) {
                    Some(id) => id,
                    None => return Ok(()),
                };
                if let Err(msg) = self.accept_client(client_id, &ev.conn) {
                    warn!("rejected connection: {msg}");
                    unsafe {
                        let _ = sys::rdma_reject(client_id.as_ptr(), std::ptr::null(), 0);
                        let _ = sys::rdma_destroy_id(client_id.as_ptr());
                    }
                }
            }
            sys::RDMA_CM_EVENT_ESTABLISHED => {
                if let Some(conn) = self.conns.get(&(ev.id as usize)) {
                    info!(
                        "connection established purpose={:?} cpu={} node_id={}",
                        conn.purpose, conn.cpu, conn.node_id
                    );
                }
            }
            sys::RDMA_CM_EVENT_DISCONNECTED => {
                let key = ev.id as usize;
                if let Some(conn) = self.conns.remove(&key) {
                    info!(
                        "connection disconnected purpose={:?} cpu={} node_id={}",
                        conn.purpose, conn.cpu, conn.node_id
                    );
                    unsafe {
                        let _ = sys::rdma_disconnect(conn.cm_id.as_ptr());
                    }
                }
            }
            _ => {
                debug!("ignored cm event {} status {}", ev.event, ev.status);
            }
        }
        Ok(())
    }

    fn accept_client(
        &mut self,
        client_id: NonNull<sys::rdma_cm_id>,
        conn_param: &sys::rdma_conn_param,
    ) -> Result<(), String> {
        let (purpose, cpu, node_id) =
            if !conn_param.private_data.is_null()
                && (conn_param.private_data_len as usize) >= std::mem::size_of::<ConnPrivateData>()
            {
                let pdata =
                    unsafe { *(conn_param.private_data as *const ConnPrivateData) };
                let purpose = pdata
                    .validate(Some(self.config.node_epoch))
                    .map_err(|e| e.to_string())?;
                (purpose, pdata.cpu(), pdata.node_id())
            } else {
                (QpPurpose::Bulk, 0, 0)
            };

        let mut qp_attr = sys::ibv_qp_init_attr {
            qp_context: std::ptr::null_mut(),
            send_cq: self.rdma.shared_cq().as_ptr(),
            recv_cq: self.rdma.shared_cq().as_ptr(),
            srq: std::ptr::null_mut(),
            cap: sys::ibv_qp_cap {
                max_send_wr: 16,
                max_recv_wr: 16,
                max_send_sge: 1,
                max_recv_sge: 1,
                max_inline_data: 0,
            },
            qp_type: sys::IBV_QPT_RC,
            sq_sig_all: 0,
        };

        let rc = unsafe {
            sys::rdma_create_qp(client_id.as_ptr(), self.rdma.pd().as_ptr(), &mut qp_attr)
        };
        if rc != 0 {
            return Err(format!(
                "rdma_create_qp failed: {}",
                io::Error::last_os_error()
            ));
        }

        let mut accept_param = sys::rdma_conn_param {
            private_data: std::ptr::null(),
            private_data_len: 0,
            responder_resources: 16,
            initiator_depth: 16,
            flow_control: 1,
            retry_count: 4,
            rnr_retry_count: 3,
            srq: 0,
            qp_num: 0,
        };

        let rc = unsafe { sys::rdma_accept(client_id.as_ptr(), &mut accept_param) };
        if rc != 0 {
            let err = io::Error::last_os_error();
            unsafe { sys::rdma_destroy_qp(client_id.as_ptr()) };
            return Err(format!("rdma_accept failed: {err}"));
        }

        let key = client_id.as_ptr() as usize;
        self.conns.insert(
            key,
            PassiveConnection {
                cm_id: client_id,
                purpose,
                cpu,
                node_id,
            },
        );
        info!("accepted connection purpose={purpose:?} cpu={cpu} node_id={node_id}");
        Ok(())
    }

    pub fn run_until_stopped(&mut self, stop: Arc<AtomicBool>) -> io::Result<()> {
        let hb = Arc::clone(&self.heartbeat);
        let hb_stop = Arc::clone(&stop);
        let hb_handle = thread::spawn(move || {
            while !hb_stop.load(Ordering::Relaxed) {
                hb.tick();
                thread::sleep(Duration::from_millis(10));
            }
        });

        while !stop.load(Ordering::Relaxed) {
            self.poll_and_handle_once(50)?;
        }

        let _ = hb_handle.join();
        Ok(())
    }
}

impl Drop for ProviderRuntime {
    fn drop(&mut self) {
        self.conns.clear();
        unsafe {
            let _ = sys::rdma_destroy_id(self.listen_id.as_ptr());
            sys::rdma_destroy_event_channel(self.channel.as_ptr());
        }
    }
}

fn read_boot_id_lo() -> u64 {
    if let Ok(s) = fs::read_to_string("/proc/sys/kernel/random/boot_id") {
        let hex: String = s.chars().filter(|c| c.is_ascii_hexdigit()).take(16).collect();
        if let Ok(v) = u64::from_str_radix(&hex, 16) {
            return v;
        }
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rdma::local_rdma_ipv4;
    use std::ffi::c_void;
    use std::net::Ipv4Addr;

    #[test]
    fn test_chunk_mr_and_persistence_across_reopen() {
        let rdma = RdmaDeviceContext::open(None).expect("open RDMA device");
        let test_dir = PathBuf::from("/dev/shm/roswap-test-chunk");
        let _ = fs::remove_dir_all(&test_dir);

        let rkey_first;
        {
            let mut chunk =
                Chunk::create(rdma.pd(), &test_dir, 7, 1, 4 << 20).expect("create 4MB chunk");
            assert_ne!(chunk.rkey(), 0);
            assert_ne!(chunk.lkey(), 0);
            rkey_first = chunk.rkey();
            chunk.as_mut_slice()[0..8].copy_from_slice(b"ROSWAP01");
        }

        // Simulate agent crash/restart: file still exists on tmpfs/hugetlbfs, reopen and verify content
        let chunk_reopened =
            Chunk::create(rdma.pd(), &test_dir, 7, 1, 4 << 20).expect("reopen chunk");
        assert_eq!(&chunk_reopened.as_slice()[0..8], b"ROSWAP01");
        assert_ne!(chunk_reopened.rkey(), 0);
        let _ = rkey_first;

        chunk_reopened
            .release_and_unlink()
            .expect("unlink chunk file");
        let _ = fs::remove_dir_all(&test_dir);
    }

    #[test]
    fn test_provider_one_sided_rdma_write_and_read_loopback() {
        let target_ip = local_rdma_ipv4().expect("find RDMA interface IPv4");
        let listen_addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 18599);
        let connect_addr = SocketAddrV4::new(target_ip, 18599);
        let test_dir = PathBuf::from("/dev/shm/roswap-test-rdma");
        let _ = fs::remove_dir_all(&test_dir);

        let cfg = ProviderConfig {
            device_name: None,
            listen_addr,
            mem_dir: test_dir.clone(),
            chunk_size_mb: 4,
            num_chunks: 1,
            node_epoch: 3,
        };

        let mut provider = ProviderRuntime::init(cfg).expect("init provider");
        let remote_chunk_addr = provider.chunks[0].addr();
        let remote_chunk_rkey = provider.chunks[0].rkey();
        let remote_hb_addr = provider.heartbeat.addr();
        let remote_hb_rkey = provider.heartbeat.rkey();

        let stop = Arc::new(AtomicBool::new(false));
        let stop_bg = Arc::clone(&stop);

        let server_thread = thread::spawn(move || {
            provider.run_until_stopped(stop_bg).expect("provider loop");
            provider
        });

        // Client side: simulate roswap.ko connecting via rdma_cm and issuing 4KB one-sided RDMA WRITE & READ
        unsafe {
            let ch = sys::rdma_create_event_channel();
            assert!(!ch.is_null());

            let mut cid: *mut sys::rdma_cm_id = std::ptr::null_mut();
            assert_eq!(
                sys::rdma_create_id(ch, &mut cid, std::ptr::null_mut(), sys::RDMA_PS_TCP),
                0
            );

            let mut dst = sockaddr_from_v4(connect_addr);
            assert_eq!(
                sys::rdma_resolve_addr(
                    cid,
                    std::ptr::null_mut(),
                    &mut dst as *mut sys::sockaddr_in as *mut sys::sockaddr,
                    2000
                ),
                0
            );

            let mut ev: *mut sys::rdma_cm_event = std::ptr::null_mut();
            assert_eq!(sys::rdma_get_cm_event(ch, &mut ev), 0);
            assert_eq!((*ev).event, sys::RDMA_CM_EVENT_ADDR_RESOLVED);
            sys::rdma_ack_cm_event(ev);

            assert_eq!(sys::rdma_resolve_route(cid, 2000), 0);
            assert_eq!(sys::rdma_get_cm_event(ch, &mut ev), 0);
            assert_eq!((*ev).event, sys::RDMA_CM_EVENT_ROUTE_RESOLVED);
            sys::rdma_ack_cm_event(ev);

            let pd = sys::ibv_alloc_pd((*cid).verbs);
            assert!(!pd.is_null());
            let cq = sys::ibv_create_cq(
                (*cid).verbs,
                32,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                0,
            );
            assert!(!cq.is_null());

            let mut qp_attr = sys::ibv_qp_init_attr {
                qp_context: std::ptr::null_mut(),
                send_cq: cq,
                recv_cq: cq,
                srq: std::ptr::null_mut(),
                cap: sys::ibv_qp_cap {
                    max_send_wr: 16,
                    max_recv_wr: 16,
                    max_send_sge: 1,
                    max_recv_sge: 1,
                    max_inline_data: 0,
                },
                qp_type: sys::IBV_QPT_RC,
                sq_sig_all: 0,
            };
            assert_eq!(sys::rdma_create_qp(cid, pd, &mut qp_attr), 0);

            let pdata = ConnPrivateData::new(QpPurpose::Fault, 0, 1, 3);
            let mut conn_param = sys::rdma_conn_param {
                private_data: &pdata as *const ConnPrivateData as *const c_void,
                private_data_len: std::mem::size_of::<ConnPrivateData>() as u8,
                responder_resources: 16,
                initiator_depth: 16,
                flow_control: 1,
                retry_count: 4,
                rnr_retry_count: 3,
                srq: 0,
                qp_num: 0,
            };
            assert_eq!(sys::rdma_connect(cid, &mut conn_param), 0);
            assert_eq!(sys::rdma_get_cm_event(ch, &mut ev), 0);
            assert_eq!((*ev).event, sys::RDMA_CM_EVENT_ESTABLISHED);
            sys::rdma_ack_cm_event(ev);

            // Allocate two local 4KB pages (write_buf and read_buf)
            let mut write_buf = vec![0u8; 4096];
            for (i, b) in write_buf.iter_mut().enumerate() {
                *b = ((i * 31 + 7) & 0xFF) as u8;
            }
            let mut read_buf = vec![0u8; 4096];

            let write_mr = sys::ibv_reg_mr(
                pd,
                write_buf.as_mut_ptr() as *mut c_void,
                4096,
                sys::IBV_ACCESS_LOCAL_WRITE,
            );
            let read_mr = sys::ibv_reg_mr(
                pd,
                read_buf.as_mut_ptr() as *mut c_void,
                4096,
                sys::IBV_ACCESS_LOCAL_WRITE,
            );
            assert!(!write_mr.is_null() && !read_mr.is_null());

            // 1. One-sided RDMA WRITE 4KB to provider chunk at offset 64KB
            let page_offset = 64 * 1024;
            let mut sge_w = sys::ibv_sge {
                addr: write_buf.as_ptr() as u64,
                length: 4096,
                lkey: (*write_mr).lkey,
            };
            let mut wr_w: sys::ibv_send_wr = std::mem::zeroed();
            wr_w.wr_id = 101;
            wr_w.sg_list = &mut sge_w;
            wr_w.num_sge = 1;
            wr_w.opcode = sys::IBV_WR_RDMA_WRITE;
            wr_w.send_flags = sys::IBV_SEND_SIGNALED;
            wr_w.wr.rdma.remote_addr = remote_chunk_addr + page_offset;
            wr_w.wr.rdma.rkey = remote_chunk_rkey;

            let mut bad_wr: *mut sys::ibv_send_wr = std::ptr::null_mut();
            assert_eq!(sys::ibv_post_send((*cid).qp, &mut wr_w, &mut bad_wr), 0);

            let mut wc: sys::ibv_wc = std::mem::zeroed();
            while sys::ibv_poll_cq(cq, 1, &mut wc) == 0 {}
            assert_eq!(wc.status, sys::IBV_WC_SUCCESS);
            assert_eq!(wc.wr_id, 101);

            // 2. One-sided RDMA READ 4KB back from provider chunk at offset 64KB
            let mut sge_r = sys::ibv_sge {
                addr: read_buf.as_mut_ptr() as u64,
                length: 4096,
                lkey: (*read_mr).lkey,
            };
            let mut wr_r: sys::ibv_send_wr = std::mem::zeroed();
            wr_r.wr_id = 102;
            wr_r.sg_list = &mut sge_r;
            wr_r.num_sge = 1;
            wr_r.opcode = sys::IBV_WR_RDMA_READ;
            wr_r.send_flags = sys::IBV_SEND_SIGNALED;
            wr_r.wr.rdma.remote_addr = remote_chunk_addr + page_offset;
            wr_r.wr.rdma.rkey = remote_chunk_rkey;

            assert_eq!(sys::ibv_post_send((*cid).qp, &mut wr_r, &mut bad_wr), 0);
            while sys::ibv_poll_cq(cq, 1, &mut wc) == 0 {}
            assert_eq!(wc.status, sys::IBV_WC_SUCCESS);
            assert_eq!(wc.wr_id, 102);
            assert_eq!(write_buf, read_buf);

            // 3. One-sided RDMA READ 8-byte heartbeat counter
            let mut hb_sge = sys::ibv_sge {
                addr: read_buf.as_mut_ptr() as u64,
                length: 8,
                lkey: (*read_mr).lkey,
            };
            let mut wr_hb: sys::ibv_send_wr = std::mem::zeroed();
            wr_hb.wr_id = 103;
            wr_hb.sg_list = &mut hb_sge;
            wr_hb.num_sge = 1;
            wr_hb.opcode = sys::IBV_WR_RDMA_READ;
            wr_hb.send_flags = sys::IBV_SEND_SIGNALED;
            wr_hb.wr.rdma.remote_addr = remote_hb_addr;
            wr_hb.wr.rdma.rkey = remote_hb_rkey;

            assert_eq!(sys::ibv_post_send((*cid).qp, &mut wr_hb, &mut bad_wr), 0);
            while sys::ibv_poll_cq(cq, 1, &mut wc) == 0 {}
            assert_eq!(wc.status, sys::IBV_WC_SUCCESS);
            let tick_val = u64::from_ne_bytes(read_buf[0..8].try_into().unwrap());
            assert!(tick_val >= 1);

            let _ = sys::rdma_disconnect(cid);
            let _ = sys::ibv_dereg_mr(write_mr);
            let _ = sys::ibv_dereg_mr(read_mr);
            sys::rdma_destroy_qp(cid);
            let _ = sys::ibv_destroy_cq(cq);
            let _ = sys::ibv_dealloc_pd(pd);
            let _ = sys::rdma_destroy_id(cid);
            sys::rdma_destroy_event_channel(ch);
        }

        stop.store(true, Ordering::Relaxed);
        let mut provider = server_thread.join().expect("join server thread");
        for chunk in provider.chunks.drain(..) {
            let _ = chunk.release_and_unlink();
        }
        let _ = fs::remove_dir_all(&test_dir);
    }
}
