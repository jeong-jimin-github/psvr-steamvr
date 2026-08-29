#pragma once

#include "openvr_driver.h"
#include "odyssey_optical_tracker.h"
#include "wmr_hid.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class WmrControllerDevice : public vr::ITrackedDeviceServerDriver
{
public:
  explicit WmrControllerDevice(bool left);
  ~WmrControllerDevice();

  vr::EVRInitError Activate(uint32_t unObjectId) override;
  void Deactivate() override;
  void EnterStandby() override;
  void *GetComponent(const char *pchComponentNameAndVersion) override;
  void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
  vr::DriverPose_t GetPose() override;

  void RunFrame(const vr::DriverPose_t &hmd_pose);
  const std::string &SerialNumber() const { return serial_; }

  // Entry point for the PSVR-camera constellation tracker. Position must be
  // expressed in the driver's world coordinate system.
  void SubmitOpticalPosition(double x, double y, double z, float confidence, uint32_t visible_leds)
  {
    optical_.SubmitPosition(x, y, z, confidence, visible_leds);
  }
  void InvalidateOpticalPosition() { optical_.Invalidate(); }

private:
  void PoseThread();
  vr::DriverPose_t BuildPose(const vr::DriverPose_t &hmd_pose, const WmrControllerState &state) const;

  bool left_ = false;
  std::string serial_;
  uint32_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
  std::atomic<bool> active_{false};
  std::thread pose_thread_;
  WmrHidController hid_;
  OdysseyOpticalTracker optical_;
  vr::DriverPose_t last_hmd_{};

  vr::VRInputComponentHandle_t in_trigger_click_ = 0;
  vr::VRInputComponentHandle_t in_trigger_value_ = 0;
  vr::VRInputComponentHandle_t in_grip_ = 0;
  vr::VRInputComponentHandle_t in_thumbstick_x_ = 0;
  vr::VRInputComponentHandle_t in_thumbstick_y_ = 0;
  vr::VRInputComponentHandle_t in_thumbstick_click_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_x_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_y_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_touch_ = 0;
  vr::VRInputComponentHandle_t in_trackpad_click_ = 0;
  vr::VRInputComponentHandle_t in_system_ = 0;
  vr::VRInputComponentHandle_t in_menu_ = 0;
  bool last_home_ = false;
  mutable bool did_auto_snap_ = false;
};
