#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# 准备一棵指定版本的内核树，用来编译 roswap.ko 或在虚拟机里启动。
#
#   kernel-tree.sh resolve <系列>          打印 "<版本> <源码地址>"
#   kernel-tree.sh prepare <系列> <目录>   defconfig + modules_prepare，只够编译外部模块
#   kernel-tree.sh build   <系列> <目录>   用 virtme-ng 编出可启动的内核
#
# 系列可以是 5.10、6.12 这样的 longterm 分支，也可以是 stable（最新稳定版）
# 或 mainline（最新 rc）。在 GitHub Actions 里会把 version=<版本> 写进 GITHUB_OUTPUT。

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
FRAGMENT=$ROOT/tests/e2e/kernel.config
CACHE=${KERNEL_TARBALL_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/roswap-kernel}

die() {
	printf 'kernel-tree: %s\n' "$*" >&2
	exit 1
}

fetch() {
	curl -fsSL --retry 5 --retry-delay 5 --connect-timeout 30 "$@"
}

# kernel.org 的 releases.json 只列还在维护的分支。分支 EOL 之后从 CDN 目录里找最后一个补丁版本。
resolve() {
	local series=$1 json line major listing patch

	json=$(fetch https://www.kernel.org/releases.json)
	line=$(SERIES="$series" python3 -c '
import json, os, sys
series = os.environ["SERIES"]
for r in json.load(sys.stdin)["releases"]:
    v, m = r["version"], r["moniker"]
    if series in ("stable", "mainline"):
        hit = m == series
    else:
        hit = m in ("longterm", "stable") and (v == series or v.startswith(series + "."))
    if hit and r.get("source"):
        print(v, r["source"])
        break
' <<<"$json")
	if [[ -n $line ]]; then
		printf '%s\n' "$line"
		return
	fi
	[[ $series =~ ^[0-9]+\.[0-9]+$ ]] || die "releases.json 里没有 $series"
	major=${series%%.*}
	listing=$(fetch "https://cdn.kernel.org/pub/linux/kernel/v${major}.x/")
	patch=$(grep -oE "linux-${series//./\\.}\.[0-9]+\.tar\.xz" <<<"$listing" |
		sed -E 's/.*\.([0-9]+)\.tar\.xz/\1/' | sort -n | tail -n1 || true)
	if [[ -n $patch ]]; then
		printf '%s.%s %s\n' "$series" "$patch" \
			"https://cdn.kernel.org/pub/linux/kernel/v${major}.x/linux-${series}.${patch}.tar.xz"
		return
	fi
	grep -q "linux-${series}.tar.xz" <<<"$listing" || die "找不到 $series 的源码包"
	printf '%s %s\n' "$series" "https://cdn.kernel.org/pub/linux/kernel/v${major}.x/linux-${series}.tar.xz"
}

publish_version() {
	if [[ -n ${GITHUB_OUTPUT:-} ]]; then
		echo "version=$1" >>"$GITHUB_OUTPUT"
	fi
}

unpack() {
	local version=$1 url=$2 dir=$3 tarball

	mkdir -p "$CACHE"
	tarball=$CACHE/$(basename "$url")
	if [[ ! -s $tarball ]]; then
		echo "下载 $url"
		fetch -o "$tarball.part" "$url"
		mv "$tarball.part" "$tarball"
	fi
	rm -rf "$dir"
	mkdir -p "$dir"
	tar -xf "$tarball" -C "$dir" --strip-components=1
	echo "$version" >"$dir/.roswap-version"
}

# KMAKE_ARGS 用来给老内核换编译器，例如 "CC=gcc-11 HOSTCC=gcc-11"。
kmake() {
	local dir=$1
	shift
	# shellcheck disable=SC2086
	make -C "$dir" ${KMAKE_ARGS:-} "$@"
}

merge_fragment() {
	local dir=$1
	(cd "$dir" && scripts/kconfig/merge_config.sh -m .config "$FRAGMENT" >/dev/null)
	kmake "$dir" olddefconfig >/dev/null
	require_options "$dir"
}

# 依赖不满足时 olddefconfig 会把选项悄悄关掉，这里把关键的几个再查一遍。
require_options() {
	local dir=$1 opt
	for opt in MODULES INFINIBAND INFINIBAND_ADDR_TRANS RDMA_RXE SWAP; do
		grep -q "^CONFIG_${opt}=[ym]" "$dir/.config" || die "$dir/.config 里没有打开 CONFIG_${opt}"
	done
}

cmd_prepare() {
	local series=$1 dir=$2 version url
	read -r version url < <(resolve "$series")
	publish_version "$version"
	unpack "$version" "$url" "$dir"
	kmake "$dir" defconfig >/dev/null
	merge_fragment "$dir"
	kmake "$dir" -j"$(nproc)" modules_prepare
	echo "内核树 $dir 已就绪（$version，仅头文件）"
}

cmd_build() {
	local series=$1 dir=$2 version url
	command -v vng >/dev/null || die "需要 virtme-ng（vng）"
	read -r version url < <(resolve "$series")
	publish_version "$version"
	if [[ -f $dir/.roswap-built && $(cat "$dir/.roswap-version" 2>/dev/null) == "$version" ]]; then
		echo "复用缓存的 $version"
		return
	fi
	unpack "$version" "$url" "$dir"
	(cd "$dir" && vng --kconfig)
	merge_fragment "$dir"
	kmake "$dir" -j"$(nproc)"
	# 外部模块只需要头文件、脚本和 Module.symvers。去掉目标文件，缓存小一半以上。
	find "$dir" -name '*.o' -delete
	rm -f "$dir"/vmlinux.o "$dir"/.tmp_vmlinux*
	touch "$dir/.roswap-built"
	echo "内核 $version 已编好：$dir"
}

main() {
	local cmd=${1:-}
	case $cmd in
	resolve)
		[[ $# -eq 2 ]] || die "用法: $0 resolve <系列>"
		resolve "$2"
		;;
	prepare)
		[[ $# -eq 3 ]] || die "用法: $0 prepare <系列> <目录>"
		cmd_prepare "$2" "$3"
		;;
	build)
		[[ $# -eq 3 ]] || die "用法: $0 build <系列> <目录>"
		cmd_build "$2" "$3"
		;;
	*)
		die "用法: $0 resolve|prepare|build ..."
		;;
	esac
}

main "$@"
