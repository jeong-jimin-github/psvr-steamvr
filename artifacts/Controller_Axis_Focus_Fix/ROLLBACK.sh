#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:?usage: ROLLBACK.sh TARGET_COPY_ROOT}"
[[ -d "$TARGET/src" ]] || { echo "ERROR: target must contain src: $TARGET" >&2; exit 2; }
cp "$ROOT/original/wmr_hid.cpp" "$TARGET/src/wmr_hid.cpp"
cp "$ROOT/original/display_component.cpp" "$TARGET/src/display_component.cpp"
cp "$ROOT/original/display_component.h" "$TARGET/src/display_component.h"
cp "$ROOT/original/hmd_device.cpp" "$TARGET/src/hmd_device.cpp"
echo "RESTORED:$TARGET"