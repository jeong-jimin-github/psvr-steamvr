#include "gearvr_ble.h"
#include "driverlog.h"

#include <windows.h>
#include <cstdio>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Enumeration.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <sstream>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Devices::Enumeration;

namespace
{
constexpr wchar_t kServiceUuid[] = L"{4f63756c-7573-2054-6872-65656d6f7465}";
constexpr wchar_t kNotifyUuid[] = L"{c8c51726-81bc-483b-a052-f7a14ea3d281}";
constexpr wchar_t kWriteUuid[] = L"{c8c51726-81bc-483b-a052-f7a14ea3d282}";

std::string Narrow(const std::wstring &w)
{
  if (w.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string AddrToString(uint64_t addr)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           int((addr >> 40) & 0xFF), int((addr >> 32) & 0xFF), int((addr >> 24) & 0xFF),
           int((addr >> 16) & 0xFF), int((addr >> 8) & 0xFF), int(addr & 0xFF));
  return buf;
}

bool NameLooksLikeGear(const std::wstring &name)
{
  std::wstring n = name;
  for (auto &c : n)
    c = towlower(c);
  return n.find(L"gear") != std::wstring::npos ||
         n.find(L"oculus") != std::wstring::npos ||
         n.find(L"samsung") != std::wstring::npos;
}

int16_t LeI16(const uint8_t *p)
{
  return static_cast<int16_t>(p[0] | (p[1] << 8));
}

IBuffer BytesToBuffer(const uint8_t *data, size_t n)
{
  DataWriter writer;
  writer.WriteBytes(array_view<uint8_t const>(data, data + n));
  return writer.DetachBuffer();
}

void TryPair(BluetoothLEDevice const &device)
{
  try
  {
    auto pairing = device.DeviceInformation().Pairing();
    FileLog("GearVR: can_pair=%d is_paired=%d", pairing.CanPair() ? 1 : 0, pairing.IsPaired() ? 1 : 0);
    if (pairing.IsPaired() || !pairing.CanPair())
      return;
    auto custom = pairing.Custom();
    auto token = custom.PairingRequested([](DeviceInformationCustomPairing const &, DevicePairingRequestedEventArgs const &args) {
      args.Accept();
    });
    auto result = custom.PairAsync(DevicePairingKinds::ConfirmOnly).get();
    FileLog("GearVR: pair status=%d", int(result.Status()));
    custom.PairingRequested(token);
  }
  catch (hresult_error const &e)
  {
    FileLog("GearVR: pair exception %s", Narrow(e.message().c_str()).c_str());
  }
}

bool HasGearService(BluetoothLEDevice const &device)
{
  auto svcs = device.GetGattServicesForUuidAsync(guid(kServiceUuid), BluetoothCacheMode::Uncached).get();
  if (svcs.Status() == GattCommunicationStatus::Success && svcs.Services().Size() > 0)
    return true;
  auto all = device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
  FileLog("GearVR: gatt status=%d services=%d conn=%d", int(all.Status()), int(all.Services().Size()),
          int(device.ConnectionStatus()));
  bool apple = false;
  for (auto const &s : all.Services())
  {
    const std::string id = Narrow(std::wstring(to_hstring(s.Uuid()).c_str()));
    FileLog("GearVR:  service %s", id.c_str());
    if (id.find("89d3502b") != std::string::npos || id.find("7905f431") != std::string::npos)
      apple = true;
  }
  if (apple)
    FileLog("GearVR: skipping Apple gadget");
  return false;
}

bool WriteCmd(GattCharacteristic const &ch, uint8_t a, uint8_t b)
{
  const uint8_t cmd[2] = {a, b};
  auto status = ch.WriteValueAsync(BytesToBuffer(cmd, 2), GattWriteOption::WriteWithoutResponse).get();
  if (status != GattCommunicationStatus::Success)
    status = ch.WriteValueAsync(BytesToBuffer(cmd, 2)).get();
  return status == GattCommunicationStatus::Success;
}

struct Found
{
  uint64_t addr = 0;
  std::wstring name;
  int16_t rssi = -127;
  bool matched = false;
  BluetoothAddressType type = BluetoothAddressType::Public;
};

Quaternion Multiply(const Quaternion &a, const Quaternion &b)
{
  Quaternion r;
  r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return r;
}
} // namespace

std::vector<std::string> GearVrBle::Scan(int timeout_ms)
{
  std::vector<std::string> out;
  try
  {
    init_apartment(apartment_type::multi_threaded);
    std::mutex mu;
    std::vector<Found> found;
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    watcher.Received([&](BluetoothLEAdvertisementWatcher const &, BluetoothLEAdvertisementReceivedEventArgs const &args) {
      Found f;
      f.addr = args.BluetoothAddress();
      f.name = args.Advertisement().LocalName().c_str();
      f.rssi = args.RawSignalStrengthInDBm();
      f.type = args.BluetoothAddressType();
      bool has_svc = false;
      for (auto const &u : args.Advertisement().ServiceUuids())
      {
        if (to_hstring(u) == kServiceUuid)
          has_svc = true;
      }
      f.matched = has_svc || NameLooksLikeGear(f.name);
      std::lock_guard<std::mutex> lock(mu);
      for (auto &e : found)
      {
        if (e.addr == f.addr)
        {
          if (f.name.size() > e.name.size())
            e.name = f.name;
          e.rssi = f.rssi;
          e.matched = e.matched || f.matched;
          return;
        }
      }
      found.push_back(f);
    });
    watcher.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    watcher.Stop();
    std::vector<Found> snapshot;
    {
      std::lock_guard<std::mutex> lock(mu);
      snapshot = found;
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const Found &a, const Found &b) {
      if (a.matched != b.matched)
        return a.matched > b.matched;
      return a.rssi > b.rssi;
    });
    for (auto &f : snapshot)
    {
      bool gatt = false;
      // Do not try to pair arbitrary nearby BLE devices. The Gear VR controller
      // advertises its OculusThreemote service while HOME is blinking.
      if (f.matched)
      {
        try
        {
          auto probe = BluetoothLEDevice::FromBluetoothAddressAsync(f.addr, f.type).get();
          if (probe)
          {
            gatt = HasGearService(probe);
            probe.Close();
          }
        }
        catch (hresult_error const &e)
        {
          FileLog("GearVR: probe %s exception %s", AddrToString(f.addr).c_str(), Narrow(e.message().c_str()).c_str());
        }
      }
      char line[256];
      snprintf(line, sizeof(line), "%s type=%d rssi=%d name=%s gearvr_adv=%d gearvr_gatt=%d",
               AddrToString(f.addr).c_str(), int(f.type), int(f.rssi), Narrow(f.name).c_str(),
               f.matched ? 1 : 0, gatt ? 1 : 0);
      out.emplace_back(line);
    }
  }
  catch (hresult_error const &e)
  {
    out.push_back(std::string("scan error: ") + Narrow(e.message().c_str()));
  }
  return out;
}

bool GearVrBle::Start()
{
  Stop();
  run_ = true;
  connected_ = false;
  ahrs_.Reset();
  recenter_ = Quaternion{};
  have_sample_ = false;
  thread_ = std::thread(&GearVrBle::ThreadMain, this);
  return true;
}

void GearVrBle::Stop()
{
  run_ = false;
  if (thread_.joinable())
    thread_.join();
  connected_ = false;
}

GearVrState GearVrBle::GetState() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void GearVrBle::Recenter(const Quaternion &hmd_rotation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const Quaternion raw = ahrs_.Orientation();
  const Quaternion inverse_raw{raw.w, -raw.x, -raw.y, -raw.z};
  recenter_ = Multiply(hmd_rotation, inverse_raw);
  state_.rotation = Multiply(recenter_, raw);
  FileLog("GearVR: recentered to HMD orientation");
}

void GearVrBle::HandlePacket(const uint8_t *data, int size)
{
  if (size != 60)
    return;

  GearVrState s;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s = state_;
  }

  s.trigger = (data[58] & (1 << 0)) != 0;
  s.home = (data[58] & (1 << 1)) != 0;
  s.back = (data[58] & (1 << 2)) != 0;
  s.touch_click = (data[58] & (1 << 3)) != 0;
  s.vol_down = (data[58] & (1 << 4)) != 0;
  s.vol_up = (data[58] & (1 << 5)) != 0;

  const int axis_x = ((((data[54] & 0x0F) << 6) | ((data[55] & 0xFC) >> 2)) & 0x3FF);
  const int axis_y = ((((data[55] & 0x03) << 8) | data[56]) & 0x3FF);
  s.touch_active = axis_x != 0 || axis_y != 0;
  if (s.touch_active)
  {
    s.touch_x = (axis_x / 1023.f) * 2.f - 1.f;
    s.touch_y = (axis_y / 1023.f) * 2.f - 1.f;
  }
  else
  {
    s.touch_x = 0.f;
    s.touch_y = 0.f;
  }

  // Scaling recovered from Samsung's Gear VR input service.
  constexpr float accel_scale = 10000.f * 9.80665f / 2048.f * 0.00001f;
  constexpr float gyro_scale = 10000.f * 0.017453292f / 14.285f * 0.0001f;
  const float ax = LeI16(data + 4) * accel_scale;
  const float ay = LeI16(data + 6) * accel_scale;
  const float az = LeI16(data + 8) * accel_scale;
  const float gx = LeI16(data + 10) * gyro_scale;
  const float gy = LeI16(data + 12) * gyro_scale;
  const float gz = LeI16(data + 14) * gyro_scale;

  // Recenter() runs on SteamVR's server thread while notifications arrive on
  // the BLE callback thread. Keep the filter and calibration quaternion in the
  // same critical section so a pose cannot mix two sensor frames.
  std::lock_guard<std::mutex> lock(mutex_);
  ahrs_.Update(gx, gy, gz, ax, ay, az, 0.015f);
  s.rotation = Multiply(recenter_, ahrs_.Orientation());
  s.packets++;
  s.connected = true;
  s.name = state_.name;
  s.address = state_.address;
  state_ = s;
}

void GearVrBle::ThreadMain()
{
  try
  {
    init_apartment(apartment_type::multi_threaded);
    while (run_.load())
    {
      ConnectionAttempt();
      connected_ = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.connected = false;
      }
      for (int i = 0; i < 10 && run_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  catch (hresult_error const &e)
  {
    FileLog("GearVR: thread exception %s", Narrow(e.message().c_str()).c_str());
    connected_ = false;
  }
}

void GearVrBle::ConnectionAttempt()
{
  try
  {
    FileLog("GearVR: BLE thread start, scanning...");

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Found> found;
    Found best;
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    watcher.Received([&](BluetoothLEAdvertisementWatcher const &, BluetoothLEAdvertisementReceivedEventArgs const &args) {
      Found f;
      f.addr = args.BluetoothAddress();
      f.name = args.Advertisement().LocalName().c_str();
      f.rssi = args.RawSignalStrengthInDBm();
      f.type = args.BluetoothAddressType();
      bool has_svc = false;
      for (auto const &u : args.Advertisement().ServiceUuids())
      {
        if (to_hstring(u) == kServiceUuid)
          has_svc = true;
      }
      f.matched = has_svc || NameLooksLikeGear(f.name);
      std::lock_guard<std::mutex> lock(mu);
      for (auto &e : found)
      {
        if (e.addr == f.addr)
        {
          if (f.name.size() > e.name.size())
            e.name = f.name;
          e.rssi = f.rssi;
          e.matched = e.matched || f.matched;
          if (e.matched)
            cv.notify_all();
          return;
        }
      }
      found.push_back(f);
      if (f.matched)
        cv.notify_all();
    });
    watcher.Start();
    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait_for(lock, std::chrono::seconds(12), [&] {
        if (!run_.load())
          return true;
        for (auto const &candidate : found)
          if (candidate.matched)
            return true;
        return false;
      });
    }
    watcher.Stop();

    if (!run_.load())
      return;

    std::vector<Found> candidates;
    {
      std::lock_guard<std::mutex> lock(mu);
      candidates = found;
    }
    // Named non-Gear devices are known unrelated peripherals. Keep unnamed
    // advertisements as a fallback because some controller firmware omits its
    // local name while in pairing mode.
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Found &c) {
                       return !c.matched && !c.name.empty();
                     }),
                     candidates.end());
    FileLog("GearVR: saw %d BLE advertisers", int(candidates.size()));
    std::sort(candidates.begin(), candidates.end(), [](const Found &a, const Found &b) {
      if (a.matched != b.matched)
        return a.matched > b.matched;
      return a.rssi > b.rssi;
    });

    for (auto &c : candidates)
    {
      if (!run_.load())
        return;
      FileLog("GearVR: probing %s rssi=%d name=%s match=%d", AddrToString(c.addr).c_str(),
              int(c.rssi), Narrow(c.name).c_str(), c.matched ? 1 : 0);
      try
      {
        auto probe = BluetoothLEDevice::FromBluetoothAddressAsync(c.addr, c.type).get();
        if (!probe)
          continue;
        const bool ok = HasGearService(probe);
        if (!ok)
        {
          probe.Close();
          continue;
        }
        best = c;
        best.matched = true;
        probe.Close();
        break;
      }
      catch (hresult_error const &e)
      {
        FileLog("GearVR: probe failed %s %s", AddrToString(c.addr).c_str(), Narrow(e.message().c_str()).c_str());
      }
    }

    if (!best.matched)
    {
      FileLog("GearVR: no Gear VR GATT service on nearby BLE devices");
      return;
    }

    FileLog("GearVR: found %s %s rssi=%d", AddrToString(best.addr).c_str(),
            Narrow(best.name).c_str(), int(best.rssi));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.name = Narrow(best.name);
      state_.address = AddrToString(best.addr);
    }

    auto device = BluetoothLEDevice::FromBluetoothAddressAsync(best.addr, best.type).get();
    if (!device)
    {
      FileLog("GearVR: FromBluetoothAddressAsync failed");
      return;
    }

    auto session = GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()).get();
    session.MaintainConnection(true);
    FileLog("GearVR: GATT session maintain_connection=1 paired=%d",
            device.DeviceInformation().Pairing().IsPaired() ? 1 : 0);

    auto services = device.GetGattServicesForUuidAsync(guid(kServiceUuid), BluetoothCacheMode::Uncached).get();
    if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0)
    {
      FileLog("GearVR: custom service missing (status=%d)", int(services.Status()));
      return;
    }
    auto service = services.Services().GetAt(0);
    const auto access = service.RequestAccessAsync().get();
    FileLog("GearVR: service access=%d", int(access));
    auto chars = service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
    if (chars.Status() != GattCommunicationStatus::Success)
    {
      FileLog("GearVR: characteristic discovery failed (%d)", int(chars.Status()));
      return;
    }
    GattCharacteristic notify{nullptr};
    GattCharacteristic write{nullptr};
    for (auto const &c : chars.Characteristics())
    {
      const auto id = to_hstring(c.Uuid());
      if (id == kNotifyUuid)
        notify = c;
      else if (id == kWriteUuid)
        write = c;
    }
    if (!notify || !write)
    {
      FileLog("GearVR: notify/write characteristics missing");
      return;
    }

    GattCommunicationStatus cccd = GattCommunicationStatus::Unreachable;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
      cccd = notify.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::Notify)
                 .get();
      FileLog("GearVR: CCCD notify attempt=%d status=%d", attempt, int(cccd));
      if (cccd == GattCommunicationStatus::Success)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    if (cccd != GattCommunicationStatus::Success)
    {
      FileLog("GearVR: CCCD notify failed (%d)", int(cccd));
      return;
    }

    notify.ValueChanged([&](GattCharacteristic const &, GattValueChangedEventArgs const &args) {
      auto reader = DataReader::FromBuffer(args.CharacteristicValue());
      std::vector<uint8_t> buf(reader.UnconsumedBufferLength());
      if (!buf.empty())
        reader.ReadBytes(buf);
      HandlePacket(buf.data(), static_cast<int>(buf.size()));
    });

    // Samsung initialization sequence: repeated sensor mode, low-power mode
    // toggle, then repeated high-rate VR mode.
    const uint8_t init[][2] = {
        {0x01, 0x00}, {0x01, 0x00}, {0x01, 0x00},
        {0x06, 0x00}, {0x07, 0x00},
        {0x08, 0x00}, {0x08, 0x00}, {0x08, 0x00}};
    for (auto const &cmd : init)
    {
      if (!WriteCmd(write, cmd[0], cmd[1]))
        FileLog("GearVR: init write %02X%02X failed", cmd[0], cmd[1]);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    connected_ = true;
    FileLog("GearVR: connected, streaming");

    auto last_keepalive = std::chrono::steady_clock::now();
    while (run_.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const auto now = std::chrono::steady_clock::now();
      if (now - last_keepalive > std::chrono::seconds(8))
      {
        WriteCmd(write, 0x04, 0x00);
        last_keepalive = now;
      }
      if (device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
      {
        FileLog("GearVR: disconnected");
        connected_ = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          state_.connected = false;
        }
        break;
      }
    }

    WriteCmd(write, 0x00, 0x00);
    device.Close();
  }
  catch (hresult_error const &e)
  {
    FileLog("GearVR: exception %s", Narrow(e.message().c_str()).c_str());
    connected_ = false;
  }
}
