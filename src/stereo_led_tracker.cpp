#include "stereo_led_tracker.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
double Dist3(const StereoLedPoint &a, const StereoLedPoint &b)
{
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  const double dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool ParseDouble(const std::string &line, const char *key, double &dst)
{
  const std::string prefix = std::string(key) + "=";
  if (line.rfind(prefix, 0) != 0)
    return false;
  try
  {
    dst = std::stod(line.substr(prefix.size()));
    return true;
  }
  catch (...)
  {
    return false;
  }
}
}

bool StereoCalibration::Load(const std::string &path)
{
  std::ifstream f(path);
  if (!f)
    return false;
  std::string line;
  while (std::getline(f, line))
  {
    ParseDouble(line, "fx", fx) || ParseDouble(line, "fy", fy) ||
        ParseDouble(line, "cx", cx) || ParseDouble(line, "cy", cy) ||
        ParseDouble(line, "baseline_m", baseline_m) || ParseDouble(line, "epipolar_px", epipolar_px) ||
        ParseDouble(line, "min_depth_m", min_depth_m) || ParseDouble(line, "max_depth_m", max_depth_m);
  }
  return fx > 0.0 && fy > 0.0 && baseline_m > 0.0;
}

bool StereoCalibration::SaveTemplate(const std::string &path, int eye_width, int eye_height) const
{
  StereoCalibration c = *this;
  c.FillFallback(eye_width, eye_height);
  std::ofstream f(path);
  if (!f)
    return false;
  f << "# PSVR camera stereo calibration. Replace fallback values with measured calibration for accurate meters.\n";
  f << "fx=" << c.fx << "\n";
  f << "fy=" << c.fy << "\n";
  f << "cx=" << c.cx << "\n";
  f << "cy=" << c.cy << "\n";
  f << "baseline_m=" << c.baseline_m << "\n";
  f << "epipolar_px=" << c.epipolar_px << "\n";
  f << "min_depth_m=" << c.min_depth_m << "\n";
  f << "max_depth_m=" << c.max_depth_m << "\n";
  return true;
}

void StereoCalibration::FillFallback(int eye_width, int eye_height)
{
  if (fx <= 0.0)
  {
    // The official PS Camera FOV is about 85 degrees. This pinhole estimate is only
    // for motion/geometry smoke tests until a per-camera calibration file is supplied.
    const double half_fov_rad = 85.0 * 3.14159265358979323846 / 360.0;
    fx = (eye_width * 0.5) / std::tan(half_fov_rad);
  }
  if (fy <= 0.0)
    fy = fx;
  if (cx <= 0.0)
    cx = (eye_width - 1) * 0.5;
  if (cy <= 0.0)
    cy = (eye_height - 1) * 0.5;
  if (baseline_m <= 0.0)
    baseline_m = 0.085;
}

bool SplitSideBySide(const PsvrCameraFrame &frame, PsvrCameraFrame &left, PsvrCameraFrame &right)
{
  if (frame.width < 2 || (frame.width % 2) != 0 || frame.height <= 0 ||
      frame.gray.size() < static_cast<size_t>(frame.width) * frame.height)
    return false;

  const int eye_w = frame.width / 2;
  left.width = right.width = eye_w;
  left.height = right.height = frame.height;
  left.timestamp_100ns = right.timestamp_100ns = frame.timestamp_100ns;
  left.gray.resize(static_cast<size_t>(eye_w) * frame.height);
  right.gray.resize(static_cast<size_t>(eye_w) * frame.height);
  for (int y = 0; y < frame.height; ++y)
  {
    const uint8_t *src = frame.gray.data() + static_cast<size_t>(y) * frame.width;
    std::copy(src, src + eye_w, left.gray.begin() + static_cast<size_t>(y) * eye_w);
    std::copy(src + eye_w, src + frame.width, right.gray.begin() + static_cast<size_t>(y) * eye_w);
  }
  return true;
}

StereoLedTrackingResult TrackStereoLeds(const PsvrCameraFrame &left, const PsvrCameraFrame &right,
                                        StereoCalibration calibration)
{
  StereoLedTrackingResult out;
  if (left.width <= 0 || left.height <= 0 || right.width != left.width || right.height != left.height)
    return out;

  calibration.FillFallback(left.width, left.height);
  out.left_detection = DetectLedBlobs(left.gray, left.width, left.height);
  out.right_detection = DetectLedBlobs(right.gray, right.width, right.height);

  struct Candidate
  {
    int li = -1;
    int ri = -1;
    double cost = 0.0;
  };
  std::vector<Candidate> candidates;
  for (int li = 0; li < static_cast<int>(out.left_detection.blobs.size()); ++li)
  {
    const auto &l = out.left_detection.blobs[li];
    for (int ri = 0; ri < static_cast<int>(out.right_detection.blobs.size()); ++ri)
    {
      const auto &r = out.right_detection.blobs[ri];
      const double dy = std::fabs(l.y - r.y);
      const double disparity = l.x - r.x;
      if (dy > calibration.epipolar_px || disparity <= 0.75)
        continue;
      const double z = calibration.fx * calibration.baseline_m / disparity;
      if (z < calibration.min_depth_m || z > calibration.max_depth_m)
        continue;
      const double score_ratio = std::max(l.score, r.score) / std::max(1.f, std::min(l.score, r.score));
      if (score_ratio > 4.0)
        continue;
      const double area_ratio = static_cast<double>(std::max(l.area, r.area)) /
                                std::max(1, std::min(l.area, r.area));
      if (area_ratio > 5.0)
        continue;
      candidates.push_back({li, ri, dy + std::fabs(std::log(score_ratio)) * 2.0 +
                                        std::fabs(std::log(area_ratio))});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.cost < b.cost;
  });

  std::vector<uint8_t> used_l(out.left_detection.blobs.size(), 0);
  std::vector<uint8_t> used_r(out.right_detection.blobs.size(), 0);
  for (const auto &c : candidates)
  {
    if (used_l[c.li] || used_r[c.ri])
      continue;
    const auto &l = out.left_detection.blobs[c.li];
    const auto &r = out.right_detection.blobs[c.ri];
    const double disparity = l.x - r.x;
    const double z = calibration.fx * calibration.baseline_m / disparity;
    StereoLedPoint p;
    p.z = z;
    p.x = ((l.x - calibration.cx) * z / calibration.fx) - calibration.baseline_m * 0.5;
    p.y = -(l.y - calibration.cy) * z / calibration.fy;
    p.left = l;
    p.right = r;
    const float image_quality = std::min(1.f, (l.mean_excess + r.mean_excess) / 80.f);
    const float epi_quality = static_cast<float>(std::max(0.0, 1.0 - std::fabs(l.y - r.y) /
                                                                   std::max(1.0, calibration.epipolar_px)));
    p.confidence = std::clamp(0.25f + image_quality * 0.45f + epi_quality * 0.30f, 0.f, 1.f);
    out.points.push_back(p);
    used_l[c.li] = 1;
    used_r[c.ri] = 1;
  }

  // The Odyssey ring is compact relative to arm-scale separation. Build up to two
  // spatial clusters and reject constellations whose reconstructed LEDs spread too far.
  struct WorkCluster
  {
    std::vector<int> indices;
    double x = 0.0, y = 0.0, z = 0.0;
  };
  std::vector<WorkCluster> clusters;
  std::vector<int> order(out.points.size());
  for (int i = 0; i < static_cast<int>(order.size()); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](int a, int b) { return out.points[a].confidence > out.points[b].confidence; });

  for (int idx : order)
  {
    const auto &p = out.points[idx];
    int best = -1;
    double best_d = 1e9;
    for (int ci = 0; ci < static_cast<int>(clusters.size()); ++ci)
    {
      StereoLedPoint centroid;
      centroid.x = clusters[ci].x;
      centroid.y = clusters[ci].y;
      centroid.z = clusters[ci].z;
      const double d = Dist3(p, centroid);
      if (d < best_d)
      {
        best_d = d;
        best = ci;
      }
    }
    if (best < 0 || best_d > 0.20)
    {
      if (clusters.size() >= 2)
        continue;
      clusters.push_back({{idx}, p.x, p.y, p.z});
    }
    else
    {
      auto &cl = clusters[best];
      cl.indices.push_back(idx);
      double sw = 0.0, sx = 0.0, sy = 0.0, sz = 0.0;
      for (int pi : cl.indices)
      {
        const double w = std::max(0.05f, out.points[pi].confidence);
        sw += w;
        sx += out.points[pi].x * w;
        sy += out.points[pi].y * w;
        sz += out.points[pi].z * w;
      }
      cl.x = sx / sw;
      cl.y = sy / sw;
      cl.z = sz / sw;
    }
  }

  for (const auto &cl : clusters)
  {
    ControllerOpticalCluster c;
    c.x = cl.x;
    c.y = cl.y;
    c.z = cl.z;
    c.visible_leds = static_cast<int>(cl.indices.size());
    double sum_sq = 0.0;
    double conf = 0.0;
    for (int pi : cl.indices)
    {
      const double dx = out.points[pi].x - c.x;
      const double dy = out.points[pi].y - c.y;
      const double dz = out.points[pi].z - c.z;
      sum_sq += dx * dx + dy * dy + dz * dz;
      conf += out.points[pi].confidence;
    }
    c.rms_radius_m = cl.indices.empty() ? 0.0 : std::sqrt(sum_sq / cl.indices.size());
    const double avg_conf = cl.indices.empty() ? 0.0 : conf / cl.indices.size();
    const double count_factor = std::min(1.0, cl.indices.size() / 5.0);
    const double geometry_factor = std::clamp(1.0 - c.rms_radius_m / 0.14, 0.0, 1.0);
    c.confidence = static_cast<float>(avg_conf * (0.35 + 0.65 * count_factor) * geometry_factor);
    c.valid = c.visible_leds >= 2 && c.rms_radius_m <= 0.14 && c.confidence >= 0.20f;
    out.controllers.push_back(c);
  }

  std::sort(out.controllers.begin(), out.controllers.end(), [](const ControllerOpticalCluster &a,
                                                               const ControllerOpticalCluster &b) {
    return a.x < b.x;
  });
  return out;
}
