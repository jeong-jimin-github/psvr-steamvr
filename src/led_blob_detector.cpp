#include "led_blob_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>

namespace
{
inline int ClampI(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
}

LedDetectionResult DetectLedBlobs(const std::vector<uint8_t> &gray, int width, int height,
                                  int local_radius, int max_blobs)
{
  LedDetectionResult result;
  if (width <= 2 || height <= 2 || gray.size() < static_cast<size_t>(width) * height)
    return result;

  const int iw = width + 1;
  std::vector<uint32_t> integral(static_cast<size_t>(iw) * (height + 1), 0);
  for (int y = 0; y < height; ++y)
  {
    uint32_t row = 0;
    for (int x = 0; x < width; ++x)
    {
      row += gray[static_cast<size_t>(y) * width + x];
      integral[static_cast<size_t>(y + 1) * iw + (x + 1)] =
          integral[static_cast<size_t>(y) * iw + (x + 1)] + row;
    }
  }

  std::vector<float> excess(static_cast<size_t>(width) * height, 0.f);
  double sum = 0.0, sum2 = 0.0;
  size_t n = 0;
  for (int y = 0; y < height; ++y)
  {
    const int y0 = ClampI(y - local_radius, 0, height - 1);
    const int y1 = ClampI(y + local_radius, 0, height - 1);
    for (int x = 0; x < width; ++x)
    {
      const int x0 = ClampI(x - local_radius, 0, width - 1);
      const int x1 = ClampI(x + local_radius, 0, width - 1);
      const uint32_t box = integral[static_cast<size_t>(y1 + 1) * iw + (x1 + 1)] -
                           integral[static_cast<size_t>(y0) * iw + (x1 + 1)] -
                           integral[static_cast<size_t>(y1 + 1) * iw + x0] +
                           integral[static_cast<size_t>(y0) * iw + x0];
      const int count = (x1 - x0 + 1) * (y1 - y0 + 1);
      const float bg = static_cast<float>(box) / static_cast<float>(count);
      const float e = static_cast<float>(gray[static_cast<size_t>(y) * width + x]) - bg;
      excess[static_cast<size_t>(y) * width + x] = e;
      if (((x + y * 3) & 7) == 0)
      {
        sum += e;
        sum2 += e * e;
        ++n;
      }
    }
  }

  const double mean = n ? sum / n : 0.0;
  const double variance = n ? std::max(0.0, sum2 / n - mean * mean) : 0.0;
  result.noise_sigma = static_cast<float>(std::sqrt(variance));
  result.threshold = std::clamp(static_cast<float>(mean + result.noise_sigma * 3.2), 6.f, 42.f);

  // WMR ring LEDs should be among the brightest tiny features in the image. A low
  // fixed raw threshold allowed ordinary textured surfaces to fill the 64-blob cap.
  // Use a per-frame 99.99th-percentile luminance floor so this adapts to exposure
  // changes while keeping only the brightest ~0.01% of pixels as LED candidates.
  std::array<size_t, 256> histogram{};
  const size_t pixels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < pixels; ++i)
    ++histogram[gray[i]];
  const size_t quantile_rank = static_cast<size_t>(std::ceil(pixels * 0.9999));
  size_t cumulative = 0;
  int bright_floor = 24;
  for (int v = 0; v < 256; ++v)
  {
    cumulative += histogram[static_cast<size_t>(v)];
    if (cumulative >= quantile_rank)
    {
      bright_floor = v;
      break;
    }
  }
  result.bright_floor = std::clamp(bright_floor, 24, 220);

  std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0);
  for (size_t i = 0; i < mask.size(); ++i)
  {
    const uint8_t raw = gray[i];
    if (excess[i] >= result.threshold && raw >= result.bright_floor)
      mask[i] = 1;
  }

  std::vector<uint8_t> visited(mask.size(), 0);
  const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
  std::queue<int> q;

  for (int y = 1; y < height - 1; ++y)
  {
    for (int x = 1; x < width - 1; ++x)
    {
      const int seed = y * width + x;
      if (!mask[seed] || visited[seed])
        continue;

      visited[seed] = 1;
      q.push(seed);
      int area = 0, peak = 0;
      int min_x = x, max_x = x, min_y = y, max_y = y;
      double weight = 0.0, wx = 0.0, wy = 0.0, excess_sum = 0.0;

      while (!q.empty())
      {
        const int idx = q.front();
        q.pop();
        const int px = idx % width;
        const int py = idx / width;
        const float e = std::max(1.f, excess[idx]);
        const int raw = gray[idx];
        ++area;
        peak = std::max(peak, raw);
        min_x = std::min(min_x, px);
        max_x = std::max(max_x, px);
        min_y = std::min(min_y, py);
        max_y = std::max(max_y, py);
        weight += e;
        wx += e * px;
        wy += e * py;
        excess_sum += e;

        for (int k = 0; k < 8; ++k)
        {
          const int nx = px + dx[k], ny = py + dy[k];
          if (nx < 0 || nx >= width || ny < 0 || ny >= height)
            continue;
          const int ni = ny * width + nx;
          if (mask[ni] && !visited[ni])
          {
            visited[ni] = 1;
            q.push(ni);
          }
        }
      }

      const int bw = max_x - min_x + 1;
      const int bh = max_y - min_y + 1;
      if (area < 2 || area > 450 || bw > 42 || bh > 42 ||
          peak < result.bright_floor || weight <= 0.0)
        continue;

      LedBlob b;
      b.x = static_cast<float>(wx / weight);
      b.y = static_cast<float>(wy / weight);
      b.area = area;
      b.peak = peak;
      b.mean_excess = static_cast<float>(excess_sum / area);
      const float compactness = static_cast<float>(area) / static_cast<float>(std::max(1, bw * bh));
      b.score = b.mean_excess * std::sqrt(static_cast<float>(area)) * (0.4f + 0.6f * compactness);
      result.blobs.push_back(b);
    }
  }

  std::sort(result.blobs.begin(), result.blobs.end(), [](const LedBlob &a, const LedBlob &b) {
    return a.score > b.score;
  });
  if (static_cast<int>(result.blobs.size()) > max_blobs)
    result.blobs.resize(max_blobs);
  return result;
}
