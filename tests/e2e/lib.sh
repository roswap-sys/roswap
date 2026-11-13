# SPDX-License-Identifier: GPL-2.0
# 端到端场景的公共函数。由 run.sh source，不要直接执行。

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
# 避开 roswapd 的默认端口，本机已有守护进程时也能跑。
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
	[[ -e $1 ]] || die "找不到 $1"
}

e2e_rdma_dev() {
	if [[ -n ${ROSWAP_E2E_DEV:-} ]]; then
		printf '%s\n' "$ROSWAP_E2E_DEV"
		return
	fi
	[[ -d /sys/class/infiniband/rxe0 ]] || die "没有 rxe0。先加载 rdma_rxe 并 rdma link add，或设置 ROSWAP_E2E_DEV"
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
	[[ -n $netdev ]] || die "rdma link show 里没有 $dev"
	ip=$(ip -4 -o addr show dev "$netdev" | awk 'NR == 1 { print $4 }')
	ip=${ip%%/*}
	[[ -n $ip ]] || die "$dev 的网卡 $netdev 没有 IPv4"
	printf '%s\n' "$ip"
}

render_config() {
	local dev=$1 ip=$2

	sed -e "s/@RDMA_DEV@/${dev}/g" -e "s/@RDMA_IP@/${ip}/g" \
		"$E2E_DIR/provider.toml" >"$E2E_LOG_DIR/provider.toml"
	cp "$E2E_DIR/controller.toml" "$E2E_LOG_DIR/controller.toml"
}

wait_for() {
	local desc=$1 timeout=$2
	local start=$SECONDS rc
	shift 2
	until "$@"; do
		rc=$?
		if (( rc >= 2 )); then
			printf 'e2e: %s 失败\n' "$desc" >&2
			return "$rc"
		fi
		if (( SECONDS - start >= timeout )); then
			printf 'e2e: 等待超时: %s\n' "$desc" >&2
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

e2e_stop_stack() {
	stop_io_loop || true
	swapoff /dev/roswap0 2>/dev/null || true
	rmdir /sys/fs/cgroup/roswap-e2e 2>/dev/null || true
	if [[ -d /sys/module/roswap ]]; then
		rmmod roswap || true
	fi
	stop_pid_file "$E2E_LOG_DIR/provider.pid"
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
		tail -n 80 "$E2E_LOG_DIR/provider.log" >&2 || true
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
		say "dmesg -w 不可用，失败时改看 dmesg 快照"
	fi
}

e2e_init() {
	local dev ip

	[[ $(id -u) -eq 0 ]] || die "需要 root（加载模块、配置 swap）"
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
	trap e2e_on_exit EXIT
	trap 'exit 143' INT TERM
}

start_controller() {
	say "启动 controller"
	"$ROSWAPD" "$E2E_LOG_DIR/controller.toml" >>"$E2E_LOG_DIR/controller.log" 2>&1 &
	echo $! >"$E2E_LOG_DIR/controller.pid"
	wait_for "controller 监听 ${E2E_CTRL_PORT}" 20 controller_ready
}

controller_ready() {
	local pid
	pid=$(cat "$E2E_LOG_DIR/controller.pid")
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "controller 进程已退出，见 $E2E_LOG_DIR/controller.log" >&2
		return 2
	fi
	port_listening "$E2E_CTRL_PORT"
}

# 脚本开了 pipefail，不要写 "cmd | grep -q"：grep 提前退出时 cmd 会被 SIGPIPE 打断，
# 整条管道按失败算。
port_listening() {
	[[ -n $(ss -Hltn "sport = :$1") ]]
}

start_provider() {
	say "启动 provider"
	"$ROSWAPD" "$E2E_LOG_DIR/provider.toml" >>"$E2E_LOG_DIR/provider.log" 2>&1 &
	echo $! >"$E2E_LOG_DIR/provider.pid"
	wait_for "provider 监听 ${E2E_GRPC_PORT}" 30 provider_ready
}

provider_ready() {
	local pid
	pid=$(cat "$E2E_LOG_DIR/provider.pid")
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "provider 进程已退出，见 $E2E_LOG_DIR/provider.log" >&2
		return 2
	fi
	grep -q "provider initialized" "$E2E_LOG_DIR/provider.log" || return 1
	port_listening "$E2E_GRPC_PORT"
}

load_module() {
	say "加载 roswap.ko"
	# insmod 不处理依赖。rxe 只带起 ib_core，rdma_resolve_addr 这些符号在 rdma_cm 里。
	# 测试虚拟机把 rdma_cm 编进了内核，modprobe 找不到模块是正常的。
	modprobe -q rdma_cm 2>/dev/null || true
	insmod "$ROOT/kernel/roswap.ko" chunk_size_mb="$CHUNK_MB" capacity_mb="$CAPACITY_MB"
	[[ -b /dev/roswap0 ]] || die "insmod 之后没有 /dev/roswap0"
}

kernel_ready() {
	local line chunks links
	line=$("$ROSWAPCTL" status 2>/dev/null) || return 1
	chunks=$(awk '{ for (i = 1; i <= NF; i++) if ($i == "chunks") print $(i + 1) }' <<<"$line")
	links=$(awk '{ for (i = 1; i <= NF; i++) if ($i == "links_up") print $(i + 1) }' <<<"$line")
	[[ ${chunks:-0} -ge 2 && ${links:-0} -ge 1 ]] || return 1
	line=$("$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}" 2>/dev/null) || return 1
	[[ $line == *"synced true"* ]]
}

wait_kernel() {
	wait_for "chunk 下发并且链路 up" 30 kernel_ready
	"$ROSWAPCTL" status
	"$ROSWAPCTL" cluster "127.0.0.1:${E2E_CTRL_PORT}"
}

bring_up() {
	start_controller
	load_module
	start_provider
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
