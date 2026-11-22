// Entry point. Modes:
//   (no arguments)  The About window when this is the installed copy, and the setup window otherwise.
//   --run           The application: capture, overlay, and persistence. The service starts it this way.
//   --service       The service that keeps the application running in each session. See service/service.h.
//   --install       The setup window. With --quiet, installs without one.
//   --uninstall     The uninstall window, as Settings > Apps opens it. With --quiet, removes without one.
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

int RunApplication() {
  HANDLE instance = CreateMutexW(nullptr, TRUE, kAppMutexName);
  if (instance == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  DisablePowerThrottling();
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  HANDLE trigger = CreateEventW(nullptr, FALSE, FALSE, kTriggerEventName);
  HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
  if (trigger == nullptr || quit == nullptr) return 1;

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = ControllerProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kControllerClass;
  RegisterClassW(&window_class);

  App app;
  // A hidden top-level window: unlike a message-only window, it receives display change broadcasts.
  app.controller = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                   window_class.hInstance, nullptr);
  if (app.controller == nullptr || !app.compositor.Initialize()) return 1;

  app.store.Start(app.controller);
  app.overlay = std::make_unique<Overlay>(app.controller, app.compositor, app.store);
  g_app = &app;
  app.overlay->Warmup();
  app.store.Warmup();
  ScheduleTrim(app);

  std::thread hook_thread([&app, quit] { RunKeyboardHook(app.controller, quit); });
  std::thread capture_thread([&app, trigger, quit] { RunCaptureLoop(app.controller, trigger, quit); });
  Trace("application ready");

  bool running = true;
  while (running) {
    DWORD wait = MsgWaitForMultipleObjectsEx(1, &quit, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_OBJECT_0) break;
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
  app.store.Stop();
  DropPendingClipboard(app);
  g_app = nullptr;
  app.overlay.reset();
  app.compositor.Shutdown();
  DestroyWindow(app.controller);
  CloseHandle(quit);
  CloseHandle(trigger);
  CloseHandle(instance);
  return 0;
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
  if (Find(arguments, L"--run") >= 0) return RunApplication();
  if (const int at = Find(arguments, L"--elevated"); at >= 0) return RunElevatedSteps(arguments, at);
  if (Find(arguments, L"--install") >= 0) return quiet ? RunQuietSetup(SetupAction::kInstall) : RunWindow(SetupWindowMode::kInstall);
  if (Find(arguments, L"--uninstall") >= 0) {
    if (quiet) return RunQuietSetup(SetupAction::kUninstall);
    if (HandOffToTemporaryCopy()) return 0;
    const int result = RunWindow(SetupWindowMode::kUninstall);
    RemoveTemporaryCopyAtExit();
    return result;
  }
  if (Find(arguments, L"--quit") >= 0) {
    RequestQuit();
    return 0;
  }
  return RunWindow(IsInstalledCopy() ? SetupWindowMode::kAbout : SetupWindowMode::kInstall);
}

}  // namespace

}  // namespace sstk

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return sstk::Dispatch(); }
