#include "hmd_device.h"
#include "driverlog.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstring>

PsvrHmdDevice::PsvrHmdDevice()
{
  char buf[256]{};
  vr::VRSettings()->GetString("driver_psvr", "serialNumber", buf, sizeof(buf));
  if (buf[0])
    serial_ = buf;
  buf[0] = 0;
  vr::VRSettings()->GetString("driver_psvr", "modelNumber", buf, sizeof(buf));
  if (buf[0])
    model_ = buf;
  seat_height_ = vr::VRSettings()->GetFloat("driver_psvr", "seatHeightMeters");
  if (seat_height_ <= 0.1f)
    seat_height_ = 1.65f;
}

PsvrHmdDevice::~PsvrHmdDevice()
{
  Deactivate();
}

vr::EVRInitError PsvrHmdDevice::Activate(uint32_t unObjectId)
{
  object_id_ = unObjectId;
  active_ = true;
  last_worn_ = false;
  did_puton_recenter_ = false;
  last_buttons_ = 0;

  const uint32_t render_w = static_cast<uint32_t>(vr::VRSettings()->GetInt32("driver_psvr", "renderWidth"));
  const uint32_t render_h = static_cast<uint32_t>(vr::VRSettings()->GetInt32("driver_psvr", "renderHeight"));

  PsvrOptics optics{};
  const float k1 = vr::VRSettings()->GetFloat("driver_psvr", "distortionK1");
  const float k2 = vr::VRSettings()->GetFloat("driver_psvr", "distortionK2");
  const float grow = vr::VRSettings()->GetFloat("driver_psvr", "distortionGrow");
  const float chroma = vr::VRSettings()->GetFloat("driver_psvr", "chromaticAberration");
  const float fov = vr::VRSettings()->GetFloat("driver_psvr", "fovDegrees");
  optics.k1 = k1;
  optics.k2 = k2;
  optics.chromatic = chroma;
  if (grow > 0.1f && grow < 2.f)
    optics.grow = grow;
  if (fov >= 60.f && fov <= 120.f)
  {
    const float half = 0.5f * fov * 3.1415926535f / 180.f;
    const float th = std::tan(half);
    optics.tan_left = optics.tan_right = th;
    optics.tan_top = optics.tan_bottom = th * (1080.f / 960.f);
  }

  DriverLog("PSVR optics k1=%.3f k2=%.3f grow=%.3f fovOverride=%.1f tanH=%.3f tanV=%.3f/%.3f",
            optics.k1, optics.k2, optics.grow, fov, optics.tan_left, optics.tan_top, optics.tan_bottom);

  const PsvrDisplayInfo display_info = PsvrHardware::FindHeadsetDisplay();
  display_ = std::make_unique<PsvrDisplayComponent>(
      display_info,
      render_w ? render_w : 1344,
      render_h ? render_h : 1512,
      optics);

  if (!hw_.Open())
    DriverLog("PSVR HMD: USB open failed — headset will show as disconnected");
  else
    hw_.EnterVr();

  vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);
  vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, model_.c_str());
  vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Sony");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "psvr");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_HardwareRevision_String, "CUH-ZVR");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingFirmwareVersion_String, "1.0.0");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_RegisteredDeviceType_String, "sony/psvr");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, serial_.c_str());

  float ipd = vr::VRSettings()->GetFloat("driver_psvr", "ipdMeters");
  if (ipd <= 0.01f)
    ipd = vr::VRSettings()->GetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float);
  if (ipd <= 0.01f)
    ipd = 0.0631f;
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, ipd);
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, static_cast<float>(display_->RefreshHz()));
  vr::VRProperties()->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);

  vr::VRProperties()->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, true);
  // Must be false: debug mode parks the 1920x1080 "Headset Window" on the
  // primary monitor, leaving the PSVR HDMI output showing the desktop.
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, true);
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_DeviceCanPowerOff_Bool, true);
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_HasCamera_Bool, false);
  vr::VRProperties()->SetBoolProperty(container, vr::Prop_Identifiable_Bool, true);
  vr::VRProperties()->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_Invalid);
  vr::VRProperties()->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 2);

  // NVIDIA reports the SIE HMD EDID as VID_D94D/PID_6A04 (byte-swapped SNY).
  vr::VRProperties()->SetInt32Property(container, vr::Prop_EdidVendorID_Int32, 0xD94D);
  vr::VRProperties()->SetInt32Property(container, vr::Prop_EdidProductID_Int32, 0x6A04);

  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceOff_String, "{psvr}/icons/headset_status_off.png");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearching_String, "{psvr}/icons/headset_status_searching.gif");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{psvr}/icons/headset_status_searching_alert.gif");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReady_String, "{psvr}/icons/headset_status_ready.png");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{psvr}/icons/headset_status_ready_alert.png");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceNotReady_String, "{psvr}/icons/headset_status_error.png");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceStandby_String, "{psvr}/icons/headset_status_standby.png");
  vr::VRProperties()->SetStringProperty(container, vr::Prop_NamedIconPathDeviceAlertLow_String, "{psvr}/icons/headset_status_ready_low.png");

  pose_thread_ = std::thread(&PsvrHmdDevice::PoseThread, this);
  hotkey_thread_ = std::thread(&PsvrHmdDevice::HotkeyThread, this);
  DriverLog("PSVR HMD activated id=%u display=%s %d Hz",
            object_id_, display_->FoundRealDisplay() ? "real" : "fallback", display_->RefreshHz());
  DriverLog("PSVR recenter: headset mute, or put the headset on, or Ctrl+Shift+Home");
  return vr::VRInitError_None;
}

void PsvrHmdDevice::Deactivate()
{
  if (!active_.exchange(false))
    return;
  if (hotkey_thread_.joinable())
    hotkey_thread_.join();
  if (pose_thread_.joinable())
    pose_thread_.join();
  hw_.SetVrMode(false);
  hw_.Close();
  object_id_ = vr::k_unTrackedDeviceIndexInvalid;
}

void PsvrHmdDevice::EnterStandby()
{
  DriverLog("PSVR HMD standby");
}

void *PsvrHmdDevice::GetComponent(const char *pchComponentNameAndVersion)
{
  if (pchComponentNameAndVersion &&
      strcmp(pchComponentNameAndVersion, vr::IVRDisplayComponent_Version) == 0)
    return display_.get();
  return nullptr;
}

void PsvrHmdDevice::DebugRequest(const char *, char *pchResponseBuffer, uint32_t unResponseBufferSize)
{
  if (unResponseBufferSize >= 1)
    pchResponseBuffer[0] = 0;
}

vr::DriverPose_t PsvrHmdDevice::GetPose()
{
  return BuildPose();
}

vr::DriverPose_t PsvrHmdDevice::BuildPose() const
{
  vr::DriverPose_t pose{};
  pose.qWorldFromDriverRotation = {1, 0, 0, 0};
  pose.qDriverFromHeadRotation = {1, 0, 0, 0};
  pose.qRotation = {1, 0, 0, 0};

  const PsvrPose hw = hw_.GetPose();
  pose.deviceIsConnected = hw.connected;
  pose.vecPosition[1] = seat_height_;
  pose.shouldApplyHeadModel = true;
  pose.willDriftInYaw = true;
  pose.poseTimeOffset = 0.0;

  if (!hw.connected)
  {
    pose.poseIsValid = false;
    pose.result = vr::TrackingResult_Uninitialized;
    return pose;
  }

  pose.qRotation.w = hw.rotation.w;
  pose.qRotation.x = hw.rotation.x;
  pose.qRotation.y = hw.rotation.y;
  pose.qRotation.z = hw.rotation.z;
  pose.poseIsValid = true;
  pose.result = vr::TrackingResult_Running_OK;
  return pose;
}

void PsvrHmdDevice::PoseThread()
{
  while (active_.load())
  {
    if (object_id_ != vr::k_unTrackedDeviceIndexInvalid)
    {
      const vr::DriverPose_t pose = BuildPose();
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, pose, sizeof(pose));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

void PsvrHmdDevice::RequestRecenter(const char *reason)
{
  hw_.Recenter();
  DriverLog("PSVR recenter (%s)", reason ? reason : "");
}

void PsvrHmdDevice::HotkeyThread()
{
  hotkey_thread_id_ = GetCurrentThreadId();
  const bool ok_home = RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_SHIFT, VK_HOME);
  const bool ok_c = RegisterHotKey(nullptr, 2, MOD_CONTROL | MOD_SHIFT, 'C');
  DriverLog("PSVR hotkeys Ctrl+Shift+Home=%d Ctrl+Shift+C=%d", ok_home ? 1 : 0, ok_c ? 1 : 0);

  MSG msg{};
  while (active_.load())
  {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_HOTKEY)
        RequestRecenter("hotkey");
    }
    Sleep(50);
  }
  UnregisterHotKey(nullptr, 1);
  UnregisterHotKey(nullptr, 2);
}

void PsvrHmdDevice::RunFrame()
{
  // RunFrame executes on vrserver's driver loop and can see compositor HWNDs;
  // the pose worker thread can live on a non-interactive window station.
  if (display_)
    display_->PinCompositorWindow();

  const PsvrPose hw = hw_.GetPose();
  const uint8_t pressed = hw.buttons & ~last_buttons_;
  last_buttons_ = hw.buttons;
  if (pressed)
    DriverLog("PSVR button bits=0x%02X pressed=0x%02X", hw.buttons, pressed);
  // Mute (bit3), volume+ (bit1), volume- (bit2) all snap the current view to center.
  if (pressed & 0x0E)
    RequestRecenter("headset-button");

  if (hw.worn && !last_worn_ && !did_puton_recenter_)
  {
    RequestRecenter("put-on");
    did_puton_recenter_ = true;
  }
  last_worn_ = hw.worn;
}

void PsvrHmdDevice::ProcessEvent(const vr::VREvent_t &ev)
{
  if (ev.eventType == vr::VREvent_IpdChanged)
  {
    const float ipd = vr::VRSettings()->GetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float);
    vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);
    vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, ipd);
  }
  else if (ev.eventType == vr::VREvent_SeatedZeroPoseReset)
  {
    RequestRecenter("steamvr-seated-reset");
  }
}
