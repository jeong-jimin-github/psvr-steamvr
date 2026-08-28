#include "driverlog.h"

#ifndef PSVR_STANDALONE
#include "openvr_driver.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <windows.h>

static std::mutex g_log_mutex;
static FILE *g_file = nullptr;

static void EnsureFile()
{
  if (g_file)
    return;
  wchar_t path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, path) == 0)
    return;
  wchar_t file[MAX_PATH];
  swprintf_s(file, L"%spsvr_driver.log", path);
  _wfopen_s(&g_file, file, L"a");
}

static void WriteLine(const char *buf)
{
  std::lock_guard<std::mutex> lock(g_log_mutex);
  EnsureFile();
  if (g_file)
  {
    const time_t now = time(nullptr);
    tm t{};
    localtime_s(&t, &now);
    fprintf(g_file, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
            t.tm_hour, t.tm_min, t.tm_sec, buf);
    fflush(g_file);
  }
#ifndef PSVR_STANDALONE
  if (vr::VRDriverLog())
    vr::VRDriverLog()->Log(buf);
#endif
}

void FileLog(const char *pchFormat, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, pchFormat);
  vsnprintf_s(buf, sizeof(buf), _TRUNCATE, pchFormat, args);
  va_end(args);
  WriteLine(buf);
}

void DriverLog(const char *pchFormat, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, pchFormat);
  vsnprintf_s(buf, sizeof(buf), _TRUNCATE, pchFormat, args);
  va_end(args);
  WriteLine(buf);
}
