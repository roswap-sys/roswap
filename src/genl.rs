// SPDX-License-Identifier: GPL-2.0

use std::io;
use std::os::fd::RawFd;

pub const RS_CMD_NODE_ADD: u8 = 1;
pub const RS_CMD_CHUNK_ADD: u8 = 2;
pub const RS_CMD_CHUNK_UPDATE: u8 = 3;
pub const RS_CMD_GET_STATS: u8 = 4;
pub const RS_CMD_NODE_DEL: u8 = 5;
pub const RS_CMD_NODE_SET_STATE: u8 = 6;
pub const RS_CMD_CHUNK_RETIRE: u8 = 7;
pub const RS_EVT_NODE_STATE: u8 = 8;
pub const RS_EVT_DATA_LOST: u8 = 9;

pub const RS_NODE_UP: u32 = 0;
pub const RS_NODE_SUSPECT: u32 = 1;
pub const RS_NODE_DEAD: u32 = 2;

pub const RS_A_NODE_ID: u16 = 1;
pub const RS_A_ADDR: u16 = 2;
pub const RS_A_CHUNK_GID: u16 = 3;
pub const RS_A_EPOCH: u16 = 4;
pub const RS_A_RADDR: u16 = 5;
pub const RS_A_RKEY: u16 = 6;
pub const RS_A_CHUNK_MB: u16 = 7;
pub const RS_A_NR_SLOTS: u16 = 8;
pub const RS_A_USED_SLOTS: u16 = 9;
pub const RS_A_FREE_EXTENTS: u16 = 10;
pub const RS_A_NR_NODES: u16 = 11;
pub const RS_A_NR_CHUNKS: u16 = 12;
pub const RS_A_LINKS_UP: u16 = 13;
pub const RS_A_HB_RADDR: u16 = 14;
pub const RS_A_HB_RKEY: u16 = 15;
pub const RS_A_NODE_STATE: u16 = 16;
pub const RS_A_BOOT_ID: u16 = 17;
pub const RS_A_DEGRADED: u16 = 18;
pub const RS_A_LOST: u16 = 19;
pub const RS_A_NODE_LIST: u16 = 20;

const AF_NETLINK: i32 = 16;
const SOCK_RAW: i32 = 3;
const NETLINK_GENERIC: i32 = 16;
const GENL_ID_CTRL: u16 = 0x10;
const CTRL_CMD_GETFAMILY: u8 = 3;
const CTRL_ATTR_FAMILY_ID: u16 = 1;
const CTRL_ATTR_FAMILY_NAME: u16 = 2;
const CTRL_ATTR_MCAST_GROUPS: u16 = 7;
const CTRL_ATTR_MCAST_GRP_NAME: u16 = 1;
const CTRL_ATTR_MCAST_GRP_ID: u16 = 2;
const SOL_NETLINK: i32 = 270;
const NETLINK_ADD_MEMBERSHIP: i32 = 1;
const NLM_F_REQUEST: u16 = 1;
const NLM_F_ACK: u16 = 4;
const NLMSG_ERROR: u16 = 2;
const NLMSG_HDRLEN: usize = 16;
const GENL_HDRLEN: usize = 4;

#[repr(C)]
struct SockaddrNl {
    family: u16,
    pad: u16,
    pid: u32,
    groups: u32,
}

extern "C" {
    fn socket(domain: i32, typ: i32, protocol: i32) -> i32;
    fn bind(sockfd: i32, addr: *const SockaddrNl, addrlen: u32) -> i32;
    fn send(sockfd: i32, buf: *const u8, len: usize, flags: i32) -> isize;
    fn recv(sockfd: i32, buf: *mut u8, len: usize, flags: i32) -> isize;
    fn close(fd: i32) -> i32;
    fn setsockopt(sockfd: i32, level: i32, optname: i32, optval: *const u32, optlen: u32) -> i32;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NodeInfo {
    pub id: u16,
    pub state: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KernelStats {
    pub nr_slots: u32,
    pub used_slots: u32,
    pub free_extents: u32,
    pub nr_nodes: u32,
    pub nr_chunks: u32,
    pub links_up: u32,
    pub degraded_slots: u32,
    pub lost_slots: u32,
    pub nodes: Vec<NodeInfo>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NodeHb {
    pub raddr: u64,
    pub rkey: u32,
    pub boot_id: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum KernelEvent {
    NodeState {
        node_id: u16,
        state: u32,
        boot_id: u64,
    },
    DataLost {
        node_id: u16,
        lost: u32,
    },
}

pub struct GenlClient {
    fd: RawFd,
    seq: u32,
    family: Option<u16>,
}

impl GenlClient {
    pub fn open() -> io::Result<Self> {
        let fd = unsafe { socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let addr = SockaddrNl {
            family: AF_NETLINK as u16,
            pad: 0,
            pid: 0,
            groups: 0,
        };
        let rc = unsafe { bind(fd, &addr, std::mem::size_of::<SockaddrNl>() as u32) };
        if rc < 0 {
            let err = io::Error::last_os_error();
            unsafe { close(fd) };
            return Err(err);
        }
        Ok(Self {
            fd,
            seq: 1,
            family: None,
        })
    }

    pub fn node_add(&mut self, node_id: u16, addr: &str, hb: Option<&NodeHb>) -> io::Result<()> {
        self.command(RS_CMD_NODE_ADD, |msg| {
            msg.put_u16(RS_A_NODE_ID, node_id);
            msg.put_str(RS_A_ADDR, addr);
            if let Some(hb) = hb {
                msg.put_u64(RS_A_HB_RADDR, hb.raddr);
                msg.put_u32(RS_A_HB_RKEY, hb.rkey);
                msg.put_u64(RS_A_BOOT_ID, hb.boot_id);
            }
        })
    }

    pub fn node_del(&mut self, node_id: u16) -> io::Result<()> {
        self.command(RS_CMD_NODE_DEL, |msg| msg.put_u16(RS_A_NODE_ID, node_id))
    }

    pub fn node_set_state(&mut self, node_id: u16, state: u32) -> io::Result<()> {
        self.command(RS_CMD_NODE_SET_STATE, |msg| {
            msg.put_u16(RS_A_NODE_ID, node_id);
            msg.put_u32(RS_A_NODE_STATE, state);
        })
    }

    pub fn chunk_retire(&mut self, node_id: u16, gid: u32) -> io::Result<()> {
        self.command(RS_CMD_CHUNK_RETIRE, |msg| {
            msg.put_u16(RS_A_NODE_ID, node_id);
            msg.put_u32(RS_A_CHUNK_GID, gid);
        })
    }

    pub fn join_events(&mut self) -> io::Result<()> {
        let payload = self.ctrl_getfamily()?;
        let attrs = parse_attrs(&payload);
        let id = attr_u16(&attrs, CTRL_ATTR_FAMILY_ID)?;
        let group = event_group_id(&payload)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "roswap events group"))?;
        let rc = unsafe {
            setsockopt(
                self.fd,
                SOL_NETLINK,
                NETLINK_ADD_MEMBERSHIP,
                &group,
                std::mem::size_of::<u32>() as u32,
            )
        };
        if rc < 0 {
            return Err(io::Error::last_os_error());
        }
        self.family = Some(id);
        Ok(())
    }

    pub fn wait_event(&mut self, timeout_ms: i32) -> io::Result<Option<KernelEvent>> {
        let mut pfd = crate::sys::pollfd {
            fd: self.fd,
            events: crate::sys::POLLIN,
            revents: 0,
        };
        let rc = unsafe { crate::sys::poll(&mut pfd, 1, timeout_ms) };
        if rc < 0 {
            return Err(io::Error::last_os_error());
        }
        if rc == 0 {
            return Ok(None);
        }
        let mut storage = vec![0u8; 8192];
        let n = unsafe { recv(self.fd, storage.as_mut_ptr(), storage.len(), 0) };
        if n < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(parse_event(&storage[..n as usize]))
    }

    pub fn chunk_add(
        &mut self,
        node_id: u16,
        gid: u32,
        epoch: u32,
        raddr: u64,
        rkey: u32,
        size_mb: u32,
    ) -> io::Result<()> {
        self.command(RS_CMD_CHUNK_ADD, |msg| {
            msg.put_u16(RS_A_NODE_ID, node_id);
            msg.put_u32(RS_A_CHUNK_GID, gid);
            msg.put_u32(RS_A_EPOCH, epoch);
            msg.put_u64(RS_A_RADDR, raddr);
            msg.put_u32(RS_A_RKEY, rkey);
            msg.put_u32(RS_A_CHUNK_MB, size_mb);
        })
    }

    pub fn chunk_update(
        &mut self,
        node_id: u16,
        gid: u32,
        epoch: u32,
        raddr: u64,
        rkey: u32,
    ) -> io::Result<()> {
        self.command(RS_CMD_CHUNK_UPDATE, |msg| {
            msg.put_u16(RS_A_NODE_ID, node_id);
            msg.put_u32(RS_A_CHUNK_GID, gid);
            msg.put_u32(RS_A_EPOCH, epoch);
            msg.put_u64(RS_A_RADDR, raddr);
            msg.put_u32(RS_A_RKEY, rkey);
        })
    }

    pub fn stats(&mut self) -> io::Result<KernelStats> {
        let payload = self.command_payload(RS_CMD_GET_STATS, |_| {})?;
        stats_from_payload(&payload)
    }

    fn command(&mut self, cmd: u8, fill: impl FnOnce(&mut Msg)) -> io::Result<()> {
        self.command_payload(cmd, fill).map(|_| ())
    }

    fn command_payload(&mut self, cmd: u8, fill: impl FnOnce(&mut Msg)) -> io::Result<Vec<u8>> {
        let family = self.family_id()?;
        let seq = self.seq;
        self.seq = self.seq.wrapping_add(1);
        let mut msg = Msg::new(family, NLM_F_REQUEST | NLM_F_ACK, seq, cmd);
        fill(&mut msg);
        msg.finish();
        self.talk(family, seq, &msg.buf)
    }

    fn family_id(&mut self) -> io::Result<u16> {
        if let Some(id) = self.family {
            return Ok(id);
        }
        let payload = self.ctrl_getfamily()?;
        let id = attr_u16(&parse_attrs(&payload), CTRL_ATTR_FAMILY_ID)?;
        self.family = Some(id);
        Ok(id)
    }

    fn ctrl_getfamily(&mut self) -> io::Result<Vec<u8>> {
        let seq = self.seq;
        self.seq = self.seq.wrapping_add(1);
        let mut msg = Msg::new(
            GENL_ID_CTRL,
            NLM_F_REQUEST | NLM_F_ACK,
            seq,
            CTRL_CMD_GETFAMILY,
        );
        msg.put_str(CTRL_ATTR_FAMILY_NAME, "roswap");
        msg.finish();
        self.talk(GENL_ID_CTRL, seq, &msg.buf)
    }

    fn talk(&self, expect_type: u16, seq: u32, buf: &[u8]) -> io::Result<Vec<u8>> {
        let sent = unsafe { send(self.fd, buf.as_ptr(), buf.len(), 0) };
        if sent < 0 || sent as usize != buf.len() {
            return Err(io::Error::last_os_error());
        }
        let mut storage = vec![0u8; 8192];
        let mut data = Vec::new();
        loop {
            let n = unsafe { recv(self.fd, storage.as_mut_ptr(), storage.len(), 0) };
            if n < 0 {
                return Err(io::Error::last_os_error());
            }
            if absorb_reply(&mut data, &storage[..n as usize], expect_type, seq)? {
                return Ok(data);
            }
        }
    }
}

impl Drop for GenlClient {
    fn drop(&mut self) {
        unsafe { close(self.fd) };
    }
}

#[derive(Clone)]
struct Msg {
    buf: Vec<u8>,
}

impl Msg {
    fn new(ty: u16, flags: u16, seq: u32, cmd: u8) -> Self {
        let mut buf = vec![0u8; NLMSG_HDRLEN + GENL_HDRLEN];
        buf[4..6].copy_from_slice(&ty.to_ne_bytes());
        buf[6..8].copy_from_slice(&flags.to_ne_bytes());
        buf[8..12].copy_from_slice(&seq.to_ne_bytes());
        buf[16] = cmd;
        buf[17] = 1;
        Self { buf }
    }

    fn put_bytes(&mut self, attr: u16, data: &[u8]) {
        let len = 4 + data.len();
        let start = self.buf.len();
        self.buf.resize(start + align(len), 0);
        self.buf[start..start + 2].copy_from_slice(&(len as u16).to_ne_bytes());
        self.buf[start + 2..start + 4].copy_from_slice(&attr.to_ne_bytes());
        self.buf[start + 4..start + 4 + data.len()].copy_from_slice(data);
    }

    fn put_u16(&mut self, attr: u16, value: u16) {
        self.put_bytes(attr, &value.to_ne_bytes());
    }

    fn put_u32(&mut self, attr: u16, value: u32) {
        self.put_bytes(attr, &value.to_ne_bytes());
    }

    fn put_u64(&mut self, attr: u16, value: u64) {
        self.put_bytes(attr, &value.to_ne_bytes());
    }

    fn put_str(&mut self, attr: u16, value: &str) {
        let mut bytes = value.as_bytes().to_vec();
        bytes.push(0);
        self.put_bytes(attr, &bytes);
    }

    fn finish(&mut self) {
        let len = self.buf.len() as u32;
        self.buf[..4].copy_from_slice(&len.to_ne_bytes());
    }
}

fn align(n: usize) -> usize {
    (n + 3) & !3
}

fn absorb_reply(data: &mut Vec<u8>, buf: &[u8], expect_type: u16, seq: u32) -> io::Result<bool> {
    let mut offset = 0;
    while offset + NLMSG_HDRLEN <= buf.len() {
        let len = u32::from_ne_bytes(buf[offset..offset + 4].try_into().unwrap()) as usize;
        if len < NLMSG_HDRLEN || offset + len > buf.len() {
            break;
        }
        let ty = u16::from_ne_bytes(buf[offset + 4..offset + 6].try_into().unwrap());
        let msg_seq = u32::from_ne_bytes(buf[offset + 8..offset + 12].try_into().unwrap());
        if msg_seq == seq {
            if ty == NLMSG_ERROR {
                if len < NLMSG_HDRLEN + 4 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "short netlink error",
                    ));
                }
                let err = i32::from_ne_bytes(
                    buf[offset + NLMSG_HDRLEN..offset + NLMSG_HDRLEN + 4]
                        .try_into()
                        .unwrap(),
                );
                if err != 0 {
                    return Err(io::Error::from_raw_os_error(-err));
                }
                return Ok(true);
            }
            if ty == expect_type && len >= NLMSG_HDRLEN + GENL_HDRLEN {
                *data = buf[offset + NLMSG_HDRLEN + GENL_HDRLEN..offset + len].to_vec();
            }
        }
        offset += align(len);
    }
    Ok(false)
}

fn stats_from_payload(payload: &[u8]) -> io::Result<KernelStats> {
    let attrs = parse_attrs(payload);
    Ok(KernelStats {
        nr_slots: attr_u32(&attrs, RS_A_NR_SLOTS)?,
        used_slots: attr_u32(&attrs, RS_A_USED_SLOTS)?,
        free_extents: attr_u32(&attrs, RS_A_FREE_EXTENTS)?,
        nr_nodes: attr_u32(&attrs, RS_A_NR_NODES)?,
        nr_chunks: attr_u32(&attrs, RS_A_NR_CHUNKS)?,
        links_up: attr_u32(&attrs, RS_A_LINKS_UP)?,
        degraded_slots: attr_u32(&attrs, RS_A_DEGRADED)?,
        lost_slots: attr_u32(&attrs, RS_A_LOST)?,
        nodes: node_list(&attrs)?,
    })
}

fn node_list(attrs: &[(u16, Vec<u8>)]) -> io::Result<Vec<NodeInfo>> {
    let Some((_, bytes)) = attrs.iter().find(|(attr, _)| *attr == RS_A_NODE_LIST) else {
        return Ok(Vec::new());
    };
    if bytes.len() % 4 != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "node list length",
        ));
    }
    let mut nodes = Vec::with_capacity(bytes.len() / 4);
    for item in bytes.as_chunks::<4>().0 {
        nodes.push(NodeInfo {
            id: u16::from_le_bytes([item[0], item[1]]),
            state: u32::from(item[2]),
        });
    }
    Ok(nodes)
}

fn event_group_id(payload: &[u8]) -> Option<u32> {
    let attrs = parse_attrs(payload);
    let (_, groups) = attrs
        .iter()
        .find(|(attr, _)| *attr == CTRL_ATTR_MCAST_GROUPS)?;
    for (_, body) in parse_attrs(groups) {
        let inner = parse_attrs(&body);
        let name = inner
            .iter()
            .find(|(attr, _)| *attr == CTRL_ATTR_MCAST_GRP_NAME)
            .map(|(_, data)| data.as_slice());
        if name != Some(b"events\0") {
            continue;
        }
        return attr_u32(&inner, CTRL_ATTR_MCAST_GRP_ID).ok();
    }
    None
}

fn parse_event(buf: &[u8]) -> Option<KernelEvent> {
    if buf.len() < NLMSG_HDRLEN + GENL_HDRLEN {
        return None;
    }
    let len = u32::from_ne_bytes(buf[..4].try_into().ok()?) as usize;
    if len < NLMSG_HDRLEN + GENL_HDRLEN || len > buf.len() {
        return None;
    }
    let attrs = parse_attrs(&buf[NLMSG_HDRLEN + GENL_HDRLEN..len]);
    let node_id = attr_u16(&attrs, RS_A_NODE_ID).ok()?;
    match buf[NLMSG_HDRLEN] {
        RS_EVT_NODE_STATE => Some(KernelEvent::NodeState {
            node_id,
            state: attr_u32(&attrs, RS_A_NODE_STATE).ok()?,
            boot_id: attr_u64(&attrs, RS_A_BOOT_ID).unwrap_or(0),
        }),
        RS_EVT_DATA_LOST => Some(KernelEvent::DataLost {
            node_id,
            lost: attr_u32(&attrs, RS_A_LOST).ok()?,
        }),
        _ => None,
    }
}

fn parse_attrs(mut payload: &[u8]) -> Vec<(u16, Vec<u8>)> {
    let mut out = Vec::new();
    while payload.len() >= 4 {
        let len = u16::from_ne_bytes(payload[..2].try_into().unwrap()) as usize;
        if len < 4 || len > payload.len() {
            break;
        }
        let attr = u16::from_ne_bytes(payload[2..4].try_into().unwrap());
        out.push((attr, payload[4..len].to_vec()));
        let step = align(len);
        if step > payload.len() {
            break;
        }
        payload = &payload[step..];
    }
    out
}

fn attr_u16(attrs: &[(u16, Vec<u8>)], id: u16) -> io::Result<u16> {
    let bytes = attrs
        .iter()
        .find(|(attr, _)| *attr == id)
        .map(|(_, data)| data.as_slice())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing u16 attribute"))?;
    bytes
        .try_into()
        .map(u16::from_ne_bytes)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "short u16 attribute"))
}

fn attr_u64(attrs: &[(u16, Vec<u8>)], id: u16) -> io::Result<u64> {
    let bytes = attrs
        .iter()
        .find(|(attr, _)| *attr == id)
        .map(|(_, data)| data.as_slice())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing u64 attribute"))?;
    bytes
        .try_into()
        .map(u64::from_ne_bytes)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "short u64 attribute"))
}

fn attr_u32(attrs: &[(u16, Vec<u8>)], id: u16) -> io::Result<u32> {
    let bytes = attrs
        .iter()
        .find(|(attr, _)| *attr == id)
        .map(|(_, data)| data.as_slice())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing u32 attribute"))?;
    bytes
        .try_into()
        .map(u32::from_ne_bytes)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "short u32 attribute"))
}

pub fn is_already_exists(err: &io::Error) -> bool {
    err.raw_os_error() == Some(17)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn payload(msg: &Msg) -> &[u8] {
        &msg.buf[NLMSG_HDRLEN + GENL_HDRLEN..]
    }

    fn attr(attrs: &[(u16, Vec<u8>)], id: u16) -> &[u8] {
        attrs
            .iter()
            .find(|(attr, _)| *attr == id)
            .unwrap()
            .1
            .as_slice()
    }

    fn nl_error(seq: u32, code: i32) -> Vec<u8> {
        let mut buf = vec![0u8; NLMSG_HDRLEN + 4];
        let len = buf.len() as u32;
        buf[..4].copy_from_slice(&len.to_ne_bytes());
        buf[4..6].copy_from_slice(&NLMSG_ERROR.to_ne_bytes());
        buf[8..12].copy_from_slice(&seq.to_ne_bytes());
        buf[NLMSG_HDRLEN..].copy_from_slice(&code.to_ne_bytes());
        buf
    }

    #[test]
    fn abi_command_and_attribute_numbers() {
        assert_eq!(
            [
                RS_CMD_NODE_ADD,
                RS_CMD_CHUNK_ADD,
                RS_CMD_CHUNK_UPDATE,
                RS_CMD_GET_STATS,
                RS_CMD_NODE_DEL,
                RS_CMD_NODE_SET_STATE,
                RS_CMD_CHUNK_RETIRE,
                RS_EVT_NODE_STATE,
                RS_EVT_DATA_LOST,
            ],
            [1, 2, 3, 4, 5, 6, 7, 8, 9]
        );
        assert_eq!(RS_A_NODE_ID, 1);
        assert_eq!(RS_A_ADDR, 2);
        assert_eq!(RS_A_CHUNK_GID, 3);
        assert_eq!(RS_A_EPOCH, 4);
        assert_eq!(RS_A_RADDR, 5);
        assert_eq!(RS_A_RKEY, 6);
        assert_eq!(RS_A_CHUNK_MB, 7);
        assert_eq!(RS_A_LINKS_UP, 13);
        assert_eq!(RS_A_HB_RADDR, 14);
        assert_eq!(RS_A_HB_RKEY, 15);
        assert_eq!(RS_A_NODE_STATE, 16);
        assert_eq!(RS_A_BOOT_ID, 17);
        assert_eq!(RS_A_DEGRADED, 18);
        assert_eq!(RS_A_LOST, 19);
        assert_eq!(RS_A_NODE_LIST, 20);
        assert_eq!([RS_NODE_UP, RS_NODE_SUSPECT, RS_NODE_DEAD], [0, 1, 2]);
        assert_eq!(GENL_ID_CTRL, 0x10);
        assert_eq!(CTRL_CMD_GETFAMILY, 3);
    }

    #[test]
    fn node_add_message_roundtrips() {
        let mut msg = Msg::new(22, NLM_F_REQUEST | NLM_F_ACK, 7, RS_CMD_NODE_ADD);
        msg.put_u16(RS_A_NODE_ID, 4);
        msg.put_str(RS_A_ADDR, "10.0.0.8:18515");
        msg.finish();
        assert_eq!(
            u32::from_ne_bytes(msg.buf[..4].try_into().unwrap()),
            msg.buf.len() as u32
        );
        assert_eq!(u16::from_ne_bytes(msg.buf[4..6].try_into().unwrap()), 22);
        assert_eq!(
            u16::from_ne_bytes(msg.buf[6..8].try_into().unwrap()),
            NLM_F_REQUEST | NLM_F_ACK
        );
        assert_eq!(u32::from_ne_bytes(msg.buf[8..12].try_into().unwrap()), 7);
        assert_eq!(msg.buf[16], RS_CMD_NODE_ADD);
        assert_eq!(msg.buf[17], 1);
        assert_eq!(msg.buf.len() % 4, 0);
        let attrs = parse_attrs(payload(&msg));
        assert_eq!(attr_u16(&attrs, RS_A_NODE_ID).unwrap(), 4);
        assert_eq!(attr(&attrs, RS_A_ADDR), b"10.0.0.8:18515\0");
    }

    #[test]
    fn chunk_add_carries_every_field_and_pads_the_u16() {
        let mut msg = Msg::new(9, NLM_F_REQUEST | NLM_F_ACK, 3, RS_CMD_CHUNK_ADD);
        msg.put_u16(RS_A_NODE_ID, 2);
        msg.put_u32(RS_A_CHUNK_GID, 7);
        msg.put_u32(RS_A_EPOCH, 11);
        msg.put_u64(RS_A_RADDR, 0x0123_4567_89ab_cdef);
        msg.put_u32(RS_A_RKEY, 0x00ab_cdef);
        msg.put_u32(RS_A_CHUNK_MB, 256);
        msg.finish();
        let attrs = parse_attrs(payload(&msg));
        assert_eq!(attr_u16(&attrs, RS_A_NODE_ID).unwrap(), 2);
        assert_eq!(attr_u32(&attrs, RS_A_CHUNK_GID).unwrap(), 7);
        assert_eq!(attr_u32(&attrs, RS_A_EPOCH).unwrap(), 11);
        assert_eq!(
            u64::from_ne_bytes(attr(&attrs, RS_A_RADDR).try_into().unwrap()),
            0x0123_4567_89ab_cdef
        );
        assert_eq!(attr_u32(&attrs, RS_A_RKEY).unwrap(), 0x00ab_cdef);
        assert_eq!(attr_u32(&attrs, RS_A_CHUNK_MB).unwrap(), 256);
        assert_eq!(msg.buf.len() % 4, 0);
    }

    #[test]
    fn chunk_update_names_the_node_and_omits_the_size() {
        let mut msg = Msg::new(9, NLM_F_REQUEST, 1, RS_CMD_CHUNK_UPDATE);
        msg.put_u16(RS_A_NODE_ID, 2);
        msg.put_u32(RS_A_CHUNK_GID, 4);
        msg.put_u32(RS_A_EPOCH, 8);
        msg.put_u64(RS_A_RADDR, 0x1000);
        msg.put_u32(RS_A_RKEY, 99);
        msg.finish();
        let attrs = parse_attrs(payload(&msg));
        assert_eq!(msg.buf[16], RS_CMD_CHUNK_UPDATE);
        assert_eq!(attr_u16(&attrs, RS_A_NODE_ID).unwrap(), 2);
        assert!(attr_u32(&attrs, RS_A_CHUNK_MB).is_err());
        assert_eq!(attr_u32(&attrs, RS_A_RKEY).unwrap(), 99);
    }

    #[test]
    fn family_lookup_names_roswap() {
        let mut msg = Msg::new(
            GENL_ID_CTRL,
            NLM_F_REQUEST | NLM_F_ACK,
            1,
            CTRL_CMD_GETFAMILY,
        );
        msg.put_str(CTRL_ATTR_FAMILY_NAME, "roswap");
        msg.finish();
        let attrs = parse_attrs(payload(&msg));
        assert_eq!(attr(&attrs, CTRL_ATTR_FAMILY_NAME), b"roswap\0");
    }

    #[test]
    fn odd_length_string_does_not_shift_the_next_attribute() {
        let mut msg = Msg::new(1, NLM_F_REQUEST, 1, RS_CMD_NODE_ADD);
        msg.put_str(RS_A_ADDR, "ab");
        msg.put_u32(RS_A_CHUNK_GID, 5);
        msg.finish();
        let attrs = parse_attrs(payload(&msg));
        assert_eq!(attr(&attrs, RS_A_ADDR), b"ab\0");
        assert_eq!(attr_u32(&attrs, RS_A_CHUNK_GID).unwrap(), 5);
    }

    #[test]
    fn truncated_and_short_attributes_are_rejected() {
        let empty = parse_attrs(&[4, 0, 1, 0]);
        assert_eq!(empty.len(), 1);
        assert!(empty[0].1.is_empty());
        assert!(parse_attrs(&[8, 0, 1, 0, 1, 2]).is_empty());
        let mut raw = RS_A_NR_SLOTS.to_ne_bytes().to_vec();
        let mut short = vec![5, 0];
        short.append(&mut raw);
        short.push(9);
        assert!(attr_u32(&parse_attrs(&short), RS_A_NR_SLOTS).is_err());
        assert!(attr_u32(&[], RS_A_NR_SLOTS)
            .unwrap_err()
            .to_string()
            .contains("missing"));
        assert!(attr_u16(&[], RS_A_NODE_ID).is_err());
    }

    #[test]
    fn stats_payload_requires_every_counter() {
        let mut msg = Msg::new(9, 0, 1, RS_CMD_GET_STATS);
        msg.put_u32(RS_A_NR_SLOTS, 256);
        msg.put_u32(RS_A_USED_SLOTS, 3);
        msg.put_u32(RS_A_FREE_EXTENTS, 61);
        msg.put_u32(RS_A_NR_NODES, 2);
        msg.put_u32(RS_A_NR_CHUNKS, 2);
        msg.finish();
        assert!(stats_from_payload(payload(&msg)).is_err());
        msg.put_u32(RS_A_LINKS_UP, 4);
        msg.finish();
        assert!(stats_from_payload(payload(&msg)).is_err());
        msg.put_u32(RS_A_DEGRADED, 5);
        msg.put_u32(RS_A_LOST, 6);
        msg.put_bytes(99, &[1, 2, 3, 4]);
        msg.finish();
        assert_eq!(
            stats_from_payload(payload(&msg)).unwrap(),
            KernelStats {
                nr_slots: 256,
                used_slots: 3,
                free_extents: 61,
                nr_nodes: 2,
                nr_chunks: 2,
                links_up: 4,
                degraded_slots: 5,
                lost_slots: 6,
                nodes: Vec::new(),
            }
        );
        let mut bad = msg.clone();
        bad.put_bytes(RS_A_NODE_LIST, &[2, 0, 1, 0, 9]);
        bad.finish();
        assert!(stats_from_payload(payload(&bad)).is_err());
        msg.put_bytes(RS_A_NODE_LIST, &[2, 0, 1, 0, 4, 1, 2, 0]);
        msg.finish();
        let stats = stats_from_payload(payload(&msg)).unwrap();
        assert_eq!(
            stats.nodes,
            vec![
                NodeInfo { id: 2, state: 1 },
                NodeInfo {
                    id: 0x0104,
                    state: 2
                },
            ]
        );
    }

    #[test]
    fn heartbeat_attributes_and_events_roundtrip() {
        let hb = NodeHb {
            raddr: 0x1000,
            rkey: 7,
            boot_id: 9,
        };
        let mut msg = Msg::new(9, NLM_F_REQUEST | NLM_F_ACK, 1, RS_CMD_NODE_ADD);
        msg.put_u16(RS_A_NODE_ID, 3);
        msg.put_str(RS_A_ADDR, "10.0.0.3:1");
        msg.put_u64(RS_A_HB_RADDR, hb.raddr);
        msg.put_u32(RS_A_HB_RKEY, hb.rkey);
        msg.put_u64(RS_A_BOOT_ID, hb.boot_id);
        msg.finish();
        let attrs = parse_attrs(payload(&msg));
        assert_eq!(attr_u64(&attrs, RS_A_HB_RADDR).unwrap(), 0x1000);
        assert_eq!(attr_u32(&attrs, RS_A_HB_RKEY).unwrap(), 7);
        assert!(attr_u64(&[(RS_A_BOOT_ID, vec![1, 2])], RS_A_BOOT_ID).is_err());

        let mut event = Msg::new(9, 0, 0, RS_EVT_NODE_STATE);
        event.put_u16(RS_A_NODE_ID, 3);
        event.put_u32(RS_A_NODE_STATE, RS_NODE_SUSPECT);
        event.put_u64(RS_A_BOOT_ID, 11);
        event.finish();
        assert_eq!(
            parse_event(&event.buf),
            Some(KernelEvent::NodeState {
                node_id: 3,
                state: RS_NODE_SUSPECT,
                boot_id: 11,
            })
        );
        let mut lost = Msg::new(9, 0, 0, RS_EVT_DATA_LOST);
        lost.put_u16(RS_A_NODE_ID, 3);
        lost.put_u32(RS_A_LOST, 4);
        lost.finish();
        assert_eq!(
            parse_event(&lost.buf),
            Some(KernelEvent::DataLost {
                node_id: 3,
                lost: 4,
            })
        );
        assert_eq!(parse_event(&[0, 1, 2]), None);

        let mut inner = Msg::new(0, 0, 0, 0);
        inner.put_str(CTRL_ATTR_MCAST_GRP_NAME, "events");
        inner.put_u32(CTRL_ATTR_MCAST_GRP_ID, 12);
        let mut wrapped = Msg::new(0, 0, 0, 0);
        wrapped.put_bytes(1, payload(&inner));
        let mut groups = Msg::new(0x10, 0, 1, CTRL_CMD_GETFAMILY);
        groups.put_bytes(CTRL_ATTR_MCAST_GROUPS, payload(&wrapped));
        groups.finish();
        assert_eq!(event_group_id(payload(&groups)), Some(12));
        assert_eq!(event_group_id(&[]), None);
    }

    #[test]
    #[ignore]
    fn kernel_rejects_illegal_netlink() {
        let mut client = GenlClient::open().expect("netlink");
        client
            .node_add(9, "127.0.0.1:18619", None)
            .expect("first add");
        client
            .node_add(9, "127.0.0.1:18619", None)
            .expect("same address is idempotent");
        let changed = client
            .node_add(9, "127.0.0.1:18620", None)
            .expect_err("address change");
        assert!(is_already_exists(&changed));
        let missing = client
            .node_set_state(65000, RS_NODE_SUSPECT)
            .expect_err("unknown node");
        assert_eq!(missing.raw_os_error(), Some(2));
        let retired = client.chunk_retire(65000, 1).expect_err("unknown chunk");
        assert_eq!(retired.raw_os_error(), Some(2));
        let short = client
            .command(RS_CMD_NODE_SET_STATE, |msg| {
                msg.put_bytes(RS_A_NODE_ID, &[1]);
                msg.put_u32(RS_A_NODE_STATE, RS_NODE_DEAD);
            })
            .expect_err("short attribute");
        assert_eq!(short.raw_os_error(), Some(22));
    }

    #[test]
    fn ack_keeps_the_reply_and_a_negative_code_becomes_errno() {
        let mut reply = Msg::new(22, 0, 9, RS_CMD_GET_STATS);
        reply.put_u32(RS_A_NR_SLOTS, 8);
        reply.finish();
        let mut buf = reply.buf.clone();
        buf.extend(nl_error(9, 0));
        let mut data = Vec::new();
        assert!(absorb_reply(&mut data, &buf, 22, 9).unwrap());
        assert_eq!(attr_u32(&parse_attrs(&data), RS_A_NR_SLOTS).unwrap(), 8);

        let err = absorb_reply(&mut data, &nl_error(9, -17), 22, 9).unwrap_err();
        assert!(is_already_exists(&err));
        assert!(!is_already_exists(&io::Error::from_raw_os_error(2)));

        let mut ignored = Vec::new();
        assert!(!absorb_reply(&mut ignored, &nl_error(8, 0), 22, 9).unwrap());
        assert!(ignored.is_empty());
        let mut short = nl_error(9, 0);
        short[..4].copy_from_slice(&16u32.to_ne_bytes());
        assert!(absorb_reply(&mut ignored, &short[..NLMSG_HDRLEN], 22, 9).is_err());
    }
}
