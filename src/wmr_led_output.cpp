#include "wmr_led_output.h"
#include "wmr_led_protocol.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <sstream>
#include <unordered_map>
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
  while (!s.empty() && s.back() == L'\0')
    s.pop_back();
  return s;
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
    const auto mac = take12(dev + 4);
    if (!mac.empty())
      return mac;
  }
  for (size_t i = 0; i + 13 <= lower.size(); ++i)
  {
    if (lower[i] != L'&')
      continue;
    const auto mac = take12(i + 1);
    if (mac.empty())
      continue;
    const wchar_t next = (i + 13 < lower.size()) ? lower[i + 13] : L'\0';
    if (next == L'\0' || next == L'_' || next == L'\\')
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
    const auto lower_name = Lower(name);
    const bool left = lower_name.find(L"left") != std::wstring::npos;
    const bool right = lower_name.find(L"right") != std::wstring::npos;
    if (left == right)
      continue;
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(info.DevInst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
      continue;
    const auto mac = ExtractMac(id);
    if (!mac.empty())
      out[mac] = left;
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

std::vector<Candidate> Enumerate()
{
  const auto handed = BluetoothHandedness();
  GUID hid_guid{};
  HidD_GetHidGuid(&hid_guid);
  HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
    return {};

  std::vector<Candidate> out;
  for (DWORD i = 0;; ++i)
  {
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, i, &iface))
      break;

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &required, nullptr);
    if (!required)
      continue;
    std::vector<uint8_t> storage(required);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, required, nullptr, &devinfo))
      continue;

    const std::wstring path_lower = Lower(detail->DevicePath);
    const bool path_match = path_lower.find(L"vid&0002045e_pid&065d") != std::wstring::npos ||
                            (path_lower.find(L"vid_045e") != std::wstring::npos &&
                             path_lower.find(L"pid_065d") != std::wstring::npos);
    if (!path_match)
      continue;

    Candidate c;
    c.path = detail->DevicePath;

    std::wstring parent_id;
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, devinfo.DevInst, 0) == CR_SUCCESS)
    {
      wchar_t id[MAX_DEVICE_ID_LEN]{};
      if (CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS)
        parent_id = id;
    }
    const auto mac = ExtractMac(parent_id);
    const auto hand = mac.empty() ? handed.end() : handed.find(mac);
    if (hand != handed.end())
      c.left = hand->second;
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

  const uint64_t now_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  const auto packet = BuildWmrLedTimesyncPacket(1, 1, intensity, now_us, 500, 1);
  std::array<uint8_t, 64> report{};
  std::copy(packet.begin(), packet.end(), report.begin());

  // First try the normal HID output path. Windows HidBth often denies a
  // GENERIC_WRITE CreateFile for WMR controllers while still exposing Raw Input.
  HANDLE h = CreateFileW(chosen->path.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr);
  if (h != INVALID_HANDLE_VALUE)
  {
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
    if (ok && written == report.size())
    {
      std::ostringstream ss;
      ss << (left ? "left" : "right") << " WMR LED intensity packet sent via WriteFile: "
         << std::clamp(intensity, 1, 399);
      status = ss.str();
      return true;
    }
    std::ostringstream ss;
    ss << "WMR WriteFile output failed (error " << err << ", bytes " << written << ")";
    status = ss.str();
  }
  else
  {
    std::ostringstream ss;
    ss << "Windows denied WMR GENERIC_WRITE handle (error " << GetLastError() << ")";
    status = ss.str();
  }

  // HidD_SetOutputReport uses IOCTL_HID_SET_OUTPUT_REPORT. Some Bluetooth HID
  // stacks permit this on a metadata handle even when WriteFile access is denied.
  HANDLE ctl = CreateFileW(chosen->path.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
  if (ctl == INVALID_HANDLE_VALUE)
  {
    const DWORD err = GetLastError();
    std::ostringstream ss;
    ss << status << "; control handle denied (error " << err << ")";
    status = ss.str();
    return false;
  }
  SetLastError(ERROR_SUCCESS);
  const BOOL set_ok = HidD_SetOutputReport(ctl, report.data(), static_cast<ULONG>(report.size()));
  const DWORD set_err = set_ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(ctl);
  if (!set_ok)
  {
    std::ostringstream ss;
    ss << status << "; HidD_SetOutputReport failed (error " << set_err << ")";
    status = ss.str();
    return false;
  }

  std::ostringstream ss;
  ss << (left ? "left" : "right") << " WMR LED intensity packet sent via HidD_SetOutputReport: "
     << std::clamp(intensity, 1, 399);
  status = ss.str();
  return true;
}
