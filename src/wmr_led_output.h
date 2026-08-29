#pragma once

#include <string>

// Best-effort output-only path. Windows commonly denies GENERIC_WRITE for paired
// Bluetooth WMR controllers; failure is non-fatal and Raw Input tracking continues.
bool TrySetOdysseyLedIntensity(bool left, int intensity, std::string &status);
