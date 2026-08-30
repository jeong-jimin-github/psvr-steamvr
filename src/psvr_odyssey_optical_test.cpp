#include "odyssey_optical_tracker.h"
#include "psvr_camera_capture.h"
#include "psvr_camera_firmware.h"
#include "stereo_led_tracker.h"
#include "wmr_hid.h"
#include "wmr_led_output.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
struct Options
{
  bool list = false;
  bool force_sbs = false;
  bool swap_eyes = false;
  bool try_led_max = false;
  int camera = -1;
  int camera_right = -1;
  int seconds = 30;
  std::string calibration = "psvr_camera_calibration.txt";
  std::string camera_firmware;
  std::string dump_prefix;
};

std::wstring Lower(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return s;
}

void Usage()
{
  std::puts(
      "PSVR camera + Samsung Odyssey controller optical smoke test (no PSVR HMD required)\n"
      "  psvr_odyssey_optical_test --list\n"
      "  psvr_odyssey_optical_test [--camera N] [--camera-right N] [--seconds N]\n"
      "      [--force-sbs] [--swap-eyes] [--try-led-max]\n"
      "      [--calibration file] [--camera-firmware file] [--dump-prefix path]\n\n"
      "If one camera device produces a wide stereo frame, it is split left/right automatically.\n"
      "Use --force-sbs if your driver exposes the two eyes in one even-width frame but the aspect\n"
      "ratio is not obviously stereo. Use --camera-right for drivers that expose two devices.\n"
      "--try-led-max attempts WMR LED intensity 399 only if Windows grants a HID output handle;\n"
      "failure is non-fatal and Raw Input/camera testing continues.\n"
      "--camera-firmware uploads PS4 Camera firmware when USB Boot 05A9:0580 is present,\n"
      "then waits for the camera to re-enumerate before continuing.\n");
}

Options Parse(int argc, char **argv)
{
  Options o;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next_int = [&](int &dst) {
      if (i + 1 < argc)
        dst = std::atoi(argv[++i]);
    };
    if (a == "--list") o.list = true;
    else if (a == "--force-sbs") o.force_sbs = true;
    else if (a == "--swap-eyes") o.swap_eyes = true;
    else if (a == "--try-led-max") o.try_led_max = true;
    else if (a == "--camera") next_int(o.camera);
    else if (a == "--camera-right") next_int(o.camera_right);
    else if (a == "--seconds") next_int(o.seconds);
    else if (a == "--calibration" && i + 1 < argc) o.calibration = argv[++i];
    else if (a == "--camera-firmware" && i + 1 < argc) o.camera_firmware = argv[++i];
    else if (a == "--dump-prefix" && i + 1 < argc) o.dump_prefix = argv[++i];
    else if (a == "--help" || a == "-h") { Usage(); std::exit(0); }
  }
  return o;
}

bool IsPsvrCameraName(const std::wstring &name)
{
  const auto n = Lower(name);
  return n.find(L"playstation") != std::wstring::npos || n.find(L"ps4") != std::wstring::npos ||
         n.find(L"sony") != std::wstring::npos || n.find(L"ps camera") != std::wstring::npos ||
         n.find(L"ov580") != std::wstring::npos;
}

bool HasPsvrCamera(const std::vector<PsvrCameraDeviceInfo> &devices)
{
  return std::any_of(devices.begin(), devices.end(),
                     [](const auto &d) { return IsPsvrCameraName(d.name); });
}

int AutoCamera(const std::vector<PsvrCameraDeviceInfo> &devices)
{
  for (const auto &d : devices)
    if (IsPsvrCameraName(d.name))
      return d.index;
  return devices.empty() ? -1 : devices.front().index;
}

void SaveRawPgm(const std::string &path, const PsvrCameraFrame &frame)
{
  if (frame.width <= 0 || frame.height <= 0 || frame.gray.empty())
    return;
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return;
  f << "P5\n" << frame.width << " " << frame.height << "\n255\n";
  f.write(reinterpret_cast<const char *>(frame.gray.data()),
          static_cast<std::streamsize>(frame.gray.size()));
}

void SavePgm(const std::string &path, const PsvrCameraFrame &frame, const LedDetectionResult &det)
{
  if (frame.width <= 0 || frame.height <= 0 || frame.gray.empty())
    return;
  std::vector<uint8_t> image = frame.gray;
  for (const auto &b : det.blobs)
  {
    const int cx = static_cast<int>(b.x + 0.5f);
    const int cy = static_cast<int>(b.y + 0.5f);
    for (int d = -6; d <= 6; ++d)
    {
      const int x = cx + d;
      const int y = cy + d;
      if (x >= 0 && x < frame.width && cy >= 0 && cy < frame.height)
        image[static_cast<size_t>(cy) * frame.width + x] = d == 0 ? 0 : 255;
      if (cx >= 0 && cx < frame.width && y >= 0 && y < frame.height)
        image[static_cast<size_t>(y) * frame.width + cx] = d == 0 ? 0 : 255;
    }
  }
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return;
  f << "P5\n" << frame.width << " " << frame.height << "\n255\n";
  f.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
}

void SubmitClusters(const StereoLedTrackingResult &tracking,
                    const WmrControllerState &ls, const WmrControllerState &rs,
                    OdysseyOpticalTracker &left, OdysseyOpticalTracker &right)
{
  left.Invalidate();
  right.Invalidate();
  std::vector<ControllerOpticalCluster> valid;
  for (const auto &c : tracking.controllers)
    if (c.valid)
      valid.push_back(c);
  if (valid.empty())
    return;

  auto submit = [](OdysseyOpticalTracker &dst, const ControllerOpticalCluster &c) {
    dst.SubmitPosition(c.x, c.y, -c.z, c.confidence, static_cast<uint32_t>(c.visible_leds));
  };

  if (valid.size() >= 2)
  {
    std::sort(valid.begin(), valid.end(), [](const auto &a, const auto &b) { return a.x < b.x; });
    submit(left, valid.front());
    submit(right, valid.back());
    return;
  }

  const auto &c = valid.front();
  if (ls.connected && !rs.connected)
    submit(left, c);
  else if (rs.connected && !ls.connected)
    submit(right, c);
  else if (c.x < 0.0)
    submit(left, c);
  else
    submit(right, c);
}

void PrintMeasurement(const char *name, const OdysseyOpticalMeasurement &m)
{
  if (!m.valid)
  {
    std::printf("%s=lost", name);
    return;
  }
  std::printf("%s=(%+.3f,%+.3f,%+.3f)m q=%.2f leds=%u",
              name, m.position[0], m.position[1], m.position[2], m.confidence, m.visible_leds);
}
}

int main(int argc, char **argv)
{
  const Options opt = Parse(argc, argv);

  // Start controller Raw Input before touching the camera so hardware diagnostics still
  // report controller traffic when the PS4 Camera is in USB boot mode or fails to open.
  WmrHidController left_hid(true), right_hid(false);
  left_hid.Start();
  right_hid.Start();

  if (opt.try_led_max)
  {
    std::string status;
    const bool l = TrySetOdysseyLedIntensity(true, 399, status);
    std::printf("LED left: %s%s\n", l ? "OK - " : "SKIP - ", status.c_str());
    const bool r = TrySetOdysseyLedIntensity(false, 399, status);
    std::printf("LED right: %s%s\n", r ? "OK - " : "SKIP - ", status.c_str());
  }

  auto devices = PsvrCameraCapture::Enumerate();
  std::string boot_status;
  const bool boot_present = IsPs4CameraBootPresent(boot_status);
  if (boot_present && !opt.camera_firmware.empty() && !HasPsvrCamera(devices))
  {
    std::printf("Camera boot: %s\n", boot_status.c_str());
    std::string upload_status;
    if (UploadPs4CameraFirmware(opt.camera_firmware, upload_status))
    {
      std::printf("Camera firmware: OK - %s\n", upload_status.c_str());
      for (int i = 0; i < 32 && !HasPsvrCamera(devices); ++i)
      {
        Sleep(250);
        devices = PsvrCameraCapture::Enumerate();
      }
    }
    else
    {
      std::fprintf(stderr, "Camera firmware: FAIL - %s\n", upload_status.c_str());
    }
  }

  std::wcout << L"Media Foundation video devices:\n";
  for (const auto &d : devices)
    std::wcout << L"  [" << d.index << L"] " << d.name << L"\n";

  if (opt.list)
  {
    Sleep(1200);
    const auto ls = left_hid.GetState();
    const auto rs = right_hid.GetState();
    std::printf("HID diagnostic: L=%d pkt=%u R=%d pkt=%u\n",
                ls.connected ? 1 : 0, ls.packets, rs.connected ? 1 : 0, rs.packets);
    if (devices.empty())
      std::printf("Camera USB diagnostic: %s\n", boot_present ? boot_status.c_str() : "05A9:0580 boot device not present");
    left_hid.Stop();
    right_hid.Stop();
    return devices.empty() ? 2 : 0;
  }

  if (devices.empty())
  {
    if (boot_present && opt.camera_firmware.empty())
      std::fprintf(stderr,
                   "PS4 Camera is connected as USB Boot 05A9:0580. Re-run with --camera-firmware <firmware.bin>.\n");
    else
      std::fprintf(stderr,
                   "No Media Foundation camera device found after camera initialization. Check the OV580/UVC driver.\n");
    left_hid.Stop();
    right_hid.Stop();
    return 2;
  }

  const int camera_index = opt.camera >= 0 ? opt.camera : AutoCamera(devices);
  PsvrCameraCapture cam_left;
  if (!cam_left.Open(camera_index, 2560, 800) && !cam_left.Open(camera_index, 1280, 800))
  {
    std::fprintf(stderr, "Could not open camera %d: %s\n", camera_index, cam_left.LastError().c_str());
    return 3;
  }
  std::wcout << L"Opened camera [" << camera_index << L"] " << cam_left.Name()
             << L" format=" << cam_left.Width() << L"x" << cam_left.Height() << L"\n";

  PsvrCameraCapture cam_right;
  const bool dual_device = opt.camera_right >= 0;
  if (dual_device && !cam_right.Open(opt.camera_right, 1280, 800))
  {
    std::fprintf(stderr, "Could not open right camera %d: %s\n", opt.camera_right, cam_right.LastError().c_str());
    return 4;
  }

  OdysseyOpticalTracker left_tracker(true), right_tracker(false);

  StereoCalibration cal;
  const bool loaded_cal = cal.Load(opt.calibration);
  if (!loaded_cal)
    std::printf("Calibration '%s' not found/invalid: using 85deg-FOV + 85mm-baseline smoke-test fallback.\n",
                opt.calibration.c_str());

  const auto started = std::chrono::steady_clock::now();
  auto last_print = started - std::chrono::seconds(1);
  int frames = 0;
  bool warned_mono = false;

  while (std::chrono::steady_clock::now() - started < std::chrono::seconds(std::max(1, opt.seconds)))
  {
    PsvrCameraFrame raw, left_frame, right_frame;
    if (!cam_left.Read(raw))
    {
      const auto &camera_error = cam_left.LastError();
      if (!camera_error.empty())
        std::fprintf(stderr, "Camera read failed: %s\n", camera_error.c_str());
      Sleep(10);
      continue;
    }

    bool have_stereo = false;
    if (dual_device)
    {
      if (cam_right.Read(right_frame))
      {
        left_frame = raw;
        have_stereo = left_frame.width == right_frame.width && left_frame.height == right_frame.height;
      }
    }
    else
    {
      have_stereo = SplitPsvrPackedStereo(raw, left_frame, right_frame);
      if (!have_stereo)
      {
        const bool obvious_sbs = raw.width >= raw.height * 2;
        if (opt.force_sbs || obvious_sbs)
          have_stereo = SplitSideBySide(raw, left_frame, right_frame);
      }
    }

    if (!have_stereo)
    {
      if (!warned_mono)
      {
        std::fprintf(stderr,
                     "Camera frame is %dx%d and is not auto-detected as side-by-side stereo. "
                     "LED detection can run, but XYZ needs stereo. Try --force-sbs or --camera-right N.\n",
                     raw.width, raw.height);
        warned_mono = true;
      }
      const auto mono = DetectLedBlobs(raw.gray, raw.width, raw.height);
      if (std::chrono::steady_clock::now() - last_print >= std::chrono::milliseconds(500))
      {
        const auto ls = left_hid.GetState();
        const auto rs = right_hid.GetState();
        std::printf("frame=%d MONO blobs=%zu thr=%.1f noise=%.1f HID L=%d pkt=%u R=%d pkt=%u\n",
                    frames, mono.blobs.size(), mono.threshold, mono.noise_sigma,
                    ls.connected ? 1 : 0, ls.packets, rs.connected ? 1 : 0, rs.packets);
        last_print = std::chrono::steady_clock::now();
      }
      ++frames;
      continue;
    }

    if (opt.swap_eyes)
      std::swap(left_frame, right_frame);
    cal.FillFallback(left_frame.width, left_frame.height);
    const auto tracking = TrackStereoLeds(left_frame, right_frame, cal);
    const auto ls = left_hid.GetState();
    const auto rs = right_hid.GetState();
    SubmitClusters(tracking, ls, rs, left_tracker, right_tracker);

    const auto now = std::chrono::steady_clock::now();
    if (now - last_print >= std::chrono::milliseconds(500))
    {
      std::printf("frame=%d eye=%dx%d blobs=%zu/%zu floor=%d/%d thr=%.1f/%.1f noise=%.1f/%.1f pairs=%zu clusters=%zu HID L=%d pkt=%u R=%d pkt=%u | ",
                  frames, left_frame.width, left_frame.height,
                  tracking.left_detection.blobs.size(), tracking.right_detection.blobs.size(),
                  tracking.left_detection.bright_floor, tracking.right_detection.bright_floor,
                  tracking.left_detection.threshold, tracking.right_detection.threshold,
                  tracking.left_detection.noise_sigma, tracking.right_detection.noise_sigma,
                  tracking.points.size(), tracking.controllers.size(),
                  ls.connected ? 1 : 0, ls.packets, rs.connected ? 1 : 0, rs.packets);
      PrintMeasurement("L", left_tracker.GetMeasurement());
      std::printf(" | ");
      PrintMeasurement("R", right_tracker.GetMeasurement());
      std::printf("\n");
      if (!opt.dump_prefix.empty())
      {
        SaveRawPgm(opt.dump_prefix + "-left-raw.pgm", left_frame);
        SaveRawPgm(opt.dump_prefix + "-right-raw.pgm", right_frame);
        SavePgm(opt.dump_prefix + "-left.pgm", left_frame, tracking.left_detection);
        SavePgm(opt.dump_prefix + "-right.pgm", right_frame, tracking.right_detection);
      }
      last_print = now;
    }
    ++frames;
  }

  left_hid.Stop();
  right_hid.Stop();
  cam_right.Close();
  cam_left.Close();
  std::printf("Done. frames=%d. Camera-space convention: +X right, +Y up, -Z forward.\n", frames);
  return 0;
}
