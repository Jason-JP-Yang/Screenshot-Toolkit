#include "sstk/base/product.h"

#include <cwchar>

namespace sstk {

namespace {

std::wstring EnvironmentFolder(const wchar_t* name) {
  wchar_t folder[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(name, folder, MAX_PATH);
  return length > 0 && length < MAX_PATH ? std::wstring(folder) : std::wstring();
}

std::wstring Under(const std::wstring& folder, const wchar_t* name) { return folder.empty() ? folder : folder + L"\\" + name; }

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

std::wstring InstallDirectory() { return Under(EnvironmentFolder(L"ProgramFiles"), kProductName); }

std::wstring InstalledExecutablePath() { return Under(InstallDirectory(), kExecutableName); }

std::wstring UpdateDirectory() { return Under(InstallDirectory(), L"update"); }

std::wstring StagedExecutablePath() { return Under(UpdateDirectory(), kExecutableName); }

std::wstring DataDirectory() { return Under(EnvironmentFolder(L"ProgramData"), kProductName); }

std::wstring SystemDirectory() {
  wchar_t directory[MAX_PATH];
  const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
  return length > 0 && length < MAX_PATH ? std::wstring(directory) : std::wstring(L"C:\\Windows\\System32");
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty()) return false;
  const std::wstring left = LongPath(a);
  const std::wstring right = LongPath(b);
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) ==
         CSTR_EQUAL;
}

bool IsInstalledCopy() { return SamePath(ExecutablePath(), InstalledExecutablePath()); }

bool DeleteTree(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return true;
  if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) != FALSE;
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) DeleteTree(path + L"\\" + data.cFileName);
      } while (FindNextFileW(find, &data));
      FindClose(find);
    }
  }
  return RemoveDirectoryW(path.c_str()) != FALSE;
}

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
