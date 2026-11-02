// Privilege model: the application itself runs unelevated, so drag and drop, opening files, and
// the clipboard behave normally with every other program. Only a minimal hook host runs elevated,
// because Windows does not deliver keys typed into administrator windows to an unelevated hook.
//
// The elevated task runs without a UAC prompt at every logon, so its executable has to live where
// only administrators can write. Pointing it at a user-writable path would let any program
// replace the file and silently gain administrator rights, hence the copy into Program Files.
#include "install.h"

#include "platform.h"

#include <knownfolders.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

namespace sstk {

namespace {

constexpr wchar_t kTaskFolder[] = L"Screenshot Toolkit";
constexpr wchar_t kHookTask[] = L"Screenshot Toolkit\\Hook Host";
constexpr wchar_t kAppTask[] = L"Screenshot Toolkit\\Application";
constexpr wchar_t kExeName[] = L"ScreenshotToolkit.exe";
constexpr wchar_t kMessageTitle[] = L"Screenshot Toolkit";

bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

std::wstring CurrentExecutablePath() {
  std::wstring path(32768, L'\0');
  path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
  return path;
}

std::wstring InstallDirectory() {
  PWSTR folder = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) {
    result = std::wstring(folder) + L"\\" + kTaskFolder;
  }
  CoTaskMemFree(folder);
  return result;
}

std::wstring CurrentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  BYTE buffer[256];
  DWORD size = 0;
  std::wstring result;
  if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
    PWSTR text = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &text)) {
      result = text;
      LocalFree(text);
    }
  }
  CloseHandle(token);
  return result;
}

int RelaunchElevated(const wchar_t* argument) {
  const std::wstring self = CurrentExecutablePath();
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = self.c_str();
  info.lpParameters = argument;
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) return 1;
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  return static_cast<int>(code);
}

bool RunHidden(std::wstring command_line) {
  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return code == 0;
}

std::wstring TaskXml(const std::wstring& sid, const std::wstring& command, const wchar_t* arguments, const wchar_t* run_level,
                     const wchar_t* description) {
  std::wstring xml;
  xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n";
  xml += L"<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
  xml += L"<RegistrationInfo><Description>";
  xml += description;
  xml += L"</Description></RegistrationInfo>\n";
  xml += L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>\n";
  xml += L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>";
  xml += run_level;
  xml += L"</RunLevel></Principal></Principals>\n";
  xml += L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>";
  xml += L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>";
  xml += L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>";
  xml += L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>";
  xml += L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>";
  xml += L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>";
  xml += L"<WakeToRun>false</WakeToRun><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority>";
  xml += L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure></Settings>\n";
  xml += L"<Actions Context=\"Author\"><Exec><Command>\"" + command + L"\"</Command>";
  if (arguments != nullptr) {
    xml += L"<Arguments>";
    xml += arguments;
    xml += L"</Arguments>";
  }
  xml += L"</Exec></Actions>\n</Task>\n";
  return xml;
}

bool RegisterTask(const wchar_t* name, const std::wstring& xml) {
  wchar_t directory[MAX_PATH];
  wchar_t file[MAX_PATH];
  if (GetTempPathW(MAX_PATH, directory) == 0 || GetTempFileNameW(directory, L"sst", 0, file) == 0) return false;
  HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const wchar_t bom = 0xFEFF;
  DWORD written = 0;
  WriteFile(handle, &bom, sizeof(bom), &written, nullptr);
  WriteFile(handle, xml.data(), static_cast<DWORD>(xml.size() * sizeof(wchar_t)), &written, nullptr);
  CloseHandle(handle);

  std::wstring command = L"schtasks.exe /Create /F /TN \"";
  command += name;
  command += L"\" /XML \"";
  command += file;
  command += L"\"";
  const bool ok = RunHidden(command);
  DeleteFileW(file);
  return ok;
}

void DeleteTask(const wchar_t* name) {
  std::wstring command = L"schtasks.exe /Delete /F /TN \"";
  command += name;
  command += L"\"";
  RunHidden(command);
}

void StartTask(const wchar_t* name) {
  std::wstring command = L"schtasks.exe /Run /TN \"";
  command += name;
  command += L"\"";
  RunHidden(command);
}

void SignalEventByName(const wchar_t* name) {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
  if (event != nullptr) {
    SetEvent(event);
    CloseHandle(event);
  }
}

void WaitForMutexRelease(const wchar_t* name) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (mutex == nullptr) return;
    CloseHandle(mutex);
    Sleep(100);
  }
}

void StopRunningInstances() {
  SignalEventByName(kQuitEventName);
  SignalEventByName(kHookHostQuitEventName);
  WaitForMutexRelease(kAppMutexName);
  WaitForMutexRelease(kHookHostMutexName);
}

}  // namespace

void RequestQuit() { SignalEventByName(kQuitEventName); }

int InstallApplication() {
  if (!IsElevated()) return RelaunchElevated(L"--install");

  StopRunningInstances();

  const std::wstring directory = InstallDirectory();
  const std::wstring sid = CurrentUserSid();
  if (directory.empty() || sid.empty()) {
    MessageBoxW(nullptr, L"Could not resolve the install location or the current user.", kMessageTitle, MB_ICONERROR);
    return 1;
  }
  CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring target = directory + L"\\" + kExeName;
  const std::wstring self = CurrentExecutablePath();
  if (_wcsicmp(self.c_str(), target.c_str()) != 0 && !CopyFileW(self.c_str(), target.c_str(), FALSE)) {
    MessageBoxW(nullptr, L"Could not copy the program into Program Files.", kMessageTitle, MB_ICONERROR);
    return 1;
  }

  const bool ok = RegisterTask(kHookTask, TaskXml(sid, target, L"--hook-host", L"HighestAvailable",
                                                  L"Keeps PrintScreen and Win+Shift+S working while an administrator window has focus.")) &&
                  RegisterTask(kAppTask, TaskXml(sid, target, nullptr, L"LeastPrivilege",
                                                 L"Instant screenshots. Runs quietly in the background."));
  if (!ok) {
    MessageBoxW(nullptr, L"Could not register the logon tasks.", kMessageTitle, MB_ICONERROR);
    return 1;
  }

  StartTask(kHookTask);
  StartTask(kAppTask);
  MessageBoxW(nullptr, L"Installed. PrintScreen and Win+Shift+S now take instant screenshots, and it starts automatically at logon.",
              kMessageTitle, MB_ICONINFORMATION);
  return 0;
}

int UninstallApplication() {
  if (!IsElevated()) return RelaunchElevated(L"--uninstall");

  StopRunningInstances();
  DeleteTask(kHookTask);
  DeleteTask(kAppTask);

  const std::wstring directory = InstallDirectory();
  if (!directory.empty()) {
    const std::wstring target = directory + L"\\" + kExeName;
    if (!DeleteFileW(target.c_str())) MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!RemoveDirectoryW(directory.c_str())) MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  }
  MessageBoxW(nullptr, L"Uninstalled. Screenshots saved in your Pictures folder were left in place.", kMessageTitle, MB_ICONINFORMATION);
  return 0;
}

}  // namespace sstk
