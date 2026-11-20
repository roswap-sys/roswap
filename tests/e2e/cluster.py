# SPDX-License-Identifier: GPL-2.0
"""Controller, provider, and roswap.ko lifecycle for one scenario."""

from __future__ import annotations

import hashlib
import json
import os
import signal
import subprocess
import time
from pathlib import Path

from env import (
    BLKCHECK,
    CAPACITY_MB,
    CGROUP,
    CHUNK_MB,
    CTRL_PORT,
    E2EError,
    E2E_DIR,
    KO,
    LATREAD,
    MEM_DIR,
    MEMHOLD,
    ROSWAPCTL,
    ROSWAPD,
    assert_dmesg_clean,
    dmesg_count,
    mark_dmesg,
    port_listening,
    require_files,
    require_root,
    say,
    setup_memcg,
    stop_pid,
    swap_used_kb,
    wait_until,
)
from genl import GenlClient, GenlError

CTRL_ADDR = f"127.0.0.1:{CTRL_PORT}"


def grpc_port(node_id: int) -> int:
    return 12320 if node_id == 1 else 12330 + node_id


def rdma_port(node_id: int) -> int:
    return 18615 if node_id == 1 else 18614 + node_id


class Cluster:
    def __init__(self, log_dir: Path):
        self.log_dir = Path(log_dir)
        self.mark = ""
        self.genl: GenlClient | None = None
        self.procs: dict[str, subprocess.Popen] = {}
        self.want_chunks = 2
        self.want_links = 1
        self.want_nodes = 1
        self.providers = 1
        self._dev = ""
        self._ip = ""

    def prepare(self) -> None:
        require_root()
        require_files()
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self._dev, self._ip = rdma_endpoint()
        say(f"RDMA {self._dev} {self._ip}")
        (self.log_dir / "rdma.dev").write_text(self._dev + "\n")
        (self.log_dir / "rdma.ip").write_text(self._ip + "\n")
        text = (E2E_DIR / "controller.toml").read_text()
        (self.log_dir / "controller.toml").write_text(text)
        self.stop_stack()
        if MEM_DIR.exists():
            subprocess.run(["rm", "-rf", str(MEM_DIR)], check=False)
        self.mark = mark_dmesg()

    def close_genl(self) -> None:
        if self.genl is not None:
            self.genl.close()
            self.genl = None

    def client(self) -> GenlClient:
        if self.genl is None:
            self.genl = GenlClient()
        return self.genl

    def stats(self):
        try:
            return self.client().stats()
        except GenlError:
            self.close_genl()
            raise

    def cleanup(self, failed: bool = False) -> None:
        if failed:
            self.dump()
        self.stop_stack()
        subprocess.run(["rm", "-rf", str(MEM_DIR)], check=False)

    def dump(self) -> None:
        print("===== logs =====", flush=True)
        for path in sorted(self.log_dir.glob("*.log")):
            print(f"----- {path.name} -----", flush=True)
            lines = path.read_text(errors="replace").splitlines()
            print("\n".join(lines[-80:]), flush=True)
        subprocess.run([str(ROSWAPCTL), "cluster", "--json", CTRL_ADDR], check=False)
        subprocess.run([str(ROSWAPCTL), "status"], check=False)

    def stop_stack(self) -> None:
        self.stop_io()
        subprocess.run(["swapoff", "/dev/roswap0"], check=False)
        subprocess.run(["rmdir", str(CGROUP)], check=False)
        self.unload()
        for key in list(self.procs):
            self._stop_key(key)
        self.close_genl()

    def _stop_key(self, key: str) -> None:
        proc = self.procs.pop(key, None)
        if proc is None:
            return
        if proc.poll() is None:
            stop_pid(proc.pid)
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()

    def set_dead_after(self, seconds: int) -> None:
        path = self.log_dir / "controller.toml"
        lines = []
        for line in path.read_text().splitlines():
            if line.startswith("dead_after_s"):
                lines.append(f"dead_after_s = {seconds}")
            else:
                lines.append(line)
        path.write_text("\n".join(lines) + "\n")

    def start_controller(self) -> None:
        say("starting controller")
        log = open(self.log_dir / "controller.log", "w", encoding="utf-8")
        proc = subprocess.Popen(
            [str(ROSWAPD), str(self.log_dir / "controller.toml")],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        self.procs["controller"] = proc
        wait_until("controller listening", 20, lambda: self._ready("controller", CTRL_PORT, None))

    def start_provider(
        self,
        node_id: int = 1,
        epoch: int = 1,
        boot_id: str | None = None,
        controller: str | None = CTRL_ADDR,
    ) -> None:
        grpc = grpc_port(node_id)
        rdma = rdma_port(node_id)
        mem = MEM_DIR / str(node_id)
        mem.mkdir(parents=True, exist_ok=True)
        cfg = self.log_dir / f"provider-{node_id}.toml"
        text = (E2E_DIR / "provider.toml").read_text()
        text = (
            text.replace("@RDMA_DEV@", self._dev)
            .replace("@RDMA_IP@", self._ip)
            .replace("@RDMA_PORT@", str(rdma))
            .replace("@GRPC_PORT@", str(grpc))
            .replace("@MEM_DIR@", str(mem))
            .replace("@NODE_ID@", str(node_id))
            .replace("node_epoch = 1", f"node_epoch = {epoch}")
        )
        if controller is None:
            text = text.replace(f'controller_addr = "{CTRL_ADDR}"', 'controller_addr = ""')
        elif controller != CTRL_ADDR:
            text = text.replace(CTRL_ADDR, controller)
        cfg.write_text(text)
        log_path = self.log_dir / (f"provider-{node_id}.log" if node_id != 1 else "provider.log")
        say(f"starting provider {node_id}")
        log = open(log_path, "w", encoding="utf-8")
        env = os.environ.copy()
        if boot_id:
            env["ROSWAP_TEST_BOOT_ID"] = boot_id
        else:
            env.pop("ROSWAP_TEST_BOOT_ID", None)
        proc = subprocess.Popen(
            [str(ROSWAPD), str(cfg)],
            stdout=log,
            stderr=subprocess.STDOUT,
            env=env,
        )
        self.procs[f"provider-{node_id}"] = proc
        wait_until(
            f"provider {node_id} listening",
            30,
            lambda: self._ready(f"provider-{node_id}", grpc, "provider initialized"),
        )

    def _ready(self, key: str, port: int, needle: str | None) -> bool:
        proc = self.procs[key]
        if proc.poll() is not None:
            raise E2EError(f"{key} exited; see {self.log_dir}")
        if needle:
            log = self._log_for(key)
            if needle not in log.read_text(errors="replace"):
                return False
        return port_listening(port)

    def _log_for(self, key: str) -> Path:
        if key == "controller":
            return self.log_dir / "controller.log"
        node = key.split("-", 1)[1]
        if node == "1":
            return self.log_dir / "provider.log"
        return self.log_dir / f"provider-{node}.log"

    def pid(self, node_id: int) -> int:
        return self.procs[f"provider-{node_id}"].pid

    def freeze(self, node_id: int) -> None:
        os.kill(self.pid(node_id), signal.SIGSTOP)

    def thaw(self, node_id: int) -> None:
        os.kill(self.pid(node_id), signal.SIGCONT)

    def kill_provider(self, node_id: int) -> None:
        proc = self.procs.get(f"provider-{node_id}")
        if proc is not None and proc.poll() is None:
            os.kill(proc.pid, signal.SIGKILL)

    def stop_provider(self, node_id: int) -> None:
        self._stop_key(f"provider-{node_id}")
        wait_until(f"provider {node_id} port free", 10, lambda: not port_listening(grpc_port(node_id)))

    def load(self, capacity_mb: int = CAPACITY_MB, extra: tuple[str, ...] = ()) -> None:
        say("loading roswap.ko " + " ".join(extra))
        subprocess.run(["modprobe", "-q", "rdma_cm"], check=False)
        cmd = [
            "insmod",
            str(KO),
            f"chunk_size_mb={CHUNK_MB}",
            f"capacity_mb={capacity_mb}",
            *extra,
        ]
        subprocess.run(cmd, check=True)
        if not Path("/dev/roswap0").exists():
            raise E2EError("/dev/roswap0 is missing after insmod")
        self.close_genl()

    def insmod_fails(self, *params: str) -> None:
        cmd = ["insmod", str(KO), f"chunk_size_mb={CHUNK_MB}"]
        if not any(item.startswith("capacity_mb=") for item in params):
            cmd.append(f"capacity_mb={CAPACITY_MB}")
        cmd.extend(params)
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
        if proc.returncode == 0:
            self.unload()
            raise E2EError(f"insmod accepted {' '.join(params)}")

    def unload(self) -> None:
        self.close_genl()
        if not Path("/sys/module/roswap").exists():
            return
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if subprocess.run(["rmmod", "roswap"], check=False).returncode == 0:
                return
            if not Path("/sys/module/roswap").exists():
                return
            time.sleep(0.1)
        subprocess.run(["rmmod", "roswap"], check=True)

    def bring_up(
        self,
        providers: int = 1,
        protection: str | None = None,
        dead_after: int = 30,
        capacity_mb: int = CAPACITY_MB,
        extra: tuple[str, ...] = (),
    ) -> None:
        self.providers = providers
        self.want_chunks = providers * 2
        self.want_links = providers
        self.want_nodes = providers
        self.set_dead_after(dead_after)
        params = list(extra)
        if protection:
            params.append(f"protection={protection}")
        self.start_controller()
        self.load(capacity_mb, tuple(params))
        for node_id in range(1, providers + 1):
            self.start_provider(node_id)
        self.wait_kernel()

    def kernel_ready(self) -> bool:
        try:
            st = self.stats()
        except (GenlError, OSError):
            return False
        if st.nr_chunks < self.want_chunks or st.links_up < self.want_links:
            return False
        try:
            view = self.cluster()
        except (E2EError, json.JSONDecodeError, subprocess.CalledProcessError):
            return False
        synced = sum(1 for node in view["nodes"] if node["synced"])
        return synced >= self.want_nodes

    def wait_kernel(self) -> None:
        wait_until("chunk applied and link up", 30, self.kernel_ready)
        subprocess.run([str(ROSWAPCTL), "status"], check=False)
        subprocess.run([str(ROSWAPCTL), "cluster", "--json", CTRL_ADDR], check=False)

    def cluster(self) -> dict:
        proc = subprocess.run(
            [str(ROSWAPCTL), "cluster", "--json", CTRL_ADDR],
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(proc.stdout)

    def node(self, node_id: int) -> dict:
        for node in self.cluster()["nodes"]:
            if node["node_id"] == node_id:
                return node
        raise E2EError(f"node {node_id} is not in the cluster")

    def node_state(self, node_id: int) -> str | None:
        st = self.stats()
        for nid, name in st.nodes:
            if nid == node_id:
                return name
        return None

    def wait_state(self, node_id: int, want: str, timeout: float) -> None:
        wait_until(
            f"node {node_id} {want}",
            timeout,
            lambda: self.node_state(node_id) == want,
        )

    def write_sysfs(self, name: str, value: str) -> None:
        Path(f"/sys/module/roswap/parameters/{name}").write_text(value + "\n")

    def drop_connection(self, node_id: int | None = None) -> None:
        self.write_sysfs("drop_connection", f"node={node_id}" if node_id else "1")

    def drop_heartbeat(self, node_id: int | None = None) -> None:
        self.write_sysfs("drop_heartbeat", f"node={node_id}" if node_id else "1")

    def write_pattern(self, count: int = 256, seek: int = 0) -> str:
        src = self.log_dir / "pattern"
        subprocess.run(
            ["dd", "if=/dev/urandom", f"of={src}", "bs=4096", f"count={count}", "status=none"],
            check=True,
        )
        cmd = [
            "dd",
            f"if={src}",
            "of=/dev/roswap0",
            "bs=4096",
            f"count={count}",
            "oflag=direct",
            "conv=fsync",
            "status=none",
        ]
        if seek:
            cmd.append(f"seek={seek}")
        subprocess.run(cmd, check=True)
        return hashlib.sha256(src.read_bytes()).hexdigest()

    def read_hash(self, count: int = 256, skip: int = 0) -> str:
        dst = self.log_dir / "readback"
        cmd = [
            "dd",
            "if=/dev/roswap0",
            f"of={dst}",
            "bs=4096",
            f"count={count}",
            "iflag=direct",
            "status=none",
        ]
        if skip:
            cmd.append(f"skip={skip}")
        subprocess.run(cmd, check=True)
        return hashlib.sha256(dst.read_bytes()).hexdigest()

    def start_io(self) -> None:
        self.stop_io()
        stop = self.log_dir / "io.stop"
        fail = self.log_dir / "io.fail"
        stop.unlink(missing_ok=True)
        fail.unlink(missing_ok=True)
        script = (
            "while [ ! -f \"$1\" ]; do "
            "dd if=/dev/roswap0 of=/dev/null bs=4096 count=1024 iflag=direct status=none "
            "|| { touch \"$2\"; exit 1; }; done"
        )
        proc = subprocess.Popen(["bash", "-c", script, "io", str(stop), str(fail)])
        self.procs["io"] = proc

    def stop_io(self) -> None:
        stop = self.log_dir / "io.stop"
        stop.parent.mkdir(parents=True, exist_ok=True)
        stop.write_text("1\n")
        proc = self.procs.pop("io", None)
        if proc is not None:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    def io_failed(self) -> bool:
        return (self.log_dir / "io.fail").exists()

    def blkcheck(self, *args: str) -> None:
        subprocess.run([str(BLKCHECK), "/dev/roswap0", *args], check=True)

    def latread(self, samples: int = 100) -> None:
        subprocess.run([str(LATREAD), "/dev/roswap0", str(samples)], check=True)

    def setup_swap(self) -> None:
        subprocess.run(["swapoff", "-a"], check=False)
        subprocess.run(["mkswap", "/dev/roswap0"], check=True, capture_output=True)
        subprocess.run(["swapon", "-p", "100", "/dev/roswap0"], check=True)

    def memhold(self, *args: str) -> subprocess.Popen:
        setup_memcg()

        def preexec():
            with open(CGROUP / "cgroup.procs", "w", encoding="ascii") as procs:
                procs.write(str(os.getpid()))

        return subprocess.Popen([str(MEMHOLD), *args], preexec_fn=preexec)

    def watch_swap(self, proc: subprocess.Popen) -> int:
        peak = 0
        while proc.poll() is None:
            peak = max(peak, swap_used_kb())
            time.sleep(0.1)
        return max(peak, swap_used_kb())

    def gave_up(self) -> int:
        return dmesg_count(self.mark, "gave up")

    def check_dmesg(self) -> None:
        assert_dmesg_clean(self.mark)


def rdma_endpoint() -> tuple[str, str]:
    dev = os.environ.get("ROSWAP_E2E_DEV", "rxe0")
    if os.environ.get("ROSWAP_E2E_IP"):
        return dev, os.environ["ROSWAP_E2E_IP"]
    if not Path(f"/sys/class/infiniband/{dev}").exists():
        raise E2EError(f"no {dev}. Load rdma_rxe and run rdma link add, or set ROSWAP_E2E_DEV")
    show = subprocess.run(["rdma", "link", "show"], check=True, capture_output=True, text=True).stdout
    netdev = ""
    for line in show.splitlines():
        if f"{dev}/" not in line:
            continue
        parts = line.split()
        if "netdev" in parts:
            netdev = parts[parts.index("netdev") + 1]
            break
    if not netdev:
        raise E2EError(f"{dev} is not in rdma link show")
    addr = subprocess.run(
        ["ip", "-4", "-o", "addr", "show", "dev", netdev],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()
    if len(addr) < 4:
        raise E2EError(f"netdev {netdev} for {dev} has no IPv4")
    return dev, addr[3].split("/")[0]

