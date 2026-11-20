#!/bin/bash
set -euo pipefail

if ! modinfo rdma_rxe >/dev/null 2>&1; then
  bash "$(dirname "$0")/apt-install.sh" "linux-modules-extra-$(uname -r)"
fi
sudo modprobe rdma_rxe
root=$(cd "$(dirname "$0")/../.." && pwd)
sudo python3 "$root/tests/e2e/run.py" --setup-rxe
