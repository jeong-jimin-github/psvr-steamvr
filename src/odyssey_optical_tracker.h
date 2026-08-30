#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

// Thread-safe handoff between the camera/constellation tracker and the
// SteamVR controller device. Camera code should submit positions in the same
// driver/world coordinate system used by vr::DriverPose_t.
struct OdysseyOpticalMeasurement
{
  bool valid = false;
  double position[3] = {0.0, 0.0, 0.0};
  float confidence = 0.0f;
  uint32_t visible_leds = 0;
  std::chrono::steady_clock::time_point timestamp{};
};

class OdysseyOpticalTracker
{
public:
  explicit OdysseyOpticalTracker(bool left) : left_(left) {}

  void SubmitPosition(double x, double y, double z, float confidence, uint32_t visible_leds)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    measurement_.valid = true;
    measurement_.position[0] = x;
    measurement_.position[1] = y;
    measurement_.position[2] = z;
    measurement_.confidence = confidence;
    measurement_.visible_leds = visible_leds;
    measurement_.timestamp = std::chrono::steady_clock::now();
  }

  void Invalidate()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    measurement_.valid = false;
    measurement_.confidence = 0.0f;
    measurement_.visible_leds = 0;
  }

  OdysseyOpticalMeasurement GetMeasurement() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    OdysseyOpticalMeasurement out = measurement_;
    if (out.valid)
    {
      // Do not let an old optical correction pin the hand in space after the
      // constellation leaves the PSVR camera FOV. The later fusion stage can
      // replace this hard timeout with prediction/covariance growth.
      const auto age = std::chrono::steady_clock::now() - out.timestamp;
      if (age > std::chrono::milliseconds(250))
      {
        out.valid = false;
        out.confidence = 0.0f;
        out.visible_leds = 0;
      }
    }
    return out;
  }

  bool IsLeft() const { return left_; }

private:
  bool left_ = false;
  mutable std::mutex mutex_;
  OdysseyOpticalMeasurement measurement_{};
};
