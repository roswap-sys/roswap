# SPDX-License-Identifier: GPL-2.0
"""AF_NETLINK client for the roswap generic netlink family."""

from __future__ import annotations

import select
import socket
import struct

AF_NETLINK = 16
NETLINK_GENERIC = 16
SOL_NETLINK = 270
NETLINK_ADD_MEMBERSHIP = 1
GENL_ID_CTRL = 0x10
CTRL_CMD_GETFAMILY = 3
CTRL_ATTR_FAMILY_ID = 1
CTRL_ATTR_FAMILY_NAME = 2
CTRL_ATTR_MCAST_GROUPS = 7
CTRL_ATTR_MCAST_GRP_NAME = 1
CTRL_ATTR_MCAST_GRP_ID = 2
NLM_F_REQUEST = 1
NLM_F_ACK = 4
NLMSG_ERROR = 2
NLMSG_HDRLEN = 16
GENL_HDRLEN = 4

RS_CMD_NODE_ADD = 1
RS_CMD_CHUNK_ADD = 2
RS_CMD_CHUNK_UPDATE = 3
RS_CMD_GET_STATS = 4
RS_CMD_NODE_DEL = 5
RS_CMD_NODE_SET_STATE = 6
RS_CMD_CHUNK_RETIRE = 7
RS_EVT_NODE_STATE = 8
RS_EVT_DATA_LOST = 9

RS_A_NODE_ID = 1
RS_A_ADDR = 2
RS_A_CHUNK_GID = 3
RS_A_EPOCH = 4
RS_A_RADDR = 5
RS_A_RKEY = 6
RS_A_CHUNK_MB = 7
RS_A_NR_SLOTS = 8
RS_A_USED_SLOTS = 9
RS_A_FREE_EXTENTS = 10
RS_A_NR_NODES = 11
RS_A_NR_CHUNKS = 12
RS_A_LINKS_UP = 13
RS_A_NODE_STATE = 16
RS_A_BOOT_ID = 17
RS_A_DEGRADED = 18
RS_A_LOST = 19
RS_A_NODE_LIST = 20
RS_A_HEDGE_SENT = 22
RS_A_HEDGE_WON = 23
RS_A_KEY_WAIT = 24

STATE_NAME = {0: "up", 1: "suspect", 2: "dead"}


class GenlError(OSError):
    pass


def align(n: int) -> int:
    return (n + 3) & ~3


def build_msg(ty: int, flags: int, seq: int, cmd: int, attrs: list[tuple[int, bytes]]) -> bytes:
    payload = bytearray()
    for attr, data in attrs:
        length = 4 + len(data)
        payload += struct.pack("=HH", length, attr)
        payload += data
        payload += b"\x00" * (align(length) - length)
    total = NLMSG_HDRLEN + GENL_HDRLEN + len(payload)
    return struct.pack("=IHHIIBBH", total, ty, flags, seq, 0, cmd, 1, 0) + bytes(payload)


def parse_attrs(payload: bytes) -> list[tuple[int, bytes]]:
    out = []
    while len(payload) >= 4:
        length, attr = struct.unpack_from("=HH", payload)
        if length < 4 or length > len(payload):
            break
        out.append((attr, payload[4:length]))
        step = align(length)
        if step > len(payload):
            break
        payload = payload[step:]
    return out


def attr_map(payload: bytes) -> dict[int, bytes]:
    return {attr: data for attr, data in parse_attrs(payload)}


def need_u16(attrs: dict[int, bytes], attr: int) -> int:
    data = attrs[attr]
    if len(data) != 2:
        raise GenlError(22, "short u16")
    return struct.unpack("=H", data)[0]


def need_u32(attrs: dict[int, bytes], attr: int) -> int:
    data = attrs[attr]
    if len(data) != 4:
        raise GenlError(22, "short u32")
    return struct.unpack("=I", data)[0]


def optional_u32(attrs: dict[int, bytes], attr: int) -> int:
    data = attrs.get(attr)
    if data is None:
        return 0
    return need_u32(attrs, attr)


def optional_u64(attrs: dict[int, bytes], attr: int) -> int:
    data = attrs.get(attr)
    if data is None or len(data) != 8:
        return 0
    return struct.unpack("=Q", data)[0]


class Stats:
    def __init__(self, payload: bytes):
        attrs = attr_map(payload)
        self.nr_slots = need_u32(attrs, RS_A_NR_SLOTS)
        self.used_slots = need_u32(attrs, RS_A_USED_SLOTS)
        self.free_extents = need_u32(attrs, RS_A_FREE_EXTENTS)
        self.nr_nodes = need_u32(attrs, RS_A_NR_NODES)
        self.nr_chunks = need_u32(attrs, RS_A_NR_CHUNKS)
        self.links_up = need_u32(attrs, RS_A_LINKS_UP)
        self.degraded = need_u32(attrs, RS_A_DEGRADED)
        self.lost = need_u32(attrs, RS_A_LOST)
        self.hedge_sent = optional_u32(attrs, RS_A_HEDGE_SENT)
        self.hedge_won = optional_u32(attrs, RS_A_HEDGE_WON)
        self.key_wait = optional_u32(attrs, RS_A_KEY_WAIT)
        self.nodes = []
        raw = attrs.get(RS_A_NODE_LIST, b"")
        if len(raw) % 4:
            raise GenlError(22, "node list length")
        for i in range(0, len(raw), 4):
            node_id = raw[i] | (raw[i + 1] << 8)
            self.nodes.append((node_id, STATE_NAME.get(raw[i + 2], "up")))


class Event:
    def __init__(self, cmd: int, node_id: int, state: int = 0, boot_id: int = 0, lost: int = 0):
        self.cmd = cmd
        self.node_id = node_id
        self.state = state
        self.boot_id = boot_id
        self.lost = lost

    @property
    def state_name(self) -> str:
        return STATE_NAME.get(self.state, "up")


def parse_event(buf: bytes) -> Event | None:
    if len(buf) < NLMSG_HDRLEN + GENL_HDRLEN:
        return None
    length = struct.unpack_from("=I", buf)[0]
    if length < NLMSG_HDRLEN + GENL_HDRLEN or length > len(buf):
        return None
    cmd = buf[NLMSG_HDRLEN]
    attrs = attr_map(buf[NLMSG_HDRLEN + GENL_HDRLEN : length])
    if RS_A_NODE_ID not in attrs:
        return None
    node_id = need_u16(attrs, RS_A_NODE_ID)
    if cmd == RS_EVT_NODE_STATE:
        return Event(cmd, node_id, need_u32(attrs, RS_A_NODE_STATE), optional_u64(attrs, RS_A_BOOT_ID))
    if cmd == RS_EVT_DATA_LOST and RS_A_LOST in attrs:
        return Event(cmd, node_id, lost=need_u32(attrs, RS_A_LOST))
    return None


def event_group_id(payload: bytes) -> int | None:
    groups = attr_map(payload).get(CTRL_ATTR_MCAST_GROUPS)
    if groups is None:
        return None
    for _, body in parse_attrs(groups):
        inner = attr_map(body)
        if inner.get(CTRL_ATTR_MCAST_GRP_NAME) != b"events\0":
            continue
        if CTRL_ATTR_MCAST_GRP_ID in inner:
            return need_u32(inner, CTRL_ATTR_MCAST_GRP_ID)
    return None


class GenlClient:
    def __init__(self):
        self.sock = socket.socket(AF_NETLINK, socket.SOCK_RAW, NETLINK_GENERIC)
        self.sock.bind((0, 0))
        self.sock.settimeout(60)
        self.seq = 1
        self.family: int | None = None

    def close(self) -> None:
        self.sock.close()

    def stats(self) -> Stats:
        return Stats(self._command(RS_CMD_GET_STATS, []))

    def node_add(self, node_id: int, addr: str) -> None:
        self._command(RS_CMD_NODE_ADD, [
            (RS_A_NODE_ID, struct.pack("=H", node_id)),
            (RS_A_ADDR, addr.encode() + b"\0"),
        ])

    def node_del(self, node_id: int) -> None:
        self._command(RS_CMD_NODE_DEL, [(RS_A_NODE_ID, struct.pack("=H", node_id))])

    def node_set_state(self, node_id: int, state: int) -> None:
        self._command(RS_CMD_NODE_SET_STATE, [
            (RS_A_NODE_ID, struct.pack("=H", node_id)),
            (RS_A_NODE_STATE, struct.pack("=I", state)),
        ])

    def chunk_add(self, node_id: int, gid: int, epoch: int, raddr: int, rkey: int, size_mb: int) -> None:
        self._command(RS_CMD_CHUNK_ADD, [
            (RS_A_NODE_ID, struct.pack("=H", node_id)),
            (RS_A_CHUNK_GID, struct.pack("=I", gid)),
            (RS_A_EPOCH, struct.pack("=I", epoch)),
            (RS_A_RADDR, struct.pack("=Q", raddr)),
            (RS_A_RKEY, struct.pack("=I", rkey)),
            (RS_A_CHUNK_MB, struct.pack("=I", size_mb)),
        ])

    def chunk_update(self, node_id: int, gid: int, epoch: int, raddr: int, rkey: int) -> None:
        self._command(RS_CMD_CHUNK_UPDATE, [
            (RS_A_NODE_ID, struct.pack("=H", node_id)),
            (RS_A_CHUNK_GID, struct.pack("=I", gid)),
            (RS_A_EPOCH, struct.pack("=I", epoch)),
            (RS_A_RADDR, struct.pack("=Q", raddr)),
            (RS_A_RKEY, struct.pack("=I", rkey)),
        ])

    def chunk_retire(self, node_id: int, gid: int) -> None:
        self._command(RS_CMD_CHUNK_RETIRE, [
            (RS_A_NODE_ID, struct.pack("=H", node_id)),
            (RS_A_CHUNK_GID, struct.pack("=I", gid)),
        ])

    def raw(self, cmd: int, attrs: list[tuple[int, bytes]]) -> None:
        self._command(cmd, attrs)

    def _family(self) -> int:
        if self.family is None:
            payload = self._talk(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, [
                (CTRL_ATTR_FAMILY_NAME, b"roswap\0"),
            ])
            self.family = need_u16(attr_map(payload), CTRL_ATTR_FAMILY_ID)
        return self.family

    def _command(self, cmd: int, attrs: list[tuple[int, bytes]]) -> bytes:
        return self._talk(self._family(), cmd, attrs)

    def _talk(self, ty: int, cmd: int, attrs: list[tuple[int, bytes]]) -> bytes:
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        self.sock.sendall(build_msg(ty, NLM_F_REQUEST | NLM_F_ACK, seq, cmd, attrs))
        data = b""
        while True:
            try:
                buf = self.sock.recv(16384)
            except socket.timeout as err:
                raise GenlError(110, "netlink reply timed out") from err
            done, piece = absorb(buf, ty, seq)
            if piece is not None:
                data = piece
            if done:
                return data


class GenlEvents:
    def __init__(self):
        self.client = GenlClient()
        payload = self.client._talk(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, [
            (CTRL_ATTR_FAMILY_NAME, b"roswap\0"),
        ])
        group = event_group_id(payload)
        if group is None:
            raise GenlError(2, "roswap events group")
        self.client.sock.setsockopt(SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, struct.pack("I", group))
        self.client.sock.setblocking(False)

    def close(self) -> None:
        self.client.close()

    def poll(self, timeout_ms: int) -> Event | None:
        ready, _, _ = select.select([self.client.sock], [], [], timeout_ms / 1000)
        if not ready:
            return None
        buf = self.client.sock.recv(16384)
        return parse_event(buf)


def absorb(buf: bytes, expect_type: int, seq: int) -> tuple[bool, bytes | None]:
    offset = 0
    data = None
    while offset + NLMSG_HDRLEN <= len(buf):
        length = struct.unpack_from("=I", buf, offset)[0]
        if length < NLMSG_HDRLEN or offset + length > len(buf):
            break
        ty = struct.unpack_from("=H", buf, offset + 4)[0]
        msg_seq = struct.unpack_from("=I", buf, offset + 8)[0]
        if msg_seq == seq:
            if ty == NLMSG_ERROR:
                if length < NLMSG_HDRLEN + 4:
                    raise GenlError(22, "short netlink error")
                err = struct.unpack_from("=i", buf, offset + NLMSG_HDRLEN)[0]
                if err != 0:
                    raise GenlError(-err, "netlink")
                return True, data
            if ty == expect_type and length >= NLMSG_HDRLEN + GENL_HDRLEN:
                data = buf[offset + NLMSG_HDRLEN + GENL_HDRLEN : offset + length]
        offset += align(length)
    return False, data


def _self_check() -> None:
    msg = build_msg(22, NLM_F_REQUEST | NLM_F_ACK, 7, RS_CMD_NODE_ADD, [
        (RS_A_NODE_ID, struct.pack("=H", 4)),
        (RS_A_ADDR, b"10.0.0.8:18515\0"),
    ])
    assert len(msg) % 4 == 0
    assert struct.unpack_from("=I", msg)[0] == len(msg)
    assert msg[16] == RS_CMD_NODE_ADD
    attrs = attr_map(msg[NLMSG_HDRLEN + GENL_HDRLEN :])
    assert need_u16(attrs, RS_A_NODE_ID) == 4
    assert attrs[RS_A_ADDR] == b"10.0.0.8:18515\0"
    inner = build_msg(0, 0, 0, 0, [
        (CTRL_ATTR_MCAST_GRP_NAME, b"events\0"),
        (CTRL_ATTR_MCAST_GRP_ID, struct.pack("=I", 12)),
    ])
    wrapped = build_msg(0, 0, 0, 0, [(1, inner[NLMSG_HDRLEN + GENL_HDRLEN :])])
    groups = build_msg(0x10, 0, 1, CTRL_CMD_GETFAMILY, [
        (CTRL_ATTR_MCAST_GROUPS, wrapped[NLMSG_HDRLEN + GENL_HDRLEN :]),
    ])
    assert event_group_id(groups[NLMSG_HDRLEN + GENL_HDRLEN :]) == 12
    bad = struct.pack("=IHHII", 20, NLMSG_ERROR, 0, 9, 0) + struct.pack("=i", -17)
    try:
        absorb(bad, 22, 9)
    except GenlError as err:
        assert err.errno == 17
    else:
        raise SystemExit("negative netlink code was not an error")
    print("genl self-check ok")


if __name__ == "__main__":
    _self_check()
