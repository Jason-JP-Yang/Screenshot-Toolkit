#include "sstk/base/settings.h"

#include "sstk/base/product.h"
#include "sstk/config/core.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace sstk {

namespace {

constexpr wchar_t kSection[] = L"General";
constexpr double kMinimumDwell = 0.5;
constexpr double kMaximumDwell = 120.0;

std::wstring ReadString(const std::wstring& path, const wchar_t* key) {
  wchar_t buffer[2048];
  GetPrivateProfileStringW(kSection, key, L"", buffer, ARRAYSIZE(buffer), path.c_str());
  std::wstring value = buffer;
  const size_t first = value.find_first_not_of(L" \t\"");
  const size_t last = value.find_last_not_of(L" \t\"");
  return first == std::wstring::npos ? std::wstring() : value.substr(first, last - first + 1);
}

bool ReadFlag(const std::wstring& path, const wchar_t* key, bool fallback) {
  return GetPrivateProfileIntW(kSection, key, fallback ? 1 : 0, path.c_str()) != 0;
}

bool IsAbsolute(const std::wstring& path) {
  return (path.size() > 2 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) || path.rfind(L"\\\\", 0) == 0;
}

}  // namespace

Settings ReadSettings() {
  Settings settings{L"", config::kDwellSeconds, config::kCaptureIncludesThumbnails, config::kDeleteToRecycleBin, config::kAutoUpdate};
  const std::wstring path = SettingsPath();
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return settings;

  const std::wstring folder = ReadString(path, L"GalleryFolder");
  if (!folder.empty()) {
    wchar_t expanded[2048];
    const DWORD length = ExpandEnvironmentStringsW(folder.c_str(), expanded, ARRAYSIZE(expanded));
    if (length > 0 && length <= ARRAYSIZE(expanded) && IsAbsolute(expanded)) settings.gallery_folder = expanded;
  }
  const std::wstring dwell = ReadString(path, L"DwellSeconds");
  if (!dwell.empty()) {
    wchar_t* end = nullptr;
    const double seconds = std::wcstod(dwell.c_str(), &end);
    if (end != dwell.c_str() && std::isfinite(seconds)) settings.dwell_seconds = std::clamp(seconds, kMinimumDwell, kMaximumDwell);
  }
  settings.capture_includes_thumbnails = ReadFlag(path, L"CaptureIncludesThumbnails", settings.capture_includes_thumbnails);
  settings.delete_to_recycle_bin = ReadFlag(path, L"DeleteToRecycleBin", settings.delete_to_recycle_bin);
  settings.auto_update = ReadFlag(path, L"AutoUpdate", settings.auto_update);
  return settings;
}

const Settings& CurrentSettings() {
  static const Settings settings = ReadSettings();
  return settings;
}

std::wstring SettingsPath() {
  const std::wstring directory = DataDirectory();
  return directory.empty() ? directory : directory + L"\\settings.ini";
}

std::wstring DefaultSettingsText() {
  wchar_t dwell[32];
  swprintf_s(dwell, L"%g", config::kDwellSeconds);
  std::wstring text;
  text += L"; Screenshot Toolkit settings, read when it starts. After a change, choose Quit and then Quick Restart in its About window.\r\n";
  text += L"; A line left out keeps its default.\r\n\r\n";
  text += L"[General]\r\n";
  text += L"; Where screenshots are saved. Empty means Pictures\\Screenshots. Variables such as %USERPROFILE% work.\r\n";
  text += L"GalleryFolder=\r\n";
  text += L"; Seconds a thumbnail stays on screen.\r\n";
  text += std::wstring(L"DwellSeconds=") + dwell + L"\r\n";
  text += L"; 1 lets screenshots show Screenshot Toolkit's own thumbnails, 0 keeps them out.\r\n";
  text += std::wstring(L"CaptureIncludesThumbnails=") + (config::kCaptureIncludesThumbnails ? L"1" : L"0") + L"\r\n";
  text += L"; 1 sends screenshots dropped on the rubbish bin to the Recycle Bin, 0 deletes them for good.\r\n";
  text += std::wstring(L"DeleteToRecycleBin=") + (config::kDeleteToRecycleBin ? L"1" : L"0") + L"\r\n";
  text += L"; 1 checks GitHub for a newer build every 12 hours and at each start, and installs it while you are away.\r\n";
  text += L"; 0 checks only when you choose Check for Updates.\r\n";
  text += std::wstring(L"AutoUpdate=") + (config::kAutoUpdate ? L"1" : L"0") + L"\r\n";
  return text;
}

}  // namespace sstk
