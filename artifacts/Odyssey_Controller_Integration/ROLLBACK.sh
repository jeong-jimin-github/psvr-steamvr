#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-C:/Users/jm/Documents/psvr-steamvr}"
ART="C:/Users/jm/Documents/psvr-steamvr/artifacts/Odyssey_Controller_Integration"
cp "$ART/original/CMakeLists.txt" "$ROOT/CMakeLists.txt"
cp "$ART/original/device_provider.cpp" "$ROOT/src/device_provider.cpp"
cp "$ART/original/device_provider.h" "$ROOT/src/device_provider.h"
rm -f "$ROOT/src/wmr_hid.h" "$ROOT/src/wmr_hid.cpp" "$ROOT/src/wmr_controller_device.h" "$ROOT/src/wmr_controller_device.cpp" "$ROOT/driver/resources/input/odyssey_controller_profile.json"
echo "ROLLBACK_RESULT=restored GearVR provider branch; removed Odyssey WMR Raw Input branch"
echo "ROLLBACK_ROOT=$ROOT"
