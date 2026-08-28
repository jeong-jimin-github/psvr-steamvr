#include "psvr_hw.h"
#include "driverlog.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <winusb.h>
#include <devguid.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
constexpr uint16_t kSonyVid = 0x054C;
constexpr uint16_t kPsvrPid = 0x09AF;

// Installed by the existing "PSVR Bridge" WinUSB INF (oem57.inf).
static const GUID GUID_PSVR_CONTROL =
    {0xA6B9C7D4, 0x2B0F, 0x4E48, {0x9E, 0x4B, 0x77, 0xC2, 0xE9, 0xD5, 0xA3, 0xF1}};

constexpr uint8_t kCmdPower = 0x17;
constexpr uint8_t kCmdTracking = 0x11;
constexpr uint8_t kCmdVrMode = 0x23;
constexpr uint8_t kMagic = 0xAA;

// OpenHMD axis remap: PSVR IMU -> Y-up, -Z forward (OpenVR).
void AccelFromSample(const int16_t smp[3], float out[3])
{
  constexpr float s = 9.81f / 16384.f;
  out[0] = static_cast<float>(smp[1]) * s;
  out[1] = static_cast<float>(smp[0]) * s;
  out[2] = static_cast<float>(smp[2]) * -s;
}

void GyroFromSample(const int16_t smp[3], float out[3])
{
  constexpr float s = 0.00105f;
  out[0] = static_cast<float>(smp[1]) * s;
  out[1] = static_cast<float>(smp[0]) * s;
  out[2] = static_cast<float>(smp[2]) * -s;
}

uint32_t TickDelta(uint32_t next, uint32_t last)
{
  uint32_t d = next - last;
  if (d > 0xffffff)
    d += 0x1000000;
  return d;
}

Quaternion Multiply(const Quaternion &a, const Quaternion &b)
{
  Quaternion r;
  r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return r;
}

std::wstring ToLower(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(), ::towlower);
  return s;
}

bool PathHasMi04(const std::wstring &path)
{
  const std::wstring p = ToLower(path);
  return p.find(L"vid_054c") != std::wstring::npos &&
         p.find(L"pid_09af") != std::wstring::npos &&
         p.find(L"mi_04") != std::wstring::npos;
}

HANDLE OpenDevicePath(const std::wstring &path, DWORD access)
{
  return CreateFileW(path.c_str(), access,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED, nullptr);
}
} // namespace

bool PsvrHardware::Open(bool start_sensor_thread)
{
  Close();
  if (!OpenControl())
  {
    FileLog("PSVR: failed to open WinUSB control interface (MI_05)");
    return false;
  }
  if (!OpenSensors())
  {
    FileLog("PSVR: failed to open HID sensor interface (MI_04)");
    Close();
    return false;
  }

  ahrs_.Reset();
  recenter_ = Quaternion{};
  have_tick_ = false;
  bias_samples_ = 0;
  bias_ready_ = false;
  gyro_bias_[0] = gyro_bias_[1] = gyro_bias_[2] = 0.f;

  {
    std::lock_guard<std::mutex> lock(pose_mutex_);
    pose_ = PsvrPose{};
    pose_.connected = true;
  }

  open_ = true;
  if (start_sensor_thread)
  {
    run_ = true;
    sensor_thread_ = std::thread(&PsvrHardware::SensorThread, this);
  }
  FileLog("PSVR: hardware opened");
  return true;
}

void PsvrHardware::Close()
{
  run_ = false;
  if (sensor_thread_.joinable())
    sensor_thread_.join();

  if (winusb_handle_)
  {
    WinUsb_Free(winusb_handle_);
    winusb_handle_ = nullptr;
  }
  if (control_handle_)
  {
    CloseHandle(control_handle_);
    control_handle_ = nullptr;
  }
  if (sensor_handle_)
  {
    CancelIo(sensor_handle_);
    CloseHandle(sensor_handle_);
    sensor_handle_ = nullptr;
  }
  open_ = false;
  std::lock_guard<std::mutex> lock(pose_mutex_);
  pose_.connected = false;
}

bool PsvrHardware::OpenControl()
{
  HDEVINFO info = SetupDiGetClassDevsW(&GUID_PSVR_CONTROL, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE)
  {
    FileLog("PSVR: SetupDiGetClassDevs control failed (%lu)", GetLastError());
    return false;
  }

  SP_DEVICE_INTERFACE_DATA iface{};
  iface.cbSize = sizeof(iface);
  bool ok = false;
  for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_PSVR_CONTROL, index, &iface); ++index)
  {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &needed, nullptr);
    std::vector<uint8_t> buf(needed);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, needed, nullptr, nullptr))
      continue;

    HANDLE h = OpenDevicePath(detail->DevicePath, GENERIC_READ | GENERIC_WRITE);
    if (h == INVALID_HANDLE_VALUE)
    {
      FileLog("PSVR: CreateFile control failed (%lu) path=%ls", GetLastError(), detail->DevicePath);
      continue;
    }

    WINUSB_INTERFACE_HANDLE winusb = nullptr;
    if (!WinUsb_Initialize(h, &winusb))
    {
      FileLog("PSVR: WinUsb_Initialize failed (%lu)", GetLastError());
      CloseHandle(h);
      continue;
    }

    USB_INTERFACE_DESCRIPTOR desc{};
    if (!WinUsb_QueryInterfaceSettings(winusb, 0, &desc))
    {
      FileLog("PSVR: QueryInterfaceSettings failed (%lu)", GetLastError());
      WinUsb_Free(winusb);
      CloseHandle(h);
      continue;
    }

    out_pipe_ = 0;
    for (UCHAR p = 0; p < desc.bNumEndpoints; ++p)
    {
      WINUSB_PIPE_INFORMATION pipe{};
      if (!WinUsb_QueryPipe(winusb, 0, p, &pipe))
        continue;
      FileLog("PSVR: control pipe type=%u id=0x%02x max=%u",
              pipe.PipeType, pipe.PipeId, pipe.MaximumPacketSize);
      const bool is_out = (pipe.PipeId & 0x80) == 0;
      if (is_out &&
          (pipe.PipeType == UsbdPipeTypeInterrupt || pipe.PipeType == UsbdPipeTypeBulk))
      {
        out_pipe_ = pipe.PipeId;
      }
    }

    ULONG timeout = 1000;
    if (out_pipe_)
      WinUsb_SetPipePolicy(winusb, out_pipe_, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    control_handle_ = h;
    winusb_handle_ = winusb;
    use_control_xfer_ = (out_pipe_ == 0);
    FileLog("PSVR: control open, out_pipe=0x%02x control_xfer=%d", out_pipe_, use_control_xfer_ ? 1 : 0);
    ok = true;
    break;
  }

  SetupDiDestroyDeviceInfoList(info);
  return ok;
}

bool PsvrHardware::OpenSensors()
{
  GUID hid_guid;
  HidD_GetHidGuid(&hid_guid);
  HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE)
    return false;

  SP_DEVICE_INTERFACE_DATA iface{};
  iface.cbSize = sizeof(iface);
  bool ok = false;
  for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, index, &iface); ++index)
  {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &needed, nullptr);
    std::vector<uint8_t> buf(needed);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, needed, nullptr, nullptr))
      continue;
    if (!PathHasMi04(detail->DevicePath))
      continue;

    HANDLE h = OpenDevicePath(detail->DevicePath, GENERIC_READ);
    if (h == INVALID_HANDLE_VALUE)
    {
      FileLog("PSVR: CreateFile sensors failed (%lu)", GetLastError());
      continue;
    }

    HIDD_ATTRIBUTES attr{};
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != kSonyVid || attr.ProductID != kPsvrPid)
    {
      CloseHandle(h);
      continue;
    }

    HidD_SetNumInputBuffers(h, 64);
    sensor_handle_ = h;
    FileLog("PSVR: sensors open %ls", detail->DevicePath);
    ok = true;
    break;
  }

  SetupDiDestroyDeviceInfoList(info);
  return ok;
}

bool PsvrHardware::WriteControl(const uint8_t *data, unsigned length)
{
  if (!winusb_handle_)
    return false;

  uint8_t packet[64]{};
  if (length > sizeof(packet))
    length = sizeof(packet);
  memcpy(packet, data, length);

  ULONG written = 0;
  if (!use_control_xfer_ && out_pipe_)
  {
    if (WinUsb_WritePipe(static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_),
                         out_pipe_, packet, sizeof(packet), &written, nullptr))
    {
      FileLog("PSVR: WritePipe cmd=0x%02x bytes=%lu", packet[0], written);
      return true;
    }
    FileLog("PSVR: WritePipe failed (%lu), falling back to control xfer", GetLastError());
    use_control_xfer_ = true;
  }

  WINUSB_SETUP_PACKET setup{};
  setup.RequestType = 0x21; // host-to-device | class | interface
  setup.Request = 0x09;     // SET_REPORT
  setup.Value = 0x0200;     // output report, id 0
  setup.Index = 5;          // interface 5
  setup.Length = static_cast<USHORT>(sizeof(packet));
  if (!WinUsb_ControlTransfer(static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_),
                              setup, packet, sizeof(packet), &written, nullptr))
  {
    FileLog("PSVR: ControlTransfer failed (%lu)", GetLastError());
    return false;
  }
  FileLog("PSVR: ControlTransfer cmd=0x%02x bytes=%lu", packet[0], written);
  return true;
}

bool PsvrHardware::PowerOn()
{
  const uint8_t cmd[] = {kCmdPower, 0x00, kMagic, 0x04, 0x01, 0x00, 0x00, 0x00};
  return WriteControl(cmd, sizeof(cmd));
}

bool PsvrHardware::PowerOff()
{
  const uint8_t cmd[] = {kCmdPower, 0x00, kMagic, 0x04, 0x00, 0x00, 0x00, 0x00};
  return WriteControl(cmd, sizeof(cmd));
}

bool PsvrHardware::SetVrMode(bool enabled)
{
  const uint8_t on = enabled ? 0x01 : 0x00;
  const uint8_t cmd[] = {kCmdVrMode, 0x00, kMagic, 0x04, on, 0x00, 0x00, 0x00};
  return WriteControl(cmd, sizeof(cmd));
}

bool PsvrHardware::EnableTracking()
{
  const uint8_t cmd[] = {kCmdTracking, 0x00, kMagic, 0x08, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
  return WriteControl(cmd, sizeof(cmd));
}

bool PsvrHardware::EnterVr()
{
  bool ok = PowerOn();
  Sleep(150);
  ok = EnableTracking() && ok;
  Sleep(50);
  ok = SetVrMode(true) && ok;
  FileLog("PSVR: EnterVr %s", ok ? "ok" : "FAILED");
  return ok;
}

void PsvrHardware::SensorThread()
{
  uint8_t buf[128];
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE h = static_cast<HANDLE>(sensor_handle_);

  while (run_.load())
  {
    ResetEvent(ov.hEvent);
    DWORD got = 0;
    BOOL queued = ReadFile(h, buf, sizeof(buf), &got, &ov);
    if (!queued)
    {
      const DWORD err = GetLastError();
      if (err != ERROR_IO_PENDING)
      {
        FileLog("PSVR: ReadFile sensors failed (%lu)", err);
        Sleep(20);
        continue;
      }
      const DWORD wait = WaitForSingleObject(ov.hEvent, 200);
      if (wait == WAIT_TIMEOUT)
      {
        CancelIoEx(h, &ov);
        WaitForSingleObject(ov.hEvent, 50);
        continue;
      }
      if (!GetOverlappedResult(h, &ov, &got, FALSE))
        continue;
    }
    if (got >= 64)
    {
      const uint8_t *pkt = buf;
      int size = static_cast<int>(got);
      // Windows may prefix a report ID of 0.
      if (got == 65 && buf[0] == 0)
      {
        pkt = buf + 1;
        size = 64;
      }
      HandleSensorPacket(pkt, size);
    }
  }
  CloseHandle(ov.hEvent);
}

void PsvrHardware::HandleSensorPacket(const uint8_t *buffer, int size)
{
  if (size < 64)
    return;

  const uint8_t buttons = buffer[0];
  const uint8_t state = buffer[6];
  const bool worn = (state & 0x01) != 0;
  const bool hdmi_disconnected = (state & 0x04) != 0;

  auto read16 = [](const uint8_t *p) -> int16_t {
    return static_cast<int16_t>(p[0] | (p[1] << 8));
  };
  auto read32 = [](const uint8_t *p) -> uint32_t {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
  };

  struct Sample
  {
    uint32_t tick;
    int16_t gyro[3];
    int16_t accel[3];
  } samples[2];

  // OpenHMD packet.c layout starting at offset 16.
  const uint8_t *p = buffer + 16;
  for (int i = 0; i < 2; ++i)
  {
    samples[i].tick = read32(p);
    p += 4;
    for (int a = 0; a < 3; ++a)
    {
      samples[i].gyro[a] = read16(p);
      p += 2;
    }
    for (int a = 0; a < 3; ++a)
    {
      samples[i].accel[a] = read16(p);
      p += 2;
    }
  }
  const uint16_t proximity = static_cast<uint16_t>(buffer[54] | (buffer[55] << 8));

  uint32_t tick_delta = 500;
  if (have_tick_)
  {
    tick_delta = TickDelta(samples[0].tick, last_tick_);
    if (tick_delta < 475 || tick_delta > 525)
      tick_delta = 500;
  }
  have_tick_ = true;

  for (int i = 0; i < 2; ++i)
  {
    float accel[3], gyro[3];
    AccelFromSample(samples[i].accel, accel);
    GyroFromSample(samples[i].gyro, gyro);

    if (!bias_ready_)
    {
      gyro_bias_[0] += gyro[0];
      gyro_bias_[1] += gyro[1];
      gyro_bias_[2] += gyro[2];
      ++bias_samples_;
      if (bias_samples_ >= 800) // ~0.4s at 2 kHz
      {
        gyro_bias_[0] /= static_cast<float>(bias_samples_);
        gyro_bias_[1] /= static_cast<float>(bias_samples_);
        gyro_bias_[2] /= static_cast<float>(bias_samples_);
        bias_ready_ = true;
        FileLog("PSVR: gyro bias %.5f %.5f %.5f", gyro_bias_[0], gyro_bias_[1], gyro_bias_[2]);
      }
    }
    else
    {
      gyro[0] -= gyro_bias_[0];
      gyro[1] -= gyro_bias_[1];
      gyro[2] -= gyro_bias_[2];
    }

    const float dt = static_cast<float>(tick_delta) * 1.0e-6f;
    ahrs_.Update(gyro[0], gyro[1], gyro[2], accel[0], accel[1], accel[2], dt);
    if (i == 0)
      tick_delta = TickDelta(samples[1].tick, samples[0].tick);
  }
  last_tick_ = samples[1].tick;

  const Quaternion raw = ahrs_.Orientation();
  const Quaternion oriented = Multiply(recenter_, raw);

  std::lock_guard<std::mutex> lock(pose_mutex_);
  pose_.rotation = oriented;
  pose_.connected = true;
  // The sensor report's state bit is the debounced on-head signal. The raw
  // 16-bit proximity field rests around 39k on this unit, so treating values
  // above 300 as "worn" made SteamVR seize focus for the entire session.
  pose_.worn = worn;
  pose_.hdmi_ok = !hdmi_disconnected;
  pose_.buttons = buttons;
  pose_.proximity = proximity;
}

PsvrPose PsvrHardware::GetPose() const
{
  std::lock_guard<std::mutex> lock(pose_mutex_);
  return pose_;
}

void PsvrHardware::Recenter()
{
  std::lock_guard<std::mutex> lock(pose_mutex_);
  const Quaternion q = ahrs_.Orientation();
  // Current look direction becomes OpenVR forward (identity).
  recenter_.w = q.w;
  recenter_.x = -q.x;
  recenter_.y = -q.y;
  recenter_.z = -q.z;
  const Quaternion oriented = Multiply(recenter_, q);
  pose_.rotation = oriented;
  FileLog("PSVR: recenter raw=(%.3f %.3f %.3f %.3f) -> (%.3f %.3f %.3f %.3f)",
          q.w, q.x, q.y, q.z,
          oriented.w, oriented.x, oriented.y, oriented.z);
}

bool PsvrHardware::ReadRawSensor(uint8_t out[64])
{
  if (!sensor_handle_)
    return false;
  uint8_t buf[128]{};
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD got = 0;
  HANDLE h = static_cast<HANDLE>(sensor_handle_);
  BOOL queued = ReadFile(h, buf, sizeof(buf), &got, &ov);
  if (!queued)
  {
    if (GetLastError() != ERROR_IO_PENDING)
    {
      CloseHandle(ov.hEvent);
      return false;
    }
    if (WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0)
    {
      CancelIoEx(h, &ov);
      CloseHandle(ov.hEvent);
      return false;
    }
    if (!GetOverlappedResult(h, &ov, &got, FALSE))
    {
      CloseHandle(ov.hEvent);
      return false;
    }
  }
  CloseHandle(ov.hEvent);
  const uint8_t *pkt = buf;
  if (got == 65 && buf[0] == 0)
    pkt = buf + 1;
  if (got < 64)
    return false;
  memcpy(out, pkt, 64);
  return true;
}

PsvrDisplayInfo PsvrHardware::FindHeadsetDisplay()
{
  PsvrDisplayInfo result{};
  DISPLAY_DEVICEW adapter{};
  adapter.cb = sizeof(adapter);

  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
  {
    if (!(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE))
      continue;

    DISPLAY_DEVICEW monitor{};
    monitor.cb = sizeof(monitor);
    for (DWORD m = 0; EnumDisplayDevicesW(adapter.DeviceName, m, &monitor, 0); ++m)
    {
      const std::wstring id = ToLower(monitor.DeviceID);
      const std::wstring name = ToLower(monitor.DeviceString);
      const bool match = id.find(L"sny6a04") != std::wstring::npos ||
                         name.find(L"sie") != std::wstring::npos ||
                         (name.find(L"hmd") != std::wstring::npos && name.find(L"generic") == std::wstring::npos);
      if (!match)
        continue;

      DEVMODEW dm{};
      dm.dmSize = sizeof(dm);
      if (!EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
        continue;

      result.found = true;
      result.x = dm.dmPosition.x;
      result.y = dm.dmPosition.y;
      result.width = static_cast<int>(dm.dmPelsWidth);
      result.height = static_cast<int>(dm.dmPelsHeight);
      result.refresh_hz = dm.dmDisplayFrequency > 0 ? static_cast<int>(dm.dmDisplayFrequency) : 60;
      result.device_name = adapter.DeviceName;
      result.monitor_name = monitor.DeviceString;
      FileLog("PSVR: display %ls %dx%d@%d at (%d,%d)",
              result.monitor_name.c_str(), result.width, result.height,
              result.refresh_hz, result.x, result.y);
      return result;
    }
  }

  // Fallback: first non-primary 1920x1080 display.
  adapter = {};
  adapter.cb = sizeof(adapter);
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i)
  {
    if (!(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE))
      continue;
    if (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
      continue;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
      continue;
    if (dm.dmPelsWidth == 1920 && dm.dmPelsHeight == 1080)
    {
      result.found = true;
      result.x = dm.dmPosition.x;
      result.y = dm.dmPosition.y;
      result.width = 1920;
      result.height = 1080;
      result.refresh_hz = dm.dmDisplayFrequency > 0 ? static_cast<int>(dm.dmDisplayFrequency) : 60;
      result.device_name = adapter.DeviceName;
      result.monitor_name = adapter.DeviceString;
      FileLog("PSVR: fallback display %dx%d@%d at (%d,%d)",
              result.width, result.height, result.refresh_hz, result.x, result.y);
      return result;
    }
  }

  FileLog("PSVR: headset display not found");
  return result;
}
