// Entry point. Modes:
//   (no arguments)  The About window when this is the installed copy, and the setup window otherwise.
//   --run           The application: capture, overlay, and persistence. The service starts it this way. With
//                   --takeover, it replaces the copy running in this session once it is ready.
//   --service       The service that keeps the application running in each session. See service/service.h.
//   --apply-update  Puts a downloaded update in place. See update/updater.h.
//   --install       The setup window. With --quiet, installs without one.
//   --uninstall     The uninstall window, as Settings > Apps opens it. With --quiet, removes without one.
//   --about         The About window.
//   --elevated <install|uninstall> <window>   The setup steps, run elevated for the window.
//   --quit          Asks the running application to exit. It stays down until the next sign-in.
#include "sstk/base/platform.h"
#include "sstk/base/product.h"
#include "sstk/capture/capture.h"
#include "sstk/capture/hook.h"
#include "sstk/config/core.h"
#include "sstk/graphics/compositor.h"
#include "sstk/overlay/dragdrop.h"
#include "sstk/overlay/overlay.h"
#include "sstk/service/service.h"
#include "sstk/setup/installer.h"
#include "sstk/setup/window.h"
#include "sstk/storage/store.h"
#include "sstk/update/state.h"
#include "sstk/update/updater.h"

#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sstk {

namespace {

constexpr wchar_t kControllerClass[] = L"ScreenshotToolkit.Controller";
constexpr UINT_PTR kTrimTimer = 2;
constexpr UINT_PTR kClipboardTimer = 3;
constexpr UINT_PTR kUpdateTimer = 4;
constexpr UINT kUpdateIdlePollMs = 5000;
constexpr DWORD kTakeoverWaitMs = 10000;

// Watches the service's update state through the registry's own notification, so it costs nothing until it changes.
class UpdateWatch {
 public:
  ~UpdateWatch() {
    if (key_ != nullptr) RegCloseKey(key_);
    if (event_ != nullptr) CloseHandle(event_);
  }

  bool Open() {
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UpdateKeyPath().c_str(), 0, KEY_NOTIFY | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key_) != ERROR_SUCCESS) {
      key_ = nullptr;
      return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return Arm();
  }

  bool Arm() {
    return event_ != nullptr && RegNotifyChangeKeyValue(key_, FALSE, REG_NOTIFY_CHANGE_LAST_SET, event_, TRUE) == ERROR_SUCCESS;
  }

  HANDLE event() const { return event_; }

 private:
  HKEY key_ = nullptr;
  HANDLE event_ = nullptr;
};

DWORD InputIdleMs() {
  LASTINPUTINFO input{sizeof(input)};
  return GetLastInputInfo(&input) ? GetTickCount() - input.dwTime : 0;
}

// Everything owned by the UI thread.
struct App {
  HWND controller = nullptr;
  Compositor compositor;
  Store store;
  std::unique_ptr<Overlay> overlay;
  HGLOBAL pending_clipboard = nullptr;
  bool pending_promise = false;  // A bitmap to be made only when something asks for it, as for a scrolling screenshot.
  bool installing_clipboard = false;
  uint64_t pending_clipboard_sequence = 0;
  int clipboard_attempts = 0;
  DWORD installed_clipboard_sequence = 0;
  bool clearing_clipboard = false;
};

App* g_app = nullptr;

void ScheduleTrim(App& app) { SetTimer(app.controller, kTrimTimer, config::kTrimDelayMs, nullptr); }

// Returns every page this process is not actively using. A press after this pays only soft page
// faults, measured at a few milliseconds, because the pages remain cached in RAM.
void TrimMemory(App& app) {
  if (!app.overlay->IsIdle() || !app.store.IsIdle()) {
    ScheduleTrim(app);
    return;
  }
  KillTimer(app.controller, kTrimTimer);
  app.compositor.ReleaseUnusedMemory();
  app.store.ReleaseCodec();
  HEAP_OPTIMIZE_RESOURCES_INFORMATION heap_options{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
  HeapSetInformation(nullptr, HeapOptimizeResources, &heap_options, sizeof(heap_options));
  EmptyWorkingSet(GetCurrentProcess());
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
  Trace("trimmed, commit %.1f MB", memory.PrivateUsage / 1048576.0);
}

// Handed to the shell, so that the viewer does not inherit this process's UI Access right.
void OpenInViewer(const std::wstring& path) {
  EnsureOleInitialized();
  AllowSetForegroundWindow(ASFW_ANY);
  const std::wstring quoted = L"\"" + path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
}

void DropPendingClipboard(App& app) {
  if (app.pending_clipboard != nullptr) {
    GlobalFree(app.pending_clipboard);
    app.pending_clipboard = nullptr;
  }
  app.pending_promise = false;
  KillTimer(app.controller, kClipboardTimer);
}

void InstallPendingClipboard(App& app) {
  if (app.pending_clipboard == nullptr && !app.pending_promise) return;
  if (app.pending_clipboard_sequence != app.store.latest_clipboard_sequence()) {
    DropPendingClipboard(app);
    return;
  }
  // Another program may hold the clipboard open for a moment, so this retries on a timer instead of blocking.
  if (!OpenClipboard(app.controller)) {
    if (++app.clipboard_attempts >= config::kClipboardMaxRetries) {
      Trace("clipboard stayed busy, giving up");
      DropPendingClipboard(app);
    } else {
      SetTimer(app.controller, kClipboardTimer, config::kClipboardRetryMs, nullptr);
    }
    return;
  }
  // Emptying it tells this window that what it had there is gone, which is not so for what it is about to put.
  app.installing_clipboard = true;
  EmptyClipboard();
  app.installing_clipboard = false;
  // With no bitmap, the format alone goes on the clipboard, and WM_RENDERFORMAT comes for the rest.
  if (SetClipboardData(CF_DIB, app.pending_clipboard) != nullptr) {
    app.pending_clipboard = nullptr;  // The system owns the memory from here on.
  }
  CloseClipboard();
  app.installed_clipboard_sequence = GetClipboardSequenceNumber();
  DropPendingClipboard(app);
}

void ClearInstalledClipboard(App& app) {
  DropPendingClipboard(app);
  // Anything copied since then belongs to the user, so it is left alone.
  if (app.installed_clipboard_sequence == 0 || GetClipboardSequenceNumber() != app.installed_clipboard_sequence) {
    app.clearing_clipboard = false;
    return;
  }
  if (!OpenClipboard(app.controller)) {
    if (++app.clipboard_attempts >= config::kClipboardMaxRetries) {
      Trace("clipboard stayed busy, giving up on clearing it");
      app.clearing_clipboard = false;
    } else {
      app.clearing_clipboard = true;
      SetTimer(app.controller, kClipboardTimer, config::kClipboardRetryMs, nullptr);
    }
    return;
  }
  EmptyClipboard();
  CloseClipboard();
  app.installed_clipboard_sequence = 0;
  app.clearing_clipboard = false;
}

// An update the service holds asks for a quiet moment: nothing on screen, and nobody at the keyboard or mouse for a
// while. The check runs on a timer only while one waits.
void WatchUpdate(App& app) {
  UpdateStatus status;
  if (ReadUpdateStatus(&status) && status.state == UpdateState::kReady) {
    SetTimer(app.controller, kUpdateTimer, kUpdateIdlePollMs, nullptr);
  } else {
    KillTimer(app.controller, kUpdateTimer);
  }
}

void OfferUpdate(App& app) {
  if (!app.overlay->IsIdle() || !app.store.IsIdle() || InputIdleMs() < config::kUpdateIdleSeconds * 1000u) return;
  if (AskServiceToApplyUpdate()) KillTimer(app.controller, kUpdateTimer);
}

LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  App* app = g_app;
  if (app == nullptr || app->overlay == nullptr) return DefWindowProcW(window, message, wparam, lparam);

  switch (message) {
    case kMsgFlash:
      KillTimer(window, kTrimTimer);
      app->overlay->OnFlash(reinterpret_cast<HMONITOR>(wparam));
      Trace("flash committed");
      return 0;
    case kMsgCaptured:
      app->overlay->OnCaptured(std::unique_ptr<CaptureResult>(reinterpret_cast<CaptureResult*>(lparam)));
      return 0;
    case kMsgClipboardReady:
      DropPendingClipboard(*app);
      app->clearing_clipboard = false;
      app->pending_clipboard = reinterpret_cast<HGLOBAL>(lparam);
      app->pending_promise = lparam == 0;
      app->pending_clipboard_sequence = static_cast<uint64_t>(wparam);
      app->clipboard_attempts = 0;
      InstallPendingClipboard(*app);
      return 0;
    case WM_RENDERFORMAT:
      // Something asks for the bitmap promised. The clipboard is open already, and takes it as it is.
      if (wparam == CF_DIB) {
        if (HGLOBAL dib = app->store.RenderLongClipboard(); dib != nullptr && SetClipboardData(CF_DIB, dib) == nullptr) GlobalFree(dib);
      }
      return 0;
    case WM_DESTROYCLIPBOARD:
      if (!app->installing_clipboard) app->store.ForgetLongClipboard();
      return 0;
    case kMsgClearClipboard:
      app->clipboard_attempts = 0;
      ClearInstalledClipboard(*app);
      return 0;
    case kMsgDragFrame:
      app->overlay->OnDragFrame();
      return 0;
    case kMsgDragFeedback:
      app->overlay->OnDragFeedback(static_cast<uint64_t>(wparam), lparam != 0);
      return 0;
    case kMsgDragFinished:
      app->overlay->OnDragFinished(std::unique_ptr<FileDragOutcome>(reinterpret_cast<FileDragOutcome*>(lparam)));
      return 0;
    case kMsgDragCancel:
      app->overlay->OnDragCancel();
      return 0;
    case kMsgSelectorStep:
      app->overlay->OnSelectorStep(static_cast<int>(lparam));
      return 0;
    case kMsgScrollAreas:
      app->overlay->OnScrollAreas(std::unique_ptr<ScrollAreas>(reinterpret_cast<ScrollAreas*>(lparam)));
      return 0;
    case kMsgScrollChord:
      app->overlay->OnScrollChord(wparam != 0);
      return 0;
    case kMsgScrollWheel:
      app->overlay->OnScrollWheel(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case kMsgScrollUpdate:
      app->overlay->OnScrollUpdate(std::unique_ptr<ScrollUpdate>(reinterpret_cast<ScrollUpdate*>(lparam)));
      return 0;
    case kMsgOpenShot:
      OpenInViewer(*std::unique_ptr<std::wstring>(reinterpret_cast<std::wstring*>(lparam)));
      return 0;
    case WM_TIMER:
      if (wparam == Overlay::kTickTimer) {
        app->overlay->OnTick();
        if (app->overlay->IsIdle()) ScheduleTrim(*app);
      } else if (wparam == kTrimTimer) {
        TrimMemory(*app);
      } else if (wparam == kClipboardTimer) {
        KillTimer(window, kClipboardTimer);
        if (app->clearing_clipboard) {
          ClearInstalledClipboard(*app);
        } else {
          InstallPendingClipboard(*app);
        }
      } else if (wparam == kUpdateTimer) {
        OfferUpdate(*app);
      }
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      app->overlay->OnDisplayChange();
      ScheduleTrim(*app);
      return 0;
    case WM_ENDSESSION:
      if (wparam) PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

// Waits for a handle while still answering messages, so that broadcasts to this process's windows never stall.
DWORD WaitAnswering(HANDLE handle, DWORD timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    const DWORD wait = MsgWaitForMultipleObjectsEx(1, &handle, left, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait != WAIT_OBJECT_0 + 1) return wait;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) return WAIT_FAILED;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
}

// Captures until asked to quit: the hooks, the capture thread, and the message loop.
int Serve(App& app) {
  HANDLE trigger = CreateEventW(nullptr, FALSE, FALSE, kTriggerEventName);
  HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
  // A copy taken over was asked to quit through this name. Whatever that left set is not meant for this one.
  if (quit != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) ResetEvent(quit);
  if (trigger == nullptr || quit == nullptr) {
    if (trigger != nullptr) CloseHandle(trigger);
    if (quit != nullptr) CloseHandle(quit);
    return 1;
  }
  ScheduleTrim(app);
  UpdateWatch updates;
  const bool watching = updates.Open();
  WatchUpdate(app);

  std::thread hook_thread([&app, quit] { RunKeyboardHook(app.controller, quit); });
  std::thread capture_thread([&app, trigger, quit] { RunCaptureLoop(app.controller, trigger, quit); });
  Trace("application ready");

  const HANDLE waits[2] = {quit, updates.event()};
  bool running = true;
  while (running) {
    const DWORD wait = MsgWaitForMultipleObjectsEx(watching ? 2 : 1, waits, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_OBJECT_0) break;
    if (watching && wait == WAIT_OBJECT_0 + 1) {
      updates.Arm();
      WatchUpdate(app);
    }
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  // The event also releases the hook and capture threads when the loop ended through WM_QUIT.
  SetEvent(quit);
  hook_thread.join();
  capture_thread.join();
  CloseHandle(quit);
  CloseHandle(trigger);
  return 0;
}

// With `takeover`, a copy already running in this session hands over to this one, as after an update.
int RunApplication(bool takeover) {
  HANDLE instance = CreateMutexW(nullptr, TRUE, kAppMutexName);
  if (instance == nullptr) return 1;
  const bool replacing = GetLastError() == ERROR_ALREADY_EXISTS;
  if (replacing && !takeover) {
    CloseHandle(instance);
    return 0;
  }

  DisablePowerThrottling();
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = ControllerProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kControllerClass;
  RegisterClassW(&window_class);

  App app;
  int result = 1;
  // A hidden top-level window: unlike a message-only window, it receives display change broadcasts.
  app.controller = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                   window_class.hInstance, nullptr);
  if (app.controller != nullptr && app.compositor.Initialize()) {
    app.store.Start(app.controller);
    app.overlay = std::make_unique<Overlay>(app.controller, app.compositor, app.store);
    g_app = &app;
    app.overlay->Warmup();
    app.store.Warmup();
    // The old copy is asked to go only once this one is ready, so screenshots pause just while it exits.
    DWORD handed = WAIT_OBJECT_0;
    if (replacing) {
      RequestQuit();
      handed = WaitAnswering(instance, kTakeoverWaitMs);
    }
    if (handed == WAIT_OBJECT_0 || handed == WAIT_ABANDONED) result = Serve(app);
    app.store.Stop();
    DropPendingClipboard(app);
    g_app = nullptr;
    app.overlay.reset();
    app.compositor.Shutdown();
  }
  if (app.controller != nullptr) DestroyWindow(app.controller);
  CloseHandle(instance);
  return result;
}

// The words after the program's name. None of this program's own arguments is quoted, and splitting them here keeps
// shell32 out of the service.
std::vector<std::wstring> Arguments() {
  const wchar_t* p = GetCommandLineW();
  if (*p == L'"') {
    for (++p; *p != L'\0' && *p != L'"'; ++p) {
    }
    if (*p != L'\0') ++p;
  } else {
    while (*p != L'\0' && *p != L' ' && *p != L'\t') ++p;
  }
  std::vector<std::wstring> arguments;
  while (*p != L'\0') {
    while (*p == L' ' || *p == L'\t') ++p;
    const wchar_t* start = p;
    while (*p != L'\0' && *p != L' ' && *p != L'\t') ++p;
    if (p > start) arguments.emplace_back(start, p);
  }
  return arguments;
}

int Find(const std::vector<std::wstring>& arguments, const wchar_t* name) {
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (_wcsicmp(arguments[i].c_str(), name) == 0) return static_cast<int>(i);
  }
  return -1;
}

int RunWindow(SetupWindowMode mode) {
  SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
  return RunSetupWindow(mode);
}

int RunElevatedSteps(const std::vector<std::wstring>& arguments, int at) {
  if (at + 2 >= static_cast<int>(arguments.size())) return 1;
  const SetupAction action = _wcsicmp(arguments[at + 1].c_str(), L"uninstall") == 0 ? SetupAction::kUninstall : SetupAction::kInstall;
  HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(arguments[at + 2].c_str(), nullptr, 10)));
  return RunSetupSteps(action, IsWindow(window) ? window : nullptr);
}

int Dispatch() {
  const std::vector<std::wstring> arguments = Arguments();
  const bool quiet = Find(arguments, L"--quiet") >= 0;
  if (Find(arguments, L"--service") >= 0) return RunService();
  if (Find(arguments, L"--run") >= 0) return RunApplication(Find(arguments, L"--takeover") >= 0);
  if (Find(arguments, L"--apply-update") >= 0) return RunApplyUpdate();
  if (const int at = Find(arguments, L"--elevated"); at >= 0) return RunElevatedSteps(arguments, at);
  if (Find(arguments, L"--install") >= 0) return quiet ? RunQuietSetup(SetupAction::kInstall) : RunWindow(SetupWindowMode::kInstall);
  if (Find(arguments, L"--quit") >= 0) {
    RequestQuit();
    return 0;
  }
  // The uninstall and About windows run from a copy in %TEMP%, which removes itself when they close.
  const bool uninstall = Find(arguments, L"--uninstall") >= 0;
  if (uninstall && quiet) return RunQuietSetup(SetupAction::kUninstall);
  if (uninstall || Find(arguments, L"--about") >= 0 || IsInstalledCopy()) {
    if (HandOffToTemporaryCopy(uninstall ? L"--uninstall" : L"--about")) return 0;
    const int result = RunWindow(uninstall ? SetupWindowMode::kUninstall : SetupWindowMode::kAbout);
    RemoveTemporaryCopyAtExit();
    return result;
  }
  return RunWindow(SetupWindowMode::kInstall);
}

}  // namespace

}  // namespace sstk

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return sstk::Dispatch(); }
