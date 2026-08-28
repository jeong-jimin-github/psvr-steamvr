#include "device_provider.h"
#include "driverlog.h"

vr::EVRInitError PsvrDeviceProvider::Init(vr::IVRDriverContext *pDriverContext)
{
  VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
  DriverLog("PSVR driver init");

  hmd_ = std::make_unique<PsvrHmdDevice>();
  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(hmd_->SerialNumber().c_str(),
                                                    vr::TrackedDeviceClass_HMD, hmd_.get()))
  {
    DriverLog("PSVR: TrackedDeviceAdded failed");
    return vr::VRInitError_Driver_Failed;
  }

  gearvr_controller_ = std::make_unique<GearVrControllerDevice>();
  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(gearvr_controller_->SerialNumber().c_str(),
                                                    vr::TrackedDeviceClass_Controller,
                                                    gearvr_controller_.get()))
  {
    DriverLog("GearVR: TrackedDeviceAdded failed");
    gearvr_controller_.reset();
  }

  left_controller_ = std::make_unique<WmrControllerDevice>(true);
  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(left_controller_->SerialNumber().c_str(),
                                                    vr::TrackedDeviceClass_Controller, left_controller_.get()))
  {
    DriverLog("Odyssey left: TrackedDeviceAdded failed");
    left_controller_.reset();
  }
  right_controller_ = std::make_unique<WmrControllerDevice>(false);
  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(right_controller_->SerialNumber().c_str(),
                                                    vr::TrackedDeviceClass_Controller, right_controller_.get()))
  {
    DriverLog("Odyssey right: TrackedDeviceAdded failed");
    right_controller_.reset();
  }
  return vr::VRInitError_None;
}

void PsvrDeviceProvider::Cleanup()
{
  DriverLog("PSVR driver cleanup");
  right_controller_.reset();
  left_controller_.reset();
  gearvr_controller_.reset();
  hmd_.reset();
}

const char *const *PsvrDeviceProvider::GetInterfaceVersions()
{
  return vr::k_InterfaceVersions;
}

void PsvrDeviceProvider::RunFrame()
{
  if (hmd_)
    hmd_->RunFrame();
  if (gearvr_controller_ && hmd_)
    gearvr_controller_->RunFrame(hmd_->GetPose());
  if (left_controller_ && hmd_)
    left_controller_->RunFrame(hmd_->GetPose());
  if (right_controller_ && hmd_)
    right_controller_->RunFrame(hmd_->GetPose());

  vr::VREvent_t ev{};
  while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev)))
  {
    if (hmd_)
      hmd_->ProcessEvent(ev);
  }
}

bool PsvrDeviceProvider::ShouldBlockStandbyMode()
{
  return false;
}

void PsvrDeviceProvider::EnterStandby() {}
void PsvrDeviceProvider::LeaveStandby() {}
