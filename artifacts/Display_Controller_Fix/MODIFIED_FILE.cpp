// ===== FILE: src\display_component.cpp =====
#include "display_component.h"
#include "driverlog.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cmath>

namespace
{
DWORD FindVrCompositorPid()
{
  DWORD pid = 0;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  PROCESSENTRY32W process{};
  process.dwSize = sizeof(process);
  for (BOOL ok = Process32FirstW(snapshot, &process); ok; ok = Process32NextW(snapshot, &process))
  {
    if (_wcsicmp(process.szExeFile, L"vrcompositor.exe") == 0)
    {
      pid = process.th32ProcessID;
      break;
    }
  }
  CloseHandle(snapshot);
  return pid;
}

struct WindowSearch
{
  DWORD pid;
  HWND hwnd;
};

BOOL CALLBACK FindHeadsetWindowProc(HWND hwnd, LPARAM param)
{
  auto *search = reinterpret_cast<WindowSearch *>(param);
  wchar_t title[256]{};
  GetWindowTextW(hwnd, title, 256);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (wcsstr(title, L"Headset Window") ||
      (search->pid && pid == search->pid && IsWindowVisible(hwnd)))
  {
    search->hwnd = hwnd;
    return FALSE;
  }
  return TRUE;
}
}

PsvrDisplayComponent::PsvrDisplayComponent(const PsvrDisplayInfo &info, uint32_t render_w, uint32_t render_h, const PsvrOptics &optics)
{
  if (info.found)
  {
    window_x_ = info.x;
    window_y_ = info.y;
    window_w_ = static_cast<uint32_t>(info.width);
    window_h_ = static_cast<uint32_t>(info.height);
    refresh_hz_ = info.refresh_hz;
    real_display_ = true;
  }
  render_w_ = render_w;
  render_h_ = render_h;
  optics_ = optics;
}

void PsvrDisplayComponent::GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight)
{
  *pnX = window_x_;
  *pnY = window_y_;
  *pnWidth = window_w_;
  *pnHeight = window_h_;
}

void PsvrDisplayComponent::PinCompositorWindow()
{
  HWND hwnd = FindWindowW(nullptr, L"Headset Window");
  if (!hwnd)
    hwnd = FindWindowW(L"Headset Window", nullptr);
  if (!hwnd)
  {
    WindowSearch search{FindVrCompositorPid(), nullptr};
    EnumWindows(FindHeadsetWindowProc, reinterpret_cast<LPARAM>(&search));
    hwnd = search.hwnd;
  }
  if (!hwnd)
  {
    // vrserver can run before vrcompositor's HWND is visible to its window
    // station. A real pointer activation on the known extended display is the
    // reliable fallback and is exactly what clears SteamVR's red guard frame.
    if (activation_attempts_ < 20)
    {
      ++activation_attempts_;
      if (activation_attempts_ >= 5)
      {
        POINT old_cursor{};
        GetCursorPos(&old_cursor);
        SetCursorPos(window_x_ + static_cast<int>(window_w_ / 2),
                     window_y_ + static_cast<int>(window_h_ / 2));
        INPUT click[2]{};
        click[0].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].type = INPUT_MOUSE;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        const UINT sent = SendInput(2, click, sizeof(INPUT));
        SetCursorPos(old_cursor.x, old_cursor.y);
        DriverLog("PSVR: activate display fallback attempt=%d sent=%u",
                  activation_attempts_, sent);
      }
    }
    return;
  }

  RECT r{};
  GetWindowRect(hwnd, &r);
  const int x = window_x_;
  const int y = window_y_;
  const int w = static_cast<int>(window_w_);
  const int h = static_cast<int>(window_h_);
  const bool needs_layout = r.left != x || r.top != y ||
                            (r.right - r.left) != w || (r.bottom - r.top) != h;
  if (needs_layout)
  {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_OVERLAPPEDWINDOW);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOPMOST;
    ex &= ~WS_EX_WINDOWEDGE;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    DriverLog("PSVR: moved Headset Window from (%d,%d %dx%d) to (%d,%d %dx%d)",
              r.left, r.top, r.right - r.left, r.bottom - r.top, x, y, w, h);
  }

  // SteamVR deliberately renders solid red in extended mode until its
  // compositor window becomes the active fullscreen window. Activate it once
  // after placement; otherwise an already-correct rectangle returned above
  // without ever completing the fullscreen transition.
  if (activation_attempts_ < 20)
  {
    ++activation_attempts_;
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
    const HWND old_foreground = GetForegroundWindow();
    const DWORD foreground_thread = old_foreground ? GetWindowThreadProcessId(old_foreground, nullptr) : 0;

    if (target_thread && target_thread != current_thread)
      AttachThreadInput(current_thread, target_thread, TRUE);
    if (foreground_thread && foreground_thread != current_thread && foreground_thread != target_thread)
      AttachThreadInput(current_thread, foreground_thread, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    // vrcompositor's extended-display path waits for a real client-area
    // activation before it swaps the red/transparent guard frame for VR.
    // Window focus alone is not sufficient on current Windows builds, so send
    // the same client activation sequence as a click without moving the cursor.
    const LPARAM center = MAKELPARAM(w / 2, h / 2);
    PostMessageW(hwnd, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(hwnd),
                 MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, center);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, center);

    if (foreground_thread && foreground_thread != current_thread && foreground_thread != target_thread)
      AttachThreadInput(current_thread, foreground_thread, FALSE);
    if (target_thread && target_thread != current_thread)
      AttachThreadInput(current_thread, target_thread, FALSE);

    compositor_activated_ = GetForegroundWindow() == hwnd;
    DriverLog("PSVR: activate Headset Window attempt=%d foreground=%d",
              activation_attempts_, compositor_activated_ ? 1 : 0);
  }
}

bool PsvrDisplayComponent::IsDisplayOnDesktop()
{
  return true;
}

bool PsvrDisplayComponent::IsDisplayRealDisplay()
{
  return real_display_;
}

void PsvrDisplayComponent::GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight)
{
  *pnWidth = render_w_;
  *pnHeight = render_h_;
}

void PsvrDisplayComponent::GetEyeOutputViewport(vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight)
{
  *pnY = 0;
  *pnWidth = window_w_ / 2;
  *pnHeight = window_h_;
  *pnX = (eEye == vr::Eye_Left) ? 0 : (window_w_ / 2);
}

void PsvrDisplayComponent::GetProjectionRaw(vr::EVREye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom)
{
  *pfLeft = -optics_.tan_left;
  *pfRight = optics_.tan_right;
  *pfTop = -optics_.tan_top;
  *pfBottom = optics_.tan_bottom;
}

vr::DistortionCoordinates_t PsvrDisplayComponent::ComputeDistortion(vr::EVREye, float fU, float fV)
{
  vr::DistortionCoordinates_t c{};
  c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = fU;
  c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = fV;
  return c;
}

bool PsvrDisplayComponent::ComputeInverseDistortion(vr::HmdVector2_t *, vr::EVREye, uint32_t, float, float)
{
  return false;
}


// ===== FILE: src\display_component.h =====
#pragma once

#include "openvr_driver.h"
#include "psvr_hw.h"

struct PsvrOptics
{
  // PSVR panel is 63mm x 71mm per eye, lens ~35.4mm away, slightly high.
  float tan_left = 0.890f;
  float tan_right = 0.890f;
  float tan_top = 0.890f;
  float tan_bottom = 1.115f;
  float k1 = 0.14f;
  float k2 = 0.04f;
  float grow = 0.96f;
  float chromatic = 0.0f;
};

class PsvrDisplayComponent : public vr::IVRDisplayComponent
{
public:
  PsvrDisplayComponent(const PsvrDisplayInfo &info, uint32_t render_w, uint32_t render_h, const PsvrOptics &optics);

  void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override;
  bool IsDisplayOnDesktop() override;
  bool IsDisplayRealDisplay() override;
  void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override;
  void GetEyeOutputViewport(vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override;
  void GetProjectionRaw(vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom) override;
  vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eEye, float fU, float fV) override;
  bool ComputeInverseDistortion(vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override;

  int RefreshHz() const { return refresh_hz_; }
  bool FoundRealDisplay() const { return real_display_; }
  void PinCompositorWindow();
  int WindowX() const { return window_x_; }
  int WindowY() const { return window_y_; }
  uint32_t WindowW() const { return window_w_; }
  uint32_t WindowH() const { return window_h_; }

private:
  int window_x_ = 0;
  int window_y_ = 0;
  uint32_t window_w_ = 1920;
  uint32_t window_h_ = 1080;
  uint32_t render_w_ = 1344;
  uint32_t render_h_ = 1512;
  int refresh_hz_ = 60;
  bool real_display_ = false;
  bool compositor_activated_ = false;
  int activation_attempts_ = 0;
  PsvrOptics optics_{};
};


// ===== FILE: src\hmd_device.cpp =====
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
  int pin_ticks = 0;
  while (active_.load())
  {
    if (object_id_ != vr::k_unTrackedDeviceIndexInvalid)
    {
      const vr::DriverPose_t pose = BuildPose();
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, pose, sizeof(pose));
    }
    // vrcompositor creates "Headset Window" shortly after Activate.
    if (display_ && (pin_ticks % 50 == 0))
      display_->PinCompositorWindow();
    ++pin_ticks;
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


// ===== FILE: src\gearvr_ble.cpp =====
#include "gearvr_ble.h"
#include "driverlog.h"

#include <windows.h>
#include <cstdio>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Enumeration.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <sstream>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Devices::Enumeration;

namespace
{
constexpr wchar_t kServiceUuid[] = L"{4f63756c-7573-2054-6872-65656d6f7465}";
constexpr wchar_t kNotifyUuid[] = L"{c8c51726-81bc-483b-a052-f7a14ea3d281}";
constexpr wchar_t kWriteUuid[] = L"{c8c51726-81bc-483b-a052-f7a14ea3d282}";

std::string Narrow(const std::wstring &w)
{
  if (w.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string AddrToString(uint64_t addr)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           int((addr >> 40) & 0xFF), int((addr >> 32) & 0xFF), int((addr >> 24) & 0xFF),
           int((addr >> 16) & 0xFF), int((addr >> 8) & 0xFF), int(addr & 0xFF));
  return buf;
}

bool NameLooksLikeGear(const std::wstring &name)
{
  std::wstring n = name;
  for (auto &c : n)
    c = towlower(c);
  return n.find(L"gear") != std::wstring::npos ||
         n.find(L"oculus") != std::wstring::npos ||
         n.find(L"samsung") != std::wstring::npos;
}

int16_t LeI16(const uint8_t *p)
{
  return static_cast<int16_t>(p[0] | (p[1] << 8));
}

IBuffer BytesToBuffer(const uint8_t *data, size_t n)
{
  DataWriter writer;
  writer.WriteBytes(array_view<uint8_t const>(data, data + n));
  return writer.DetachBuffer();
}

void TryPair(BluetoothLEDevice const &device)
{
  try
  {
    auto pairing = device.DeviceInformation().Pairing();
    FileLog("GearVR: can_pair=%d is_paired=%d", pairing.CanPair() ? 1 : 0, pairing.IsPaired() ? 1 : 0);
    if (pairing.IsPaired() || !pairing.CanPair())
      return;
    auto custom = pairing.Custom();
    auto token = custom.PairingRequested([](DeviceInformationCustomPairing const &, DevicePairingRequestedEventArgs const &args) {
      args.Accept();
    });
    auto result = custom.PairAsync(DevicePairingKinds::ConfirmOnly).get();
    FileLog("GearVR: pair status=%d", int(result.Status()));
    custom.PairingRequested(token);
  }
  catch (hresult_error const &e)
  {
    FileLog("GearVR: pair exception %s", Narrow(e.message().c_str()).c_str());
  }
}

bool HasGearService(BluetoothLEDevice const &device)
{
  auto svcs = device.GetGattServicesForUuidAsync(guid(kServiceUuid), BluetoothCacheMode::Uncached).get();
  if (svcs.Status() == GattCommunicationStatus::Success && svcs.Services().Size() > 0)
    return true;
  auto all = device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
  FileLog("GearVR: gatt status=%d services=%d conn=%d", int(all.Status()), int(all.Services().Size()),
          int(device.ConnectionStatus()));
  bool apple = false;
  for (auto const &s : all.Services())
  {
    const std::string id = Narrow(std::wstring(to_hstring(s.Uuid()).c_str()));
    FileLog("GearVR:  service %s", id.c_str());
    if (id.find("89d3502b") != std::string::npos || id.find("7905f431") != std::string::npos)
      apple = true;
  }
  if (apple)
    FileLog("GearVR: skipping Apple gadget");
  return false;
}

bool WriteCmd(GattCharacteristic const &ch, uint8_t a, uint8_t b)
{
  const uint8_t cmd[2] = {a, b};
  auto status = ch.WriteValueAsync(BytesToBuffer(cmd, 2), GattWriteOption::WriteWithoutResponse).get();
  if (status != GattCommunicationStatus::Success)
    status = ch.WriteValueAsync(BytesToBuffer(cmd, 2)).get();
  return status == GattCommunicationStatus::Success;
}

struct Found
{
  uint64_t addr = 0;
  std::wstring name;
  int16_t rssi = -127;
  bool matched = false;
  BluetoothAddressType type = BluetoothAddressType::Public;
};

Quaternion Multiply(const Quaternion &a, const Quaternion &b)
{
  Quaternion r;
  r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return r;
}
} // namespace

std::vector<std::string> GearVrBle::Scan(int timeout_ms)
{
  std::vector<std::string> out;
  try
  {
    init_apartment(apartment_type::multi_threaded);
    std::mutex mu;
    std::vector<Found> found;
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    watcher.Received([&](BluetoothLEAdvertisementWatcher const &, BluetoothLEAdvertisementReceivedEventArgs const &args) {
      Found f;
      f.addr = args.BluetoothAddress();
      f.name = args.Advertisement().LocalName().c_str();
      f.rssi = args.RawSignalStrengthInDBm();
      f.type = args.BluetoothAddressType();
      bool has_svc = false;
      for (auto const &u : args.Advertisement().ServiceUuids())
      {
        if (to_hstring(u) == kServiceUuid)
          has_svc = true;
      }
      f.matched = has_svc || NameLooksLikeGear(f.name);
      std::lock_guard<std::mutex> lock(mu);
      for (auto &e : found)
      {
        if (e.addr == f.addr)
        {
          if (f.name.size() > e.name.size())
            e.name = f.name;
          e.rssi = f.rssi;
          e.matched = e.matched || f.matched;
          return;
        }
      }
      found.push_back(f);
    });
    watcher.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    watcher.Stop();
    std::vector<Found> snapshot;
    {
      std::lock_guard<std::mutex> lock(mu);
      snapshot = found;
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const Found &a, const Found &b) {
      if (a.matched != b.matched)
        return a.matched > b.matched;
      return a.rssi > b.rssi;
    });
    for (auto &f : snapshot)
    {
      bool gatt = false;
      // Do not try to pair arbitrary nearby BLE devices. The Gear VR controller
      // advertises its OculusThreemote service while HOME is blinking.
      if (f.matched)
      {
        try
        {
          auto probe = BluetoothLEDevice::FromBluetoothAddressAsync(f.addr, f.type).get();
          if (probe)
          {
            gatt = HasGearService(probe);
            probe.Close();
          }
        }
        catch (hresult_error const &e)
        {
          FileLog("GearVR: probe %s exception %s", AddrToString(f.addr).c_str(), Narrow(e.message().c_str()).c_str());
        }
      }
      char line[256];
      snprintf(line, sizeof(line), "%s type=%d rssi=%d name=%s gearvr_adv=%d gearvr_gatt=%d",
               AddrToString(f.addr).c_str(), int(f.type), int(f.rssi), Narrow(f.name).c_str(),
               f.matched ? 1 : 0, gatt ? 1 : 0);
      out.emplace_back(line);
    }
  }
  catch (hresult_error const &e)
  {
    out.push_back(std::string("scan error: ") + Narrow(e.message().c_str()));
  }
  return out;
}

bool GearVrBle::Start()
{
  Stop();
  run_ = true;
  connected_ = false;
  ahrs_.Reset();
  recenter_ = Quaternion{};
  have_sample_ = false;
  thread_ = std::thread(&GearVrBle::ThreadMain, this);
  return true;
}

void GearVrBle::Stop()
{
  run_ = false;
  if (thread_.joinable())
    thread_.join();
  connected_ = false;
}

GearVrState GearVrBle::GetState() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void GearVrBle::Recenter()
{
  std::lock_guard<std::mutex> lock(mutex_);
  const Quaternion q = ahrs_.Orientation();
  recenter_.w = q.w;
  recenter_.x = -q.x;
  recenter_.y = -q.y;
  recenter_.z = -q.z;
  state_.rotation = Multiply(recenter_, q);
  FileLog("GearVR: recenter");
}

void GearVrBle::HandlePacket(const uint8_t *data, int size)
{
  if (size != 60)
    return;

  GearVrState s;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s = state_;
  }

  s.trigger = (data[58] & (1 << 0)) != 0;
  s.home = (data[58] & (1 << 1)) != 0;
  s.back = (data[58] & (1 << 2)) != 0;
  s.touch_click = (data[58] & (1 << 3)) != 0;
  s.vol_down = (data[58] & (1 << 4)) != 0;
  s.vol_up = (data[58] & (1 << 5)) != 0;

  const int axis_x = ((((data[54] & 0x0F) << 6) | ((data[55] & 0xFC) >> 2)) & 0x3FF);
  const int axis_y = ((((data[55] & 0x03) << 8) | data[56]) & 0x3FF);
  s.touch_active = axis_x != 0 || axis_y != 0;
  if (s.touch_active)
  {
    s.touch_x = (axis_x / 1023.f) * 2.f - 1.f;
    s.touch_y = (axis_y / 1023.f) * 2.f - 1.f;
  }
  else
  {
    s.touch_x = 0.f;
    s.touch_y = 0.f;
  }

  // Scaling recovered from Samsung's Gear VR input service.
  constexpr float accel_scale = 10000.f * 9.80665f / 2048.f * 0.00001f;
  constexpr float gyro_scale = 10000.f * 0.017453292f / 14.285f * 0.0001f;
  const float ax = LeI16(data + 4) * accel_scale;
  const float ay = LeI16(data + 6) * accel_scale;
  const float az = LeI16(data + 8) * accel_scale;
  const float gx = LeI16(data + 10) * gyro_scale;
  const float gy = LeI16(data + 12) * gyro_scale;
  const float gz = LeI16(data + 14) * gyro_scale;

  ahrs_.Update(gx, gy, gz, ax, ay, az, 0.015f);
  const Quaternion raw = ahrs_.Orientation();
  s.rotation = Multiply(recenter_, raw);
  s.packets++;
  s.connected = true;

  std::lock_guard<std::mutex> lock(mutex_);
  s.name = state_.name;
  s.address = state_.address;
  state_ = s;
}

void GearVrBle::ThreadMain()
{
  try
  {
    init_apartment(apartment_type::multi_threaded);
    while (run_.load())
    {
      ConnectionAttempt();
      connected_ = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.connected = false;
      }
      for (int i = 0; i < 10 && run_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  catch (hresult_error const &e)
  {
    FileLog("GearVR: thread exception %s", Narrow(e.message().c_str()).c_str());
    connected_ = false;
  }
}

void GearVrBle::ConnectionAttempt()
{
  try
  {
    FileLog("GearVR: BLE thread start, scanning...");

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Found> found;
    Found best;
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    watcher.Received([&](BluetoothLEAdvertisementWatcher const &, BluetoothLEAdvertisementReceivedEventArgs const &args) {
      Found f;
      f.addr = args.BluetoothAddress();
      f.name = args.Advertisement().LocalName().c_str();
      f.rssi = args.RawSignalStrengthInDBm();
      f.type = args.BluetoothAddressType();
      bool has_svc = false;
      for (auto const &u : args.Advertisement().ServiceUuids())
      {
        if (to_hstring(u) == kServiceUuid)
          has_svc = true;
      }
      f.matched = has_svc || NameLooksLikeGear(f.name);
      std::lock_guard<std::mutex> lock(mu);
      for (auto &e : found)
      {
        if (e.addr == f.addr)
        {
          if (f.name.size() > e.name.size())
            e.name = f.name;
          e.rssi = f.rssi;
          e.matched = e.matched || f.matched;
          if (e.matched)
            cv.notify_all();
          return;
        }
      }
      found.push_back(f);
      if (f.matched)
        cv.notify_all();
    });
    watcher.Start();
    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait_for(lock, std::chrono::seconds(12), [&] {
        if (!run_.load())
          return true;
        for (auto const &candidate : found)
          if (candidate.matched)
            return true;
        return false;
      });
    }
    watcher.Stop();

    if (!run_.load())
      return;

    std::vector<Found> candidates;
    {
      std::lock_guard<std::mutex> lock(mu);
      candidates = found;
    }
    // Named non-Gear devices are known unrelated peripherals. Keep unnamed
    // advertisements as a fallback because some controller firmware omits its
    // local name while in pairing mode.
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Found &c) {
                       return !c.matched && !c.name.empty();
                     }),
                     candidates.end());
    FileLog("GearVR: saw %d BLE advertisers", int(candidates.size()));
    std::sort(candidates.begin(), candidates.end(), [](const Found &a, const Found &b) {
      if (a.matched != b.matched)
        return a.matched > b.matched;
      return a.rssi > b.rssi;
    });

    for (auto &c : candidates)
    {
      if (!run_.load())
        return;
      FileLog("GearVR: probing %s rssi=%d name=%s match=%d", AddrToString(c.addr).c_str(),
              int(c.rssi), Narrow(c.name).c_str(), c.matched ? 1 : 0);
      try
      {
        auto probe = BluetoothLEDevice::FromBluetoothAddressAsync(c.addr, c.type).get();
        if (!probe)
          continue;
        const bool ok = HasGearService(probe);
        if (!ok)
        {
          probe.Close();
          continue;
        }
        best = c;
        best.matched = true;
        probe.Close();
        break;
      }
      catch (hresult_error const &e)
      {
        FileLog("GearVR: probe failed %s %s", AddrToString(c.addr).c_str(), Narrow(e.message().c_str()).c_str());
      }
    }

    if (!best.matched)
    {
      FileLog("GearVR: no Gear VR GATT service on nearby BLE devices");
      return;
    }

    FileLog("GearVR: found %s %s rssi=%d", AddrToString(best.addr).c_str(),
            Narrow(best.name).c_str(), int(best.rssi));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.name = Narrow(best.name);
      state_.address = AddrToString(best.addr);
    }

    auto device = BluetoothLEDevice::FromBluetoothAddressAsync(best.addr, best.type).get();
    if (!device)
    {
      FileLog("GearVR: FromBluetoothAddressAsync failed");
      return;
    }

    auto session = GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()).get();
    session.MaintainConnection(true);
    FileLog("GearVR: GATT session maintain_connection=1 paired=%d",
            device.DeviceInformation().Pairing().IsPaired() ? 1 : 0);

    auto services = device.GetGattServicesForUuidAsync(guid(kServiceUuid), BluetoothCacheMode::Uncached).get();
    if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0)
    {
      FileLog("GearVR: custom service missing (status=%d)", int(services.Status()));
      return;
    }
    auto service = services.Services().GetAt(0);
    const auto access = service.RequestAccessAsync().get();
    FileLog("GearVR: service access=%d", int(access));
    auto chars = service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
    if (chars.Status() != GattCommunicationStatus::Success)
    {
      FileLog("GearVR: characteristic discovery failed (%d)", int(chars.Status()));
      return;
    }
    GattCharacteristic notify{nullptr};
    GattCharacteristic write{nullptr};
    for (auto const &c : chars.Characteristics())
    {
      const auto id = to_hstring(c.Uuid());
      if (id == kNotifyUuid)
        notify = c;
      else if (id == kWriteUuid)
        write = c;
    }
    if (!notify || !write)
    {
      FileLog("GearVR: notify/write characteristics missing");
      return;
    }

    GattCommunicationStatus cccd = GattCommunicationStatus::Unreachable;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
      cccd = notify.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::Notify)
                 .get();
      FileLog("GearVR: CCCD notify attempt=%d status=%d", attempt, int(cccd));
      if (cccd == GattCommunicationStatus::Success)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    if (cccd != GattCommunicationStatus::Success)
    {
      FileLog("GearVR: CCCD notify failed (%d)", int(cccd));
      return;
    }

    notify.ValueChanged([&](GattCharacteristic const &, GattValueChangedEventArgs const &args) {
      auto reader = DataReader::FromBuffer(args.CharacteristicValue());
      std::vector<uint8_t> buf(reader.UnconsumedBufferLength());
      if (!buf.empty())
        reader.ReadBytes(buf);
      HandlePacket(buf.data(), static_cast<int>(buf.size()));
    });

    // Samsung initialization sequence: repeated sensor mode, low-power mode
    // toggle, then repeated high-rate VR mode.
    const uint8_t init[][2] = {
        {0x01, 0x00}, {0x01, 0x00}, {0x01, 0x00},
        {0x06, 0x00}, {0x07, 0x00},
        {0x08, 0x00}, {0x08, 0x00}, {0x08, 0x00}};
    for (auto const &cmd : init)
    {
      if (!WriteCmd(write, cmd[0], cmd[1]))
        FileLog("GearVR: init write %02X%02X failed", cmd[0], cmd[1]);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    connected_ = true;
    FileLog("GearVR: connected, streaming");

    auto last_keepalive = std::chrono::steady_clock::now();
    while (run_.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const auto now = std::chrono::steady_clock::now();
      if (now - last_keepalive > std::chrono::seconds(8))
      {
        WriteCmd(write, 0x04, 0x00);
        last_keepalive = now;
      }
      if (device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
      {
        FileLog("GearVR: disconnected");
        connected_ = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          state_.connected = false;
        }
        break;
      }
    }

    WriteCmd(write, 0x00, 0x00);
    device.Close();
  }
  catch (hresult_error const &e)
  {
    FileLog("GearVR: exception %s", Narrow(e.message().c_str()).c_str());
    connected_ = false;
  }
}


