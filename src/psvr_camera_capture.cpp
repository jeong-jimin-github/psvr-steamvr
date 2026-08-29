#include "psvr_camera_capture.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
std::string HrText(const char *what, HRESULT hr)
{
  std::ostringstream ss;
  ss << what << " failed hr=0x" << std::hex << static_cast<unsigned long>(hr);
  return ss.str();
}

bool StartMf(bool &com_started, bool &mf_started, std::string &error)
{
  const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  com_started = SUCCEEDED(chr);
  if (FAILED(chr) && chr != RPC_E_CHANGED_MODE)
  {
    error = HrText("CoInitializeEx", chr);
    return false;
  }
  const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr))
  {
    error = HrText("MFStartup", hr);
    if (com_started)
      CoUninitialize();
    com_started = false;
    return false;
  }
  mf_started = true;
  return true;
}

void StopMf(bool &com_started, bool &mf_started)
{
  if (mf_started)
  {
    MFShutdown();
    mf_started = false;
  }
  if (com_started)
  {
    CoUninitialize();
    com_started = false;
  }
}

std::vector<ComPtr<IMFActivate>> EnumerateActivates(std::vector<PsvrCameraDeviceInfo> *infos,
                                                    std::string &error)
{
  ComPtr<IMFAttributes> attrs;
  HRESULT hr = MFCreateAttributes(&attrs, 1);
  if (FAILED(hr))
  {
    error = HrText("MFCreateAttributes", hr);
    return {};
  }
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

  IMFActivate **raw = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attrs.Get(), &raw, &count);
  if (FAILED(hr))
  {
    error = HrText("MFEnumDeviceSources", hr);
    return {};
  }

  std::vector<ComPtr<IMFActivate>> out;
  out.reserve(count);
  if (infos)
    infos->clear();
  for (UINT32 i = 0; i < count; ++i)
  {
    ComPtr<IMFActivate> a;
    a.Attach(raw[i]);
    out.push_back(a);

    if (infos)
    {
      WCHAR *name = nullptr;
      UINT32 chars = 0;
      std::wstring friendly = L"Video device";
      if (SUCCEEDED(a->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                          &name, &chars)) && name)
      {
        friendly.assign(name, chars ? chars - 1 : 0);
        CoTaskMemFree(name);
      }
      infos->push_back({static_cast<int>(i), friendly});
    }
  }
  CoTaskMemFree(raw);
  return out;
}

bool IsSameGuid(const GUID &a, const GUID &b)
{
  return InlineIsEqualGUID(a, b) != FALSE;
}
}

struct PsvrCameraCapture::Impl
{
  ComPtr<IMFMediaSource> source;
  ComPtr<IMFSourceReader> reader;
  GUID subtype{};
  int width = 0;
  int height = 0;
  std::wstring name;
  std::string error;
  bool com_started = false;
  bool mf_started = false;
};

PsvrCameraCapture::PsvrCameraCapture() : impl_(std::make_unique<Impl>()) {}
PsvrCameraCapture::~PsvrCameraCapture() { Close(); }

std::vector<PsvrCameraDeviceInfo> PsvrCameraCapture::Enumerate()
{
  bool com = false, mf = false;
  std::string error;
  std::vector<PsvrCameraDeviceInfo> infos;
  if (!StartMf(com, mf, error))
    return infos;
  EnumerateActivates(&infos, error);
  StopMf(com, mf);
  return infos;
}

bool PsvrCameraCapture::Open(int device_index, int preferred_width, int preferred_height)
{
  Close();
  impl_->error.clear();
  if (!StartMf(impl_->com_started, impl_->mf_started, impl_->error))
    return false;

  std::vector<PsvrCameraDeviceInfo> infos;
  auto devices = EnumerateActivates(&infos, impl_->error);
  if (device_index < 0 || device_index >= static_cast<int>(devices.size()))
  {
    impl_->error = "camera index out of range";
    Close();
    return false;
  }
  impl_->name = infos[device_index].name;

  HRESULT hr = devices[device_index]->ActivateObject(IID_PPV_ARGS(&impl_->source));
  if (FAILED(hr))
  {
    impl_->error = HrText("ActivateObject", hr);
    Close();
    return false;
  }

  ComPtr<IMFAttributes> reader_attrs;
  MFCreateAttributes(&reader_attrs, 2);
  reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  reader_attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
  hr = MFCreateSourceReaderFromMediaSource(impl_->source.Get(), reader_attrs.Get(), &impl_->reader);
  if (FAILED(hr))
  {
    impl_->error = HrText("MFCreateSourceReaderFromMediaSource", hr);
    Close();
    return false;
  }

  ComPtr<IMFMediaType> best;
  UINT32 best_w = 0, best_h = 0, best_num = 0, best_den = 1;
  long long best_score = -1;
  for (DWORD i = 0;; ++i)
  {
    ComPtr<IMFMediaType> native;
    hr = impl_->reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &native);
    if (hr == MF_E_NO_MORE_TYPES)
      break;
    if (FAILED(hr))
      continue;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    if (FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h)))
      continue;
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den);
    const long long area = static_cast<long long>(w) * h;
    const long long desired_penalty = std::llabs(static_cast<long long>(w) - preferred_width) * 2000LL +
                                      std::llabs(static_cast<long long>(h) - preferred_height) * 2000LL;
    const long long fps = den ? (1000LL * num / den) : 0;
    const long long score = area * 10LL + fps - desired_penalty;
    if (score > best_score)
    {
      best_score = score;
      best = native;
      best_w = w;
      best_h = h;
      best_num = num;
      best_den = den;
    }
  }
  if (!best)
  {
    impl_->error = "camera exposes no usable video media type";
    Close();
    return false;
  }

  ComPtr<IMFMediaType> rgb;
  MFCreateMediaType(&rgb);
  rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  MFSetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, best_w, best_h);
  if (best_num)
    MFSetAttributeRatio(rgb.Get(), MF_MT_FRAME_RATE, best_num, best_den ? best_den : 1);
  rgb->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr = impl_->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb.Get());
  if (FAILED(hr))
  {
    hr = impl_->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best.Get());
    if (FAILED(hr))
    {
      impl_->error = HrText("SetCurrentMediaType", hr);
      Close();
      return false;
    }
  }

  ComPtr<IMFMediaType> current;
  hr = impl_->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
  if (FAILED(hr) || FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE,
                                               reinterpret_cast<UINT32 *>(&impl_->width),
                                               reinterpret_cast<UINT32 *>(&impl_->height))) ||
      FAILED(current->GetGUID(MF_MT_SUBTYPE, &impl_->subtype)))
  {
    impl_->error = "could not query selected camera format";
    Close();
    return false;
  }
  return true;
}

bool PsvrCameraCapture::Read(PsvrCameraFrame &frame)
{
  if (!impl_->reader)
  {
    impl_->error = "camera is not open";
    return false;
  }

  DWORD stream_index = 0, flags = 0;
  LONGLONG ts = 0;
  ComPtr<IMFSample> sample;
  const HRESULT hr = impl_->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                                &stream_index, &flags, &ts, &sample);
  if (FAILED(hr))
  {
    impl_->error = HrText("ReadSample", hr);
    return false;
  }
  if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample)
    return false;

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
  {
    impl_->error = "ConvertToContiguousBuffer failed";
    return false;
  }
  BYTE *data = nullptr;
  DWORD max_len = 0, len = 0;
  if (FAILED(buffer->Lock(&data, &max_len, &len)))
  {
    impl_->error = "camera buffer lock failed";
    return false;
  }

  frame.width = impl_->width;
  frame.height = impl_->height;
  frame.timestamp_100ns = ts;
  frame.gray.assign(static_cast<size_t>(frame.width) * frame.height, 0);
  bool converted = false;
  const size_t pixels = frame.gray.size();

  if (IsSameGuid(impl_->subtype, MFVideoFormat_RGB32) && len >= pixels * 4)
  {
    for (size_t i = 0; i < pixels; ++i)
    {
      const uint8_t b = data[i * 4 + 0];
      const uint8_t g = data[i * 4 + 1];
      const uint8_t r = data[i * 4 + 2];
      frame.gray[i] = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
    }
    converted = true;
  }
  else if (IsSameGuid(impl_->subtype, MFVideoFormat_YUY2) && len >= pixels * 2)
  {
    for (size_t i = 0; i < pixels; ++i)
      frame.gray[i] = data[i * 2];
    converted = true;
  }
  else if (IsSameGuid(impl_->subtype, MFVideoFormat_NV12) && len >= pixels)
  {
    std::copy(data, data + pixels, frame.gray.begin());
    converted = true;
  }

  buffer->Unlock();
  if (!converted)
  {
    impl_->error = "selected camera pixel format is not RGB32/YUY2/NV12";
    return false;
  }
  return true;
}

void PsvrCameraCapture::Close()
{
  if (!impl_)
    return;
  if (impl_->source)
    impl_->source->Shutdown();
  impl_->reader.Reset();
  impl_->source.Reset();
  impl_->width = 0;
  impl_->height = 0;
  StopMf(impl_->com_started, impl_->mf_started);
}

bool PsvrCameraCapture::IsOpen() const { return impl_ && impl_->reader != nullptr; }
int PsvrCameraCapture::Width() const { return impl_ ? impl_->width : 0; }
int PsvrCameraCapture::Height() const { return impl_ ? impl_->height : 0; }
const std::wstring &PsvrCameraCapture::Name() const { return impl_->name; }
const std::string &PsvrCameraCapture::LastError() const { return impl_->error; }
