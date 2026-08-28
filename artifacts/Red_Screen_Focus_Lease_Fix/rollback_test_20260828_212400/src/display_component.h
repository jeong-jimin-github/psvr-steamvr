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
  bool style_applied_ = false;
  PsvrOptics optics_{};
};
