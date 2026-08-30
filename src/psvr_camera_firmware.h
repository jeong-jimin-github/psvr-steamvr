#pragma once

#include <string>

bool IsPs4CameraBootPresent(std::string &status);
bool UploadPs4CameraFirmware(const std::string &path, std::string &status);
