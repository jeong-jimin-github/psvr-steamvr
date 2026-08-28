#pragma once

#include "gearvr_ble.h"
#include "openvr_driver.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class GearVrControllerDevice : public vr::ITrackedDeviceServerDriver
{
public:
  GearVrControllerDevice();
  ~GearVrControllerDevice();

  vr::EVRInitError Activate(uint32_t unObjectId) override;
  void Deactivate() override;
  void EnterStandby() override;
  void *GetComponent(const char *pchComponentNameAndVersion) override;
  void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
  vr::DriverPose_t GetPose() override;

  void RunFrame(const vr::DriverPose_t &hmd_pose);
  const std::string &SerialNumber() const { return serial_; }
  bool IsConnected() const { return ble_.IsConnected(); }

private:
  void PoseThread();
  vr::DriverPose_t BuildPose(const vr::DriverPose_t &hmd_pose, const GearVrState &s) const;

  std::string serial_ = "GEARVR-CTRL-001";
  uint32_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
  std::atomic<bool> active_{false};
  std::thread pose_thread_;
  GearVrBle ble_;
  mutable std::mutex hmd_mutex_;
  vr::DriverPose_t last_hmd_{};
  vr::VRInputComponentHandle_t in_trigger_click_ = 0;
  vr::VRInputComponentHandle_t in_trigger_value_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_x_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_y_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_touch_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_click_ = 0;
  vr::VRInputComponentHandle_t in_system_ = 0;
  vr::VRInputComponentHandle_t in_app_menu_ = 0;
  bool last_home_ = false;
  bool did_auto_snap_ = false;
};
