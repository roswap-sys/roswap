#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
E2E_OUT=${1:?usage: $0 <output-dir> [scenarios...]}
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
