#include "sstk/base/platform.h"

#include "sstk/base/settings.h"

#include <wincodec.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

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
  static const bool visible = CurrentSettings().capture_includes_thumbnails || EnvironmentFlag(L"SSTK_DEBUG_VISIBLE");
  return visible;
}

bool Tracing() {
  static const bool by_environment = EnvironmentFlag(L"SSTK_TRACE");
  if (by_environment) return true;
  wchar_t path[MAX_PATH + 32];
  const DWORD length = GetTempPathW(MAX_PATH, path);
  if (length == 0 || length >= MAX_PATH) return false;
  wcscat_s(path, L"ScreenshotToolkit.trace");
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void TraceImage(const wchar_t* name, const uint32_t* pixels, int width, int height) {
  if (!Tracing() || pixels == nullptr || width <= 0 || height <= 0) return;
  wchar_t directory[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH, directory);
  if (length == 0 || length >= MAX_PATH) return;
  const std::wstring path = std::wstring(directory) + L"ScreenshotToolkit-" + name + L".png";
  // PNG takes no 32-bit pixels without alpha, and the top byte here says nothing.
  const UINT stride = static_cast<UINT>(width) * 3;
  std::vector<BYTE> bytes(static_cast<size_t>(stride) * height);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
    bytes[i * 3] = static_cast<BYTE>(pixels[i]);
    bytes[i * 3 + 1] = static_cast<BYTE>(pixels[i] >> 8);
    bytes[i * 3 + 2] = static_cast<BYTE>(pixels[i] >> 16);
  }
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    const bool written =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &options)) &&
        SUCCEEDED(frame->Initialize(options.Get())) && SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
        SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), stride, static_cast<UINT>(bytes.size()), bytes.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (!written) Trace("trace: could not write %ls", path.c_str());
  }
  if (SUCCEEDED(com)) CoUninitialize();
}

void Trace(const char* format, ...) {
  if (!Tracing()) return;

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
