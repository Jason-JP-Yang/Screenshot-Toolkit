#include "sstk/service/service.h"

#include "sstk/base/product.h"
#include "sstk/update/updater.h"

#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <climits>
#include <map>
#include <mutex>

namespace sstk {

namespace {

// User-defined service controls. The first range plus a session number starts the application in that session.
constexpr DWORD kControlStartFirst = 128;
constexpr DWORD kControlStartLast = 191;
constexpr DWORD kControlCheck = 254;
constexpr DWORD kControlApply = 255;

// Requests queued for the supervising thread beside the session changes, which use WTS_* values.
constexpr DWORD kRequestStart = 0xFFFFFFFF;
constexpr DWORD kRequestCheck = 0xFFFFFFFE;
constexpr DWORD kRequestApply = 0xFFFFFFFD;

constexpr ULONGLONG kQuickExitMs = 10000;  // Failing sooner than this after starting counts as failing at once.
constexpr ULONGLONG kRestartMs = 1000;
constexpr int kFastRestarts = 3;
constexpr ULONGLONG kBackoffMs = 15000;
constexpr ULONGLONG kBackoffMaxMs = 300000;
// A session can be announced a moment before its user's token is ready.
constexpr ULONGLONG kTokenRetryMs = 1000;
constexpr int kTokenAttempts = 30;
// Sessions whose application was asked to quit, kept so that a service restarted by an update leaves them be.
constexpr wchar_t kHeldValue[] = L"HeldSessions";

struct Session {
  HANDLE process = nullptr;
  ULONGLONG started = 0;
  ULONGLONG retry_at = 0;  // 0 when nothing is scheduled.
  int failures = 0;
  int attempts = 0;
  bool held = false;  // Exited cleanly, so it stays down until the next sign-in or a request.
};

struct ServiceEvent {
  DWORD kind;
  DWORD session;
};

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop = nullptr;
HANDLE g_wake = nullptr;
std::mutex g_events_lock;
std::vector<ServiceEvent> g_events;

void ReportStatus(DWORD state) {
  const bool settled = state == SERVICE_RUNNING || state == SERVICE_STOPPED;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
  g_status.dwWin32ExitCode = NO_ERROR;
  g_status.dwWaitHint = settled ? 0 : 25000;
  g_status.dwCheckPoint = settled ? 0 : g_status.dwCheckPoint + 1;
  SetServiceStatus(g_status_handle, &g_status);
}

void Post(DWORD kind, DWORD session) {
  {
    std::lock_guard lock(g_events_lock);
    g_events.push_back({kind, session});
  }
  SetEvent(g_wake);
}

DWORD WINAPI Handler(DWORD control, DWORD event_type, void* event_data, void*) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      ReportStatus(SERVICE_STOP_PENDING);
      SetEvent(g_stop);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
      Post(event_type, static_cast<WTSSESSION_NOTIFICATION*>(event_data)->dwSessionId);
      return NO_ERROR;
    case kControlCheck:
      Post(kRequestCheck, 0);
      return NO_ERROR;
    case kControlApply:
      Post(kRequestApply, 0);
      return NO_ERROR;
    default:
      if (control >= kControlStartFirst && control <= kControlStartLast) {
        Post(kRequestStart, control - kControlStartFirst);
        return NO_ERROR;
      }
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

bool HasUiAccess(HANDLE process) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
  DWORD value = 0;
  DWORD size = 0;
  const BOOL read = GetTokenInformation(token, TokenUIAccess, &value, sizeof(value), &size);
  CloseHandle(token);
  return read && value != 0;
}

bool SendControl(DWORD code) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  bool sent = false;
  if (SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_USER_DEFINED_CONTROL)) {
    SERVICE_STATUS status{};
    sent = ControlService(service, code, &status) != FALSE;
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  return sent;
}

std::vector<DWORD> LoadHeld() {
  std::vector<DWORD> held;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProductKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return held;
  DWORD ids[64];
  DWORD size = sizeof(ids);
  DWORD type = 0;
  if (RegQueryValueExW(key, kHeldValue, nullptr, &type, reinterpret_cast<BYTE*>(ids), &size) == ERROR_SUCCESS && type == REG_BINARY) {
    held.assign(ids, ids + size / sizeof(DWORD));
  }
  RegCloseKey(key);
  return held;
}

std::vector<DWORD> HeldIds(const std::map<DWORD, Session>& sessions) {
  std::vector<DWORD> held;
  for (const auto& [id, session] : sessions) {
    if (session.held && held.size() < 64) held.push_back(id);
  }
  return held;
}

void SaveHeld(const std::vector<DWORD>& held) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kProductKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, kHeldValue, 0, REG_BINARY, reinterpret_cast<const BYTE*>(held.data()), static_cast<DWORD>(held.size() * sizeof(DWORD)));
  RegCloseKey(key);
}

// After the service restarts, it takes over the applications it started before rather than start them twice, and an
// update's apply step hands it those it moved to the download, which come back to the installed file.
void AdoptRunning(std::map<DWORD, Session>& sessions, ULONGLONG now) {
  const std::wstring self = ExecutablePath();
  for (const RunningCopy& copy : FindApplications(self)) {
    Session& session = sessions[copy.session];
    if (session.process != nullptr) {
      CloseHandle(copy.process);
      continue;
    }
    session.process = copy.process;
    session.started = now;
  }
  for (const RunningCopy& copy : FindApplications(StagedExecutablePath())) {
    Session& session = sessions[copy.session];
    if (session.process == nullptr) {
      if (HANDLE fresh = LaunchApplication(copy.session, self, true)) {
        session.process = fresh;
        session.started = now;
      }
    }
    CloseHandle(copy.process);
  }
}

bool AnyRunning(const std::map<DWORD, Session>& sessions) {
  for (const auto& [id, session] : sessions) {
    if (session.process != nullptr && WaitForSingleObject(session.process, 0) == WAIT_TIMEOUT) return true;
  }
  return false;
}

void Ensure(DWORD id, Session& session, ULONGLONG now) {
  if (session.process != nullptr) {
    if (WaitForSingleObject(session.process, 0) == WAIT_TIMEOUT) return;
    CloseHandle(session.process);
    session.process = nullptr;
  }
  session.retry_at = 0;
  if (HANDLE process = LaunchApplication(id, ExecutablePath(), false)) {
    session.process = process;
    session.started = now;
    session.attempts = 0;
    return;
  }
  if (++session.attempts < kTokenAttempts) session.retry_at = now + kTokenRetryMs;
}

void Exited(Session& session, ULONGLONG now) {
  DWORD code = 0;
  GetExitCodeProcess(session.process, &code);
  CloseHandle(session.process);
  session.process = nullptr;
  // A clean exit: asked to quit, already running, or the user signing out.
  if (code == 0 || session.held) {
    session.held = true;
    session.failures = 0;
    return;
  }
  session.failures = now - session.started < kQuickExitMs ? session.failures + 1 : 1;
  const int beyond = std::min(session.failures - kFastRestarts - 1, 8);
  session.retry_at = now + (beyond < 0 ? kRestartMs : std::min(kBackoffMaxMs, kBackoffMs << beyond));
}

void Handle(std::map<DWORD, Session>& sessions, Updater& updater, const ServiceEvent& event, ULONGLONG now) {
  if (event.kind == kRequestCheck) {
    updater.Check(true);
    return;
  }
  if (event.kind == kRequestApply) {
    updater.Apply();
    return;
  }
  Session& session = sessions[event.session];
  switch (event.kind) {
    case WTS_SESSION_LOGON:
    case kRequestStart:
      session.held = false;
      session.failures = 0;
      session.attempts = 0;
      Ensure(event.session, session, now);
      updater.CheckSoon(now);
      break;
    case WTS_SESSION_LOGOFF:
      session.held = true;
      session.retry_at = 0;
      if (session.process == nullptr) sessions.erase(event.session);
      break;
    case WTS_CONSOLE_CONNECT:
    case WTS_REMOTE_CONNECT:
    case WTS_SESSION_UNLOCK:
      if (!session.held && session.retry_at == 0) Ensure(event.session, session, now);
      break;
    default:
      break;
  }
}

void Supervise() {
  std::map<DWORD, Session> sessions;
  Updater updater;
  const ULONGLONG start = GetTickCount64();
  updater.Begin(start);
  for (const DWORD id : LoadHeld()) sessions[id].held = true;
  AdoptRunning(sessions, start);
  WTS_SESSION_INFOW* list = nullptr;
  DWORD count = 0;
  if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count)) {
    for (DWORD i = 0; i < count; ++i) {
      const DWORD id = list[i].SessionId;
      if (id == 0 || (list[i].State != WTSActive && list[i].State != WTSDisconnected)) continue;
      Session& session = sessions[id];
      if (!session.held) Ensure(id, session, start);
      updater.CheckSoon(start);
    }
    WTSFreeMemory(list);
  }
  std::vector<DWORD> saved_held = HeldIds(sessions);
  SaveHeld(saved_held);

  for (;;) {
    std::vector<HANDLE> handles{g_stop, g_wake, updater.done()};
    std::vector<DWORD> owners;
    ULONGLONG next = ULLONG_MAX;
    for (const auto& [id, session] : sessions) {
      if (session.process != nullptr && handles.size() < MAXIMUM_WAIT_OBJECTS) {
        handles.push_back(session.process);
        owners.push_back(id);
      }
      if (session.retry_at != 0) next = std::min(next, session.retry_at);
    }
    const ULONGLONG before = GetTickCount64();
    ULONGLONG timeout = next == ULLONG_MAX ? INFINITE : (next > before ? next - before : 0);
    timeout = std::min<ULONGLONG>(timeout, updater.NextWake(before));
    const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE,
                                                timeout >= INFINITE ? INFINITE : static_cast<DWORD>(std::min<ULONGLONG>(timeout, INT_MAX)));
    const ULONGLONG now = GetTickCount64();
    if (result == WAIT_OBJECT_0) break;
    if (result == WAIT_FAILED) {
      Sleep(1000);
    } else if (result == WAIT_OBJECT_0 + 1) {
      std::vector<ServiceEvent> events;
      {
        std::lock_guard lock(g_events_lock);
        events.swap(g_events);
      }
      for (const ServiceEvent& event : events) Handle(sessions, updater, event, now);
    } else if (result == WAIT_OBJECT_0 + 2) {
      updater.Finish(now);
    } else if (result >= WAIT_OBJECT_0 + 3 && result < WAIT_OBJECT_0 + handles.size()) {
      Exited(sessions[owners[result - WAIT_OBJECT_0 - 3]], now);
    }
    for (auto& [id, session] : sessions) {
      if (session.retry_at != 0 && session.retry_at <= now) Ensure(id, session, now);
    }
    updater.Tick(now);
    // An update waits for a quiet moment, which no running application means too, unless the About window asked.
    if (updater.ready() && (updater.manual() || !AnyRunning(sessions))) updater.Apply();
    if (std::vector<DWORD> held = HeldIds(sessions); held != saved_held) {
      SaveHeld(held);
      saved_held = std::move(held);
    }
  }
  for (auto& [id, session] : sessions) {
    if (session.process != nullptr) CloseHandle(session.process);
  }
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
  if (g_status_handle == nullptr || g_stop == nullptr || g_wake == nullptr) return;
  ReportStatus(SERVICE_RUNNING);
  Supervise();
  ReportStatus(SERVICE_STOPPED);
}

}  // namespace

int RunService() {
  wchar_t name[] = L"ScreenshotToolkit";
  const SERVICE_TABLE_ENTRYW table[] = {{name, ServiceMain}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

bool AskServiceToStart() {
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session > kControlStartLast - kControlStartFirst) return false;
  return SendControl(kControlStartFirst + session);
}

bool AskServiceToCheck() { return SendControl(kControlCheck); }

bool AskServiceToApplyUpdate() { return SendControl(kControlApply); }

std::vector<RunningCopy> FindApplications(const std::wstring& image) {
  std::vector<RunningCopy> copies;
  if (image.empty()) return copies;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return copies;
  PROCESSENTRY32W entry{sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == GetCurrentProcessId() || _wcsicmp(entry.szExeFile, kExecutableName) != 0) continue;
    DWORD session = 0;
    if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session == 0) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
    if (process == nullptr) continue;
    wchar_t path[MAX_PATH * 4];
    DWORD length = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &length) && SamePath(path, image) && HasUiAccess(process)) {
      copies.push_back({process, session});
    } else {
      CloseHandle(process);
    }
  }
  CloseHandle(snapshot);
  return copies;
}

HANDLE LaunchApplication(DWORD session, const std::wstring& image, bool takeover) {
  HANDLE user = nullptr;
  if (!WTSQueryUserToken(session, &user)) return nullptr;
  HANDLE token = nullptr;
  const BOOL duplicated = DuplicateTokenEx(user, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &token);
  CloseHandle(user);
  if (!duplicated) return nullptr;

  // Without it the application still works, only below Start and Task View and deaf to administrator windows.
  DWORD ui_access = 1;
  SetTokenInformation(token, TokenUIAccess, &ui_access, sizeof(ui_access));

  void* environment = nullptr;
  CreateEnvironmentBlock(&environment, token, FALSE);
  wchar_t desktop[] = L"winsta0\\default";
  STARTUPINFOW startup{sizeof(startup)};
  startup.lpDesktop = desktop;
  PROCESS_INFORMATION process{};
  std::wstring command_line = L"\"" + image + L"\" --run";
  if (takeover) command_line += L" --takeover";
  const BOOL started = CreateProcessAsUserW(token, image.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                                            CREATE_UNICODE_ENVIRONMENT, environment, nullptr, &startup, &process);
  if (environment != nullptr) DestroyEnvironmentBlock(environment);
  CloseHandle(token);
  if (!started) return nullptr;
  CloseHandle(process.hThread);
  return process.hProcess;
}

}  // namespace sstk
