#pragma once

#include "hmd_device.h"
#include "openvr_driver.h"
#include "wmr_controller_device.h"

#include <memory>

class PsvrDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
  vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override;
  void Cleanup() override;
  const char *const *GetInterfaceVersions() override;
  void RunFrame() override;
  bool ShouldBlockStandbyMode() override;
  void EnterStandby() override;
  void LeaveStandby() override;

private:
  std::unique_ptr<PsvrHmdDevice> hmd_;
  std::unique_ptr<WmrControllerDevice> left_controller_;
  std::unique_ptr<WmrControllerDevice> right_controller_;
};
