#pragma once

#include "madgwick.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct WmrControllerState
{
  bool connected = false;
  bool left = false;
  bool menu = false;
  bool home = false;
  bool squeeze = false;
  bool thumbstick_click = false;
  bool trackpad_click = false;
  bool trackpad_touch = false;
  float trigger = 0.f;
  float thumbstick_x = 0.f;
  float thumbstick_y = 0.f;
  float trackpad_x = 0.f;
  float trackpad_y = 0.f;
  uint8_t battery = 0;
  Quaternion rotation{};
  uint32_t packets = 0;
  std::string product;
  std::string serial;
};

class WmrHidController
{
public:
  explicit WmrHidController(bool left);
  ~WmrHidController();

  bool Start();
  void Stop();
  void Recenter(const Quaternion &hmd_rotation);
  WmrControllerState GetState() const;

private:
  static void RawInputThreadMain();
  void ThreadMain();
  bool ConnectionAttempt();
  void HandleReport(const uint8_t *data, int size);

  static std::mutex registry_mutex_;
  static WmrHidController *left_instance_;
  static WmrHidController *right_instance_;
  static std::atomic<bool> raw_run_;
  static std::thread raw_thread_;

  bool left_ = false;
  std::atomic<bool> run_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  WmrControllerState state_{};
  MadgwickAhrs ahrs_;
  Quaternion recenter_{};
  uint32_t last_ticks_ = 0;
  bool have_ticks_ = false;
  std::chrono::steady_clock::time_point last_packet_{};
  float gyro_bias_[3] = {};
  int bias_samples_ = 0;
  bool bias_ready_ = false;
  uint32_t rejected_imu_samples_ = 0;
  uint32_t idle_imu_samples_ = 0;
  uint32_t stationary_imu_samples_ = 0;
};
