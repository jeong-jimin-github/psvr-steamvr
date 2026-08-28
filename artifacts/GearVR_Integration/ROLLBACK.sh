#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
  echo "usage: $0 <workspace-root>" >&2
  exit 64
fi
artifact_dir="$(cd "$(dirname "$0")" && pwd)"
root="$1"
mkdir -p "$root/src" "$root/scripts"
cp "$artifact_dir/ORIGINAL_FILE.cpp" "$root/src/gearvr_ble.cpp"
cp "$artifact_dir/original_tree/src/gearvr_ble.h" "$root/src/gearvr_ble.h"
cp "$artifact_dir/original_tree/src/psvr_ctl.cpp" "$root/src/psvr_ctl.cpp"
cp "$artifact_dir/original_tree/scripts/build.ps1" "$root/scripts/build.ps1"
cp "$artifact_dir/original_tree/scripts/install.ps1" "$root/scripts/install.ps1"
cp "$artifact_dir/original_tree/README.md" "$root/README.md"
cp "$artifact_dir/original_tree/TODO.txt" "$root/TODO.txt"
echo "RESTORED:$root"
