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
