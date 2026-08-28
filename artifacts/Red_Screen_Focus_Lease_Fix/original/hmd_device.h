#pragma once

#include "display_component.h"
#include "openvr_driver.h"
#include "psvr_hw.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

class PsvrHmdDevice : public vr::ITrackedDeviceServerDriver
{
public:
  PsvrHmdDevice();
  ~PsvrHmdDevice();

  vr::EVRInitError Activate(uint32_t unObjectId) override;
  void Deactivate() override;
  void EnterStandby() override;
  void *GetComponent(const char *pchComponentNameAndVersion) override;
  void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
  vr::DriverPose_t GetPose() override;

  void RunFrame();
  void ProcessEvent(const vr::VREvent_t &ev);
  const std::string &SerialNumber() const { return serial_; }

private:
  void PoseThread();
  void HotkeyThread();
  vr::DriverPose_t BuildPose() const;
  void RequestRecenter(const char *reason);

  std::string serial_ = "PSVR-HMD-001";
  std::string model_ = "CUH-ZVR";
  uint32_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
  std::atomic<bool> active_{false};
  std::thread pose_thread_;
  std::thread hotkey_thread_;
  unsigned hotkey_thread_id_ = 0;

  PsvrHardware hw_;
  std::unique_ptr<PsvrDisplayComponent> display_;
  float seat_height_ = 1.65f;
  uint8_t last_buttons_ = 0;
  bool last_worn_ = false;
  bool did_puton_recenter_ = false;
};
