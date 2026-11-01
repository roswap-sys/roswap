// SPDX-License-Identifier: GPL-2.0
use crate::sys;
use std::ffi::CStr;
use std::io;
use std::net::{Ipv4Addr, SocketAddrV4};
use std::ptr::NonNull;

pub const RS_CONN_MAGIC: u32 = 0x5253_5750; // "RSWP"
pub const RS_CONN_VERSION: u16 = 1;

#[repr(u8)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum QpPurpose {
    Fault = 1,
    Bulk = 2,
    Heartbeat = 3,
    PeerWrite = 4,
}

impl QpPurpose {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            1 => Some(Self::Fault),
            2 => Some(Self::Bulk),
            3 => Some(Self::Heartbeat),
            4 => Some(Self::PeerWrite),
            _ => None,
        }
    }
}

#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct ConnPrivateData {
    pub magic_be: u32,
    pub version_be: u16,
    pub purpose: u8,
    pub rsvd: u8,
    pub cpu_be: u16,
    pub node_id_be: u16,
    pub node_epoch_be: u32,
    pub cluster_uuid: [u8; 16],
    pub master_boot_id_be: u64,
    pub rsvd2: [u32; 2],
}

impl ConnPrivateData {
    pub fn new(purpose: QpPurpose, cpu: u16, node_id: u16, node_epoch: u32) -> Self {
        Self {
            magic_be: RS_CONN_MAGIC.to_be(),
            version_be: RS_CONN_VERSION.to_be(),
            purpose: purpose as u8,
            rsvd: 0,
            cpu_be: cpu.to_be(),
            node_id_be: node_id.to_be(),
            node_epoch_be: node_epoch.to_be(),
            cluster_uuid: [0u8; 16],
            master_boot_id_be: 0,
            rsvd2: [0; 2],
        }
    }

    pub fn validate(&self, expected_epoch: Option<u32>) -> Result<QpPurpose, &'static str> {
        if u32::from_be(self.magic_be) != RS_CONN_MAGIC {
            return Err("invalid magic in rs_conn_pdata");
        }
        if u16::from_be(self.version_be) != RS_CONN_VERSION {
            return Err("unsupported protocol version");
        }
        let purpose = QpPurpose::from_u8(self.purpose).ok_or("unknown qp purpose")?;
        if let Some(epoch) = expected_epoch {
            let req_epoch = u32::from_be(self.node_epoch_be);
            if req_epoch != 0 && req_epoch != epoch {
                return Err("node_epoch mismatch");
            }
        }
        Ok(purpose)
    }

    pub fn cpu(&self) -> u16 {
        u16::from_be(self.cpu_be)
    }

    pub fn node_id(&self) -> u16 {
        u16::from_be(self.node_id_be)
    }

    pub fn node_epoch(&self) -> u32 {
        u32::from_be(self.node_epoch_be)
    }
}

pub struct RdmaDeviceContext {
    pub dev_name: String,
    ctx: NonNull<sys::ibv_context>,
    pd: NonNull<sys::ibv_pd>,
    shared_cq: NonNull<sys::ibv_cq>,
}

unsafe impl Send for RdmaDeviceContext {}
unsafe impl Sync for RdmaDeviceContext {}

impl RdmaDeviceContext {
    pub fn open(preferred_dev: Option<&str>) -> io::Result<Self> {
        let mut num_devs: i32 = 0;
        let ctx_list = unsafe { sys::rdma_get_devices(&mut num_devs) };
        if ctx_list.is_null() || num_devs <= 0 {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                "no RDMA devices found (rdma_get_devices returned 0 devices)",
            ));
        }

        let mut chosen_ctx: *mut sys::ibv_context = std::ptr::null_mut();
        let mut chosen_name = String::new();

        for i in 0..(num_devs as usize) {
            let ctx_ptr = unsafe { *ctx_list.add(i) };
            if ctx_ptr.is_null() {
                continue;
            }
            let dev = unsafe { (*ctx_ptr).device };
            if dev.is_null() {
                continue;
            }
            let name_ptr = unsafe { sys::ibv_get_device_name(dev) };
            if name_ptr.is_null() {
                continue;
            }
            let name = unsafe { CStr::from_ptr(name_ptr) }
                .to_string_lossy()
                .into_owned();
            if let Some(pref) = preferred_dev {
                if name == pref {
                    chosen_ctx = ctx_ptr;
                    chosen_name = name;
                    break;
                }
            } else {
                chosen_ctx = ctx_ptr;
                chosen_name = name;
                break;
            }
        }

        unsafe { sys::rdma_free_devices(ctx_list) };

        let ctx = NonNull::new(chosen_ctx).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!("requested RDMA device {:?} not found", preferred_dev),
            )
        })?;
        let raw_pd = unsafe { sys::ibv_alloc_pd(ctx.as_ptr()) };
        let pd = match NonNull::new(raw_pd) {
            Some(p) => p,
            None => return Err(io::Error::last_os_error()),
        };

        let raw_cq = unsafe {
            sys::ibv_create_cq(
                ctx.as_ptr(),
                64,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                0,
            )
        };
        let shared_cq = match NonNull::new(raw_cq) {
            Some(c) => c,
            None => {
                let err = io::Error::last_os_error();
                unsafe {
                    sys::ibv_dealloc_pd(pd.as_ptr());
                }
                return Err(err);
            }
        };

        Ok(Self {
            dev_name: chosen_name,
            ctx,
            pd,
            shared_cq,
        })
    }

    pub fn context(&self) -> NonNull<sys::ibv_context> {
        self.ctx
    }

    pub fn pd(&self) -> NonNull<sys::ibv_pd> {
        self.pd
    }

    pub fn shared_cq(&self) -> NonNull<sys::ibv_cq> {
        self.shared_cq
    }
}

impl Drop for RdmaDeviceContext {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::ibv_destroy_cq(self.shared_cq.as_ptr());
            let _ = sys::ibv_dealloc_pd(self.pd.as_ptr());
        }
    }
}

pub struct PassiveConnection {
    pub cm_id: NonNull<sys::rdma_cm_id>,
    pub purpose: QpPurpose,
    pub cpu: u16,
    pub node_id: u16,
}

unsafe impl Send for PassiveConnection {}

impl Drop for PassiveConnection {
    fn drop(&mut self) {
        unsafe {
            if !(*self.cm_id.as_ptr()).qp.is_null() {
                sys::rdma_destroy_qp(self.cm_id.as_ptr());
            }
            let _ = sys::rdma_destroy_id(self.cm_id.as_ptr());
        }
    }
}

pub fn sockaddr_from_v4(addr: SocketAddrV4) -> sys::sockaddr_in {
    sys::sockaddr_in {
        sin_family: sys::AF_INET,
        sin_port: addr.port().to_be(),
        sin_addr: sys::in_addr {
            s_addr: u32::from_ne_bytes(addr.ip().octets()),
        },
        sin_zero: [0u8; 8],
    }
}

pub fn local_rdma_ipv4() -> Option<Ipv4Addr> {
    if let Ok(entries) = std::fs::read_dir("/sys/class/infiniband") {
        for entry in entries.flatten() {
            let net_dir = entry.path().join("ports/1/gid_attrs/ndevs");
            if let Ok(ndevs) = std::fs::read_dir(net_dir) {
                for ndev in ndevs.flatten() {
                    if let Ok(ifname) = std::fs::read_to_string(ndev.path()) {
                        let ifname = ifname.trim();
                        if let Some(ip) = ipv4_of_interface(ifname) {
                            return Some(ip);
                        }
                    }
                }
            }
        }
    }
    None
}

fn ipv4_of_interface(ifname: &str) -> Option<Ipv4Addr> {
    #[repr(C)]
    struct ifreq {
        ifr_name: [u8; 16],
        ifr_addr: sys::sockaddr_in,
        _pad: [u8; 8],
    }
    extern "C" {
        fn socket(domain: i32, ty: i32, protocol: i32) -> i32;
        fn ioctl(fd: i32, request: u64, ...) -> i32;
        fn close(fd: i32) -> i32;
    }
    const SIOCGIFADDR: u64 = 0x8915;
    unsafe {
        let fd = socket(sys::AF_INET as i32, 2, 0);
        if fd < 0 {
            return None;
        }
        let mut ifr: ifreq = std::mem::zeroed();
        let bytes = ifname.as_bytes();
        let copy_len = bytes.len().min(15);
        ifr.ifr_name[..copy_len].copy_from_slice(&bytes[..copy_len]);
        let rc = ioctl(fd, SIOCGIFADDR, &mut ifr);
        let _ = close(fd);
        if rc != 0 {
            return None;
        }
        let octets = ifr.ifr_addr.sin_addr.s_addr.to_ne_bytes();
        Some(Ipv4Addr::new(octets[0], octets[1], octets[2], octets[3]))
    }
}
