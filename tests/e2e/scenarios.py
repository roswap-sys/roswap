# SPDX-License-Identifier: GPL-2.0
"""Host and VM scenarios. Each one starts from a clean stack and leaves cleanup to the runner."""

from __future__ import annotations

import re
import subprocess
import threading
import time

import struct

from cluster import CTRL_PORT, Cluster, rdma_port
from env import (
    BLKCHECK,
    CHUNK_MB,
    E2EError,
    KO,
    port_listening,
    say,
    setup_rxe,
    wait_until,
)
from genl import (
    RS_A_CHUNK_GID,
    RS_A_EPOCH,
    RS_A_NODE_ID,
    RS_A_NODE_STATE,
    RS_A_RADDR,
    RS_CMD_CHUNK_ADD,
    RS_CMD_NODE_SET_STATE,
    RS_EVT_NODE_STATE,
    GenlError,
    GenlEvents,
)

SCENARIOS = (
    "blk",
    "swap",
    "params",
    "genl",
    "fault",
    "stress",
    "replica2",
    "hedge",
    "suspect",
    "dead",
    "robust",
)


def scenario_blk(c: Cluster) -> None:
    c.bring_up()
    say("queue limits and one extent")
    c.blkcheck()
    say("unmapped read is zero")
    c.blkcheck("--mode", "zero", "-s", "70")
    say("cross extent and cross chunk")
    c.blkcheck("--mode", "verify", "-s", "80")
    say("discard")
    c.blkcheck("--mode", "discard")
    say("latency smoke")
    c.latread(100)
    c.stop_stack()
    say("ENOSPC past the remote backing")
    c.bring_up(capacity_mb=256)
    c.blkcheck("--mode", "enospc")
    say("blk passed")


def scenario_swap(c: Cluster) -> None:
    c.bring_up()
    say("mkswap / swapon")
    c.setup_swap()
    proc = c.memhold("-m", "96m", "-i", "1", "-n", "4")
    peak = c.watch_swap(proc)
    rc = proc.wait()
    if rc != 0:
        raise E2EError(f"memhold verification failed, exit code {rc}")
    if peak <= 0:
        subprocess.run(["cat", "/proc/swaps"], check=False)
        raise E2EError(f"/dev/roswap0 was not used for swap (peak Used {peak} KiB)")
    say(f"swap passed, peak {peak} KiB")


def scenario_params(c: Cluster) -> None:
    subprocess.run(["modprobe", "-q", "rdma_cm"], check=False)
    say("invalid address")
    c.insmod_fails("local_addrs=not-an-ip")
    say("address with no matching netdev")
    c.insmod_fails("local_addrs=192.0.2.1")
    say("empty local_addrs token")
    c.insmod_fails("local_addrs=,")
    say("logical disk larger than 16TiB")
    c.insmod_fails(f"capacity_mb={(16 << 20) + 1}")
    say("zero capacity")
    proc = subprocess.run(
        ["insmod", str(KO), "chunk_size_mb=0", "capacity_mb=0"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode == 0:
        c.unload()
        raise E2EError("capacity_mb 0 was accepted")
    say("invalid protection")
    c.insmod_fails("protection=mirror")
    say("queue_depth bounds")
    c.insmod_fails("queue_depth=0")
    c.insmod_fails("queue_depth=5000")
    say("qp_budget below two")
    c.insmod_fails("qp_budget=1")
    say("omitting local_addrs")
    c.load()
    c.unload()
    say("two local RDMA addresses")
    setup_rxe(2)
    c.load(extra=(f"local_addrs={c._ip},192.168.89.1",))
    c.unload()
    say("legacy server raddr rkey")
    _legacy_server(c)
    say("small send queue still completes IO")
    c.bring_up(extra=("qp_send_depth=1",))
    c.blkcheck()
    say("params passed")


def scenario_genl(c: Cluster) -> None:
    c.bring_up()
    client = c.client()
    say("illegal attributes and unknown objects")
    _expect_errno(lambda: client.raw(RS_CMD_NODE_SET_STATE, [
        (RS_A_NODE_ID, b"\x01"),
        (RS_A_NODE_STATE, struct.pack("=I", 2)),
    ]), 22, "short attribute")
    _expect_errno(lambda: client.node_set_state(65000, 1), 2, "unknown node")
    _expect_errno(lambda: client.node_del(65000), 2, "unknown node_del")
    _expect_errno(lambda: client.chunk_retire(65000, 1), 2, "unknown retire")
    _expect_errno(lambda: client.chunk_update(65000, 1, 1, 0x1000, 7), 2, "unknown update")
    _expect_errno(lambda: client.raw(RS_CMD_CHUNK_ADD, [
        (RS_A_NODE_ID, struct.pack("=H", 1)),
        (RS_A_CHUNK_GID, struct.pack("=I", 3)),
        (RS_A_EPOCH, struct.pack("=I", 1)),
        (RS_A_RADDR, struct.pack("=Q", 0x1000)),
    ]), 22, "chunk_add missing attributes")
    client.node_add(9, "127.0.0.1:18619")
    client.node_add(9, "127.0.0.1:18619")
    _expect_errno(lambda: client.node_add(9, "127.0.0.1:18620"), 17, "address change")
    client.node_del(9)
    say("legal chunk update and retire on a connected node")
    client.chunk_add(1, 99, 1, 0x1000, 0xABC, CHUNK_MB)
    client.chunk_update(1, 99, 1, 0x2000, 0xDEF)
    client.chunk_retire(1, 99)
    client.node_set_state(1, 1)
    client.node_set_state(1, 0)
    st = client.stats()
    if st.nr_chunks < 2:
        raise E2EError(f"stats lost the provider chunks ({st.nr_chunks})")
    say("genl passed")


def scenario_fault(c: Cluster) -> None:
    c.bring_up()
    say("writing baseline data")
    expect = c.write_pattern()
    say("dropping the RC connection during IO")
    c.start_io()
    time.sleep(0.5)
    c.drop_connection()
    time.sleep(2)
    c.stop_io()
    if c.io_failed():
        raise E2EError("IO failed while the link was down")
    c.wait_kernel()
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after reconnect")
    say("fast restart should hit key wait and then the new rkey")
    before_wait = c.stats().key_wait
    old = _rkeys(c, 1)
    _busy_read(c)
    c.kill_provider(1)
    c.stop_provider(1)
    c.start_provider(1)
    wait_key = time.monotonic() + 20
    while c.stats().key_wait <= before_wait and time.monotonic() < wait_key:
        time.sleep(0.2)
    c.stop_io()
    c.wait_kernel()
    if c.stats().key_wait <= before_wait:
        raise E2EError("key_wait did not increase after a same-boot restart")
    new = _rkeys(c, 1)
    if not new or new == old:
        raise E2EError(f"rkey was not updated ({old} -> {new})")
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after the key-wait restart")
    if c.node_state(1) == "dead":
        raise E2EError("same-boot restart was judged dead")
    boot = c.node(1)["boot"]
    say("restart after the kernel gives up")
    gave = c.gave_up()
    old = _rkeys(c, 1)
    c.stop_provider(1)
    deadline = time.monotonic() + 20
    while c.gave_up() <= gave and time.monotonic() < deadline:
        time.sleep(0.2)
    if c.gave_up() <= gave:
        raise E2EError("kernel did not give up the old connection")
    time.sleep(2)
    c.start_provider(1)
    c.wait_kernel()
    if _rkeys(c, 1) == old:
        raise E2EError("rkey was not updated after give-up")
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after give-up restart")
    if c.node(1)["boot"] != boot:
        raise E2EError("boot id changed across a same-boot restart")
    if c.node_state(1) == "dead":
        raise E2EError("restart after give-up was judged dead")
    say("fault passed")


def scenario_stress(c: Cluster) -> None:
    c.bring_up()
    say("mixed IO across repeated disconnects")
    expect = c.write_pattern(seek=96 * 256)
    proc = subprocess.Popen([str(BLKCHECK), "/dev/roswap0", "--mode", "stress", "-t", "4", "-s", "32", "-d", "4"])
    time.sleep(0.3)
    for _ in range(4):
        c.drop_connection()
        time.sleep(0.4)
    rc = proc.wait()
    if rc != 0:
        raise E2EError(f"stress IO failed during disconnects, exit {rc}")
    if c.read_hash(skip=96 * 256) != expect:
        raise E2EError("readback mismatch after repeated disconnects")
    say("stress passed")


def scenario_replica2(c: Cluster) -> None:
    c.bring_up(providers=2, protection="replica2")
    say("two copies across extents")
    c.blkcheck("--mode", "verify", "-s", "80")
    expect = c.write_pattern()
    say("one node drops while IO continues")
    c.start_io()
    time.sleep(0.3)
    c.drop_connection(2)
    time.sleep(2)
    c.stop_io()
    if c.io_failed():
        raise E2EError("IO failed while node 2 was dropped")
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after a single-node drop")
    if c.node_state(1) != "up" or c.node_state(2) != "up":
        raise E2EError(f"drop changed health {c.node_state(1)} {c.node_state(2)}")
    say("same-boot rkey change restores both copies")
    old = _rkeys(c, 2)
    c.stop_provider(2)
    c.start_provider(2)
    c.wait_kernel()
    if _rkeys(c, 2) == old:
        raise E2EError("node 2 rkey did not change")
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after node 2 restart")
    if c.node_state(2) == "dead":
        raise E2EError("same-boot replica restart was judged dead")
    say("a write while one node is suspect is short")
    before = c.stats().degraded
    c.freeze(2)
    c.wait_state(2, "suspect", 2)
    subprocess.run(
        ["dd", "if=/dev/urandom", "of=/dev/roswap0", "bs=4096", "count=256", "seek=256",
         "oflag=direct", "conv=fsync", "status=none"],
        check=True,
    )
    if c.stats().degraded <= before:
        raise E2EError("extents placed while a node was suspect were not marked short")
    c.thaw(2)
    c.wait_state(2, "up", 30)
    say("replica2 passed")


def scenario_hedge(c: Cluster) -> None:
    c.bring_up(providers=2, protection="replica2", extra=("read_hedge_us=1",))
    before = c.stats().hedge_sent
    say("forcing the read hedge")
    c.blkcheck("--mode", "stress", "-t", "4", "-s", "32", "-d", "3")
    sent = c.stats().hedge_sent
    if sent <= before:
        raise E2EError(f"hedge_sent did not increase ({before} -> {sent})")
    c.blkcheck("--mode", "verify", "-s", "16")
    say("hedge passed")


def scenario_suspect(c: Cluster) -> None:
    c.bring_up(providers=2, protection="replica2", dead_after=30)
    say("swap while one provider is frozen")
    c.setup_swap()
    before = c.stats().degraded
    proc = c.memhold("-m", "96m", "-i", "1", "-n", "4")
    time.sleep(0.4)
    c.freeze(2)
    c.wait_state(2, "suspect", 2)
    rc = proc.wait()
    if rc != 0:
        raise E2EError(f"memhold failed while a replica was suspect, exit {rc}")
    if c.node_state(2) == "dead":
        raise E2EError("a short freeze was judged dead")
    if c.stats().lost != 0:
        raise E2EError("suspect lost pages")
    subprocess.run(["swapoff", "/dev/roswap0"], check=True)
    subprocess.run(
        ["dd", "if=/dev/urandom", "of=/dev/roswap0", "bs=4096", "count=256", "seek=256",
         "oflag=direct", "conv=fsync", "status=none"],
        check=True,
    )
    if c.stats().degraded <= before:
        raise E2EError("extents placed while a node was suspect were not marked short")
    c.thaw(2)
    c.wait_state(2, "up", 30)
    if c.node_state(2) == "dead":
        raise E2EError("thaw was judged dead")
    say("suspect passed")


def scenario_dead(c: Cluster) -> None:
    c.bring_up(providers=2, protection="replica2", dead_after=10)
    say("killing one replica")
    expect = c.write_pattern()
    c.kill_provider(2)
    c.wait_state(2, "dead", 20)
    if c.read_hash() != expect:
        raise E2EError("replica2 readback mismatch after a dead node")
    if c.stats().lost != 0:
        raise E2EError(f"replica2 lost pages ({c.stats().lost})")
    say("boot id change retires the old epoch and the survivor still reads")
    _boot_restart(c, expect)

    say("single provider death returns EIO and SIGBUS")
    c.stop_stack()
    c.bring_up(dead_after=10)
    c.write_pattern()
    c.setup_swap()
    proc = c.memhold("-m", "96m", "-i", "1", "-n", "30")
    time.sleep(1.5)
    c.kill_provider(1)
    rc = proc.wait()
    if rc not in (-7, 135):
        raise E2EError(f"memhold exit {rc}, want SIGBUS")
    c.wait_state(1, "dead", 20)
    read = subprocess.run(
        ["dd", "if=/dev/roswap0", "of=/dev/null", "bs=4096", "count=1", "iflag=direct", "status=none"],
        check=False,
        capture_output=True,
        text=True,
    )
    if read.returncode == 0:
        raise E2EError("read of a lost page succeeded")
    say("dead passed")


def scenario_robust(c: Cluster) -> None:
    c.bring_up(providers=2, protection="replica2", dead_after=8)
    expect = c.write_pattern()
    say("freezing every provider is not a node death")
    c.freeze(1)
    c.freeze(2)
    time.sleep(10)
    if c.node_state(1) == "dead" or c.node_state(2) == "dead":
        raise E2EError("a majority freeze was judged dead")
    c.thaw(1)
    c.thaw(2)
    c.wait_state(1, "up", 30)
    c.wait_state(2, "up", 30)
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after a majority freeze")
    say("heartbeat QP drop does not condemn the node")
    c.drop_heartbeat()
    time.sleep(2)
    if c.node_state(1) == "dead" or c.node_state(2) == "dead":
        raise E2EError("dropping the heartbeat QP was judged dead")
    c.wait_state(1, "up", 30)
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after a heartbeat drop")
    say("data QP drop does not mark the node suspect")
    c.drop_connection()
    time.sleep(1)
    if c.node_state(1) != "up" or c.node_state(2) != "up":
        raise E2EError(f"a data disconnect changed node health ({c.node_state(1)} {c.node_state(2)})")
    say("controller restart keeps a suspect node and still condemns it later")
    c.freeze(2)
    c.wait_state(2, "suspect", 2)
    c._stop_key("controller")
    wait_until("controller port free", 10, lambda: not port_listening(CTRL_PORT))
    c.start_controller()
    time.sleep(1)
    if c.node_state(2) == "dead":
        raise E2EError("controller restart immediately judged a suspect node dead")
    c.wait_state(2, "dead", 20)
    say("removing the module while a heartbeat read is in flight")
    c.stop_stack()
    c.bring_up(dead_after=30)
    c.freeze(1)
    c.wait_state(1, "suspect", 2)
    c.unload()
    say("robust passed")


def run_named(name: str, c: Cluster) -> None:
    if name not in SCENARIOS:
        raise E2EError(f"unknown scenario {name}")
    globals()[f"scenario_{name}"](c)


def _expect_errno(fn, errno: int, what: str) -> None:
    try:
        fn()
    except GenlError as err:
        if err.errno != errno:
            raise E2EError(f"{what}: errno {err.errno}, want {errno}") from err
        return
    raise E2EError(f"{what} was accepted")


def _rkeys(c: Cluster, node_id: int) -> list[int]:
    return [chunk["rkey"] for chunk in c.node(node_id)["chunks"]]


def _busy_read(c: Cluster) -> None:
    stop = c.log_dir / "io.stop"
    stop.unlink(missing_ok=True)
    script = "while [ ! -f \"$1\" ]; do dd if=/dev/roswap0 of=/dev/null bs=4096 count=64 iflag=direct status=none || true; done"
    c.procs["io"] = subprocess.Popen(["bash", "-c", script, "io", str(stop)])


def _legacy_server(c: Cluster) -> None:
    c.start_provider(1, epoch=0, controller=None)
    log = (c.log_dir / "provider.log").read_text(errors="replace")
    match = re.search(r"chunk registered gid=0 raddr=0x([0-9a-f]+) .* rkey=0x([0-9a-f]+)", log)
    if not match:
        raise E2EError("provider did not log a chunk registration")
    raddr = int(match.group(1), 16)
    rkey = int(match.group(2), 16)
    ip = (c.log_dir / "rdma.ip").read_text().strip()
    subprocess.run(["modprobe", "-q", "rdma_cm"], check=False)
    subprocess.run(
        [
            "insmod",
            str(KO),
            f"chunk_size_mb={CHUNK_MB}",
            f"capacity_mb={CHUNK_MB}",
            f"server={ip}:{rdma_port(1)}",
            f"raddr={raddr}",
            f"rkey={rkey}",
        ],
        check=True,
    )
    wait_until("legacy link up", 20, lambda: c.stats().links_up >= 1)
    c.blkcheck()
    c.unload()
    c.stop_provider(1)


def _boot_restart(c: Cluster, expect: str) -> None:
    events: list = []
    watcher = GenlEvents()

    def collect() -> None:
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            try:
                ev = watcher.poll(200)
            except OSError:
                return
            if ev is not None and ev.cmd == RS_EVT_NODE_STATE and ev.node_id == 2:
                events.append(ev)

    thread = threading.Thread(target=collect, daemon=True)
    thread.start()
    c.kill_provider(2)
    time.sleep(0.2)
    c.stop_provider(2)
    c.start_provider(2, epoch=2, boot_id="00000000000000ab")
    deadline = time.monotonic() + 30
    ready = False
    while time.monotonic() < deadline:
        try:
            node = c.node(2)
        except E2EError:
            time.sleep(0.2)
            continue
        if node["epoch"] == 2 and node["boot"] == 0xAB and node["synced"] and c.node_state(2) == "up":
            ready = True
            break
        time.sleep(0.2)
    thread.join(timeout=1)
    watcher.close()
    if not ready:
        raise E2EError("provider 2 did not rejoin with the new boot id and epoch")
    if not events:
        raise E2EError("kernel did not emit EVT_NODE_STATE for the boot change")
    if c.read_hash() != expect:
        raise E2EError("readback mismatch after a boot_id change")
    if c.stats().lost != 0:
        raise E2EError("boot_id change lost pages")

