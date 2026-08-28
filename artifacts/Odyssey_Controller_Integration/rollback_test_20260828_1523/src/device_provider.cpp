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

  controller_ = std::make_unique<GearVrControllerDevice>();
  if (!vr::VRServerDriverHost()->TrackedDeviceAdded(controller_->SerialNumber().c_str(),
                                                    vr::TrackedDeviceClass_Controller, controller_.get()))
  {
    DriverLog("GearVR: TrackedDeviceAdded failed");
    controller_.reset();
  }
  return vr::VRInitError_None;
}

void PsvrDeviceProvider::Cleanup()
{
  DriverLog("PSVR driver cleanup");
  controller_.reset();
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
  if (controller_ && hmd_)
    controller_->RunFrame(hmd_->GetPose());

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
