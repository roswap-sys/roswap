#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# virtme-ng 虚拟机里的测试入口。以 root 运行，宿主机的根文件系统只读挂进来，
# 只有 E2E_OUT 所在目录可写，结果和日志都放在那里，宿主机读 $E2E_OUT/result 判断成败。
#   tests/e2e/vm-run.sh <输出目录> [场景...]

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
E2E_OUT=${1:?用法: $0 <输出目录> [场景...]}
shift
scenarios=("$@")
if [[ ${#scenarios[@]} -eq 0 ]]; then
	scenarios=(blk swap fault params stress)
fi

mkdir -p "$E2E_OUT"
echo running >"$E2E_OUT/result"
uname -a | tee "$E2E_OUT/uname.txt"

mountpoint -q /dev/shm || mount -t tmpfs -o mode=1777 tmpfs /dev/shm
mountpoint -q /tmp || mount -t tmpfs tmpfs /tmp

rc=0
if ! "$ROOT/tests/e2e/setup-rxe.sh" >"$E2E_OUT/setup-rxe.txt" 2>&1; then
	cat "$E2E_OUT/setup-rxe.txt" >&2
	rc=1
fi

if [[ $rc -eq 0 ]]; then
	for s in "${scenarios[@]}"; do
		echo "===== $s ====="
		if E2E_LOG_DIR="$E2E_OUT/$s" "$ROOT/tests/e2e/run.sh" "$s"; then
			echo "$s ok" >>"$E2E_OUT/summary.txt"
		else
			echo "$s FAILED" >>"$E2E_OUT/summary.txt"
			rc=1
		fi
	done
fi

dmesg >"$E2E_OUT/dmesg.txt" 2>&1 || true
echo "$rc" >"$E2E_OUT/result"
exit "$rc"
