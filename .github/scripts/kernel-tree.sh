#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

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
	[[ $series =~ ^[0-9]+\.[0-9]+$ ]] || die "releases.json has no entry for $series"
	major=${series%%.*}
	listing=$(fetch "https://cdn.kernel.org/pub/linux/kernel/v${major}.x/")
	patch=$(grep -oE "linux-${series//./\\.}\.[0-9]+\.tar\.xz" <<<"$listing" |
		sed -E 's/.*\.([0-9]+)\.tar\.xz/\1/' | sort -n | tail -n1 || true)
	if [[ -n $patch ]]; then
		printf '%s.%s %s\n' "$series" "$patch" \
			"https://cdn.kernel.org/pub/linux/kernel/v${major}.x/linux-${series}.${patch}.tar.xz"
		return
	fi
	grep -q "linux-${series}.tar.xz" <<<"$listing" || die "no source tarball for $series"
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
		echo "downloading $url"
		fetch -o "$tarball.part" "$url"
		mv "$tarball.part" "$tarball"
	fi
	rm -rf "$dir"
	mkdir -p "$dir"
	tar -xf "$tarball" -C "$dir" --strip-components=1
	echo "$version" >"$dir/.roswap-version"
}

kmake() {
	local dir=$1
	shift
	make -C "$dir" ${KMAKE_ARGS:-} "$@"
}

merge_fragment() {
	local dir=$1
	(cd "$dir" && scripts/kconfig/merge_config.sh -m .config "$FRAGMENT" >/dev/null)
	kmake "$dir" olddefconfig >/dev/null
	require_options "$dir"
}

require_options() {
	local dir=$1 opt
	for opt in MODULES INFINIBAND INFINIBAND_ADDR_TRANS RDMA_RXE SWAP PROVE_LOCKING DEBUG_ATOMIC_SLEEP; do
		grep -q "^CONFIG_${opt}=[ym]" "$dir/.config" || die "$dir/.config does not enable CONFIG_${opt}"
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
	echo "kernel tree $dir ready ($version, headers only)"
}

cmd_build() {
	local series=$1 dir=$2 version url
	command -v vng >/dev/null || die "virtme-ng (vng) is required"
	read -r version url < <(resolve "$series")
	publish_version "$version"
	if [[ -f $dir/.roswap-built && $(cat "$dir/.roswap-version" 2>/dev/null) == "$version" ]]; then
		echo "reusing cached $version"
		return
	fi
	unpack "$version" "$url" "$dir"
	(cd "$dir" && vng --kconfig)
	merge_fragment "$dir"
	kmake "$dir" -j"$(nproc)"
	find "$dir" -name '*.o' -delete
	rm -f "$dir"/vmlinux.o "$dir"/.tmp_vmlinux*
	touch "$dir/.roswap-built"
	echo "kernel $version built: $dir"
}

main() {
	local cmd=${1:-}
	case $cmd in
	resolve)
		[[ $# -eq 2 ]] || die "usage: $0 resolve <series>"
		resolve "$2"
		;;
	prepare)
		[[ $# -eq 3 ]] || die "usage: $0 prepare <series> <dir>"
		cmd_prepare "$2" "$3"
		;;
	build)
		[[ $# -eq 3 ]] || die "usage: $0 build <series> <dir>"
		cmd_build "$2" "$3"
		;;
	*)
		die "usage: $0 resolve|prepare|build ..."
		;;
	esac
}

main "$@"
