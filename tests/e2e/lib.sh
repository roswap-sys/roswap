# SPDX-License-Identifier: GPL-2.0

E2E_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$E2E_DIR/../.." && pwd)
E2E_LOG_DIR=${E2E_LOG_DIR:-/tmp/roswap-e2e}
ROSWAPD=${ROSWAPD:-$ROOT/target/release/roswapd}
ROSWAPCTL=${ROSWAPCTL:-$ROOT/target/release/roswapctl}
BLKCHECK=${BLKCHECK:-$ROOT/tools/blkcheck}
MEMHOLD=${MEMHOLD:-$ROOT/tools/memhold}
CHUNK_MB=64
CAPACITY_MB=128
MEM_DIR=/dev/shm/roswap-e2e
E2E_CTRL_PORT=12321
E2E_GRPC_PORT=12320

say() {
	printf '== %s\n' "$*"
}

die() {
	printf 'e2e: %s\n' "$*" >&2
	exit 1
}

require_file() {
	[[ -e $1 ]] || die "not found: $1"
}

e2e_rdma_dev() {
	if [[ -n ${ROSWAP_E2E_DEV:-} ]]; then
		printf '%s\n' "$ROSWAP_E2E_DEV"
		return
	fi
	[[ -d /sys/class/infiniband/rxe0 ]] || die "no rxe0. Load rdma_rxe and run rdma link add, or set ROSWAP_E2E_DEV"
	printf '%s\n' rxe0
}

e2e_rdma_ip() {
	local dev=$1 netdev ip

	if [[ -n ${ROSWAP_E2E_IP:-} ]]; then
		printf '%s\n' "$ROSWAP_E2E_IP"
		return
	fi
	netdev=$(rdma link show | awk -v dev="$dev" '
		index($0, dev "/") && !nd {
			for (i = 1; i <= NF; i++) if ($i == "netdev") nd = $(i + 1)
		}
		END { print nd }')
	[[ -n $netdev ]] || die "$dev is not in rdma link show"
	ip=$(ip -4 -o addr show dev "$netdev" | awk 'NR == 1 { print $4 }')
	ip=${ip%%/*}
	[[ -n $ip ]] || die "netdev $netdev for $dev has no IPv4"
	printf '%s\n' "$ip"
}

render_config() {
	local dev=$1 ip=$2

	printf '%s\n' "$dev" >"$E2E_LOG_DIR/rdma.dev"
	printf '%s\n' "$ip" >"$E2E_LOG_DIR/rdma.ip"
	cp "$E2E_DIR/controller.toml" "$E2E_LOG_DIR/controller.toml"
}

wait_for() {
	local desc=$1 timeout=$2
	local start=$SECONDS rc
	shift 2
	until "$@"; do
		rc=$?
		if (( rc >= 2 )); then
			printf 'e2e: %s failed\n' "$desc" >&2
			return "$rc"
		fi
		if (( SECONDS - start >= timeout )); then
			printf 'e2e: timed out: %s\n' "$desc" >&2
			return 1
		fi
		sleep 0.2
	done
}

stop_pid_file() {
	local file=$1 pid i

	[[ -f $file ]] || return 0
	pid=$(cat "$file")
	if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
		kill -TERM "$pid" 2>/dev/null || true
		for i in $(seq 1 50); do
			kill -0 "$pid" 2>/dev/null || break
			sleep 0.1
		done
		if kill -0 "$pid" 2>/dev/null; then
			kill -KILL "$pid" 2>/dev/null || true
		fi
	fi
	rm -f "$file"
}

stop_io_loop() {
	[[ -f $E2E_LOG_DIR/io.stop ]] || touch "$E2E_LOG_DIR/io.stop"
	if [[ -f $E2E_LOG_DIR/io.pid ]]; then
		wait "$(cat "$E2E_LOG_DIR/io.pid")" 2>/dev/null || true
		rm -f "$E2E_LOG_DIR/io.pid"
	fi
}

unload_roswap() {
	local i

	[[ -d /sys/module/roswap ]] || return 0
	for i in $(seq 1 30); do
		if rmmod roswap 2>/dev/null; then
			return 0
		fi
		[[ -d /sys/module/roswap ]] || return 0
		sleep 0.1
	done
	rmmod roswap
}

e2e_stop_stack() {
	stop_io_loop || true
	swapoff /dev/roswap0 2>/dev/null || true
	rmdir /sys/fs/cgroup/roswap-e2e 2>/dev/null || true
	unload_roswap || true
	stop_pid_file "$E2E_LOG_DIR/provider.pid"
	local provider_pid
	for provider_pid in "$E2E_LOG_DIR"/provider-*.pid; do
		[[ -e $provider_pid ]] || continue
		stop_pid_file "$provider_pid"
	done
	stop_pid_file "$E2E_LOG_DIR/controller.pid"
	stop_pid_file "$E2E_LOG_DIR/dmesg.pid"
}

e2e_cleanup() {
	e2e_stop_stack
	rm -rf "$MEM_DIR"
}

e2e_on_exit() {
	local rc=$?
	trap - EXIT INT TERM
	if [[ $rc -ne 0 ]]; then
		echo "===== provider =====" >&2
		tail -n 80 "$E2E_LOG_DIR"/provider*.log >&2 || true
		echo "===== controller =====" >&2
		tail -n 80 "$E2E_LOG_DIR/controller.log" >&2 || true
		echo "===== cluster =====" >&2
		"$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}" >&2 || true
		echo "===== status =====" >&2
		"$ROSWAPCTL" status >&2 || true
		echo "===== dmesg =====" >&2
		dmesg | grep -E 'RoSwap|roswap' | tail -n 40 >&2 || true
	fi
	e2e_cleanup
	exit "$rc"
}

start_dmesg_follow() {
	: >"$E2E_LOG_DIR/dmesg.txt"
	dmesg -w >>"$E2E_LOG_DIR/dmesg.txt" 2>/dev/null &
	echo $! >"$E2E_LOG_DIR/dmesg.pid"
	sleep 0.2
	if ! kill -0 "$(cat "$E2E_LOG_DIR/dmesg.pid")" 2>/dev/null; then
		rm -f "$E2E_LOG_DIR/dmesg.pid"
		say "dmesg -w is unavailable; check the dmesg snapshot on failure"
	fi
}

e2e_init() {
	local dev ip

	[[ $(id -u) -eq 0 ]] || die "root is required to load the module and configure swap"
	require_file "$ROSWAPD"
	require_file "$ROSWAPCTL"
	require_file "$BLKCHECK"
	require_file "$MEMHOLD"
	require_file "$ROOT/kernel/roswap.ko"
	mkdir -p "$E2E_LOG_DIR"
	dev=$(e2e_rdma_dev)
	ip=$(e2e_rdma_ip "$dev")
	say "RDMA $dev $ip"
	render_config "$dev" "$ip"
	e2e_stop_stack
	rm -rf "$MEM_DIR"
	: >"$E2E_LOG_DIR/provider.log"
	: >"$E2E_LOG_DIR/controller.log"
	start_dmesg_follow
	E2E_DMESG_MARK="roswap-e2e-$$-$RANDOM"
	echo "$E2E_DMESG_MARK" >/dev/kmsg || true
	trap e2e_on_exit EXIT
	trap 'exit 143' INT TERM
}

start_controller() {
	say "starting controller"
	"$ROSWAPD" "$E2E_LOG_DIR/controller.toml" >>"$E2E_LOG_DIR/controller.log" 2>&1 &
	echo $! >"$E2E_LOG_DIR/controller.pid"
	wait_for "controller listening on ${E2E_CTRL_PORT}" 20 controller_ready
}

controller_ready() {
	local pid
	pid=$(cat "$E2E_LOG_DIR/controller.pid")
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "controller exited; see $E2E_LOG_DIR/controller.log" >&2
		return 2
	fi
	port_listening "$E2E_CTRL_PORT"
}

port_listening() {
	[[ -n $(ss -Hltn "sport = :$1") ]]
}

provider_pid_file() {
	local id=${1:-1}
	if [[ $id -eq 1 ]]; then
		printf '%s\n' "$E2E_LOG_DIR/provider.pid"
	else
		printf '%s\n' "$E2E_LOG_DIR/provider-$id.pid"
	fi
}

provider_log_file() {
	local id=${1:-1}
	if [[ $id -eq 1 ]]; then
		printf '%s\n' "$E2E_LOG_DIR/provider.log"
	else
		printf '%s\n' "$E2E_LOG_DIR/provider-$id.log"
	fi
}

provider_grpc_port() {
	local id=${1:-1}
	if [[ $id -eq 1 ]]; then
		printf '%s\n' "$E2E_GRPC_PORT"
	else
		printf '%s\n' $((12330 + id))
	fi
}

provider_rdma_port() {
	local id=${1:-1}
	if [[ $id -eq 1 ]]; then
		printf '%s\n' 18615
	else
		printf '%s\n' $((18614 + id))
	fi
}

start_provider() {
	local id=${1:-1}
	local grpc rdma mem cfg log pid dev ip
	grpc=$(provider_grpc_port "$id")
	rdma=$(provider_rdma_port "$id")
	mem=$MEM_DIR/$id
	cfg=$E2E_LOG_DIR/provider-$id.toml
	log=$(provider_log_file "$id")
	pid=$(provider_pid_file "$id")
	dev=$(cat "$E2E_LOG_DIR/rdma.dev")
	ip=$(cat "$E2E_LOG_DIR/rdma.ip")
	mkdir -p "$mem"
	sed -e "s/@RDMA_DEV@/${dev}/g" \
		-e "s/@RDMA_IP@/${ip}/g" \
		-e "s/@RDMA_PORT@/${rdma}/g" \
		-e "s/@GRPC_PORT@/${grpc}/g" \
		-e "s|@MEM_DIR@|${mem}|g" \
		-e "s/@NODE_ID@/${id}/g" \
		"$E2E_DIR/provider.toml" >"$cfg"
	if [[ $id -eq 1 ]]; then
		cp "$cfg" "$E2E_LOG_DIR/provider.toml"
	fi
	say "starting provider $id"
	: >"$log"
	"$ROSWAPD" "$cfg" >>"$log" 2>&1 &
	echo $! >"$pid"
	wait_for "provider $id listening on ${grpc}" 30 provider_ready "$id"
}

provider_ready() {
	local id=${1:-1}
	local pid log grpc
	pid=$(cat "$(provider_pid_file "$id")")
	log=$(provider_log_file "$id")
	grpc=$(provider_grpc_port "$id")
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "provider $id exited; see $log" >&2
		return 2
	fi
	grep -q "provider initialized" "$log" || return 1
	port_listening "$grpc"
}

freeze_provider() {
	local id=${1:-1}
	kill -STOP "$(cat "$(provider_pid_file "$id")")"
}

thaw_provider() {
	local id=${1:-1}
	kill -CONT "$(cat "$(provider_pid_file "$id")")"
}

kill_provider() {
	local id=${1:-1}
	kill -KILL "$(cat "$(provider_pid_file "$id")")" 2>/dev/null || true
}

load_module() {
	say "loading roswap.ko $*"
	modprobe -q rdma_cm 2>/dev/null || true
	insmod "$ROOT/kernel/roswap.ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB" "$@"
	[[ -b /dev/roswap0 ]] || die "/dev/roswap0 is missing after insmod"
}

kernel_ready() {
	local line chunks links synced
	line=$("$ROSWAPCTL" status 2>/dev/null) || return 1
	chunks=$(awk '{ for (i = 1; i <= NF; i++) if ($i == "chunks") print $(i + 1) }' <<<"$line")
	links=$(awk '{ for (i = 1; i <= NF; i++) if ($i == "links_up") print $(i + 1) }' <<<"$line")
	[[ ${chunks:-0} -ge ${WANT_CHUNKS:-2} && ${links:-0} -ge ${WANT_LINKS:-1} ]] || return 1
	line=$("$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}" 2>/dev/null) || return 1
	synced=$(grep -c "synced true" <<<"$line" || true)
	[[ ${synced:-0} -ge ${WANT_NODES:-1} ]]
}

status_field() {
	"$ROSWAPCTL" status | awk -v key="$1" '{
		for (i = 1; i <= NF; i++) if ($i == key) { print $(i + 1); exit }
	}'
}

node_state() {
	"$ROSWAPCTL" status | awk -v id="$1" '$1 == "node" && $2 == id { print $3; exit }'
}

node_boot() {
	"$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}" | awk -v id="$1" '
		$1 == "node" && $2 == id {
			for (i = 1; i <= NF; i++) if ($i == "boot") { print $(i + 1); exit }
		}'
}

wait_node_state() {
	local id=$1 want=$2
	[[ $(node_state "$id") == "$want" ]]
}

set_dead_after() {
	sed -i "s/^dead_after_s = .*/dead_after_s = $1/" "$E2E_LOG_DIR/controller.toml"
}

assert_dmesg_clean() {
	dmesg | awk -v mark="$E2E_DMESG_MARK" '
		index($0, mark) { seen = 1; next }
		seen && /circular locking dependency/ { bad = 1 }
		seen && /BUG:/ { bad = 1 }
		seen && /WARNING:/ { bad = 1 }
		seen && /blocked for more than/ { bad = 1 }
		END { exit bad ? 1 : 0 }
	' || die "dmesg has lockdep, BUG, WARNING, or a hung task"
}

wait_kernel() {
	wait_for "chunk applied and link up" 30 kernel_ready
	"$ROSWAPCTL" status
	"$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}"
}

bring_up() {
	local n=${E2E_PROVIDERS:-1} i
	start_controller
	load_module "$@"
	for ((i = 1; i <= n; i++)); do
		start_provider "$i"
	done
	wait_kernel
}

dmesg_count() {
	local n
	n=$(dmesg | grep -c -e "$1" || true)
	printf '%s\n' "${n:-0}"
}

write_pattern() {
	local src=$E2E_LOG_DIR/pattern
	dd if=/dev/urandom of="$src" bs=4096 count=256 status=none
	dd if="$src" of=/dev/roswap0 bs=4096 count=256 oflag=direct conv=fsync status=none
	sha256sum "$src" | awk '{ print $1 }'
}

read_hash() {
	local dst=$E2E_LOG_DIR/readback
	dd if=/dev/roswap0 of="$dst" bs=4096 count=256 iflag=direct status=none
	sha256sum "$dst" | awk '{ print $1 }'
}

cluster_rkey() {
	"$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}" | awk '/^  chunk / && !rkey { rkey = $8 } END { print rkey }'
}
