#pragma once

#include <cstdint>
#include <vector>

struct LedBlob
{
  float x = 0.f;
  float y = 0.f;
  int area = 0;
  int peak = 0;
  float mean_excess = 0.f;
  float score = 0.f;
};

struct LedDetectionResult
{
  std::vector<LedBlob> blobs;
  float threshold = 0.f;
  float noise_sigma = 0.f;
};

LedDetectionResult DetectLedBlobs(const std::vector<uint8_t> &gray, int width, int height,
                                  int local_radius = 7, int max_blobs = 64);
