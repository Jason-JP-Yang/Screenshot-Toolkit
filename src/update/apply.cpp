#include "sstk/base/product.h"
#include "sstk/service/service.h"
#include "sstk/setup/installer.h"
#include "sstk/update/state.h"
#include "sstk/update/updater.h"
#include "sstk/version.h"

namespace sstk {

namespace {

constexpr DWORD kServiceWaitMs = 20000;
constexpr DWORD kHandOverWaitMs = 10000;
constexpr int kReplaceTries = 40;
constexpr DWORD kReplaceRetryMs = 250;

bool StopTheService() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  bool stopped = false;
  if (SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS)) {
    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (DWORD waited = 0; waited < kServiceWaitMs; waited += 100) {
      if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED) break;
      Sleep(100);
    }
    stopped = status.dwCurrentState == SERVICE_STOPPED;
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  return stopped;
}

void StartTheService() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return;
  if (SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_START)) {
    StartServiceW(service, 0, nullptr);
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
}

// Each application running `from` moves to `to`: the new one starts fully, then asks the old one to go.
void HandOver(const std::wstring& from, const std::wstring& to) {
  for (const RunningCopy& copy : FindApplications(from)) {
    if (HANDLE fresh = LaunchApplication(copy.session, to, true)) CloseHandle(fresh);
    if (WaitForSingleObject(copy.process, kHandOverWaitMs) != WAIT_OBJECT_0) TerminateProcess(copy.process, 1);
    CloseHandle(copy.process);
  }
}

}  // namespace

int RunApplyUpdate() {
  const std::wstring self = ExecutablePath();
  const std::wstring installed = InstalledExecutablePath();
  if (installed.empty() || !SamePath(self, StagedExecutablePath())) return 1;
  SetCurrentDirectoryW(SystemDirectory().c_str());
  WriteUpdateState(UpdateState::kInstalling, SSTK_VERSION_WIDE);

  // Nothing may run the installed file while it is replaced: not the service, not an application.
  StopTheService();
  HandOver(installed, self);
  EndProcessesRunning(installed);

  // The installed file is swapped for a full copy in one rename, so it is whole whenever this stops.
  const std::wstring fresh = installed + L".new";
  bool replaced = false;
  if (CopyFileW(self.c_str(), fresh.c_str(), FALSE)) {
    for (int attempt = 0; attempt < kReplaceTries && !replaced; ++attempt) {
      replaced = MoveFileExW(fresh.c_str(), installed.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
      if (!replaced) Sleep(kReplaceRetryMs);
    }
    if (!replaced) DeleteFileW(fresh.c_str());
  }
  if (replaced) {
    RecordUninstallEntry();
    WriteUpdateState(UpdateState::kUpdated, SSTK_VERSION_WIDE);
  } else {
    WriteUpdateState(UpdateState::kFailed, SSTK_VERSION_WIDE, L"Couldn't replace the installed program.");
  }
  // Whichever file the service now runs takes the applications back from this copy, then deletes it.
  StartTheService();
  return replaced ? 0 : 1;
}

}  // namespace sstk
