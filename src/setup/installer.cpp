#include "sstk/setup/installer.h"

#include "sstk/base/product.h"
#include "sstk/base/settings.h"
#include "sstk/version.h"

#include <knownfolders.h>
#include <oleauto.h>
#include <propsys.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <tlhelp32.h>
#include <wbemidl.h>

#include <cwchar>

namespace sstk {

namespace {

enum Failure : int {
  kFailStop = 10,
  kFailCopy,
  kFailSettings,
  kFailRegister,
  kFailService,
  kFailServiceStart,
  kFailRemoveService = 20,
};

constexpr const wchar_t* kInstallSteps[] = {L"Stopping Screenshot Toolkit", L"Copying files", L"Writing settings",
                                            L"Registering with Windows", L"Starting the service"};
constexpr const wchar_t* kUninstallSteps[] = {L"Stopping Screenshot Toolkit", L"Removing the service", L"Deleting files"};

constexpr wchar_t kServiceDescription[] =
    L"Starts Screenshot Toolkit in each signed-in session with the rights it needs, and again if it stops unexpectedly.";
// The default rights, spelled out: signed-in users may send it controls, which is how an About window asks for the
// application in its session.
constexpr wchar_t kServiceSecurity[] =
    L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)";
constexpr wchar_t kShortcutName[] = L"Screenshot Toolkit.lnk";
constexpr wchar_t kTemporaryPrefix[] = L"ScreenshotToolkit-Window-";

// What earlier versions left: logon tasks in their own folder, and a hook process.
constexpr wchar_t kLegacyTaskFolder[] = L"Screenshot Toolkit";
constexpr const wchar_t* kLegacyTasks[] = {L"Launcher", L"Hook Host", L"Application"};
constexpr wchar_t kLegacyHookQuitEvent[] = L"Local\\ScreenshotToolkit.HookHost.Quit";

constexpr PROPERTYKEY kAppUserModelIdKey{{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

class Bstr {
 public:
  explicit Bstr(const wchar_t* text) : value_(SysAllocString(text)) {}
  ~Bstr() { SysFreeString(value_); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  operator BSTR() const { return value_; }

 private:
  BSTR value_;
};

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR folder = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) result = folder;
  CoTaskMemFree(folder);
  return result;
}

std::wstring ShortcutPath() {
  const std::wstring folder = KnownFolder(FOLDERID_CommonPrograms);
  return folder.empty() ? folder : folder + L"\\" + kShortcutName;
}

std::wstring Quoted(const std::wstring& path) { return L"\"" + path + L"\""; }

DWORD ParentProcessId() {
  DWORD parent = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return parent;
  PROCESSENTRY32W entry{sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == GetCurrentProcessId()) {
      parent = entry.th32ParentProcessID;
      break;
    }
  }
  CloseHandle(snapshot);
  return parent;
}

void RemoveProductKey() {
  HKEY software = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &software) != ERROR_SUCCESS) return;
  RegDeleteTreeW(software, kProductName);
  RegCloseKey(software);
}

bool StopService() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
  const DWORD error = service == nullptr ? GetLastError() : NO_ERROR;
  bool stopped = error == ERROR_SERVICE_DOES_NOT_EXIST;
  if (service != nullptr) {
    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (int attempt = 0; attempt < 150; ++attempt) {
      if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED) break;
      Sleep(100);
    }
    stopped = status.dwCurrentState == SERVICE_STOPPED;
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  return stopped;
}

// The service goes first, so that nothing it watches comes back.
bool StopEverything() {
  const bool stopped = StopService();
  RequestQuit();
  if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kLegacyHookQuitEvent)) {
    SetEvent(event);
    CloseHandle(event);
  }
  WaitForApplicationExit(5000);
  // Whatever still runs the installed file or an update waiting to go in, in any session.
  EndProcessesRunning(InstalledExecutablePath());
  EndProcessesRunning(StagedExecutablePath());
  return stopped;
}

bool CopyProgram() {
  const std::wstring self = ExecutablePath();
  const std::wstring target = InstalledExecutablePath();
  if (target.empty()) return false;
  if (SamePath(self, target)) return true;
  CreateDirectoryW(InstallDirectory().c_str(), nullptr);
  const std::wstring staged = target + L".new";
  if (!CopyFileW(self.c_str(), staged.c_str(), FALSE)) return false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    Sleep(100);
  }
  DeleteFileW(staged.c_str());
  return false;
}

// An existing file is the user's and stays as it is.
bool WriteSettings() {
  const std::wstring path = SettingsPath();
  if (path.empty()) return false;
  CreateDirectoryW(DataDirectory().c_str(), nullptr);
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
  const std::wstring text = L"\xFEFF" + DefaultSettingsText();  // The profile functions read UTF-16 when it starts with a BOM.
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
  CloseHandle(file);
  return ok && written == text.size() * sizeof(wchar_t);
}

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetNumber(HKEY key, const wchar_t* name, DWORD value) {
  return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

bool WriteUninstallEntry(const std::wstring& target) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  SYSTEMTIME today{};
  GetLocalTime(&today);
  wchar_t date[16];
  swprintf_s(date, L"%04u%02u%02u", today.wYear, today.wMonth, today.wDay);
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &attributes);
  const DWORD kilobytes = static_cast<DWORD>(((static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32 | attributes.nFileSizeLow) + 1023) / 1024);

  const bool ok = SetString(key, L"DisplayName", kProductName) && SetString(key, L"DisplayVersion", SSTK_VERSION_WIDE) &&
                  SetString(key, L"Publisher", kPublisher) && SetString(key, L"DisplayIcon", target + L",0") &&
                  SetString(key, L"InstallLocation", InstallDirectory()) && SetString(key, L"InstallDate", date) &&
                  SetString(key, L"UninstallString", Quoted(target) + L" --uninstall") &&
                  SetString(key, L"QuietUninstallString", Quoted(target) + L" --uninstall --quiet") &&
                  SetNumber(key, L"NoModify", 1) && SetNumber(key, L"NoRepair", 1) && SetNumber(key, L"EstimatedSize", kilobytes);
  RegCloseKey(key);
  return ok;
}

void RemoveUninstallEntry() {
  HKEY parent = nullptr;
  const std::wstring key = kUninstallKey;
  const std::wstring parent_path = key.substr(0, key.rfind(L'\\'));
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent_path.c_str(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &parent) != ERROR_SUCCESS) return;
  RegDeleteTreeW(parent, key.substr(key.rfind(L'\\') + 1).c_str());
  RegCloseKey(parent);
}

// Named with the same AppUserModelID as the window, so that both share one taskbar button.
bool CreateStartShortcut(const std::wstring& target) {
  const std::wstring path = ShortcutPath();
  ComPtr<IShellLinkW> link;
  if (path.empty() || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
  link->SetPath(target.c_str());
  link->SetWorkingDirectory(InstallDirectory().c_str());
  link->SetDescription(L"Instant screenshots with PrintScreen and Win+Shift+S");
  link->SetIconLocation(target.c_str(), 0);
  ComPtr<IPropertyStore> properties;
  if (SUCCEEDED(link.As(&properties))) {
    PROPVARIANT value{};
    const size_t bytes = (wcslen(kAppUserModelId) + 1) * sizeof(wchar_t);
    value.vt = VT_LPWSTR;
    value.pwszVal = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (value.pwszVal != nullptr) {
      memcpy(value.pwszVal, kAppUserModelId, bytes);
      properties->SetValue(kAppUserModelIdKey, value);
      properties->Commit();
    }
    PropVariantClear(&value);
  }
  ComPtr<IPersistFile> file;
  if (FAILED(link.As(&file)) || FAILED(file->Save(path.c_str(), TRUE))) return false;
  SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
  return true;
}

void RemoveStartShortcut() {
  const std::wstring path = ShortcutPath();
  if (!path.empty() && DeleteFileW(path.c_str())) SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}

void RemoveLegacyTasks() {
  ComPtr<ITaskService> scheduler;
  if (FAILED(CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&scheduler)))) return;
  VARIANT none;
  VariantInit(&none);
  if (FAILED(scheduler->Connect(none, none, none, none))) return;
  ComPtr<ITaskFolder> root;
  if (FAILED(scheduler->GetFolder(Bstr(L"\\"), &root))) return;
  ComPtr<ITaskFolder> folder;
  if (SUCCEEDED(scheduler->GetFolder(Bstr((std::wstring(L"\\") + kLegacyTaskFolder).c_str()), &folder))) {
    for (const wchar_t* task : kLegacyTasks) folder->DeleteTask(Bstr(task), 0);
  }
  root->DeleteFolder(Bstr(kLegacyTaskFolder), 0);
}

// Defender's list of programs allowed through Controlled folder access, changed through the same WMI method that
// Add-MpPreference and Remove-MpPreference call.
bool ChangeFolderProtection(const wchar_t* method, const std::wstring& program) {
  ComPtr<IWbemLocator> locator;
  if (FAILED(CoCreateInstance(__uuidof(WbemLocator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) return false;
  ComPtr<IWbemServices> services;
  if (FAILED(locator->ConnectServer(Bstr(L"ROOT\\Microsoft\\Windows\\Defender"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services))) {
    return false;
  }
  CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                    nullptr, EOAC_NONE);
  const Bstr type(L"MSFT_MpPreference");
  ComPtr<IWbemClassObject> definition;
  ComPtr<IWbemClassObject> signature;
  ComPtr<IWbemClassObject> parameters;
  if (FAILED(services->GetObject(type, 0, nullptr, &definition, nullptr)) || FAILED(definition->GetMethod(method, 0, &signature, nullptr)) ||
      signature == nullptr || FAILED(signature->SpawnInstance(0, &parameters))) {
    return false;
  }
  VARIANT list;
  VariantInit(&list);
  list.vt = VT_ARRAY | VT_BSTR;
  list.parray = SafeArrayCreateVector(VT_BSTR, 0, 1);
  if (list.parray == nullptr) return false;
  LONG index = 0;
  SafeArrayPutElement(list.parray, &index, Bstr(program.c_str()));
  const HRESULT put = parameters->Put(L"ControlledFolderAccessAllowedApplications", 0, &list, 0);
  VariantClear(&list);
  return SUCCEEDED(put) && SUCCEEDED(services->ExecMethod(type, Bstr(method), 0, nullptr, parameters.Get(), nullptr, nullptr));
}

int RegisterAndStartService(const std::wstring& target) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (manager == nullptr) return kFailService;
  const std::wstring binary = Quoted(target) + L" --service";
  SC_HANDLE service = nullptr;
  // A service removed a moment ago can still be on its way out.
  for (int attempt = 0; attempt < 50; ++attempt) {
    service = CreateServiceW(manager, kServiceName, kProductName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                             SERVICE_ERROR_NORMAL, binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    DWORD error = service == nullptr ? GetLastError() : NO_ERROR;
    if (error == ERROR_SERVICE_EXISTS) {
      service = OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
      if (service != nullptr && ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                                     binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kProductName)) {
        break;
      }
      error = GetLastError();
      if (service != nullptr) CloseServiceHandle(service);
      service = nullptr;
    }
    if (service != nullptr || error != ERROR_SERVICE_MARKED_FOR_DELETE) break;
    Sleep(200);
  }
  if (service == nullptr) {
    CloseServiceHandle(manager);
    return kFailService;
  }

  SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(kServiceDescription)};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
  // Should the service itself fail, Windows starts it again.
  SC_ACTION actions[] = {{SC_ACTION_RESTART, 2000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
  SERVICE_FAILURE_ACTIONSW failure{86400, nullptr, nullptr, ARRAYSIZE(actions), actions};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
  SERVICE_FAILURE_ACTIONS_FLAG on_errors{TRUE};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &on_errors);
  PSECURITY_DESCRIPTOR security = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kServiceSecurity, SDDL_REVISION_1, &security, nullptr)) {
    SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, security);
    LocalFree(security);
  }

  int result = kFailServiceStart;
  if (StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
    SERVICE_STATUS status{};
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (!QueryServiceStatus(service, &status) || status.dwCurrentState != SERVICE_START_PENDING) break;
      Sleep(100);
    }
    if (status.dwCurrentState == SERVICE_RUNNING) result = kSetupDone;
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return result;
}

bool RemoveService() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName, DELETE);
  bool removed = service == nullptr && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
  if (service != nullptr) {
    removed = DeleteService(service) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  return removed;
}

// Tracing's log, switch file, and images in this user's %TEMP%.
void RemoveTraceFiles() {
  wchar_t temp[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (length == 0 || length >= MAX_PATH) return;
  const std::wstring directory = temp;
  DeleteFileW((directory + L"ScreenshotToolkit.log").c_str());
  DeleteFileW((directory + L"ScreenshotToolkit.trace").c_str());
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW((directory + L"ScreenshotToolkit-*.png").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    DeleteFileW((directory + data.cFileName).c_str());
  } while (FindNextFileW(find, &data));
  FindClose(find);
}

// No process can delete its own file, so a hidden command prompt keeps trying once this one has gone. Should that
// not start, the folder goes at the next restart, which needs administrator rights.
void RemoveFolderAfterExit(const std::wstring& directory) {
  const std::wstring system = SystemDirectory();
  const std::wstring shell = system + L"\\cmd.exe";
  const std::wstring folder = Quoted(directory);
  std::wstring command = Quoted(shell) + L" /d /q /s /c \"for /l %i in (1,1,60) do (rd /s /q " + folder + L" 2>nul & if not exist " +
                         folder + L" (exit) else (" + Quoted(system + L"\\PING.EXE") + L" -n 2 127.0.0.1 >nul))\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system.c_str(), &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return;
  }
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) MoveFileExW((directory + L"\\" + data.cFileName).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    } while (FindNextFileW(find, &data));
    FindClose(find);
  }
  MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

int Install(HWND window) {
  auto step = [window](int number) {
    if (window != nullptr) PostMessageW(window, kMsgSetupStep, number, ARRAYSIZE(kInstallSteps));
  };
  const std::wstring target = InstalledExecutablePath();
  step(0);
  if (!StopEverything()) return kFailStop;
  // An update waiting from the copy installed before has no place over this one.
  DeleteTree(UpdateDirectory());
  RemoveProductKey();
  step(1);
  if (!CopyProgram()) return kFailCopy;
  step(2);
  if (!WriteSettings()) return kFailSettings;
  step(3);
  if (!WriteUninstallEntry(target) || !CreateStartShortcut(target)) return kFailRegister;
  AllowThroughFolderProtection(true);
  RemoveLegacyTasks();
  step(4);
  return RegisterAndStartService(target);
}

int Uninstall(HWND window) {
  auto step = [window](int number) {
    if (window != nullptr) PostMessageW(window, kMsgSetupStep, number, ARRAYSIZE(kUninstallSteps));
  };
  step(0);
  StopEverything();
  step(1);
  if (!RemoveService()) return kFailRemoveService;
  AllowThroughFolderProtection(false);
  RemoveUninstallEntry();
  RemoveProductKey();
  RemoveStartShortcut();
  RemoveLegacyTasks();
  step(2);
  DeleteTree(DataDirectory());
  RemoveTraceFiles();
  const std::wstring directory = InstallDirectory();
  if (!directory.empty() && !DeleteTree(directory)) RemoveFolderAfterExit(directory);
  return kSetupDone;
}

HANDLE RunElevated(const wchar_t* arguments, HWND owner, bool* cancelled) {
  const std::wstring self = ExecutablePath();
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.hwnd = owner;
  info.lpVerb = L"runas";
  info.lpFile = self.c_str();
  info.lpParameters = arguments;
  info.nShow = SW_HIDE;
  if (ShellExecuteExW(&info)) return info.hProcess;
  if (cancelled != nullptr) *cancelled = GetLastError() == ERROR_CANCELLED;
  return nullptr;
}

}  // namespace

int SetupStepCount(SetupAction action) {
  return action == SetupAction::kInstall ? ARRAYSIZE(kInstallSteps) : ARRAYSIZE(kUninstallSteps);
}

const wchar_t* SetupStepText(SetupAction action, int step) {
  if (step < 0 || step >= SetupStepCount(action)) return L"";
  return action == SetupAction::kInstall ? kInstallSteps[step] : kUninstallSteps[step];
}

std::wstring SetupFailureText(int code) {
  switch (code) {
    case kSetupCancelled: return L"Administrator approval was declined.";
    case kFailStop: return L"The running copy didn't stop. Try again.";
    case kFailCopy: return L"Couldn't copy the program into Program Files.";
    case kFailSettings: return L"Couldn't write the settings file.";
    case kFailRegister: return L"Couldn't register with Windows.";
    case kFailService: return L"Couldn't register the service.";
    case kFailServiceStart: return L"The service didn't start. Restart to finish.";
    case kFailRemoveService: return L"Couldn't remove the service. Try again.";
    default: return L"Something went wrong (code " + std::to_wstring(code) + L").";
  }
}

bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

int RunSetupSteps(SetupAction action, HWND window) {
  // A process's current folder cannot be removed, so this one stays out of the folders it touches.
  SetCurrentDirectoryW(SystemDirectory().c_str());
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const int result = action == SetupAction::kInstall ? Install(window) : Uninstall(window);
  if (SUCCEEDED(com)) CoUninitialize();
  return result;
}

HANDLE StartElevatedSetup(SetupAction action, HWND window, bool* cancelled) {
  wchar_t arguments[96];
  swprintf_s(arguments, L"--elevated %ls %llu", action == SetupAction::kInstall ? L"install" : L"uninstall",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window)));
  *cancelled = false;
  return RunElevated(arguments, window, cancelled);
}

int RunQuietSetup(SetupAction action) {
  if (IsElevated()) return RunSetupSteps(action, nullptr);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool cancelled = false;
  HANDLE process = RunElevated(action == SetupAction::kInstall ? L"--install --quiet" : L"--uninstall --quiet", nullptr, &cancelled);
  if (SUCCEEDED(com)) CoUninitialize();
  if (process == nullptr) return cancelled ? kSetupCancelled : 1;
  WaitForSingleObject(process, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process, &code);
  CloseHandle(process);
  return static_cast<int>(code);
}

void EndProcessesRunning(const std::wstring& image) {
  if (image.empty()) return;
  const DWORD self = GetCurrentProcessId();
  const DWORD parent = ParentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W entry{sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == self || entry.th32ProcessID == parent || _wcsicmp(entry.szExeFile, kExecutableName) != 0) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
    if (process == nullptr) continue;
    wchar_t path[MAX_PATH * 4];
    DWORD length = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &length) && SamePath(path, image) && TerminateProcess(process, 1)) {
      WaitForSingleObject(process, 3000);
    }
    CloseHandle(process);
  }
  CloseHandle(snapshot);
}

bool RecordUninstallEntry() { return WriteUninstallEntry(InstalledExecutablePath()); }

void AllowThroughFolderProtection(bool allow) {
  const std::wstring program = InstalledExecutablePath();
  if (program.empty()) return;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool changed = ChangeFolderProtection(allow ? L"Add" : L"Remove", program);
  Trace("controlled folder access %s: %s", allow ? "allow" : "remove", changed ? "done" : "not changed");
  if (SUCCEEDED(com)) CoUninitialize();
}

bool HandOffToTemporaryCopy(const wchar_t* arguments) {
  if (!IsInstalledCopy()) return false;
  wchar_t temp[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (length == 0 || length >= MAX_PATH) return false;
  const std::wstring directory = std::wstring(temp) + kTemporaryPrefix + std::to_wstring(GetCurrentProcessId());
  const std::wstring copy = directory + L"\\" + kExecutableName;
  CreateDirectoryW(directory.c_str(), nullptr);
  if (!CopyFileW(ExecutablePath().c_str(), copy.c_str(), FALSE)) {
    RemoveDirectoryW(directory.c_str());
    return false;
  }
  std::wstring command_line = Quoted(copy) + L" " + arguments;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(copy.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, temp, &startup, &process)) {
    DeleteFileW(copy.c_str());
    RemoveDirectoryW(directory.c_str());
    return false;
  }
  AllowSetForegroundWindow(process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

void RemoveTemporaryCopyAtExit() {
  const std::wstring self = ExecutablePath();
  const size_t slash = self.rfind(L'\\');
  if (slash == std::wstring::npos) return;
  const std::wstring directory = self.substr(0, slash);
  wchar_t temp[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (length == 0 || length >= MAX_PATH) return;
  if (!SamePath(directory.substr(0, directory.rfind(L'\\') + 1), temp)) return;
  if (directory.compare(directory.rfind(L'\\') + 1, wcslen(kTemporaryPrefix), kTemporaryPrefix) != 0) return;
  RemoveFolderAfterExit(directory);
}

}  // namespace sstk
