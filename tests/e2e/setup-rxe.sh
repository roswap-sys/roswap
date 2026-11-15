#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

NETDEV=roswap0
PEER=roswap1
ADDR=192.168.88.1/24

modprobe -q rdma_rxe 2>/dev/null || true
modprobe -q rdma_ucm 2>/dev/null || true
modprobe -q ib_uverbs 2>/dev/null || true
modprobe -q veth 2>/dev/null || true

ip link set lo up
if ! ip link show "$NETDEV" >/dev/null 2>&1; then
	ip link add "$NETDEV" type veth peer name "$PEER"
fi
ip link set "$NETDEV" up
ip link set "$PEER" up
ip addr replace "$ADDR" dev "$NETDEV"
if [[ ! -d /sys/class/infiniband/rxe0 ]]; then
	rdma link add rxe0 type rxe netdev "$NETDEV"
fi

for _ in $(seq 1 50); do
	if grep -qi 'ffff:c0a8:5801' /sys/class/infiniband/rxe0/ports/1/gids/* 2>/dev/null; then
		rdma link show
		exit 0
	fi
	sleep 0.1
done
rdma link show >&2 || true
echo "setup-rxe: rxe0 has no GID for 192.168.88.1" >&2
exit 1
