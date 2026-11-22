#include "sstk/update/state.h"

#include "sstk/base/product.h"

namespace sstk {

namespace {

HKEY OpenForReading() {
  HKEY key = nullptr;
  return RegOpenKeyExW(HKEY_LOCAL_MACHINE, UpdateKeyPath().c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS ? key
                                                                                                                                    : nullptr;
}

HKEY OpenForWriting() {
  HKEY key = nullptr;
  return RegCreateKeyExW(HKEY_LOCAL_MACHINE, UpdateKeyPath().c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                         nullptr, &key, nullptr) == ERROR_SUCCESS
             ? key
             : nullptr;
}

DWORD ReadDword(HKEY key, const wchar_t* name) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  return RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS ? value : 0;
}

std::wstring ReadString(HKEY key, const wchar_t* name) {
  wchar_t buffer[512];
  DWORD size = sizeof(buffer) - sizeof(wchar_t);
  DWORD type = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) != ERROR_SUCCESS || type != REG_SZ) return {};
  buffer[size / sizeof(wchar_t)] = L'\0';
  return buffer;
}

void SetDword(HKEY key, const wchar_t* name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
  RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

std::wstring UpdateKeyPath() { return std::wstring(kProductKey) + L"\\Update"; }

void CreateUpdateKey() {
  if (HKEY key = OpenForWriting()) RegCloseKey(key);
}

bool ReadUpdateStatus(UpdateStatus* status) {
  *status = UpdateStatus{};
  HKEY key = OpenForReading();
  if (key == nullptr) return false;
  status->sequence = ReadDword(key, L"Sequence");
  status->state = static_cast<UpdateState>(ReadDword(key, L"State"));
  status->progress = ReadDword(key, L"Progress");
  status->target = ReadString(key, L"Target");
  status->message = ReadString(key, L"Message");
  RegCloseKey(key);
  return true;
}

// The sequence is written last, so that a reader who sees it change also sees the rest.
void WriteUpdateState(UpdateState state, const std::wstring& target, const std::wstring& message) {
  HKEY key = OpenForWriting();
  if (key == nullptr) return;
  const DWORD sequence = ReadDword(key, L"Sequence") + 1;
  SetDword(key, L"State", static_cast<DWORD>(state));
  SetDword(key, L"Progress", 0);
  SetString(key, L"Target", target);
  SetString(key, L"Message", message);
  SetDword(key, L"Sequence", sequence);
  RegCloseKey(key);
}

void WriteUpdateProgress(DWORD progress) {
  HKEY key = OpenForWriting();
  if (key == nullptr) return;
  SetDword(key, L"Progress", progress);
  RegCloseKey(key);
}

uint64_t ReadUpdateNumber(const wchar_t* name) {
  HKEY key = OpenForReading();
  if (key == nullptr) return 0;
  uint64_t value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_QWORD) value = 0;
  RegCloseKey(key);
  return value;
}

void WriteUpdateNumber(const wchar_t* name, uint64_t value) {
  HKEY key = OpenForWriting();
  if (key == nullptr) return;
  RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

std::wstring ReadUpdateText(const wchar_t* name) {
  HKEY key = OpenForReading();
  if (key == nullptr) return {};
  std::wstring value = ReadString(key, name);
  RegCloseKey(key);
  return value;
}

void WriteUpdateText(const wchar_t* name, const std::wstring& value) {
  HKEY key = OpenForWriting();
  if (key == nullptr) return;
  SetString(key, name, value);
  RegCloseKey(key);
}

}  // namespace sstk
