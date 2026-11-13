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

    /// `rsvd2[0]`，单位 MiB。头长仍是 48 字节。
    pub fn set_chunk_size_mb(&mut self, mb: u32) {
        self.rsvd2[0] = mb.to_be();
    }

    pub fn chunk_size_mb(&self) -> u32 {
        u32::from_be(self.rsvd2[0])
    }
}

pub fn chunk_size_error(advertised_mb: u32, chunk_bytes: usize) -> Option<&'static str> {
    if advertised_mb == 0 {
        return Some("client did not advertise chunk_size_mb");
    }
    let actual_mb = (chunk_bytes >> 20) as u32;
    if chunk_bytes != (actual_mb as usize) << 20 || advertised_mb != actual_mb {
        return Some("chunk_size_mb mismatch");
    }
    None
}

/// 这些事件之后 QP 不会进入 ESTABLISHED，或者已经离开。调用方要把连接从映射里拿走。
pub fn cm_event_drops_connection(event: u32) -> bool {
    matches!(
        event,
        sys::RDMA_CM_EVENT_ADDR_ERROR
            | sys::RDMA_CM_EVENT_ROUTE_ERROR
            | sys::RDMA_CM_EVENT_CONNECT_ERROR
            | sys::RDMA_CM_EVENT_UNREACHABLE
            | sys::RDMA_CM_EVENT_REJECTED
            | sys::RDMA_CM_EVENT_DISCONNECTED
            | sys::RDMA_CM_EVENT_DEVICE_REMOVAL
            | sys::RDMA_CM_EVENT_TIMEWAIT_EXIT
    )
}

pub struct CmEventView {
    pub id: *mut sys::rdma_cm_id,
    pub event: u32,
    pub status: i32,
    pub private_data: Option<ConnPrivateData>,
}

/// `rdma_ack_cm_event` 会释放事件和它的 private_data。必须在 ack 之前把要用的字节拷出来。
pub unsafe fn snapshot_cm_event(raw: *const sys::rdma_cm_event) -> CmEventView {
    let ev = &*raw;
    CmEventView {
        id: ev.id,
        event: ev.event,
        status: ev.status,
        private_data: copy_conn_private(ev.conn.private_data, ev.conn.private_data_len),
    }
}

unsafe fn copy_conn_private(ptr: *const std::ffi::c_void, len: u8) -> Option<ConnPrivateData> {
    if ptr.is_null() || (len as usize) < std::mem::size_of::<ConnPrivateData>() {
        return None;
    }
    Some(std::ptr::read_unaligned(ptr as *const ConnPrivateData))
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
        let mut fallback_ctx: *mut sys::ibv_context = std::ptr::null_mut();
        let mut fallback_name = String::new();

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
                if fallback_ctx.is_null() {
                    fallback_ctx = ctx_ptr;
                    fallback_name = name.clone();
                }
                if rdma_dev_ipv4(&name).is_some() {
                    chosen_ctx = ctx_ptr;
                    chosen_name = name;
                    if chosen_name.starts_with("rxe") || chosen_name.starts_with("siw") {
                        break;
                    }
                }
            }
        }

        if chosen_ctx.is_null() && preferred_dev.is_none() {
            chosen_ctx = fallback_ctx;
            chosen_name = fallback_name;
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
            let _ = sys::rdma_disconnect(self.cm_id.as_ptr());
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

/// IPv4 address published in this device's GID table.
///
/// `rdma_cm` attaches a listener to the HCA that owns the matching GID. The
/// netdev's primary address can be absent from that table (an IPv6 GID still
/// names the same netdev), and binding it then leaves the id on another
/// device, or on none.
pub fn rdma_dev_ipv4(dev_name: &str) -> Option<Ipv4Addr> {
    let port = std::path::Path::new("/sys/class/infiniband")
        .join(dev_name)
        .join("ports/1");
    let entries = std::fs::read_dir(port.join("gids")).ok()?;
    let mut indexes: Vec<u32> = entries
        .flatten()
        .filter_map(|ent| ent.file_name().to_str()?.parse().ok())
        .collect();
    indexes.sort_unstable();
    let mut fallback = None;
    for index in indexes {
        let Ok(text) = std::fs::read_to_string(port.join("gids").join(index.to_string())) else {
            continue;
        };
        let Some(ip) = ipv4_from_gid(&text) else {
            continue;
        };
        let kind = std::fs::read_to_string(port.join("gid_attrs/types").join(index.to_string()))
            .unwrap_or_default();
        if kind.to_ascii_lowercase().contains("roce v2") {
            return Some(ip);
        }
        if fallback.is_none() {
            fallback = Some(ip);
        }
    }
    fallback
}

fn ipv4_from_gid(text: &str) -> Option<Ipv4Addr> {
    let groups: Vec<&str> = text.trim().split(':').collect();
    if groups.len() != 8 {
        return None;
    }
    let groups: Vec<String> = groups
        .into_iter()
        .map(|group| group.to_ascii_lowercase())
        .collect();
    if groups[..5].iter().any(|group| group != "0000") || groups[5] != "ffff" {
        return None;
    }
    let hi = u16::from_str_radix(&groups[6], 16).ok()?;
    let lo = u16::from_str_radix(&groups[7], 16).ok()?;
    let ip = Ipv4Addr::new((hi >> 8) as u8, hi as u8, (lo >> 8) as u8, lo as u8);
    if ip.is_unspecified() {
        None
    } else {
        Some(ip)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::c_void;

    fn event_with_private(pdata: *const ConnPrivateData, len: u8) -> sys::rdma_cm_event {
        sys::rdma_cm_event {
            id: std::ptr::null_mut(),
            listen_id: std::ptr::null_mut(),
            event: sys::RDMA_CM_EVENT_CONNECT_REQUEST,
            status: 0,
            conn: sys::rdma_conn_param {
                private_data: pdata as *const c_void,
                private_data_len: len,
                responder_resources: 0,
                initiator_depth: 0,
                flow_control: 0,
                retry_count: 0,
                rnr_retry_count: 0,
                srq: 0,
                qp_num: 0,
            },
        }
    }

    #[test]
    fn snapshot_keeps_private_data_after_the_source_is_overwritten() {
        let mut pdata = ConnPrivateData::new(QpPurpose::Fault, 2, 9, 3);
        pdata.set_chunk_size_mb(512);
        let ev = event_with_private(&pdata, std::mem::size_of::<ConnPrivateData>() as u8);
        let snap = unsafe { snapshot_cm_event(&ev) };
        pdata.magic_be = 0;
        pdata.rsvd2 = [0; 2];
        let copied = snap.private_data.expect("private data copied before ack");
        assert_eq!(pdata.chunk_size_mb(), 0);
        assert_eq!(copied.cpu(), 2);
        assert_eq!(copied.node_id(), 9);
        assert_eq!(copied.chunk_size_mb(), 512);
        assert_ne!(copied.chunk_size_mb(), pdata.chunk_size_mb());
        assert_eq!(copied.validate(Some(3)).unwrap(), QpPurpose::Fault);
    }

    #[test]
    fn snapshot_drops_missing_or_short_private_data() {
        let ev = event_with_private(std::ptr::null(), 0);
        let snap = unsafe { snapshot_cm_event(&ev) };
        assert!(snap.private_data.is_none());

        let pdata = ConnPrivateData::new(QpPurpose::Bulk, 0, 0, 0);
        let ev = event_with_private(&pdata, 8);
        let snap = unsafe { snapshot_cm_event(&ev) };
        assert!(snap.private_data.is_none());
    }

    #[test]
    fn chunk_size_must_match_the_exported_mr() {
        assert_eq!(
            chunk_size_error(0, 64 << 20),
            Some("client did not advertise chunk_size_mb")
        );
        assert_eq!(
            chunk_size_error(64, 512 << 20),
            Some("chunk_size_mb mismatch")
        );
        assert_eq!(chunk_size_error(512, 512 << 20), None);
        assert_eq!(
            chunk_size_error(512, (512 << 20) + 4096),
            Some("chunk_size_mb mismatch")
        );
    }

    #[test]
    fn ipv4_mapped_gid_decodes_the_address() {
        assert_eq!(
            ipv4_from_gid("0000:0000:0000:0000:0000:ffff:0a01:000e\n"),
            Some(Ipv4Addr::new(10, 1, 0, 14))
        );
        assert_eq!(
            ipv4_from_gid("0000:0000:0000:0000:0000:FFFF:c0a8:5801"),
            Some(Ipv4Addr::new(192, 168, 88, 1))
        );
        assert_eq!(
            ipv4_from_gid("fe80:0000:0000:0000:0200:00ff:fe00:0001"),
            None
        );
        assert_eq!(
            ipv4_from_gid("0000:0000:0000:0000:0000:ffff:0000:0000"),
            None
        );
        assert_eq!(ipv4_from_gid("0000:0000:0000:0000:0000:ffff:0a01"), None);
    }

    #[test]
    fn terminal_cm_events_drop_the_connection() {
        assert!(cm_event_drops_connection(sys::RDMA_CM_EVENT_DISCONNECTED));
        assert!(cm_event_drops_connection(sys::RDMA_CM_EVENT_CONNECT_ERROR));
        assert!(cm_event_drops_connection(sys::RDMA_CM_EVENT_REJECTED));
        assert!(cm_event_drops_connection(sys::RDMA_CM_EVENT_UNREACHABLE));
        assert!(cm_event_drops_connection(sys::RDMA_CM_EVENT_DEVICE_REMOVAL));
        assert!(cm_event_drops_connection(sys::RDMA_CM_EVENT_TIMEWAIT_EXIT));
        assert!(!cm_event_drops_connection(sys::RDMA_CM_EVENT_ESTABLISHED));
        assert!(!cm_event_drops_connection(
            sys::RDMA_CM_EVENT_CONNECT_REQUEST
        ));
    }
}
