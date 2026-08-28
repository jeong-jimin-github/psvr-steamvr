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
  if (!wcsstr(title, L"Headset Window"))
    return TRUE;
  if (search->pid && pid != search->pid)
    return TRUE;
  search->hwnd = hwnd;
  return FALSE;
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

void PsvrDisplayComponent::PinCompositorWindow(bool headset_worn)
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

  const int x = window_x_;
  const int y = window_y_;
  const int w = static_cast<int>(window_w_);
  const int h = static_cast<int>(window_h_);

  if (!hwnd)
    return;

  RECT r{};
  GetWindowRect(hwnd, &r);
  const bool needs_layout = r.left != x || r.top != y ||
                            (r.right - r.left) != w || (r.bottom - r.top) != h;
  const bool focus_state_changed = !have_focus_state_ || headset_worn_ != headset_worn;
  if (needs_layout || !style_applied_ || focus_state_changed)
  {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_OVERLAPPEDWINDOW);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // SteamVR's extended-mode compositor deliberately renders solid red when
    // its Headset Window is not foreground/fullscreen. Lease foreground only
    // while the proximity sensor says the headset is being worn. Otherwise
    // make the window non-activating so SteamVR cannot disrupt desktop input.
    ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (headset_worn)
      ex &= ~WS_EX_NOACTIVATE;
    else
      ex |= WS_EX_NOACTIVATE;
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER |
                     (headset_worn ? 0 : SWP_NOACTIVATE));
    style_applied_ = true;
    have_focus_state_ = true;
    headset_worn_ = headset_worn;
    DriverLog("PSVR: pinned Headset Window from (%d,%d %dx%d) to (%d,%d %dx%d)",
              r.left, r.top, r.right - r.left, r.bottom - r.top, x, y, w, h);
  }

  const HWND foreground = GetForegroundWindow();
  if (headset_worn)
  {
    if (foreground == hwnd)
      return;
    if (foreground && IsWindow(foreground))
      previous_foreground_ = reinterpret_cast<uintptr_t>(foreground);

    const DWORD current_thread = GetCurrentThreadId();
    const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (target_thread && target_thread != current_thread)
      AttachThreadInput(current_thread, target_thread, TRUE);
    if (foreground_thread && foreground_thread != current_thread && foreground_thread != target_thread)
      AttachThreadInput(current_thread, foreground_thread, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    const LPARAM center = MAKELPARAM(w / 2, h / 2);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, center);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, center);

    if (foreground_thread && foreground_thread != current_thread && foreground_thread != target_thread)
      AttachThreadInput(current_thread, foreground_thread, FALSE);
    if (target_thread && target_thread != current_thread)
      AttachThreadInput(current_thread, target_thread, FALSE);
    DriverLog("PSVR: Headset Window fullscreen focus acquired (worn=1)");
  }
  else if (foreground == hwnd && previous_foreground_)
  {
    const HWND previous = reinterpret_cast<HWND>(previous_foreground_);
    if (IsWindow(previous))
    {
      SetForegroundWindow(previous);
      DriverLog("PSVR: desktop focus restored (worn=0)");
    }
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
