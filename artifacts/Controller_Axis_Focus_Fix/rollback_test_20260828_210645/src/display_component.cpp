#include "display_component.h"
#include "driverlog.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
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

void PsvrDisplayComponent::PinCompositorWindow()
{
  const auto now = std::chrono::steady_clock::now();
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
  {
    compositor_activated_ = false;
    if (fallback_clicks_ >= 40 || now - last_action_ < std::chrono::milliseconds(200))
      return;
    last_action_ = now;
    ++fallback_clicks_;
    POINT old_cursor{};
    GetCursorPos(&old_cursor);
    SetCursorPos(x + w / 2, y + h / 2);
    INPUT click[2]{};
    click[0].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].type = INPUT_MOUSE;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(2, click, sizeof(INPUT));
    SetCursorPos(old_cursor.x, old_cursor.y);
    DriverLog("PSVR: activate display fallback attempt=%d sent=%u", fallback_clicks_, sent);
    return;
  }

  RECT r{};
  GetWindowRect(hwnd, &r);
  const bool needs_layout = r.left != x || r.top != y ||
                            (r.right - r.left) != w || (r.bottom - r.top) != h;
  if (needs_layout || !style_applied_)
  {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_OVERLAPPEDWINDOW);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    style_applied_ = true;
    DriverLog("PSVR: pinned Headset Window from (%d,%d %dx%d) to (%d,%d %dx%d)",
              r.left, r.top, r.right - r.left, r.bottom - r.top, x, y, w, h);
  }

  const bool foreground = GetForegroundWindow() == hwnd;
  if (foreground && style_applied_ && !needs_layout)
    ++success_streak_;
  else
    success_streak_ = 0;
  compositor_activated_ = success_streak_ >= 3;
  if (compositor_activated_)
    return;

  const auto min_gap = activation_attempts_ < 80 ? std::chrono::milliseconds(250)
                                                 : std::chrono::milliseconds(2000);
  if (now - last_action_ < min_gap)
    return;
  last_action_ = now;
  ++activation_attempts_;

  AllowSetForegroundWindow(ASFW_ANY);
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
  SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  SetForegroundWindow(hwnd);
  SetActiveWindow(hwnd);
  SetFocus(hwnd);

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
  if (activation_attempts_ <= 8 || (activation_attempts_ % 10) == 0)
    DriverLog("PSVR: activate Headset Window attempt=%d foreground=%d layout=%d",
              activation_attempts_, compositor_activated_ ? 1 : 0, needs_layout ? 1 : 0);
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
