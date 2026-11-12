#!/bin/bash
# Ubuntu 官方源（azure/archive/security.ubuntu.com）解析失败时，改走独立镜像再装一次。
set -euo pipefail

apt_get() {
  sudo apt-get \
    -o Acquire::Retries=2 \
    -o Acquire::http::Timeout=60 \
    -o Acquire::https::Timeout=60 \
    "$@"
}

use_fallback_mirrors() {
  local mirror="http://mirrors.edge.kernel.org/ubuntu"
  if [[ -f /etc/apt/apt-mirrors.txt ]]; then
    printf '%s\n' \
      "${mirror}/" \
      "http://mirrors.mit.edu/ubuntu/" \
      "http://ftp.osuosl.org/pub/ubuntu/" \
      | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
  fi
  sudo find /etc/apt -type f \( -name sources.list -o -name '*.list' -o -name '*.sources' \) -print0 \
    | sudo xargs -0 -r sed -i \
      -e "s|https\\?://azure.archive.ubuntu.com/ubuntu|${mirror}|g" \
      -e "s|https\\?://archive.ubuntu.com/ubuntu|${mirror}|g" \
      -e "s|https\\?://security.ubuntu.com/ubuntu|${mirror}|g"
}

if ! apt_get update || ! apt_get install -y "$@"; then
  echo "apt failed; retrying via mirrors.edge.kernel.org" >&2
  use_fallback_mirrors
  apt_get update
  apt_get install -y "$@"
fi
