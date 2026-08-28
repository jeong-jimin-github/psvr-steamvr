#pragma once

#include "madgwick.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct PsvrPose
{
  Quaternion rotation{};
  bool connected = false;
  bool worn = false;
  bool hdmi_ok = true;
  uint8_t buttons = 0;
  uint16_t proximity = 0;
};

struct PsvrDisplayInfo
{
  bool found = false;
  int x = 0;
  int y = 0;
  int width = 1920;
  int height = 1080;
  int refresh_hz = 60;
  std::wstring device_name;
  std::wstring monitor_name;
};

class PsvrHardware
{
public:
  bool Open(bool start_sensor_thread = true);
  void Close();
  bool IsOpen() const { return open_.load(); }

  bool PowerOn();
  bool PowerOff();
  bool SetVrMode(bool enabled);
  bool EnableTracking();
  bool EnterVr();

  PsvrPose GetPose() const;
  void Recenter();

  static PsvrDisplayInfo FindHeadsetDisplay();

  // Filled by the last sensor packet for the CLI tool.
  bool ReadRawSensor(uint8_t out[64]);

private:
  bool OpenControl();
  bool OpenSensors();
  bool WriteControl(const uint8_t *data, unsigned length);
  void SensorThread();
  void HandleSensorPacket(const uint8_t *buffer, int size);

  void *control_handle_ = nullptr; // HANDLE
  void *winusb_handle_ = nullptr;  // WINUSB_INTERFACE_HANDLE
  uint8_t out_pipe_ = 0;
  bool use_control_xfer_ = false;

  void *sensor_handle_ = nullptr; // HANDLE

  std::atomic<bool> open_{false};
  std::atomic<bool> run_{false};
  std::thread sensor_thread_;

  mutable std::mutex pose_mutex_;
  PsvrPose pose_{};
  MadgwickAhrs ahrs_;
  Quaternion recenter_{};
  bool have_tick_ = false;
  uint32_t last_tick_ = 0;
  float gyro_bias_[3] = {};
  int bias_samples_ = 0;
  bool bias_ready_ = false;
};
