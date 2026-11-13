// SPDX-License-Identifier: GPL-2.0
use crate::chunk::{Chunk, HeartbeatPage};
use crate::rdma::{
    chunk_size_error, cm_event_drops_connection, rdma_dev_ipv4, snapshot_cm_event,
    sockaddr_from_v4, CmEventView, ConnPrivateData, PassiveConnection, RdmaDeviceContext,
};
use crate::sys;
use log::{debug, info, warn};
use serde::{Deserialize, Deserializer};
use std::collections::HashMap;
use std::fs;
use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::path::PathBuf;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

fn empty_string_as_none<'de, D>(deserializer: D) -> Result<Option<String>, D::Error>
where
    D: Deserializer<'de>,
{
    let value = Option::<String>::deserialize(deserializer)?;
    Ok(value.filter(|s| !s.is_empty()))
}

#[derive(Clone, Debug, PartialEq, Eq, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ProviderConfig {
    /// 空字符串或省略表示自动选择 RDMA 设备。
    #[serde(deserialize_with = "empty_string_as_none")]
    pub device_name: Option<String>,
    /// `0.0.0.0` 只表示端口；实际会绑到所选 RDMA 设备的 IPv4，避免连接落到另一块设备上。
    pub listen_addr: SocketAddrV4,
    pub grpc_addr: SocketAddr,
    pub mem_dir: PathBuf,
    pub chunk_size_mb: usize,
    pub num_chunks: usize,
    pub node_epoch: u32,
    /// 向 controller 注册时用的节点号。0 留给 insmod 手工带起来的那条链路。
    #[serde(default = "default_node_id")]
    pub node_id: u16,
    /// 空字符串表示不向 controller 注册。
    #[serde(default)]
    pub controller_addr: String,
}

fn default_node_id() -> u16 {
    1
}

impl Default for ProviderConfig {
    fn default() -> Self {
        Self {
            device_name: None,
            listen_addr: SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 18515),
            grpc_addr: SocketAddr::from((Ipv4Addr::UNSPECIFIED, 12220)),
            mem_dir: PathBuf::from("/dev/shm/roswap"),
            chunk_size_mb: 64,
            num_chunks: 1,
            node_epoch: 1,
            node_id: default_node_id(),
            controller_addr: String::new(),
        }
    }
}

impl ProviderConfig {
    pub fn controller_endpoint(&self) -> io::Result<Option<SocketAddr>> {
        if self.controller_addr.is_empty() {
            return Ok(None);
        }
        self.controller_addr.parse().map(Some).map_err(|err| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("controller_addr: {err}"),
            )
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChunkExport {
    pub gid: u32,
    pub epoch: u32,
    pub raddr: u64,
    pub rkey: u32,
    pub size_mb: u32,
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
    pub fn init(config: &ProviderConfig) -> io::Result<Self> {
        unsafe {
            let _ = sys::mlockall(sys::MCL_CURRENT | sys::MCL_FUTURE);
        }

        let rdma = RdmaDeviceContext::open(config.device_name.as_deref())?;
        let listen_addr = listen_addr_on_device(config.listen_addr, &rdma.dev_name)?;
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

        let mut sin = sockaddr_from_v4(listen_addr);
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

        let bound_verbs = unsafe { (*listen_id.as_ptr()).verbs };
        if bound_verbs != rdma.context().as_ptr() {
            unsafe {
                let _ = sys::rdma_destroy_id(listen_id.as_ptr());
                sys::rdma_destroy_event_channel(channel.as_ptr());
            }
            return Err(io::Error::new(
                io::ErrorKind::AddrNotAvailable,
                format!(
                    "listen {listen_addr} is not on RDMA device {}",
                    rdma.dev_name
                ),
            ));
        }

        let mut config = config.clone();
        config.listen_addr = listen_addr;
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

    pub fn exported_chunks(&self) -> Vec<ChunkExport> {
        self.chunks
            .iter()
            .map(|chunk| ChunkExport {
                gid: chunk.gid,
                epoch: chunk.epoch,
                raddr: chunk.addr(),
                rkey: chunk.rkey(),
                size_mb: (chunk.len() >> 20) as u32,
            })
            .collect()
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

        // ack 会释放事件以及 private_data 缓冲，所以先把要用的字段拷到自己的内存里。
        let ev = unsafe { snapshot_cm_event(raw_ev) };
        unsafe {
            let _ = sys::rdma_ack_cm_event(raw_ev);
        }

        self.handle_cm_event(ev)?;
        Ok(true)
    }

    fn handle_cm_event(&mut self, ev: CmEventView) -> io::Result<()> {
        match ev.event {
            sys::RDMA_CM_EVENT_CONNECT_REQUEST => {
                let client_id = match NonNull::new(ev.id) {
                    Some(id) => id,
                    None => return Ok(()),
                };
                if let Err(msg) = self.accept_client(client_id, ev.private_data) {
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
            event if cm_event_drops_connection(event) => {
                self.drop_connection(ev.id as usize);
            }
            _ => {
                debug!("ignored cm event {} status {}", ev.event, ev.status);
            }
        }
        Ok(())
    }

    fn drop_connection(&mut self, key: usize) {
        if let Some(conn) = self.conns.remove(&key) {
            info!(
                "connection dropped purpose={:?} cpu={} node_id={}",
                conn.purpose, conn.cpu, conn.node_id
            );
        }
    }

    fn accept_client(
        &mut self,
        client_id: NonNull<sys::rdma_cm_id>,
        pdata: Option<ConnPrivateData>,
    ) -> Result<(), String> {
        let pdata = pdata.ok_or("missing rs_conn_pdata")?;
        let purpose = pdata
            .validate(Some(self.config.node_epoch))
            .map_err(|e| e.to_string())?;
        let chunk_bytes = self
            .chunks
            .first()
            .map(|chunk| chunk.len())
            .ok_or("no chunk exported")?;
        if let Some(reason) = chunk_size_error(pdata.chunk_size_mb(), chunk_bytes) {
            return Err(format!(
                "{reason}: client {} MiB, provider {} MiB",
                pdata.chunk_size_mb(),
                chunk_bytes >> 20
            ));
        }
        let cpu = pdata.cpu();
        let node_id = pdata.node_id();

        let id_verbs = unsafe { (*client_id.as_ptr()).verbs };
        let pd_ctx = unsafe { (*self.rdma.pd().as_ptr()).context };
        if id_verbs != pd_ctx {
            return Err(format!(
                "connection arrived on a different RDMA device than {}",
                self.rdma.dev_name
            ));
        }

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

fn listen_addr_on_device(listen_addr: SocketAddrV4, dev_name: &str) -> io::Result<SocketAddrV4> {
    if !listen_addr.ip().is_unspecified() {
        return Ok(listen_addr);
    }
    let ip = rdma_dev_ipv4(dev_name).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::AddrNotAvailable,
            format!("RDMA device {dev_name} has no IPv4 address"),
        )
    })?;
    Ok(SocketAddrV4::new(ip, listen_addr.port()))
}

fn read_boot_id_lo() -> u64 {
    if let Ok(s) = fs::read_to_string("/proc/sys/kernel/random/boot_id") {
        let hex: String = s
            .chars()
            .filter(|c| c.is_ascii_hexdigit())
            .take(16)
            .collect();
        if let Ok(v) = u64::from_str_radix(&hex, 16) {
            return v;
        }
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rdma::QpPurpose;
    use std::ffi::c_void;
    use std::net::Ipv4Addr;
    use std::os::unix::fs::MetadataExt;
    use std::path::Path;

    fn test_dir(name: &str) -> PathBuf {
        let base = std::env::var("ROSWAP_CHUNK_DIR").unwrap_or_else(|_| "/dev/shm".to_string());
        PathBuf::from(base).join(name)
    }

    fn mount_huge_mode(dir: &Path) -> Option<String> {
        let dir = dir.canonicalize().unwrap_or_else(|_| dir.to_path_buf());
        let text = fs::read_to_string("/proc/self/mountinfo").ok()?;
        let mut best: Option<(usize, String)> = None;
        for line in text.lines() {
            let Some((left, right)) = line.split_once(" - ") else {
                continue;
            };
            let mountpoint = left.split_whitespace().nth(4)?;
            let superopts = right.split_whitespace().nth(2).unwrap_or("");
            if !dir.starts_with(mountpoint) {
                continue;
            }
            let Some(mode) = superopts
                .split(',')
                .find_map(|opt| opt.strip_prefix("huge="))
            else {
                continue;
            };
            if best
                .as_ref()
                .map(|(len, _)| mountpoint.len() > *len)
                .unwrap_or(true)
            {
                best = Some((mountpoint.len(), mode.to_string()));
            }
        }
        best.map(|(_, mode)| mode)
    }

    fn expect_huge_pages(dir: &Path) -> bool {
        if let Some(mode) = mount_huge_mode(dir) {
            return matches!(mode.as_str(), "always" | "advise" | "within_size" | "force");
        }
        let enabled = fs::read_to_string("/sys/kernel/mm/transparent_hugepage/shmem_enabled")
            .unwrap_or_default();
        ["[always]", "[advise]", "[within_size]", "[force]"]
            .iter()
            .any(|flag| enabled.contains(flag))
    }

    fn shmem_pmd_mapped_kb(path: &Path) -> u64 {
        let text = fs::read_to_string("/proc/self/smaps").unwrap();
        let needle = path.to_string_lossy();
        let mut in_region = false;
        for line in text.lines() {
            // 映射头是 "addr-addr rw-s ... path"。设备号里也有冒号，不能靠冒号区分。
            if smaps_header(line) {
                in_region = line.contains(needle.as_ref());
                continue;
            }
            if in_region {
                if let Some(rest) = line.strip_prefix("ShmemPmdMapped:") {
                    return rest.split_whitespace().next().unwrap().parse().unwrap();
                }
            }
        }
        0
    }

    fn smaps_header(line: &str) -> bool {
        let Some(range) = line.split_whitespace().next() else {
            return false;
        };
        let Some((start, end)) = range.split_once('-') else {
            return false;
        };
        !start.is_empty()
            && !end.is_empty()
            && start.chars().all(|c| c.is_ascii_hexdigit())
            && end.chars().all(|c| c.is_ascii_hexdigit())
    }

    fn resident_pages(addr: *const u8, len: usize) -> usize {
        let pages = len / crate::chunk::PAGE_SIZE;
        let mut vec = vec![0u8; pages];
        let rc = unsafe { sys::mincore(addr as *mut _, len, vec.as_mut_ptr()) };
        assert_eq!(rc, 0, "{}", io::Error::last_os_error());
        vec.iter().filter(|b| *b & 1 == 1).count()
    }

    #[test]
    fn reopen_rejects_a_different_length() {
        let rdma = RdmaDeviceContext::open(None).expect("open RDMA device");
        let test_dir = test_dir("roswap-test-chunk-len");
        let _ = fs::remove_dir_all(&test_dir);
        {
            let chunk =
                Chunk::create(rdma.pd(), &test_dir, 1, 1, 4 << 20).expect("create 4MB chunk");
            drop(chunk);
        }
        match Chunk::create(rdma.pd(), &test_dir, 1, 1, 8 << 20) {
            Err(err) => assert_eq!(err.kind(), io::ErrorKind::InvalidInput),
            Ok(_) => panic!("different length was accepted"),
        }
        let _ = fs::remove_dir_all(&test_dir);
    }

    #[test]
    fn test_chunk_mr_and_persistence_across_reopen() {
        let rdma = RdmaDeviceContext::open(None).expect("open RDMA device");
        let test_dir = test_dir("roswap-test-chunk");
        let _ = fs::remove_dir_all(&test_dir);

        let rkey_first;
        {
            let mut chunk =
                Chunk::create(rdma.pd(), &test_dir, 7, 1, 4 << 20).expect("create 4MB chunk");
            assert_ne!(chunk.rkey(), 0);
            assert_ne!(chunk.lkey(), 0);
            let blocks = fs::metadata(&chunk.file_path).unwrap().blocks();
            assert!(blocks * 512 >= chunk.len() as u64);
            assert_eq!(
                resident_pages(chunk.as_slice().as_ptr(), chunk.len()),
                chunk.len() / 4096
            );
            if expect_huge_pages(&test_dir) {
                let kb = shmem_pmd_mapped_kb(&chunk.file_path);
                assert!(
                    kb * 1024 >= (2 << 20),
                    "ShmemPmdMapped is {kb} kB; MADV_HUGEPAGE did not allocate a huge page"
                );
            }
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
        let listen_addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 18599);
        let grpc_addr = SocketAddr::from((Ipv4Addr::UNSPECIFIED, 12220));
        let test_dir = test_dir("roswap-test-rdma");
        let _ = fs::remove_dir_all(&test_dir);

        let cfg = ProviderConfig {
            device_name: None,
            listen_addr,
            grpc_addr,
            mem_dir: test_dir.clone(),
            chunk_size_mb: 4,
            num_chunks: 1,
            node_epoch: 3,
            node_id: 1,
            controller_addr: String::new(),
        };

        let mut provider = ProviderRuntime::init(&cfg).expect("init provider");
        let connect_addr = provider.config.listen_addr;
        let remote_chunk_addr = provider.chunks[0].addr();
        let remote_chunk_rkey = provider.chunks[0].rkey();
        let exported = provider.exported_chunks();
        assert_eq!(exported.len(), 1);
        assert_eq!(exported[0].gid, 0);
        assert_eq!(exported[0].epoch, 3);
        assert_eq!(exported[0].raddr, remote_chunk_addr);
        assert_eq!(exported[0].rkey, remote_chunk_rkey);
        assert_eq!(exported[0].size_mb, 4);
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

            let mut pdata = ConnPrivateData::new(QpPurpose::Fault, 0, 1, 3);
            pdata.set_chunk_size_mb(cfg.chunk_size_mb as u32);
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
