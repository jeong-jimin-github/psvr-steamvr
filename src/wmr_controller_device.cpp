#include "wmr_controller_device.h"
#include "driverlog.h"

#include <chrono>
#include <cmath>

namespace
{
Quaternion FromHmd(const vr::HmdQuaternion_t &q)
{
  return {static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)};
}

void RotateVec(const Quaternion &q, float x, float y, float z, float out[3])
{
  const float tx = 2.f * (q.y * z - q.z * y);
  const float ty = 2.f * (q.z * x - q.x * z);
  const float tz = 2.f * (q.x * y - q.y * x);
  out[0] = x + q.w * tx + (q.y * tz - q.z * ty);
  out[1] = y + q.w * ty + (q.z * tx - q.x * tz);
  out[2] = z + q.w * tz + (q.x * ty - q.y * tx);
}

}

WmrControllerDevice::WmrControllerDevice(bool left)
    : left_(left), serial_(left ? "WMR-ODYSSEY-LEFT" : "WMR-ODYSSEY-RIGHT"), hid_(left)
{
}

WmrControllerDevice::~WmrControllerDevice()
{
  Deactivate();
}

vr::EVRInitError WmrControllerDevice::Activate(uint32_t object_id)
{
  object_id_ = object_id;
  active_ = true;
  last_hmd_.qRotation.w = 1;
  last_hmd_.qWorldFromDriverRotation.w = 1;
  last_hmd_.qDriverFromHeadRotation.w = 1;
  last_hmd_.vecPosition[1] = 1.65;

  const auto c = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);
  vr::VRProperties()->SetStringProperty(c, vr::Prop_ModelNumber_String, "Samsung HMD Odyssey Motion Controller");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Samsung");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "psvr");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_SerialNumber_String, serial_.c_str());
  vr::VRProperties()->SetStringProperty(c, vr::Prop_RenderModelName_String, "{psvr}/odyssey_controller");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{psvr}/input/odyssey_controller_profile.json");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_ControllerType_String, "holographic_controller");
  vr::VRProperties()->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                                       left_ ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
  vr::VRProperties()->SetBoolProperty(c, vr::Prop_DeviceIsWireless_Bool, true);
  vr::VRProperties()->SetBoolProperty(c, vr::Prop_Identifiable_Bool, true);

  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/trigger/click", &in_trigger_click_);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/trigger/value", &in_trigger_value_, vr::VRScalarType_Absolute,
                                             vr::VRScalarUnits_NormalizedOneSided);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/grip/click", &in_grip_);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/joystick/x", &in_thumbstick_x_, vr::VRScalarType_Absolute,
                                             vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/joystick/y", &in_thumbstick_y_, vr::VRScalarType_Absolute,
                                             vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/joystick/click", &in_thumbstick_click_);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/trackpad/x", &in_trackpad_x_, vr::VRScalarType_Absolute,
                                             vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/trackpad/y", &in_trackpad_y_, vr::VRScalarType_Absolute,
                                             vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/trackpad/touch", &in_trackpad_touch_);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/trackpad/click", &in_trackpad_click_);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/system/click", &in_system_);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/application_menu/click", &in_menu_);

  hid_.Start();
  pose_thread_ = std::thread(&WmrControllerDevice::PoseThread, this);
  DriverLog("Odyssey %s controller activated", left_ ? "left" : "right");
  return vr::VRInitError_None;
}

void WmrControllerDevice::Deactivate()
{
  if (!active_.exchange(false))
    return;
  hid_.Stop();
  if (pose_thread_.joinable())
    pose_thread_.join();
  object_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

void WmrControllerDevice::EnterStandby() {}
void *WmrControllerDevice::GetComponent(const char *) { return nullptr; }
void WmrControllerDevice::DebugRequest(const char *, char *buf, uint32_t size)
{
  if (size)
    buf[0] = 0;
}

vr::DriverPose_t WmrControllerDevice::GetPose()
{
  return BuildPose(last_hmd_, hid_.GetState());
}

void WmrControllerDevice::RunFrame(const vr::DriverPose_t &hmd_pose)
{
  last_hmd_ = hmd_pose;
  const WmrControllerState s = hid_.GetState();
  if (!did_auto_snap_ && s.connected && s.packets > 10)
  {
    hid_.Recenter(FromHmd(hmd_pose.qRotation));
    did_auto_snap_ = true;
  }
  vr::VRDriverInput()->UpdateBooleanComponent(in_trigger_click_, s.trigger > 0.55f, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_trigger_value_, s.trigger, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_grip_, s.squeeze, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_thumbstick_x_, s.thumbstick_x, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_thumbstick_y_, s.thumbstick_y, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_thumbstick_click_, s.thumbstick_click, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_trackpad_x_, s.trackpad_x, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_trackpad_y_, s.trackpad_y, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_trackpad_touch_, s.trackpad_touch, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_trackpad_click_, s.trackpad_click, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_system_, s.home, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_menu_, s.menu, 0);
  const bool recenter_btn = s.home || s.menu;
  if (recenter_btn && !last_home_)
  {
    const Quaternion hmd_q = FromHmd(hmd_pose.qRotation);
    hid_.Recenter(hmd_q);
    did_auto_snap_ = true;
    DriverLog("Odyssey %s Home/Menu snap: laser = HMD forward", left_ ? "left" : "right");
  }
  last_home_ = recenter_btn;
}

vr::DriverPose_t WmrControllerDevice::BuildPose(const vr::DriverPose_t &hmd, const WmrControllerState &s) const
{
  vr::DriverPose_t pose{};
  pose.qWorldFromDriverRotation = {1, 0, 0, 0};
  pose.qDriverFromHeadRotation = {1, 0, 0, 0};
  pose.deviceIsConnected = s.connected;
  pose.poseIsValid = s.connected && s.packets > 0;
  pose.result = pose.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
  pose.willDriftInYaw = true;
  pose.shouldApplyHeadModel = false;

  const Quaternion hmd_q = FromHmd(hmd.qRotation);
  pose.qRotation = {s.rotation.w, s.rotation.x, s.rotation.y, s.rotation.z};

  // Originate near the eyes so the ray hits what you look at after Home snap.
  float hand[3];
  RotateVec(hmd_q, left_ ? -0.07f : 0.07f, -0.06f, -0.12f, hand);
  pose.vecPosition[0] = hmd.vecPosition[0] + hand[0];
  pose.vecPosition[1] = hmd.vecPosition[1] + hand[1];
  pose.vecPosition[2] = hmd.vecPosition[2] + hand[2];
  return pose;
}

void WmrControllerDevice::PoseThread()
{
  while (active_.load())
  {
    if (object_id_ != vr::k_unTrackedDeviceIndexInvalid)
    {
      const vr::DriverPose_t pose = BuildPose(last_hmd_, hid_.GetState());
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, pose, sizeof(pose));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
}
