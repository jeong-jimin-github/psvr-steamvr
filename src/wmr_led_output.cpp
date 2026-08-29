#include "wmr_led_output.h"
#include "wmr_led_protocol.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <sstream>
#include <vector>

namespace
{
struct Candidate
{
  std::wstring path;
  std::wstring product;
  bool left = false;
};

std::wstring Lower(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(), towlower);
  return s;
}

std::vector<Candidate> Enumerate()
{
  GUID hid_guid{};
  HidD_GetHidGuid(&hid_guid);
  HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
    return {};

  std::vector<Candidate> out;
  SP_DEVICE_INTERFACE_DATA iface{};
  iface.cbSize = sizeof(iface);
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, i, &iface); ++i)
  {
    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &required, nullptr);
    if (!required)
      continue;
    std::vector<uint8_t> storage(required);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, required, nullptr, nullptr))
      continue;

    HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      continue;
    HIDD_ATTRIBUTES attr{};
    attr.Size = sizeof(attr);
    const bool match = HidD_GetAttributes(h, &attr) && attr.VendorID == 0x045e && attr.ProductID == 0x065d;
    wchar_t product[256]{};
    if (match)
      HidD_GetProductString(h, product, sizeof(product));
    CloseHandle(h);
    if (!match)
      continue;

    Candidate c;
    c.path = detail->DevicePath;
    c.product = product;
    const auto p = Lower(c.product);
    if (p.find(L"left") != std::wstring::npos)
      c.left = true;
    else if (p.find(L"right") != std::wstring::npos)
      c.left = false;
    else
      c.left = out.empty();
    out.push_back(std::move(c));
  }
  SetupDiDestroyDeviceInfoList(set);
  std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) { return a.path < b.path; });
  if (out.size() == 2 && out[0].left == out[1].left)
  {
    out[0].left = true;
    out[1].left = false;
  }
  return out;
}
}

bool TrySetOdysseyLedIntensity(bool left, int intensity, std::string &status)
{
  const auto candidates = Enumerate();
  const Candidate *chosen = nullptr;
  for (const auto &c : candidates)
  {
    if (c.left == left)
    {
      chosen = &c;
      break;
    }
  }
  if (!chosen)
  {
    status = left ? "left WMR HID not found" : "right WMR HID not found";
    return false;
  }

  HANDLE h = CreateFileW(chosen->path.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE)
  {
    std::ostringstream ss;
    ss << "Windows denied WMR HID output handle (error " << GetLastError() << "); continuing read-only";
    status = ss.str();
    return false;
  }

  const uint64_t now_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  const auto packet = BuildWmrLedTimesyncPacket(1, 1, intensity, now_us, 500, 1);
  std::array<uint8_t, 64> report{};
  std::copy(packet.begin(), packet.end(), report.begin());

  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD written = 0;
  BOOL ok = WriteFile(h, report.data(), static_cast<DWORD>(report.size()), &written, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING)
  {
    if (WaitForSingleObject(ov.hEvent, 400) == WAIT_OBJECT_0)
      ok = GetOverlappedResult(h, &ov, &written, FALSE);
    else
      CancelIoEx(h, &ov);
  }
  const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(ov.hEvent);
  CloseHandle(h);

  if (!ok || written != report.size())
  {
    std::ostringstream ss;
    ss << "WMR LED output failed (error " << err << ", bytes " << written << "); continuing without LED control";
    status = ss.str();
    return false;
  }
  std::ostringstream ss;
  ss << (left ? "left" : "right") << " WMR LED intensity packet sent: " << std::clamp(intensity, 1, 399);
  status = ss.str();
  return true;
}
