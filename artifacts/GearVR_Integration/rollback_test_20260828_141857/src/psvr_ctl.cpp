#include "gearvr_ble.h"
#include "psvr_hw.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

static void PrintUsage()
{
  std::puts(
      "psvr_ctl — PSVR processing-unit control\n"
      "  psvr_ctl status     Open USB, print display + one IMU packet\n"
      "  psvr_ctl on         Power headset on\n"
      "  psvr_ctl off        Power headset off\n"
      "  psvr_ctl vr         Power on + VR mode + tracking LEDs\n"
      "  psvr_ctl cinema     Cinematic mode\n"
      "  psvr_ctl sensors    Dump 10 IMU packets\n"
      "  psvr_ctl gearvr     Scan/connect Gear VR controller (BLE, no Windows pairing)\n");
}

int main(int argc, char **argv)
{
  const std::string cmd = (argc > 1) ? argv[1] : "status";

  if (cmd == "gearvr" || cmd == "gearvr-scan")
  {
    std::puts("Scanning BLE 15s — hold Gear VR HOME until the LED blinks...");
    auto found = GearVrBle::Scan(15000);
    if (found.empty())
      std::puts("No BLE advertisements seen.");
    for (auto &l : found)
      std::puts(l.c_str());
    if (cmd == "gearvr-scan")
      return found.empty() ? 1 : 0;

    std::puts("Connecting (20s to appear)...");
    GearVrBle ble;
    ble.Start();
    for (int i = 0; i < 40 && !ble.IsConnected(); ++i)
      Sleep(500);
    if (!ble.IsConnected())
    {
      std::puts("Did not connect. Keep the controller in pairing mode and retry.");
      ble.Stop();
      return 1;
    }
    std::puts("Connected. Press buttons for 15s...");
    for (int i = 0; i < 30; ++i)
    {
      auto s = ble.GetState();
      std::printf("pkt=%u trig=%d home=%d back=%d pad=%d,%.2f,%.2f vol=%d/%d\n",
                  s.packets, s.trigger, s.home, s.back, s.touch_click, s.touch_x, s.touch_y,
                  s.vol_up, s.vol_down);
      Sleep(500);
    }
    ble.Stop();
    return 0;
  }

  PsvrHardware hw;
  const bool start_thread = (cmd != "sensors");
  if (!hw.Open(start_thread))
  {
    std::fprintf(stderr, "Could not open PSVR (VID 054C PID 09AF).\n");
    std::fprintf(stderr, "Need WinUSB on PS VR Control (MI_05) and HID on sensors (MI_04).\n");
    return 1;
  }

  const PsvrDisplayInfo d = PsvrHardware::FindHeadsetDisplay();
  if (d.found)
    std::printf("Display: %ls  %dx%d@%d  pos=(%d,%d)\n",
                d.monitor_name.c_str(), d.width, d.height, d.refresh_hz, d.x, d.y);
  else
    std::puts("Display: not found (is HDMI connected and extended?)");

  if (cmd == "on")
  {
    std::puts(hw.PowerOn() ? "power on sent" : "power on FAILED");
  }
  else if (cmd == "off")
  {
    std::puts(hw.PowerOff() ? "power off sent" : "power off FAILED");
  }
  else if (cmd == "vr")
  {
    std::puts(hw.EnterVr() ? "VR mode sent" : "VR mode FAILED");
  }
  else if (cmd == "cinema")
  {
    std::puts(hw.SetVrMode(false) ? "cinematic mode sent" : "cinematic FAILED");
  }
  else if (cmd == "sensors")
  {
    for (int i = 0; i < 10; ++i)
    {
      uint8_t pkt[64]{};
      if (!hw.ReadRawSensor(pkt))
      {
        std::puts("sensor read failed");
        break;
      }
      std::printf("%02d:", i);
      for (int b = 0; b < 64; ++b)
        std::printf(" %02X", pkt[b]);
      std::puts("");
    }
  }
  else
  {
    std::puts(hw.EnterVr() ? "VR mode sent" : "VR mode FAILED");
    Sleep(200);
    const PsvrPose p = hw.GetPose();
    std::printf("connected=%d worn=%d hdmi_ok=%d buttons=0x%02X proximity=%u\n",
                p.connected ? 1 : 0, p.worn ? 1 : 0, p.hdmi_ok ? 1 : 0, p.buttons, p.proximity);
    std::printf("quat w=%.4f x=%.4f y=%.4f z=%.4f\n", p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z);
  }

  hw.Close();
  return 0;
}
