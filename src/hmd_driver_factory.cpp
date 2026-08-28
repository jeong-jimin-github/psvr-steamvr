#include "device_provider.h"
#include "openvr_driver.h"

#include <cstring>

#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec(dllexport)
#else
#define HMD_DLL_EXPORT extern "C"
#endif

PsvrDeviceProvider g_device_provider;

HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode)
{
  if (pInterfaceName && 0 == strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName))
    return &g_device_provider;

  if (pReturnCode)
    *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
