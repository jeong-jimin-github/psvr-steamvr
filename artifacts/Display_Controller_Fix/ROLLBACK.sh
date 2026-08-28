#!/usr/bin/env bash
set -euo pipefail
root="$1"
here="$(cd "$(dirname "$0")" && pwd)"
cp "$here/original/display_component.cpp" "$root/src/display_component.cpp"
cp "$here/original/display_component.h" "$root/src/display_component.h"
cp "$here/original/hmd_device.cpp" "$root/src/hmd_device.cpp"
cp "$here/original/gearvr_ble.cpp" "$root/src/gearvr_ble.cpp"
echo "RESTORED:$root"