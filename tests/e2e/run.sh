#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# 单机 Soft-RoCE 上的端到端场景。需要 root。
#   tests/e2e/run.sh blk|swap|fault

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib.sh"

scenario_blk() {
	local expect got
	bring_up
	say "blkcheck"
	"$BLKCHECK" /dev/roswap0
	say "O_DIRECT 读写比对"
	expect=$(write_pattern)
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "读回内容不一致"
	say "blk 通过"
}

swap_used_kb() {
	awk '$1 == "/dev/roswap0" { print $4 }' /proc/swaps
}

watch_swap_peak() {
	local child=$1 max=0 used
	echo 0 >"$E2E_LOG_DIR/swap.peak"
	while kill -0 "$child" 2>/dev/null; do
		used=$(swap_used_kb)
		if [[ ${used:-0} -gt $max ]]; then
			max=$used
			echo "$max" >"$E2E_LOG_DIR/swap.peak"
		fi
		sleep 0.1
	done
}

scenario_swap() {
	local child sampler max rc=0
	bring_up
	say "mkswap / swapon"
	mkswap /dev/roswap0 >/dev/null
	swapon /dev/roswap0
	say "cgroup 内换页并校验"
	systemd-run --scope \
		-p MemoryMax=64M -p MemorySwapMax=128M \
		"$MEMHOLD" -m 96m -i 1 -n 4 &
	child=$!
	watch_swap_peak "$child" &
	sampler=$!
	wait "$child" || rc=$?
	wait "$sampler" || true
	max=$(cat "$E2E_LOG_DIR/swap.peak")
	[[ $rc -eq 0 ]] || die "memhold 校验失败，退出码 $rc"
	[[ ${max:-0} -gt 0 ]] || die "/dev/roswap0 没有被换出使用（Used 峰值 ${max:-0} KiB）"
	say "swap 通过，峰值 ${max} KiB"
}

start_io_loop() {
	rm -f "$E2E_LOG_DIR/io.fail" "$E2E_LOG_DIR/io.stop"
	(
		while [[ ! -f $E2E_LOG_DIR/io.stop ]]; do
			if ! dd if=/dev/roswap0 of=/dev/null bs=4096 count=1024 iflag=direct status=none; then
				touch "$E2E_LOG_DIR/io.fail"
				exit 1
			fi
		done
	) &
	echo $! >"$E2E_LOG_DIR/io.pid"
}

reconnect_gave_up() {
	local n
	n=$(dmesg_count "gave up")
	[[ $n -gt ${GAVE_UP_BEFORE:-0} ]]
}

rkey_bumped() {
	local n
	n=$(dmesg_count "rkey updated")
	[[ $n -gt ${RKEY_BEFORE:-0} ]]
}

scenario_fault() {
	local expect got before after old_rkey after_rkey
	bring_up
	say "写入基线数据"
	expect=$(write_pattern)

	say "IO 中断开 RC 连接"
	start_io_loop
	sleep 0.5
	before=$(dmesg_count "connected to")
	echo 1 >/sys/module/roswap/parameters/drop_connection
	sleep 2
	stop_io_loop
	[[ ! -f $E2E_LOG_DIR/io.fail ]] || die "断链期间 IO 失败"
	after=$(dmesg_count "connected to")
	[[ $after -gt $before ]] || die "dmesg 里没有新的 connected to（之前 $before，之后 $after）"
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "断链重连后读回内容不一致"

	say "重启 provider，等待新 rkey"
	old_rkey=$(cluster_rkey)
	[[ -n $old_rkey ]] || die "重启前没有读到 rkey"
	RKEY_BEFORE=$(dmesg_count "rkey updated")
	GAVE_UP_BEFORE=$(dmesg_count "gave up")
	stop_pid_file "$E2E_LOG_DIR/provider.pid"
	wait_for "provider 端口释放" 10 provider_port_free
	# 旧 RC 还在 timewait 时，新的连接会被 STALE_CONN 拒绝，内核重试满 10 秒就放弃。
	# 等它放弃并让 timewait 结束，再启动 provider，CHUNK_UPDATE 会重新把链路拉起来。
	wait_for "内核放弃旧连接" 20 reconnect_gave_up
	sleep 2
	start_provider
	wait_for "CHUNK_UPDATE" 30 rkey_bumped
	wait_kernel
	after_rkey=$(cluster_rkey)
	[[ -n $after_rkey && $after_rkey != "$old_rkey" ]] || die "rkey 没有更新（$old_rkey -> ${after_rkey:-空}）"
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "provider 重启后读回内容不一致"
	say "fault 通过"
}

provider_port_free() {
	! ss -ltn | grep -q ":${E2E_GRPC_PORT} "
}

main() {
	local scenario=${1:-}
	case $scenario in
	blk | swap | fault) ;;
	*)
		echo "用法: $0 blk|swap|fault" >&2
		exit 2
		;;
	esac
	e2e_init
	"scenario_$scenario"
}

main "$@"
