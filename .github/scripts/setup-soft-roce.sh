#!/bin/bash
set -euo pipefail

if ! modinfo rdma_rxe >/dev/null 2>&1; then
  bash "$(dirname "$0")/apt-install.sh" "linux-modules-extra-$(uname -r)"
fi
sudo modprobe rdma_rxe
sudo "$(dirname "$0")/../../tests/e2e/setup-rxe.sh"
