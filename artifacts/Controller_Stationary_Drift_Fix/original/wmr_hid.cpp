#include "wmr_hid.h"
#include "driverlog.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <vector>

namespace
{
struct HidCandidate
{
  std::wstring path;
  std::wstring product;
  std::wstring serial;
  bool left = false;
};

std::string Narrow(const std::wstring &s)
{
  if (s.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring Lower(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(), towlower);
  while (!s.empty() && s.back() == L'\0')
    s.pop_back();
  return s;
}

bool SameHidPath(std::wstring a, std::wstring b)
{
  a = Lower(std::move(a));
  b = Lower(std::move(b));
  if (a == b)
    return true;
  const auto strip = [](const std::wstring &s) {
    const auto p = s.find(L"hid");
    return p == std::wstring::npos ? s : s.substr(p);
  };
  return strip(a) == strip(b);
}

std::wstring ExtractMac(const std::wstring &id)
{
  const std::wstring lower = Lower(id);
  const auto take12 = [&](size_t pos) -> std::wstring {
    if (pos + 12 > lower.size())
      return {};
    for (size_t j = 0; j < 12; ++j)
      if (!iswxdigit(lower[pos + j]))
        return {};
    return lower.substr(pos, 12);
  };
  const auto dev = lower.find(L"dev_");
  if (dev != std::wstring::npos)
  {
    const std::wstring mac = take12(dev + 4);
    if (!mac.empty())
      return mac;
  }
  for (size_t i = 0; i + 13 <= lower.size(); ++i)
  {
    if (lower[i] != L'&')
      continue;
    const std::wstring mac = take12(i + 1);
    if (mac.empty())
      continue;
    const wchar_t next = (i + 13 < lower.size()) ? lower[i + 13] : L'\0';
    if (next == L'\0' || next == L'_' || next == L'\\' )
      return mac;
  }
  return {};
}

std::unordered_map<std::wstring, bool> BluetoothHandedness()
{
  std::unordered_map<std::wstring, bool> out;
  HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_BLUETOOTH, nullptr, nullptr, DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE)
    return out;
  SP_DEVINFO_DATA info{};
  info.cbSize = sizeof(info);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i)
  {
    wchar_t name[256]{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr,
                                           reinterpret_cast<PBYTE>(name), sizeof(name), nullptr))
      continue;
    const std::wstring lower_name = Lower(name);
    const bool left = lower_name.find(L"left") != std::wstring::npos ||
                      lower_name.find(L"왼쪽") != std::wstring::npos;
    const bool right = lower_name.find(L"right") != std::wstring::npos ||
                       lower_name.find(L"오른쪽") != std::wstring::npos;
    if (left == right)
      continue;
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(info.DevInst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
      continue;
    const std::wstring mac = ExtractMac(id);
    if (!mac.empty())
      out[mac] = left;
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

std::vector<HidCandidate> EnumerateOdysseyControllers(bool force = false)
{
  static std::mutex cache_mutex;
  static std::vector<HidCandidate> cache;
  static std::chrono::steady_clock::time_point cache_at{};
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!force && !cache.empty() && now - cache_at < std::chrono::milliseconds(750))
      return cache;
  }

  const auto handed = BluetoothHandedness();
  GUID guid{};
  HidD_GetHidGuid(&guid);
  HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  std::vector<HidCandidate> out;
  if (set == INVALID_HANDLE_VALUE)
    return out;

  for (DWORD i = 0;; ++i)
  {
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &iface))
      break;
    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &required, nullptr);
    if (required == 0)
      continue;
    std::vector<uint8_t> bytes(required);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(bytes.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, required, nullptr, &devinfo))
      continue;

    std::wstring path_lower = Lower(detail->DevicePath);
    const bool path_match = path_lower.find(L"vid&0002045e_pid&065d") != std::wstring::npos ||
                            (path_lower.find(L"vid_045e") != std::wstring::npos &&
                             path_lower.find(L"pid_065d") != std::wstring::npos);
    if (!path_match)
      continue;

    HidCandidate c;
    c.path = detail->DevicePath;

    std::wstring parent_id;
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, devinfo.DevInst, 0) == CR_SUCCESS)
    {
      wchar_t id[MAX_DEVICE_ID_LEN]{};
      if (CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS)
        parent_id = Lower(id);
    }
    const std::wstring mac = ExtractMac(parent_id);
    const auto handed_it = mac.empty() ? handed.end() : handed.find(mac);
    if (handed_it != handed.end())
      c.left = handed_it->second;
    else if (c.product.find(L"Left") != std::wstring::npos || c.product.find(L"left") != std::wstring::npos)
      c.left = true;
    else if (c.product.find(L"Right") != std::wstring::npos || c.product.find(L"right") != std::wstring::npos)
      c.left = false;
    else
      c.left = out.empty();
    out.push_back(std::move(c));
  }
  SetupDiDestroyDeviceInfoList(set);
  std::sort(out.begin(), out.end(), [](const HidCandidate &a, const HidCandidate &b) { return a.path < b.path; });
  if (out.size() == 2 && out[0].left == out[1].left)
  {
    out[0].left = true;
    out[1].left = false;
  }

  static std::atomic<bool> logged{false};
  if (!out.empty() && !logged.exchange(true))
  {
    for (const auto &c : out)
      DriverLog("Odyssey HID mapped hand=%s product=%s path=%s", c.left ? "left" : "right",
                Narrow(c.product).c_str(), Narrow(c.path).c_str());
  }

  std::lock_guard<std::mutex> lock(cache_mutex);
  cache = out;
  cache_at = std::chrono::steady_clock::now();
  return out;
}

int32_t Read24(const uint8_t *p)
{
  // WMR controller IMU samples are signed 24-bit big-endian values. The
  // surrounding report fields are little-endian, which previously hid this
  // exception and made each gyro axis decode into unrelated magnitudes.
  int32_t v = int32_t(p[2]) | (int32_t(p[1]) << 8) | (int32_t(p[0]) << 16);
  if (v & 0x00800000)
    v |= static_cast<int32_t>(0xff000000);
  return v;
}

uint32_t Read32(const uint8_t *p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

Quaternion Multiply(const Quaternion &a, const Quaternion &b)
{
  Quaternion q;
  q.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  q.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  q.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  q.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return q;
}

void RotateVec(const Quaternion &q, float x, float y, float z, float out[3])
{
  const float tx = 2.f * (q.y * z - q.z * y);
  const float ty = 2.f * (q.z * x - q.x * z);
  const float tz = 2.f * (q.x * y - q.y * x);
  out[0] = x + q.w * tx + (q.y * tz - q.z * ty);
  out[1] = y + q.w * ty + (q.z * tx - q.x * tz);
  out[2] = z + q.w * tz + (q.x * ty - q.y * tx);
}

float YawFromForward(const Quaternion &q)
{
  float f[3];
  RotateVec(q, 0.f, 0.f, -1.f, f);
  return std::atan2(f[0], -f[2]);
}

Quaternion YawQuat(float yaw)
{
  const float half = 0.5f * yaw;
  return {std::cos(half), 0.f, std::sin(half), 0.f};
}

bool Write64(HANDLE h, const uint8_t *prefix, size_t count)
{
  uint8_t report[64]{};
  memcpy(report, prefix, count);
  DWORD written = 0;
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  BOOL ok = WriteFile(h, report, sizeof(report), &written, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING)
  {
    if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0)
      ok = GetOverlappedResult(h, &ov, &written, FALSE);
    else
      CancelIoEx(h, &ov);
  }
  CloseHandle(ov.hEvent);
  return ok && written == sizeof(report);
}
}

WmrHidController::WmrHidController(bool left) : left_(left)
{
  state_.left = left;
}

std::mutex WmrHidController::registry_mutex_;
WmrHidController *WmrHidController::left_instance_ = nullptr;
WmrHidController *WmrHidController::right_instance_ = nullptr;
std::atomic<bool> WmrHidController::raw_run_{false};
std::thread WmrHidController::raw_thread_;

WmrHidController::~WmrHidController()
{
  Stop();
}

bool WmrHidController::Start()
{
  Stop();
  run_ = true;
  ahrs_.Reset();
  ahrs_.SetBeta(0.05f);
  recenter_ = {};
  have_ticks_ = false;
  last_packet_ = {};
  gyro_bias_[0] = gyro_bias_[1] = gyro_bias_[2] = 0.f;
  bias_samples_ = 0;
  bias_ready_ = false;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex_);
    (left_ ? left_instance_ : right_instance_) = this;
    if (!raw_run_.exchange(true))
      raw_thread_ = std::thread(&WmrHidController::RawInputThreadMain);
  }
  DriverLog("Odyssey %s using Raw Input (Windows Bluetooth HID denies exclusive open)",
            left_ ? "left" : "right");
  return true;
}

void WmrHidController::Stop()
{
  run_ = false;
  if (thread_.joinable())
    thread_.join();
  bool stop_raw = false;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex_);
    WmrHidController *&slot = left_ ? left_instance_ : right_instance_;
    if (slot == this)
      slot = nullptr;
    if (!left_instance_ && !right_instance_)
      stop_raw = raw_run_.exchange(false);
  }
  if (stop_raw && raw_thread_.joinable())
    raw_thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  state_.connected = false;
}

WmrControllerState WmrHidController::GetState() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  WmrControllerState s = state_;
  if (s.connected)
  {
    const auto age = std::chrono::steady_clock::now() - last_packet_;
    if (last_packet_.time_since_epoch().count() == 0 ||
        age > std::chrono::milliseconds(1500))
    {
      s.connected = false;
      s.packets = state_.packets;
    }
  }
  return s;
}

void WmrHidController::Recenter(const Quaternion &hmd_rotation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const Quaternion raw = ahrs_.Orientation();
  const Quaternion conj{raw.w, -raw.x, -raw.y, -raw.z};
  recenter_ = Multiply(hmd_rotation, conj);
  state_.rotation = Multiply(recenter_, raw);
  DriverLog("Odyssey %s recentered to HMD orientation", left_ ? "left" : "right");
}

void WmrHidController::ThreadMain()
{
  while (run_.load())
  {
    ConnectionAttempt();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.connected = false;
    }
    for (int i = 0; i < 10 && run_.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void WmrHidController::RawInputThreadMain()
{
  const wchar_t *class_name = L"PSVR_Odyssey_RawInput_Window";
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = class_name;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    DriverLog("Odyssey Raw Input RegisterClass failed error=%lu", GetLastError());
    raw_run_ = false;
    return;
  }
  HWND window = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;
  rid.usUsage = 0x0f;
  rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
  rid.hwndTarget = window;
  if (!window || !RegisterRawInputDevices(&rid, 1, sizeof(rid)))
  {
    DriverLog("Odyssey Raw Input fallback failed error=%lu", GetLastError());
    if (window)
      DestroyWindow(window);
    raw_run_ = false;
    return;
  }
  DriverLog("Odyssey Raw Input fallback registered usage=0001:000F");

  std::vector<HidCandidate> candidates = EnumerateOdysseyControllers(true);
  std::unordered_map<HANDLE, WmrHidController *> device_map;
  auto last_refresh = std::chrono::steady_clock::now();
  const auto refresh_candidates = [&]() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh < std::chrono::milliseconds(1500) && !candidates.empty())
      return;
    last_refresh = now;
    candidates = EnumerateOdysseyControllers(true);
    device_map.clear();
  };
  const auto bind_device = [&](HANDLE hid_device) -> WmrHidController * {
    const auto cached = device_map.find(hid_device);
    if (cached != device_map.end())
      return cached->second;
    UINT name_chars = 0;
    GetRawInputDeviceInfoW(hid_device, RIDI_DEVICENAME, nullptr, &name_chars);
    std::wstring device_name(name_chars ? name_chars : 1, L'\0');
    if (name_chars)
      GetRawInputDeviceInfoW(hid_device, RIDI_DEVICENAME, device_name.data(), &name_chars);
    if (!device_name.empty() && device_name.back() == L'\0')
      device_name.pop_back();
    WmrHidController *target = nullptr;
    for (const auto &candidate : candidates)
    {
      if (!SameHidPath(candidate.path, device_name))
        continue;
      std::lock_guard<std::mutex> registry_lock(registry_mutex_);
      target = candidate.left ? left_instance_ : right_instance_;
      break;
    }
    if (target)
      device_map[hid_device] = target;
    return target;
  };

  while (raw_run_.load())
  {
    MSG msg{};
    bool handled = false;
    while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_INPUT_DEVICE_CHANGE)
      {
        refresh_candidates();
        continue;
      }
      if (msg.message != WM_INPUT)
        continue;
      UINT bytes = 0;
      GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT, nullptr, &bytes,
                      sizeof(RAWINPUTHEADER));
      std::vector<uint8_t> input(bytes);
      if (!bytes || GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT,
                                    input.data(), &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1))
        continue;
      const RAWINPUT *raw = reinterpret_cast<const RAWINPUT *>(input.data());
      if (raw->header.dwType != RIM_TYPEHID)
        continue;

      WmrHidController *target = bind_device(raw->header.hDevice);
      if (!target)
      {
        refresh_candidates();
        target = bind_device(raw->header.hDevice);
      }
      if (!target || !target->run_.load())
        continue;

      const RAWHID &hid = raw->data.hid;
      for (DWORD report_index = 0; report_index < hid.dwCount; ++report_index)
      {
        const uint8_t *report = hid.bRawData + report_index * hid.dwSizeHid;
        if (hid.dwSizeHid >= 44 && report[0] == 0x01)
          target->HandleReport(report, static_cast<int>(hid.dwSizeHid));
        else if (hid.dwSizeHid >= 44)
        {
          uint8_t with_id[64]{0x01};
          const int n = std::min(static_cast<int>(hid.dwSizeHid), 63);
          memcpy(with_id + 1, report, n);
          target->HandleReport(with_id, n + 1);
        }
      }
      handled = true;
    }
    if (!handled)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  rid.dwFlags = RIDEV_REMOVE;
  rid.hwndTarget = nullptr;
  RegisterRawInputDevices(&rid, 1, sizeof(rid));
  DestroyWindow(window);
  UnregisterClassW(class_name, wc.hInstance);
}

bool WmrHidController::ConnectionAttempt()
{
  const auto candidates = EnumerateOdysseyControllers();
  if (candidates.empty())
  {
    static std::atomic<int> misses{0};
    if ((++misses % 10) == 1)
      DriverLog("Odyssey HID scan: no VID_045E PID_065D interfaces");
    return false;
  }

  const HidCandidate *chosen = nullptr;
  for (const auto &c : candidates)
    if (c.left == left_)
      chosen = &c;
  if (!chosen && candidates.size() >= 2)
    chosen = &candidates[left_ ? 0 : 1];
  if (!chosen && candidates.size() == 1 && left_)
    chosen = &candidates[0];
  if (!chosen)
    return false;

  bool can_write = true;
  HANDLE h = CreateFileW(chosen->path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  DWORD read_write_error = ERROR_SUCCESS;
  if (h == INVALID_HANDLE_VALUE)
  {
    read_write_error = GetLastError();
    // Windows' Bluetooth HID stack owns the output side of paired WMR
    // controllers.  It still shares the input stream, so fall back to a
    // read-only handle and use the stack's already-enabled report stream.
    can_write = false;
    h = CreateFileW(chosen->path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  }
  if (h == INVALID_HANDLE_VALUE)
  {
    const DWORD read_only_error = GetLastError();
    static std::atomic<int> open_failures{0};
    if ((++open_failures % 10) == 1)
      DriverLog("Odyssey %s HID open failed rw_error=%lu ro_error=%lu path=%s", left_ ? "left" : "right",
                read_write_error, read_only_error, Narrow(chosen->path).c_str());
    return false;
  }

  DriverLog("Odyssey %s HID opened access=%s product=%s serial=%s candidates=%d",
            left_ ? "left" : "right", can_write ? "read-write" : "read-only", Narrow(chosen->product).c_str(),
            Narrow(chosen->serial).c_str(), static_cast<int>(candidates.size()));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.connected = true;
    state_.product = Narrow(chosen->product);
    state_.serial = Narrow(chosen->serial);
  }

  const uint8_t reset[] = {0x06, 0x00, 0x00, 0x00};
  const uint8_t restart[] = {0x06, 0x04, 0xc1, 0x02};
  const uint8_t status_on[] = {0x06, 0x03, 0x01, 0x00, 0x02};
  const uint8_t imu_on[] = {0x06, 0x03, 0x02, 0xe1, 0x02};
  if (can_write)
  {
    Write64(h, reset, sizeof(reset));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Write64(h, restart, sizeof(restart));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Write64(h, status_on, sizeof(status_on));
    Write64(h, imu_on, sizeof(imu_on));
  }

  while (run_.load())
  {
    uint8_t report[256]{};
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = ReadFile(h, report, sizeof(report), &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
      const DWORD wait = WaitForSingleObject(ov.hEvent, 600);
      if (wait == WAIT_OBJECT_0)
        ok = GetOverlappedResult(h, &ov, &got, FALSE);
      else
        CancelIoEx(h, &ov);
    }
    CloseHandle(ov.hEvent);
    if (ok && got > 0)
      HandleReport(report, static_cast<int>(got));
    else if (!ok && GetLastError() != ERROR_OPERATION_ABORTED)
      break;
  }
  CloseHandle(h);
  DriverLog("Odyssey %s HID disconnected", left_ ? "left" : "right");
  return true;
}

void WmrHidController::HandleReport(const uint8_t *data, int size)
{
  if (size < 45 || data[0] != 0x01)
    return;
  const uint8_t *p = data + 1;
  WmrControllerState s;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s = state_;
  }

  const uint8_t buttons = p[0];
  s.thumbstick_click = (buttons & 0x01) != 0;
  s.home = (buttons & 0x02) != 0;
  s.menu = (buttons & 0x04) != 0;
  s.squeeze = (buttons & 0x08) != 0;
  s.trackpad_click = (buttons & 0x10) != 0;
  s.trackpad_touch = (buttons & 0x40) != 0;

  int stick_x = p[1] | ((p[2] & 0x0f) << 8);
  int stick_y = (p[2] >> 4) | (p[3] << 4);
  s.thumbstick_x = std::clamp((stick_x - 2047) / 2047.f, -1.f, 1.f);
  s.thumbstick_y = std::clamp((stick_y - 2047) / 2047.f, -1.f, 1.f);
  s.trigger = p[4] / 255.f;
  s.trackpad_x = p[5] == 0xff ? 0.f : (int(p[5]) - 50) / 50.f;
  s.trackpad_y = p[6] == 0xff ? 0.f : (int(p[6]) - 50) / 50.f;
  s.battery = p[7];

  // WMR body frame is Y-down / Z-forward. OpenVR is Y-up / -Z-forward.
  // 180° about X (Monado P_oxr_wmr) maps (x,y,z) -> (x,-y,-z).
  const float ax = Read24(p + 8) / 49000.f;
  const float ay = -Read24(p + 11) / 49000.f;
  const float az = -Read24(p + 14) / 49000.f;
  float gx = Read24(p + 19) * 0.00001f;
  float gy = -Read24(p + 22) * 0.00001f;
  float gz = -Read24(p + 25) * 0.00001f;
  const float gmag = std::sqrt(gx * gx + gy * gy + gz * gz);
  if (!bias_ready_)
  {
    if (gmag < 0.25f)
    {
      gyro_bias_[0] += gx;
      gyro_bias_[1] += gy;
      gyro_bias_[2] += gz;
      ++bias_samples_;
      if (bias_samples_ >= 250)
      {
        gyro_bias_[0] /= static_cast<float>(bias_samples_);
        gyro_bias_[1] /= static_cast<float>(bias_samples_);
        gyro_bias_[2] /= static_cast<float>(bias_samples_);
        bias_ready_ = true;
        DriverLog("Odyssey %s gyro bias %.5f %.5f %.5f", left_ ? "left" : "right",
                  gyro_bias_[0], gyro_bias_[1], gyro_bias_[2]);
      }
    }
  }
  else
  {
    const float ux = gx - gyro_bias_[0];
    const float uy = gy - gyro_bias_[1];
    const float uz = gz - gyro_bias_[2];
    if (std::sqrt(ux * ux + uy * uy + uz * uz) < 0.08f)
    {
      const float a = 0.002f;
      gyro_bias_[0] += a * ux;
      gyro_bias_[1] += a * uy;
      gyro_bias_[2] += a * uz;
    }
    gx = gx - gyro_bias_[0];
    gy = gy - gyro_bias_[1];
    gz = gz - gyro_bias_[2];
  }
  const uint32_t ticks = Read32(p + 28);
  float dt = 0.001f;
  if (have_ticks_)
    dt = std::clamp((ticks - last_ticks_) * 0.0000001f, 0.0001f, 0.02f);
  last_ticks_ = ticks;
  have_ticks_ = true;

  ahrs_.Update(gx, gy, gz, ax, ay, az, dt);
  s.rotation = Multiply(recenter_, ahrs_.Orientation());
  s.connected = true;
  ++s.packets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (s.home != state_.home || s.menu != state_.menu)
      DriverLog("Odyssey %s button home=%d menu=%d raw=0x%02X", left_ ? "left" : "right", s.home ? 1 : 0,
                s.menu ? 1 : 0, buttons);
    state_ = s;
    last_packet_ = std::chrono::steady_clock::now();
  }
  if (s.packets == 1 || (s.packets % 1000) == 0)
    DriverLog("Odyssey %s streaming packets=%u battery=%u buttons=0x%02X home=%d menu=%d",
              left_ ? "left" : "right", s.packets, s.battery, buttons, s.home ? 1 : 0, s.menu ? 1 : 0);
}
