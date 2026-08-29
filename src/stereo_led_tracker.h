#pragma once

#include "led_blob_detector.h"
#include "psvr_camera_capture.h"

#include <string>
#include <vector>

struct StereoCalibration
{
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  double baseline_m = 0.085;
  double epipolar_px = 7.0;
  double min_depth_m = 0.18;
  double max_depth_m = 5.0;

  bool Load(const std::string &path);
  bool SaveTemplate(const std::string &path, int eye_width, int eye_height) const;
  void FillFallback(int eye_width, int eye_height);
};

struct StereoLedPoint
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  float confidence = 0.f;
  LedBlob left;
  LedBlob right;
};

struct ControllerOpticalCluster
{
  bool valid = false;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  float confidence = 0.f;
  int visible_leds = 0;
  double rms_radius_m = 0.0;
};

struct StereoLedTrackingResult
{
  LedDetectionResult left_detection;
  LedDetectionResult right_detection;
  std::vector<StereoLedPoint> points;
  std::vector<ControllerOpticalCluster> controllers;
};

bool SplitSideBySide(const PsvrCameraFrame &frame, PsvrCameraFrame &left, PsvrCameraFrame &right);
StereoLedTrackingResult TrackStereoLeds(const PsvrCameraFrame &left, const PsvrCameraFrame &right,
                                        StereoCalibration calibration);
