#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="${1:-C:/Users/jm/Documents/psvr-steamvr}"
mkdir -p "$target/src"
cp -f "$script_dir/original/wmr_hid.cpp" "$target/src/wmr_hid.cpp"
cp -f "$script_dir/original/wmr_hid.h" "$target/src/wmr_hid.h"
echo "RESTORED:$target"
