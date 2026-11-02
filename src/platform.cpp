#include "platform.h"

#include <cstdarg>
#include <cstdio>

namespace sstk {

namespace {

int64_t ReadFrequency() {
  LARGE_INTEGER value;
  QueryPerformanceFrequency(&value);
  return value.QuadPart;
}

bool EnvironmentFlag(const wchar_t* name) {
  wchar_t value[8];
  return GetEnvironmentVariableW(name, value, ARRAYSIZE(value)) > 0;
}

}  // namespace

int64_t QpcFrequency() {
  static const int64_t frequency = ReadFrequency();
  return frequency;
}

double NowSeconds() {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) / static_cast<double>(QpcFrequency());
}

bool OverlayVisibleToCapture() {
  static const bool visible = EnvironmentFlag(L"SSTK_DEBUG_VISIBLE");
  return visible;
}

void Trace(const char* format, ...) {
  static const bool enabled = EnvironmentFlag(L"SSTK_TRACE");
  if (!enabled) return;

  char line[512];
  int prefix = snprintf(line, sizeof(line), "%12.4f ", NowSeconds());
  va_list args;
  va_start(args, format);
  int body = vsnprintf(line + prefix, sizeof(line) - prefix - 2, format, args);
  va_end(args);
  if (body < 0) return;
  size_t length = prefix + (body < static_cast<int>(sizeof(line)) - prefix - 2 ? body : sizeof(line) - prefix - 3);
  line[length++] = '\n';

  wchar_t path[MAX_PATH + 32];
  DWORD directory = GetTempPathW(MAX_PATH, path);
  if (directory == 0 || directory >= MAX_PATH) return;
  wcscat_s(path, L"ScreenshotToolkit.log");
  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written;
  WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
  CloseHandle(file);
}

void DisablePowerThrottling() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

}  // namespace sstk
