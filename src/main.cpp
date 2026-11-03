// Entry point. Modes:
//   (no arguments)   The application: capture, overlay, and persistence.
//   --launch         Run by the logon task: starts the application in the user's session. See install.cpp.
//   --install        Copies to Program Files and registers the logon task. Prompts through UAC.
//   --uninstall      Removes the task and the installed program. Prompts through UAC.
//   --quit           Asks a running instance to exit.
#include "capture.h"
#include "compositor.h"
#include "dragdrop.h"
#include "hook.h"
#include "install.h"
#include "overlay.h"
#include "platform.h"
#include "store.h"

#include <psapi.h>
#include <shellapi.h>

#include <cwchar>
#include <memory>
#include <thread>

namespace sstk {

namespace {

constexpr wchar_t kControllerClass[] = L"ScreenshotToolkit.Controller";
constexpr UINT_PTR kTrimTimer = 2;
constexpr UINT_PTR kClipboardTimer = 3;
constexpr UINT kTrimDelayMs = 1500;
constexpr UINT kClipboardRetryMs = 20;
constexpr int kClipboardMaxRetries = 25;

// Everything owned by the UI thread.
struct App {
  HWND controller = nullptr;
  Compositor compositor;
  Store store;
  std::unique_ptr<Overlay> overlay;
  HGLOBAL pending_clipboard = nullptr;
  uint64_t pending_clipboard_sequence = 0;
  int clipboard_attempts = 0;
  DWORD installed_clipboard_sequence = 0;
  bool clearing_clipboard = false;
};

App* g_app = nullptr;

void ScheduleTrim(App& app) { SetTimer(app.controller, kTrimTimer, kTrimDelayMs, nullptr); }

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

void DropPendingClipboard(App& app) {
  if (app.pending_clipboard != nullptr) {
    GlobalFree(app.pending_clipboard);
    app.pending_clipboard = nullptr;
  }
  KillTimer(app.controller, kClipboardTimer);
}

void InstallPendingClipboard(App& app) {
  if (app.pending_clipboard == nullptr) return;
  if (app.pending_clipboard_sequence != app.store.latest_clipboard_sequence()) {
    DropPendingClipboard(app);
    return;
  }
  // Another program may hold the clipboard open for a moment, so this retries on a timer instead of blocking.
  if (!OpenClipboard(app.controller)) {
    if (++app.clipboard_attempts >= kClipboardMaxRetries) {
      Trace("clipboard stayed busy, giving up");
      DropPendingClipboard(app);
    } else {
      SetTimer(app.controller, kClipboardTimer, kClipboardRetryMs, nullptr);
    }
    return;
  }
  EmptyClipboard();
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
    if (++app.clipboard_attempts >= kClipboardMaxRetries) {
      Trace("clipboard stayed busy, giving up on clearing it");
      app.clearing_clipboard = false;
    } else {
      app.clearing_clipboard = true;
      SetTimer(app.controller, kClipboardTimer, kClipboardRetryMs, nullptr);
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
      app->pending_clipboard_sequence = static_cast<uint64_t>(wparam);
      app->clipboard_attempts = 0;
      InstallPendingClipboard(*app);
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

  std::thread hook_thread([quit] { RunKeyboardHook(quit); });
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

bool HasArgument(int argc, wchar_t** argv, const wchar_t* name) {
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], name) == 0) return true;
  }
  return false;
}

}  // namespace

}  // namespace sstk

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int result;
  if (sstk::HasArgument(argc, argv, L"--launch")) {
    result = sstk::LaunchInUserSession();
  } else if (sstk::HasArgument(argc, argv, L"--install")) {
    result = sstk::InstallApplication();
  } else if (sstk::HasArgument(argc, argv, L"--uninstall")) {
    result = sstk::UninstallApplication();
  } else if (sstk::HasArgument(argc, argv, L"--quit")) {
    sstk::RequestQuit();
    result = 0;
  } else {
    result = sstk::RunApplication();
  }
  LocalFree(argv);
  return result;
}
