#include "controller_device.h"
#include "driverlog.h"

#include <chrono>
#include <cstring>

namespace
{
Quaternion FromHmd(const vr::HmdQuaternion_t &q)
{
  return {static_cast<float>(q.w), static_cast<float>(q.x),
          static_cast<float>(q.y), static_cast<float>(q.z)};
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

GearVrControllerDevice::GearVrControllerDevice() = default;

GearVrControllerDevice::~GearVrControllerDevice()
{
  Deactivate();
}

vr::EVRInitError GearVrControllerDevice::Activate(uint32_t unObjectId)
{
  object_id_ = unObjectId;
  active_ = true;
  last_hmd_ = {};
  last_hmd_.qRotation.w = 1;
  last_hmd_.qWorldFromDriverRotation.w = 1;
  last_hmd_.qDriverFromHeadRotation.w = 1;
  last_hmd_.vecPosition[1] = 1.65;
  did_auto_snap_ = false;
  last_home_ = false;

  vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);
  vr::VRProperties()->SetStringProperty(c, vr::Prop_ModelNumber_String, "Gear VR Controller");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Samsung");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "psvr");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_SerialNumber_String, serial_.c_str());
  vr::VRProperties()->SetStringProperty(c, vr::Prop_RenderModelName_String, "{htc}/vr_controller_vive_1_5");
  vr::VRProperties()->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{psvr}/input/gearvr_controller_profile.json");
  vr::VRProperties()->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_RightHand);
  vr::VRProperties()->SetBoolProperty(c, vr::Prop_DeviceIsWireless_Bool, true);
  vr::VRProperties()->SetBoolProperty(c, vr::Prop_Identifiable_Bool, true);

  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/trigger/click", &in_trigger_click_);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/trigger/value", &in_trigger_value_,
                                             vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/trackpad/x", &in_trackpad_x_,
                                             vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateScalarComponent(c, "/input/trackpad/y", &in_trackpad_y_,
                                             vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/trackpad/touch", &in_trackpad_touch_);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/trackpad/click", &in_trackpad_click_);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/system/click", &in_system_);
  vr::VRDriverInput()->CreateBooleanComponent(c, "/input/application_menu/click", &in_app_menu_);

  ble_.Start();
  pose_thread_ = std::thread(&GearVrControllerDevice::PoseThread, this);
  DriverLog("GearVR controller activated");
  return vr::VRInitError_None;
}

void GearVrControllerDevice::Deactivate()
{
  if (!active_.exchange(false))
    return;
  if (pose_thread_.joinable())
    pose_thread_.join();
  ble_.Stop();
  object_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

void GearVrControllerDevice::EnterStandby() {}

void *GearVrControllerDevice::GetComponent(const char *)
{
  return nullptr;
}

void GearVrControllerDevice::DebugRequest(const char *, char *buf, uint32_t n)
{
  if (n)
    buf[0] = 0;
}

vr::DriverPose_t GearVrControllerDevice::GetPose()
{
  vr::DriverPose_t hmd;
  {
    std::lock_guard<std::mutex> lock(hmd_mutex_);
    hmd = last_hmd_;
  }
  return BuildPose(hmd, ble_.GetState());
}

void GearVrControllerDevice::RunFrame(const vr::DriverPose_t &hmd_pose)
{
  {
    std::lock_guard<std::mutex> lock(hmd_mutex_);
    last_hmd_ = hmd_pose;
  }
  const GearVrState s = ble_.GetState();
  if (!did_auto_snap_ && s.connected && s.packets > 10)
  {
    ble_.Recenter(FromHmd(hmd_pose.qRotation));
    did_auto_snap_ = true;
    DriverLog("GearVR initial snap: laser = HMD forward");
  }
  vr::VRDriverInput()->UpdateBooleanComponent(in_trigger_click_, s.trigger, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_trigger_value_, s.trigger ? 1.f : 0.f, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_trackpad_x_, s.touch_x, 0);
  vr::VRDriverInput()->UpdateScalarComponent(in_trackpad_y_, s.touch_y, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_trackpad_touch_, s.touch_active, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_trackpad_click_, s.touch_click, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_system_, s.home, 0);
  vr::VRDriverInput()->UpdateBooleanComponent(in_app_menu_, s.back, 0);

  if (s.home && !last_home_)
  {
    ble_.Recenter(FromHmd(hmd_pose.qRotation));
    did_auto_snap_ = true;
    DriverLog("GearVR Home snap: laser = HMD forward");
  }
  last_home_ = s.home;
}

vr::DriverPose_t GearVrControllerDevice::BuildPose(const vr::DriverPose_t &hmd, const GearVrState &s) const
{
  vr::DriverPose_t pose{};
  pose.qWorldFromDriverRotation = {1, 0, 0, 0};
  pose.qDriverFromHeadRotation = {1, 0, 0, 0};
  pose.deviceIsConnected = s.connected;
  pose.poseIsValid = s.connected;
  pose.result = s.connected ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
  pose.willDriftInYaw = true;
  pose.shouldApplyHeadModel = false;

  pose.qRotation.w = s.rotation.w;
  pose.qRotation.x = s.rotation.x;
  pose.qRotation.y = s.rotation.y;
  pose.qRotation.z = s.rotation.z;

  // Keep the 3DoF ray origin attached to the viewer rather than to world axes.
  // This prevents the pointer from sliding to a different side when the HMD
  // turns and matches the HMD-aligned orientation established by Recenter().
  float hand[3];
  RotateVec(FromHmd(hmd.qRotation), 0.07f, -0.06f, -0.12f, hand);
  pose.vecPosition[0] = hmd.vecPosition[0] + hand[0];
  pose.vecPosition[1] = hmd.vecPosition[1] + hand[1];
  pose.vecPosition[2] = hmd.vecPosition[2] + hand[2];
  return pose;
}

void GearVrControllerDevice::PoseThread()
{
  while (active_.load())
  {
    if (object_id_ != vr::k_unTrackedDeviceIndexInvalid)
    {
      vr::DriverPose_t hmd;
      {
        std::lock_guard<std::mutex> lock(hmd_mutex_);
        hmd = last_hmd_;
      }
      const vr::DriverPose_t pose = BuildPose(hmd, ble_.GetState());
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, pose, sizeof(pose));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
}
