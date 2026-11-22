#include "sstk/base/product.h"

#include <knownfolders.h>
#include <shlobj.h>

namespace sstk {

namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR folder = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) result = folder;
  CoTaskMemFree(folder);
  return result;
}

std::wstring LongPath(const std::wstring& path) {
  std::wstring result(32768, L'\0');
  const DWORD length = GetLongPathNameW(path.c_str(), result.data(), static_cast<DWORD>(result.size()));
  if (length == 0 || length >= result.size()) return path;
  result.resize(length);
  return result;
}

}  // namespace

std::wstring ExecutablePath() {
  std::wstring path(32768, L'\0');
  path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
  return path;
}

std::wstring InstallDirectory() {
  const std::wstring folder = KnownFolder(FOLDERID_ProgramFiles);
  return folder.empty() ? folder : folder + L"\\" + kProductName;
}

std::wstring InstalledExecutablePath() {
  const std::wstring directory = InstallDirectory();
  return directory.empty() ? directory : directory + L"\\" + kExecutableName;
}

std::wstring DataDirectory() {
  const std::wstring folder = KnownFolder(FOLDERID_ProgramData);
  return folder.empty() ? folder : folder + L"\\" + kProductName;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty()) return false;
  const std::wstring left = LongPath(a);
  const std::wstring right = LongPath(b);
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) ==
         CSTR_EQUAL;
}

bool IsInstalledCopy() { return SamePath(ExecutablePath(), InstalledExecutablePath()); }

// The handle is closed at once: held open, it would keep the name alive and turn away an instance starting meanwhile.
bool IsApplicationRunning() {
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kAppMutexName);
  if (mutex == nullptr) return false;
  CloseHandle(mutex);
  return true;
}

void RequestQuit() {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kQuitEventName);
  if (event == nullptr) return;
  SetEvent(event);
  CloseHandle(event);
}

bool WaitForApplicationExit(DWORD timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (IsApplicationRunning()) {
    if (GetTickCount64() >= deadline) return false;
    Sleep(50);
  }
  return true;
}

}  // namespace sstk
