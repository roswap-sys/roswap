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

E2E_CGROUP=/sys/fs/cgroup/roswap-e2e

# 直接写 cgroup v2，不依赖 systemd：测试虚拟机里没有 systemd。
setup_memcg() {
	local root=/sys/fs/cgroup
	if [[ $(stat -fc %T "$root" 2>/dev/null) != cgroup2fs ]]; then
		if grep -q " $root " /proc/mounts; then
			die "$root 不是 cgroup v2"
		fi
		mount -t cgroup2 none "$root"
	fi
	grep -qw memory "$root/cgroup.subtree_control" ||
		echo +memory >"$root/cgroup.subtree_control"
	mkdir -p "$E2E_CGROUP"
	echo 64M >"$E2E_CGROUP/memory.max"
	if [[ -f $E2E_CGROUP/memory.swap.max ]]; then
		echo 128M >"$E2E_CGROUP/memory.swap.max"
	fi
}

scenario_swap() {
	local child sampler max rc=0
	bring_up
	say "mkswap / swapon"
	# runner 上通常已经有一块优先级更高的 swap。不指定优先级时，
	# 新设备会排到它后面，页不会落到 /dev/roswap0。
	swapoff -a || true
	mkswap /dev/roswap0 >/dev/null
	swapon -p 100 /dev/roswap0
	say "cgroup 内换页并校验"
	setup_memcg
	bash -c 'echo $$ >"$1/cgroup.procs" && shift && exec "$@"' _ \
		"$E2E_CGROUP" "$MEMHOLD" -m 96m -i 1 -n 4 &
	child=$!
	watch_swap_peak "$child" &
	sampler=$!
	wait "$child" || rc=$?
	wait "$sampler" || true
	max=$(cat "$E2E_LOG_DIR/swap.peak")
	[[ $rc -eq 0 ]] || die "memhold 校验失败，退出码 $rc"
	if [[ ${max:-0} -le 0 ]]; then
		echo "----- /proc/swaps -----" >&2
		cat /proc/swaps >&2 || true
		die "/dev/roswap0 没有被换出使用（Used 峰值 ${max:-0} KiB）"
	fi
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

scenario_params() {
	local ko=$ROOT/kernel/roswap.ko
	modprobe -q rdma_cm 2>/dev/null || true
	say "非法地址"
	if insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB" local_addrs=not-an-ip \
		>"$E2E_LOG_DIR/bad-ip.txt" 2>&1; then
		rmmod roswap || true
		die "非法 local_addrs 被接受了"
	fi
	say "没有对应网卡的地址"
	if insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB" local_addrs=192.0.2.1 \
		>"$E2E_LOG_DIR/missing-ip.txt" 2>&1; then
		rmmod roswap || true
		die "不存在的网卡地址被接受了"
	fi
	say "超过 16TiB 的逻辑盘"
	if insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb=$(( (16 << 20) + 1 )) \
		>"$E2E_LOG_DIR/huge-cap.txt" 2>&1; then
		rmmod roswap || true
		die "16TiB 以上的 capacity_mb 被接受了"
	fi
	say "不写 local_addrs"
	insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB"
	[[ -b /dev/roswap0 ]] || die "默认参数没有建出 /dev/roswap0"
	# 新块设备刚注册时，udev 可能还握着打开的 fd，rmmod 会暂时报 in use。
	unload_roswap
	say "params 通过"
}

scenario_stress() {
	local expect got
	bring_up
	say "反复断连时继续读"
	expect=$(write_pattern)
	start_io_loop
	sleep 0.2
	local i
	for i in 1 2 3 4; do
		echo 1 >/sys/module/roswap/parameters/drop_connection
		sleep 0.4
	done
	sleep 2
	stop_io_loop
	[[ ! -f $E2E_LOG_DIR/io.fail ]] || die "反复断连期间 IO 失败"
	if dmesg | grep -q "possible circular locking dependency"; then
		die "lockdep 报告了锁顺序问题"
	fi
	if dmesg | grep -q "BUG: sleeping function called from invalid context"; then
		die "原子上下文里睡眠"
	fi
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "反复断连后读回内容不一致"
	say "stress 通过"
}

provider_port_free() {
	! port_listening "$E2E_GRPC_PORT"
}

main() {
	local scenario=${1:-}
	case $scenario in
	blk | swap | fault | params | stress) ;;
	*)
		echo "用法: $0 blk|swap|fault|params|stress" >&2
		exit 2
		;;
	esac
	e2e_init
	"scenario_$scenario"
}

main "$@"
