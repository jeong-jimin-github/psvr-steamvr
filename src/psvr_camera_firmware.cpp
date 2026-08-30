#include "psvr_camera_firmware.h"

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
const GUID kUsbDeviceInterface = {0xa5dcbf10, 0x6530, 0x11d2,
                                  {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};

std::wstring Lower(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return s;
}

bool FindBootPath(std::wstring &path, std::string &status)
{
  HDEVINFO set = SetupDiGetClassDevsW(&kUsbDeviceInterface, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE)
  {
    status = "SetupDiGetClassDevs(USB) failed";
    return false;
  }

  bool found = false;
  for (DWORD i = 0;; ++i)
  {
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kUsbDeviceInterface, i, &iface))
      break;

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &required, nullptr);
    if (!required)
      continue;

    std::vector<uint8_t> bytes(required);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(bytes.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, required, nullptr, &devinfo))
      continue;

    wchar_t hardware_ids[1024]{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &devinfo, SPDRP_HARDWAREID, nullptr,
                                           reinterpret_cast<PBYTE>(hardware_ids), sizeof(hardware_ids), nullptr))
      continue;
    const std::wstring ids = Lower(hardware_ids);
    if (ids.find(L"vid_05a9&pid_0580") == std::wstring::npos)
      continue;

    path = detail->DevicePath;
    found = true;
    break;
  }

  SetupDiDestroyDeviceInfoList(set);
  if (!found)
    status = "PS4 Camera USB Boot 05A9:0580 not found";
  return found;
}

bool SendControl(WINUSB_INTERFACE_HANDLE usb, UCHAR request_type, UCHAR request,
                 USHORT value, USHORT index, const uint8_t *data, USHORT length,
                 ULONG &transferred, DWORD &error)
{
  WINUSB_SETUP_PACKET setup{};
  setup.RequestType = request_type;
  setup.Request = request;
  setup.Value = value;
  setup.Index = index;
  setup.Length = length;
  transferred = 0;
  const BOOL ok = WinUsb_ControlTransfer(usb, setup, const_cast<PUCHAR>(data), length,
                                         &transferred, nullptr);
  error = ok ? ERROR_SUCCESS : GetLastError();
  return ok != FALSE;
}
}

bool IsPs4CameraBootPresent(std::string &status)
{
  std::wstring path;
  if (!FindBootPath(path, status))
    return false;
  status = "PS4 Camera USB Boot 05A9:0580 present";
  return true;
}

bool UploadPs4CameraFirmware(const std::string &path, std::string &status)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    status = "firmware file not found: " + path;
    return false;
  }
  std::vector<uint8_t> firmware((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  constexpr size_t kExpectedFirmwareBytes = 66608;
  if (firmware.size() != kExpectedFirmwareBytes)
  {
    std::ostringstream ss;
    ss << "unexpected PS4 Camera firmware size " << firmware.size()
       << " bytes (expected exactly " << kExpectedFirmwareBytes << ")";
    status = ss.str();
    return false;
  }

  std::wstring device_path;
  if (!FindBootPath(device_path, status))
    return false;

  HANDLE h = CreateFileW(device_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE)
  {
    std::ostringstream ss;
    ss << "CreateFile(PS4 Camera USB Boot) failed error=" << GetLastError();
    status = ss.str();
    return false;
  }

  WINUSB_INTERFACE_HANDLE usb = nullptr;
  if (!WinUsb_Initialize(h, &usb))
  {
    const DWORD err = GetLastError();
    CloseHandle(h);
    std::ostringstream ss;
    ss << "WinUsb_Initialize failed error=" << err;
    status = ss.str();
    return false;
  }

  constexpr size_t kChunk = 512;
  size_t offset = 0;
  bool ok = true;
  DWORD last_error = ERROR_SUCCESS;
  while (offset < firmware.size())
  {
    const USHORT len = static_cast<USHORT>(std::min(kChunk, firmware.size() - offset));
    const USHORT value = static_cast<USHORT>(offset & 0xffffu);
    const USHORT index = static_cast<USHORT>(0x14u + (offset >> 16));
    ULONG transferred = 0;
    DWORD error = ERROR_SUCCESS;
    if (!SendControl(usb, 0x40, 0x00, value, index, firmware.data() + offset, len,
                     transferred, error) || transferred != len)
    {
      ok = false;
      last_error = error;
      break;
    }
    offset += len;
  }

  if (ok)
  {
    const uint8_t reboot = 0x5b;
    ULONG transferred = 0;
    DWORD error = ERROR_SUCCESS;
    // A successful reset commonly tears down the USB handle before Windows can return success.
    SendControl(usb, 0x40, 0x00, 0x2200, 0x8018, &reboot, 1, transferred, error);
  }

  WinUsb_Free(usb);
  CloseHandle(h);

  if (!ok)
  {
    std::ostringstream ss;
    ss << "PS4 Camera firmware upload failed at " << offset << "/" << firmware.size()
       << " bytes error=" << last_error;
    status = ss.str();
    return false;
  }

  std::ostringstream ss;
  ss << "PS4 Camera firmware uploaded: " << firmware.size() << " bytes; device reset requested";
  status = ss.str();
  return true;
}
