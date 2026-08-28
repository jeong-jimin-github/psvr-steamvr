#pragma once

#include "madgwick.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GearVrState
{
  bool connected = false;
  std::string name;
  std::string address;
  bool trigger = false;
  bool home = false;
  bool back = false;
  bool touch_click = false;
  bool vol_up = false;
  bool vol_down = false;
  bool touch_active = false;
  float touch_x = 0.f; // -1..1
  float touch_y = 0.f;
  Quaternion rotation{};
  uint32_t packets = 0;
};

class GearVrBle
{
public:
  bool Start();
  void Stop();
  bool IsConnected() const { return connected_.load(); }
  GearVrState GetState() const;
  void Recenter();
  static std::vector<std::string> Scan(int timeout_ms);

private:
  void ThreadMain();
  void ConnectionAttempt();
  void HandlePacket(const uint8_t *data, int size);

  std::atomic<bool> run_{false};
  std::atomic<bool> connected_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  GearVrState state_{};
  MadgwickAhrs ahrs_;
  Quaternion recenter_{};
  bool have_sample_ = false;
};
