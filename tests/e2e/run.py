#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run one RoSwap scenario, or every scenario, on the host or inside a virtme guest."""

from __future__ import annotations

import argparse
import platform
import sys
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cluster import Cluster
from env import E2EError, dmesg_text, ensure_tmpfs, setup_rxe
from scenarios import SCENARIOS, run_named


def main() -> int:
    parser = argparse.ArgumentParser(description="RoSwap end-to-end tests")
    parser.add_argument("scenario", nargs="?", choices=[*SCENARIOS, "all"])
    parser.add_argument("--vm-out", type=Path)
    parser.add_argument("--setup-rxe", action="store_true")
    parser.add_argument("--rxe-count", type=int, default=1)
    args = parser.parse_args()
    if args.setup_rxe:
        setup_rxe(max(args.rxe_count, 1))
        return 0
    names = list(SCENARIOS) if args.scenario in (None, "all") else [args.scenario]
    if args.vm_out is not None:
        return vm_main(args.vm_out, names)
    if args.scenario is None:
        parser.error("scenario is required outside a VM")
    setup_rxe(1)
    for name in names:
        if not run_one(name, Path("/tmp/roswap-e2e") / name):
            return 1
    return 0


def vm_main(out: Path, names: list[str]) -> int:
    out.mkdir(parents=True, exist_ok=True)
    (out / "result").write_text("running\n")
    (out / "uname.txt").write_text(platform.platform() + "\n")
    ensure_tmpfs()
    rc = 0
    try:
        setup_rxe(1)
    except (E2EError, OSError) as err:
        (out / "setup-rxe.txt").write_text(str(err) + "\n")
        print(f"e2e: {err}", file=sys.stderr)
        rc = 1
    if rc == 0:
        for name in names:
            print(f"===== {name} =====", flush=True)
            if run_one(name, out / name):
                with (out / "summary.txt").open("a", encoding="utf-8") as summary:
                    summary.write(f"{name} ok\n")
            else:
                with (out / "summary.txt").open("a", encoding="utf-8") as summary:
                    summary.write(f"{name} FAILED\n")
                rc = 1
    try:
        (out / "dmesg.txt").write_text(dmesg_text())
    except OSError:
        pass
    (out / "result").write_text(f"{rc}\n")
    return rc


def run_one(name: str, log_dir: Path) -> bool:
    cluster = Cluster(log_dir)
    failed = False
    try:
        cluster.prepare()
        run_named(name, cluster)
        cluster.check_dmesg()
        return True
    except Exception as err:
        failed = True
        print(f"e2e: {name}: {err}", file=sys.stderr)
        if not isinstance(err, E2EError):
            traceback.print_exc()
        return False
    finally:
        try:
            cluster.cleanup(failed)
        except Exception:
            traceback.print_exc()
            return False


if __name__ == "__main__":
    sys.exit(main())
