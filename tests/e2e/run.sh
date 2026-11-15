#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/lib.sh"

scenario_blk() {
	local expect got
	bring_up
	say "blkcheck"
	"$BLKCHECK" /dev/roswap0
	say "O_DIRECT read/write compare"
	expect=$(write_pattern)
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "readback mismatch"
	say "blk passed"
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

setup_memcg() {
	local root=/sys/fs/cgroup
	if [[ $(stat -fc %T "$root" 2>/dev/null) != cgroup2fs ]]; then
		if grep -q " $root " /proc/mounts; then
			die "$root is not cgroup v2"
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
	swapoff -a || true
	mkswap /dev/roswap0 >/dev/null
	swapon -p 100 /dev/roswap0
	say "swap and verify inside the cgroup"
	setup_memcg
	bash -c 'echo $$ >"$1/cgroup.procs" && shift && exec "$@"' _ \
		"$E2E_CGROUP" "$MEMHOLD" -m 96m -i 1 -n 4 &
	child=$!
	watch_swap_peak "$child" &
	sampler=$!
	wait "$child" || rc=$?
	wait "$sampler" || true
	max=$(cat "$E2E_LOG_DIR/swap.peak")
	[[ $rc -eq 0 ]] || die "memhold verification failed, exit code $rc"
	if [[ ${max:-0} -le 0 ]]; then
		echo "----- /proc/swaps -----" >&2
		cat /proc/swaps >&2 || true
		die "/dev/roswap0 was not used for swap (peak Used ${max:-0} KiB)"
	fi
	say "swap passed, peak ${max} KiB"
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
	say "writing baseline data"
	expect=$(write_pattern)

	say "dropping the RC connection during IO"
	start_io_loop
	sleep 0.5
	before=$(dmesg_count "connected to")
	echo 1 >/sys/module/roswap/parameters/drop_connection
	sleep 2
	stop_io_loop
	[[ ! -f $E2E_LOG_DIR/io.fail ]] || die "IO failed while the link was down"
	after=$(dmesg_count "connected to")
	[[ $after -gt $before ]] || die "no new "connected to" in dmesg (before $before, after $after)"
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "readback mismatch after reconnect"

	say "restarting provider, waiting for a new rkey"
	old_rkey=$(cluster_rkey)
	[[ -n $old_rkey ]] || die "no rkey before restart"
	RKEY_BEFORE=$(dmesg_count "rkey updated")
	GAVE_UP_BEFORE=$(dmesg_count "gave up")
	stop_pid_file "$E2E_LOG_DIR/provider.pid"
	wait_for "provider port released" 10 provider_port_free
	wait_for "kernel gave up the old connection" 20 reconnect_gave_up
	sleep 2
	start_provider
	wait_for "CHUNK_UPDATE" 30 rkey_bumped
	wait_kernel
	after_rkey=$(cluster_rkey)
	[[ -n $after_rkey && $after_rkey != "$old_rkey" ]] || die "rkey was not updated ($old_rkey -> ${after_rkey:-empty})"
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "readback mismatch after provider restart"
	say "fault passed"
}

scenario_params() {
	local ko=$ROOT/kernel/roswap.ko
	modprobe -q rdma_cm 2>/dev/null || true
	say "invalid address"
	if insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB" local_addrs=not-an-ip \
		>"$E2E_LOG_DIR/bad-ip.txt" 2>&1; then
		rmmod roswap || true
		die "invalid local_addrs was accepted"
	fi
	say "address with no matching netdev"
	if insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB" local_addrs=192.0.2.1 \
		>"$E2E_LOG_DIR/missing-ip.txt" 2>&1; then
		rmmod roswap || true
		die "address for a missing netdev was accepted"
	fi
	say "logical disk larger than 16TiB"
	if insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb=$(( (16 << 20) + 1 )) \
		>"$E2E_LOG_DIR/huge-cap.txt" 2>&1; then
		rmmod roswap || true
		die "capacity_mb above 16TiB was accepted"
	fi
	say "omitting local_addrs"
	insmod "$ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB"
	[[ -b /dev/roswap0 ]] || die "default parameters did not create /dev/roswap0"
	unload_roswap
	say "params passed"
}

scenario_stress() {
	local expect got
	bring_up
	say "reading across repeated disconnects"
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
	[[ ! -f $E2E_LOG_DIR/io.fail ]] || die "IO failed during repeated disconnects"
	if dmesg | grep -q "possible circular locking dependency"; then
		die "lockdep reported a lock-order problem"
	fi
	if dmesg | grep -q "BUG: sleeping function called from invalid context"; then
		die "sleeping in atomic context"
	fi
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "readback mismatch after repeated disconnects"
	say "stress passed"
}

provider_port_free() {
	! port_listening "$E2E_GRPC_PORT"
}

main() {
	local scenario=${1:-}
	case $scenario in
	blk | swap | fault | params | stress) ;;
	*)
		echo "usage: $0 blk|swap|fault|params|stress" >&2
		exit 2
		;;
	esac
	e2e_init
	"scenario_$scenario"
}

main "$@"
