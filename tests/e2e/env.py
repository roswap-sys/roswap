# SPDX-License-Identifier: GPL-2.0
"""Process, cgroup, dmesg, and Soft-RoCE helpers for the e2e runner."""

from __future__ import annotations

import os
import random
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
E2E_DIR = ROOT / "tests" / "e2e"
CHUNK_MB = 64
CAPACITY_MB = 128
MEM_DIR = Path("/dev/shm/roswap-e2e")
CTRL_PORT = 12321
ROSWAPD = Path(os.environ.get("ROSWAPD", ROOT / "target" / "release" / "roswapd"))
ROSWAPCTL = Path(os.environ.get("ROSWAPCTL", ROOT / "target" / "release" / "roswapctl"))
BLKCHECK = Path(os.environ.get("BLKCHECK", ROOT / "tools" / "blkcheck"))
MEMHOLD = Path(os.environ.get("MEMHOLD", ROOT / "tools" / "memhold"))
LATREAD = Path(os.environ.get("LATREAD", ROOT / "tools" / "latread"))
KO = ROOT / "kernel" / "roswap.ko"
CGROUP = Path("/sys/fs/cgroup/roswap-e2e")

DMESG_BAD = (
    "circular locking dependency",
    "BUG:",
    "WARNING:",
    "blocked for more than",
    "KASAN",
    "UBSAN",
    "Oops",
    "general protection fault",
    "Call Trace",
    "refcount_t",
)


class E2EError(Exception):
    pass


def say(text: str) -> None:
    print(f"== {text}", flush=True)


def require_root() -> None:
    if os.geteuid() != 0:
        raise E2EError("root is required to load the module and configure swap")


def require_files() -> None:
    for path in (ROSWAPD, ROSWAPCTL, BLKCHECK, MEMHOLD, LATREAD, KO):
        if not path.exists():
            raise E2EError(f"not found: {path}")


def run(cmd: list[str], check: bool = True, env: dict | None = None, timeout: float | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=check, env=env, timeout=timeout, text=True)


def port_listening(port: int) -> bool:
    proc = subprocess.run(
        ["ss", "-Hltn", f"sport = :{port}"],
        check=False,
        capture_output=True,
        text=True,
    )
    return bool(proc.stdout.strip())


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def stop_pid(pid: int) -> None:
    if not alive(pid):
        return
    os.kill(pid, 15)
    deadline = time.monotonic() + 5
    while alive(pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    if alive(pid):
        os.kill(pid, 9)


def wait_until(desc: str, timeout: float, fn) -> None:
    start = time.monotonic()
    while True:
        if fn():
            return
        if time.monotonic() - start >= timeout:
            raise E2EError(f"timed out: {desc}")
        time.sleep(0.2)


def mark_dmesg() -> str:
    token = f"roswap-e2e-{os.getpid()}-{random.randrange(1 << 31)}"
    try:
        with open("/dev/kmsg", "w", encoding="ascii") as kmsg:
            kmsg.write(token + "\n")
    except OSError:
        pass
    return token


def dmesg_text() -> str:
    return subprocess.check_output(["dmesg"], text=True, errors="replace")


def dmesg_after(token: str) -> str:
    text = dmesg_text()
    idx = text.rfind(token)
    if idx < 0:
        return text
    return text[idx + len(token) :]


def assert_dmesg_clean(token: str) -> None:
    for line in dmesg_after(token).splitlines():
        for pat in DMESG_BAD:
            if pat in line:
                raise E2EError(f"dmesg: {line}")


def dmesg_count(token: str, needle: str) -> int:
    return dmesg_after(token).count(needle)


def setup_memcg() -> None:
    root = Path("/sys/fs/cgroup")
    mounted = False
    try:
        mounted = subprocess.run(
            ["findmnt", "-n", "-o", "FSTYPE", "--target", str(root)],
            check=False,
            capture_output=True,
            text=True,
        ).stdout.strip() == "cgroup2"
    except OSError:
        mounted = False
    if not mounted:
        if root.exists() and any(root.iterdir()):
            raise E2EError(f"{root} is not cgroup v2")
        root.mkdir(parents=True, exist_ok=True)
        run(["mount", "-t", "cgroup2", "none", str(root)])
    control = root / "cgroup.subtree_control"
    if "memory" not in control.read_text():
        control.write_text("+memory\n")
    CGROUP.mkdir(parents=True, exist_ok=True)
    (CGROUP / "memory.max").write_text("64M\n")
    swap_max = CGROUP / "memory.swap.max"
    if swap_max.exists():
        swap_max.write_text("128M\n")


def swap_used_kb() -> int:
    try:
        text = Path("/proc/swaps").read_text()
    except OSError:
        return 0
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] == "/dev/roswap0" and len(parts) > 3:
            return int(parts[3])
    return 0


def _gid_ready(rxe: str, gid: str) -> bool:
    port = Path(f"/sys/class/infiniband/{rxe}/ports/1/gids")
    if not port.is_dir():
        return False
    for path in port.iterdir():
        try:
            if gid.lower() in path.read_text(errors="replace").lower():
                return True
        except OSError:
            continue
    return False


def _setup_link(netdev: str, peer: str, addr: str, rxe: str, gid: str) -> None:
    run(["ip", "link", "set", "lo", "up"])
    if not Path(f"/sys/class/net/{netdev}").exists():
        run(["ip", "link", "add", netdev, "type", "veth", "peer", "name", peer])
    run(["ip", "link", "set", netdev, "up"])
    run(["ip", "link", "set", peer, "up"])
    run(["ip", "addr", "replace", addr, "dev", netdev])
    if not Path(f"/sys/class/infiniband/{rxe}").exists():
        run(["rdma", "link", "add", rxe, "type", "rxe", "netdev", netdev])
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if _gid_ready(rxe, gid):
            return
        time.sleep(0.1)
    subprocess.run(["rdma", "link", "show"], check=False)
    raise E2EError(f"{rxe} has no GID for {addr}")


def setup_rxe(count: int = 1) -> None:
    for mod in ("rdma_rxe", "rdma_ucm", "ib_uverbs", "veth"):
        subprocess.run(["modprobe", "-q", mod], check=False)
    _setup_link("roswap0", "roswap1", "192.168.88.1/24", "rxe0", "ffff:c0a8:5801")
    if count > 1:
        _setup_link("roswap2", "roswap3", "192.168.89.1/24", "rxe1", "ffff:c0a8:5901")
    subprocess.run(["rdma", "link", "show"], check=False)


def ensure_tmpfs() -> None:
    for path in ("/dev/shm", "/tmp"):
        proc = subprocess.run(["mountpoint", "-q", path], check=False)
        if proc.returncode != 0:
            run(["mount", "-t", "tmpfs", "-o", "mode=1777", "tmpfs", path])
