#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:?usage: ROLLBACK.sh TARGET_COPY_ROOT}"
[[ -d "$TARGET/src" ]] || { echo "ERROR: target must contain src: $TARGET" >&2; exit 2; }
cp "$ROOT/original/controller_device.cpp" "$TARGET/src/controller_device.cpp"
cp "$ROOT/original/controller_device.h" "$TARGET/src/controller_device.h"
cp "$ROOT/original/gearvr_ble.cpp" "$TARGET/src/gearvr_ble.cpp"
cp "$ROOT/original/gearvr_ble.h" "$TARGET/src/gearvr_ble.h"
cp "$ROOT/original/device_provider.cpp" "$TARGET/src/device_provider.cpp"
cp "$ROOT/original/device_provider.h" "$TARGET/src/device_provider.h"
echo "RESTORED:$TARGET"