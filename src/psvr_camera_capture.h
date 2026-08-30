#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct PsvrCameraDeviceInfo
{
  int index = -1;
  std::wstring name;
};

struct PsvrCameraFrame
{
  int width = 0;
  int height = 0;
  int64_t timestamp_100ns = 0;
  std::vector<uint8_t> gray;
};

class PsvrCameraCapture
{
public:
  PsvrCameraCapture();
  ~PsvrCameraCapture();

  PsvrCameraCapture(const PsvrCameraCapture &) = delete;
  PsvrCameraCapture &operator=(const PsvrCameraCapture &) = delete;

  static std::vector<PsvrCameraDeviceInfo> Enumerate();

  bool Open(int device_index, int preferred_width = 1280, int preferred_height = 800);
  bool Read(PsvrCameraFrame &frame);
  void Close();

  bool IsOpen() const;
  int Width() const;
  int Height() const;
  const std::wstring &Name() const;
  const std::string &LastError() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
