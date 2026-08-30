#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// WMR motion-controller LED control / time-sync packet (message type 0x03).
// The intensity field is 9 bits in practice and valid in the 1..399 range.
// A camera tracker can update the timestamp from its exposure timing later;
// keeping packet construction isolated here lets the Windows HID output path
// be implemented without coupling it to the optical detector.
inline std::array<uint8_t, 12> BuildWmrLedTimesyncPacket(uint8_t command_counter,
                                                         uint8_t timesync_counter,
                                                         int led_intensity,
                                                         uint64_t device_time_us,
                                                         int value2 = 500,
                                                         uint8_t train_type = 1)
{
  std::array<uint8_t, 12> packet{};
  timesync_counter &= 0x03;
  led_intensity = std::clamp(led_intensity, 1, 399);
  value2 = std::clamp(value2, 0, 1023);

  packet[0] = 0x03;
  packet[1] = command_counter;
  packet[2] = static_cast<uint8_t>(timesync_counter | ((led_intensity & 0x3f) << 2));
  packet[3] = static_cast<uint8_t>(((led_intensity >> 6) & 0x07) | ((device_time_us & 0x1f) << 3));
  packet[4] = static_cast<uint8_t>(device_time_us >> 5);
  packet[5] = static_cast<uint8_t>(device_time_us >> 13);
  packet[6] = static_cast<uint8_t>(device_time_us >> 21);
  packet[7] = static_cast<uint8_t>(device_time_us >> 29);
  packet[8] = static_cast<uint8_t>(device_time_us >> 37);
  packet[9] = static_cast<uint8_t>(device_time_us >> 45);
  packet[10] = static_cast<uint8_t>(((device_time_us >> 53) & 0x03) | ((value2 & 0x3f) << 2));
  packet[11] = static_cast<uint8_t>(((value2 >> 6) & 0x1f) | ((train_type & 0x03) << 5));
  return packet;
}
