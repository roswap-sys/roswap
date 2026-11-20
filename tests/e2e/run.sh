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
	local expect got before after old_rkey after_rkey boot_before boot_after
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
	boot_before=$(node_boot 1)
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
	boot_after=$(node_boot 1)
	[[ -n $after_rkey && $after_rkey != "$old_rkey" ]] || die "rkey was not updated ($old_rkey -> ${after_rkey:-empty})"
	got=$(read_hash)
	[[ $expect == "$got" ]] || die "readback mismatch after provider restart"
	[[ $(node_state 1) != dead ]] || die "same-boot restart was judged dead"
	[[ -n $boot_before && $boot_before == "$boot_after" ]] ||
		die "boot id changed across a same-boot restart ($boot_before -> ${boot_after:-empty})"
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

wait_state_for() {
	local id=$1 want=$2 tenths=$3 i
	for ((i = 0; i < tenths; i++)); do
		[[ $(node_state "$id") == "$want" ]] && return 0
		sleep 0.1
	done
	node_state "$id" >&2 || true
	return 1
}

scenario_genl() {
	local cargo_home=${CARGO_HOME:-$HOME/.cargo} rustup_home=${RUSTUP_HOME:-}
	local home cargo
	load_module
	cargo=$(command -v cargo || true)
	if [[ -n ${SUDO_USER:-} ]]; then
		home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
		cargo_home=$home/.cargo
		# sudo drops the caller's rustup dir, so the proxy has no toolchain.
		[[ -z $rustup_home && -d $home/.rustup ]] && rustup_home=$home/.rustup
		[[ -x $home/.cargo/bin/cargo ]] && cargo=$home/.cargo/bin/cargo
	fi
	[[ -n $cargo ]] || die "cargo is required for the netlink test"
	[[ -n $rustup_home ]] && export RUSTUP_HOME=$rustup_home
	CARGO_HOME=$cargo_home CARGO_TARGET_DIR=/tmp/roswap-genl-target \
		"$cargo" test --locked --manifest-path "$ROOT/Cargo.toml" --lib \
		kernel_rejects_illegal_netlink -- --ignored --exact --test-threads=1
	unload_roswap
	say "genl passed"
}

scenario_suspect() {
	local child rc=0 before
	set_dead_after 30
	WANT_CHUNKS=4 WANT_NODES=2 WANT_LINKS=2 E2E_PROVIDERS=2 \
		bring_up protection=replica2
	say "swap while one provider is frozen"
	swapoff -a || true
	mkswap /dev/roswap0 >/dev/null
	swapon -p 100 /dev/roswap0
	setup_memcg
	before=$(status_field degraded)
	bash -c 'echo $$ >"$1/cgroup.procs" && shift && exec "$@"' _ \
		"$E2E_CGROUP" "$MEMHOLD" -m 96m -i 1 -n 4 &
	child=$!
	sleep 0.4
	freeze_provider 2
	wait_state_for 2 suspect 20 || die "provider 2 did not become suspect within 2s"
	wait "$child" || rc=$?
	[[ $rc -eq 0 ]] || die "memhold failed while a replica was suspect, exit $rc"
	[[ $(node_state 2) != dead ]] || die "a short freeze was judged dead"
	[[ $(status_field lost) == 0 ]] || die "suspect lost pages"
	dd if=/dev/urandom of=/dev/roswap0 bs=4096 count=256 seek=256 oflag=direct \
		conv=fsync status=none || die "write during SUSPECT failed"
	[[ $(status_field degraded) -gt ${before:-0} ]] ||
		die "extents placed while a node was suspect were not marked short"
	thaw_provider 2
	wait_state_for 2 up 30 || die "provider 2 did not return to up"
	[[ $(node_state 2) != dead ]] || die "thaw was judged dead"
	say "suspect passed"
}

scenario_dead() {
	local expect child rc=0
	set_dead_after 10
	WANT_CHUNKS=4 WANT_NODES=2 WANT_LINKS=2 E2E_PROVIDERS=2 \
		bring_up protection=replica2
	say "killing one replica"
	expect=$(write_pattern)
	kill_provider 2
	wait_for "provider 2 dead" 20 wait_node_state 2 dead
	[[ $(read_hash) == "$expect" ]] || die "replica2 readback mismatch after a dead node"
	[[ $(status_field lost) == 0 ]] || die "replica2 lost pages ($(status_field lost))"

	say "single provider death returns EIO and SIGBUS"
	e2e_stop_stack
	set_dead_after 10
	bring_up
	write_pattern >/dev/null
	swapoff -a || true
	mkswap /dev/roswap0 >/dev/null
	swapon -p 100 /dev/roswap0
	setup_memcg
	bash -c 'echo $$ >"$1/cgroup.procs" && shift && exec "$@"' _ \
		"$E2E_CGROUP" "$MEMHOLD" -m 96m -i 1 -n 30 &
	child=$!
	sleep 1.5
	kill_provider 1
	wait "$child" || rc=$?
	[[ $rc -eq 135 ]] || die "memhold exit $rc, want SIGBUS (135)"
	wait_for "provider 1 dead" 20 wait_node_state 1 dead
	if dd if=/dev/roswap0 of=/dev/null bs=4096 count=1 iflag=direct status=none \
		>"$E2E_LOG_DIR/lost-read.txt" 2>&1; then
		die "read of a lost page succeeded"
	fi
	say "dead passed"
}

scenario_robust() {
	local expect
	set_dead_after 8
	WANT_CHUNKS=4 WANT_NODES=2 WANT_LINKS=2 E2E_PROVIDERS=2 \
		bring_up protection=replica2
	expect=$(write_pattern)

	say "freezing every provider is not a node death"
	freeze_provider 1
	freeze_provider 2
	sleep 10
	[[ $(node_state 1) != dead && $(node_state 2) != dead ]] ||
		die "a majority freeze was judged dead"
	thaw_provider 1
	thaw_provider 2
	wait_state_for 1 up 30 || die "provider 1 stayed down after thaw"
	wait_state_for 2 up 30 || die "provider 2 stayed down after thaw"
	[[ $(read_hash) == "$expect" ]] || die "readback mismatch after a majority freeze"

	say "heartbeat QP drop does not condemn the node"
	echo 1 >/sys/module/roswap/parameters/drop_heartbeat
	sleep 2
	[[ $(node_state 1) != dead && $(node_state 2) != dead ]] ||
		die "dropping the heartbeat QP was judged dead"
	wait_state_for 1 up 30 || die "provider 1 did not return after a heartbeat drop"
	[[ $(read_hash) == "$expect" ]] || die "readback mismatch after a heartbeat drop"

	say "data QP drop does not mark the node suspect"
	echo 1 >/sys/module/roswap/parameters/drop_connection
	sleep 1
	[[ $(node_state 1) == up && $(node_state 2) == up ]] ||
		die "a data disconnect changed node health ($(node_state 1) $(node_state 2))"

	say "controller restart keeps a suspect node and still condemns it later"
	freeze_provider 2
	wait_state_for 2 suspect 20 || die "provider 2 did not become suspect"
	kill -KILL "$(cat "$E2E_LOG_DIR/controller.pid")" 2>/dev/null || true
	wait_for "controller port free" 10 controller_port_free
	start_controller
	sleep 1
	[[ $(node_state 2) != dead ]] || die "controller restart immediately judged a suspect node dead"
	wait_for "suspect becomes dead" 20 wait_node_state 2 dead

	say "removing the module while a heartbeat read is in flight"
	e2e_stop_stack
	set_dead_after 30
	bring_up
	freeze_provider 1
	wait_state_for 1 suspect 20 || die "provider 1 did not become suspect before rmmod"
	unload_roswap
	say "robust passed"
}

controller_port_free() {
	! port_listening "$E2E_CTRL_PORT"
}

main() {
	local scenario=${1:-}
	case $scenario in
	blk | swap | fault | params | stress | genl | suspect | dead | robust) ;;
	*)
		echo "usage: $0 blk|swap|fault|params|stress|genl|suspect|dead|robust" >&2
		exit 2
		;;
	esac
	e2e_init
	"scenario_$scenario"
	assert_dmesg_clean
}

main "$@"
