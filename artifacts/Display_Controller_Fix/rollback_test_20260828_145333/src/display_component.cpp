#include "display_component.h"
#include "driverlog.h"

#include <windows.h>

#include <cmath>

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
  HWND hwnd = FindWindowW(L"Headset Window", nullptr);
  if (!hwnd)
    hwnd = FindWindowW(nullptr, L"Headset Window");
  if (!hwnd)
    return;

  RECT r{};
  GetWindowRect(hwnd, &r);
  const int x = window_x_;
  const int y = window_y_;
  const int w = static_cast<int>(window_w_);
  const int h = static_cast<int>(window_h_);
  if (r.left == x && r.top == y && (r.right - r.left) == w && (r.bottom - r.top) == h)
    return;

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
