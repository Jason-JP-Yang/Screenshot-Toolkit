#include "sstk/capture/hook.h"

#include "sstk/config/core.h"
#include "sstk/config/hooks.h"

#include <psapi.h>

#include <atomic>
#include <iterator>

namespace sstk {

namespace {

// An unassigned virtual key. Injecting it while Win is held stops the Start menu from opening on release.
constexpr WORD kMaskKey = 0xFF;

constexpr UINT kMsgSyncMouseHook = WM_APP + 1;
constexpr int kMouseButtonCount = 4;

HWND g_controller = nullptr;
HHOOK g_hook = nullptr;
HHOOK g_mouse_hook = nullptr;
std::atomic<HookMode> g_mode{HookMode::kIdle};
std::atomic<bool> g_watch{false};
std::atomic<DWORD> g_hook_thread{0};
bool g_key_down[256] = {};
bool g_capture_held[std::size(config::kCaptureChords)] = {};
bool g_previous_key_held = false;
bool g_next_key_held = false;
bool g_scroll_key_held = false;
bool g_cancel_key_held = false;
bool g_button_held[kMouseButtonCount] = {};
bool g_editor_held[256] = {};  // Keys pressed for the editing panel, whose release is its too.
std::atomic<ULONGLONG> g_editor_beat{0};

bool IsHeld(int virtual_key) { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; }

bool AreHeld(const config::Modifiers& wanted) {
  return (IsHeld(VK_LWIN) || IsHeld(VK_RWIN)) == wanted.win && IsHeld(VK_SHIFT) == wanted.shift &&
         IsHeld(VK_CONTROL) == wanted.control && IsHeld(VK_MENU) == wanted.alt;
}

void MaskWinKeyRelease() {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = kMaskKey;
  input.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &input, sizeof(input));
}

// Opened by name on every press: the event only exists while the application is alive,
// and a cached handle would keep signaling a dead instance after a restart.
bool Capture(int chord) {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, config::kCaptureChords[chord].opens_editor ? kEditTriggerEventName : kTriggerEventName);
  if (event == nullptr) return false;
  SetEvent(event);
  CloseHandle(event);
  Trace("hook: capture chord %d", chord);
  if (config::kCaptureChords[chord].modifiers.win) MaskWinKeyRelease();
  return true;
}

bool StepSelector(int direction) {
  PostMessageW(g_controller, kMsgSelectorStep, 0, direction);
  if (config::kSelectorModifiers.win) MaskWinKeyRelease();
  return true;
}

bool CancelDrag(int) {
  PostMessageW(g_controller, kMsgDragCancel, 0, 0);
  return true;
}

bool ScrollChord(int) {
  Trace("hook: scroll chord");
  PostMessageW(g_controller, kMsgScrollChord, 0, 0);
  if (config::kScrollChord.modifiers.win) MaskWinKeyRelease();
  return true;
}

// While a scrolling screenshot runs, a capture chord finishes it instead of taking another screenshot.
bool FinishScrolling(int chord) {
  PostMessageW(g_controller, kMsgScrollChord, 1, 0);
  if (config::kCaptureChords[chord].modifiers.win) MaskWinKeyRelease();
  return true;
}

// Returns true when the event must not reach the rest of the system. A press taken here keeps its release too,
// even if the mode has moved on: a stray right button release would open a context menu under the pointer.
bool HandleChord(bool& held, bool is_down, bool allowed, bool (*fire)(int), int argument) {
  if (!is_down) {
    const bool swallow = held;
    held = false;
    return swallow;
  }
  if (held) return true;  // Auto-repeat of a press that already fired.
  if (!allowed) return false;
  held = fire(argument);
  return held;
}

HookMode Mode() { return g_mode.load(std::memory_order_relaxed); }

bool IsSelecting() {
  return (Mode() == HookMode::kSelector || Mode() == HookMode::kScrollReady) && AreHeld(config::kSelectorModifiers);
}

bool IsScrolling() { return Mode() == HookMode::kScrolling; }

bool IsScrollChord() {
  if (Mode() != HookMode::kScrollReady && Mode() != HookMode::kScrolling) return false;
  if (AreHeld(config::kScrollChord.modifiers)) return true;
  config::Modifiers shifted = config::kScrollChord.modifiers;
  shifted.shift = true;
  return config::kScrollChordTakesShift && AreHeld(shifted);
}

bool IsDragging() { return Mode() == HookMode::kDrag; }

// The panel has the keys only while it answers, so that a stalled one can never keep them from the system.
bool EditorListening() {
  return Mode() == HookMode::kEditor && GetTickCount64() - g_editor_beat.load(std::memory_order_relaxed) < config::kEditorKeyTimeoutMs;
}

// Keys pressed for the panel never reach the system, so their state is kept here.
bool EditorHeld(int virtual_key) { return g_editor_held[virtual_key] || IsHeld(virtual_key); }

LPARAM EditorFlags() {
  LPARAM flags = 0;
  if (EditorHeld(VK_CONTROL) || EditorHeld(VK_LCONTROL) || EditorHeld(VK_RCONTROL)) flags |= kEditorKeyControl;
  if (EditorHeld(VK_SHIFT) || EditorHeld(VK_LSHIFT) || EditorHeld(VK_RSHIFT)) flags |= kEditorKeyShift;
  if (EditorHeld(VK_MENU) || EditorHeld(VK_LMENU) || EditorHeld(VK_RMENU)) flags |= kEditorKeyAlt;
  return flags;
}

// Returns true when the key is the panel's. A key already down when the panel opened is released to the system as usual.
bool HandleEditorKey(DWORD virtual_key, bool is_down) {
  if (virtual_key >= std::size(g_editor_held)) return false;
  if (!is_down) {
    if (!g_editor_held[virtual_key]) return false;
    g_editor_held[virtual_key] = false;
    PostMessageW(g_controller, kMsgEditorKey, virtual_key, EditorFlags());
    return true;
  }
  if (!EditorListening()) return false;
  const bool repeat = g_editor_held[virtual_key];
  g_editor_held[virtual_key] = true;
  PostMessageW(g_controller, kMsgEditorKey, virtual_key, EditorFlags() | kEditorKeyDown | (repeat ? kEditorKeyRepeat : 0));
  return true;
}

WPARAM Stamp() { return static_cast<WPARAM>(NowSeconds() * 1e6); }

bool IsModifier(DWORD virtual_key) {
  switch (virtual_key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
    case kMaskKey:
      return true;
    default:
      return false;
  }
}

// A key the rest of the system gets, pressed rather than held on.
void NoteKey(DWORD virtual_key, bool is_down) {
  if (virtual_key >= std::size(g_key_down)) return;
  const bool repeat = is_down && g_key_down[virtual_key];
  g_key_down[virtual_key] = is_down;
  if (is_down && !repeat && !IsModifier(virtual_key) && g_watch.load(std::memory_order_relaxed)) PostMessageW(g_controller, kMsgKeyInput, Stamp(), 0);
}

void NotePointer(POINT point) {
  if (g_watch.load(std::memory_order_relaxed)) PostMessageW(g_controller, kMsgPointerInput, Stamp(), MAKELPARAM(point.x, point.y));
}

bool IsButtonDown(WPARAM message) {
  return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
}

// Returns true when the key event must not reach the rest of the system.
bool HandleKey(DWORD virtual_key, bool is_down) {
  for (int i = 0; i < static_cast<int>(std::size(config::kCaptureChords)); ++i) {
    const config::CaptureChord& chord = config::kCaptureChords[i];
    if (virtual_key == chord.key) {
      return HandleChord(g_capture_held[i], is_down, AreHeld(chord.modifiers), IsScrolling() ? FinishScrolling : Capture, i);
    }
  }
  if (virtual_key == config::kScrollChord.key) return HandleChord(g_scroll_key_held, is_down, IsScrollChord(), ScrollChord, 0);
  if (virtual_key == config::kSelectorPreviousKey) return HandleChord(g_previous_key_held, is_down, IsSelecting(), StepSelector, -1);
  if (virtual_key == config::kSelectorNextKey) return HandleChord(g_next_key_held, is_down, IsSelecting(), StepSelector, 1);
  if (virtual_key == config::kDragCancelKey) return HandleChord(g_cancel_key_held, is_down, IsDragging(), CancelDrag, 0);
  return false;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const bool is_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    if (HandleEditorKey(key->vkCode, is_down)) return 1;
    if (HandleKey(key->vkCode, is_down)) return 1;
    NoteKey(key->vkCode, is_down);
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
}

bool HandleButton(WPARAM message, const MSLLHOOKSTRUCT& info) {
  config::MouseButton button;
  bool is_down;
  switch (message) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      button = config::MouseButton::kRight;
      is_down = message == WM_RBUTTONDOWN;
      break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      button = config::MouseButton::kMiddle;
      is_down = message == WM_MBUTTONDOWN;
      break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      button = HIWORD(info.mouseData) == XBUTTON1 ? config::MouseButton::kBack : config::MouseButton::kForward;
      is_down = message == WM_XBUTTONDOWN;
      break;
    default:
      return false;
  }

  bool& held = g_button_held[static_cast<int>(button)];
  if (button == config::kDragCancelButton && IsDragging()) return HandleChord(held, is_down, true, CancelDrag, 0);
  if (button == config::kSelectorPreviousButton) return HandleChord(held, is_down, IsSelecting(), StepSelector, -1);
  if (button == config::kSelectorNextButton) return HandleChord(held, is_down, IsSelecting(), StepSelector, 1);
  return HandleChord(held, is_down, false, nullptr, 0);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    if ((wparam == WM_MOUSEWHEEL || wparam == WM_MOUSEHWHEEL) && EditorListening()) {
      // Taken wherever the pointer is, as the window with the focus would otherwise scroll beneath the panel.
      const LPARAM flags = EditorFlags() | (wparam == WM_MOUSEHWHEEL ? kEditorWheelHorizontal : 0);
      const WPARAM packed = static_cast<WPARAM>(HIWORD(info.mouseData)) | static_cast<WPARAM>(flags) << 16;
      PostMessageW(g_controller, kMsgEditorWheel, packed, MAKELPARAM(info.pt.x, info.pt.y));
      return 1;
    }
    if (wparam == WM_MOUSEWHEEL || wparam == WM_MOUSEHWHEEL) {
      // Reported, never taken: the scroll itself is what a scrolling screenshot records.
      if (wparam == WM_MOUSEWHEEL && IsScrolling()) PostMessageW(g_controller, kMsgScrollWheel, 0, MAKELPARAM(info.pt.x, info.pt.y));
      NotePointer(info.pt);
    } else if (HandleButton(wparam, info)) {
      // The hook may have been outliving its mode only to catch this release.
      PostThreadMessageW(GetCurrentThreadId(), kMsgSyncMouseHook, 0, 0);
      return 1;
    } else if (IsButtonDown(wparam)) {
      NotePointer(info.pt);
    }
  }
  return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

void SyncMouseHook() {
  bool wanted = g_mode.load(std::memory_order_relaxed) != HookMode::kIdle || g_watch.load(std::memory_order_relaxed);
  for (bool held : g_button_held) wanted = wanted || held;
  if (wanted == (g_mouse_hook != nullptr)) return;
  if (wanted) {
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (g_mouse_hook == nullptr) Trace("mouse hook failed: %lu", GetLastError());
  } else {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
}

void InstallHook() {
  HHOOK replacement = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
  if (replacement == nullptr) {
    Trace("SetWindowsHookEx failed: %lu", GetLastError());
    return;
  }
  if (g_hook != nullptr) UnhookWindowsHookEx(g_hook);
  g_hook = replacement;
}

// Settings broadcasts and the like slowly page things back in, so idle memory is handed back now and then.
void TrimWorkingSet() { EmptyWorkingSet(GetCurrentProcess()); }

void RemoveHooks() {
  if (g_mouse_hook != nullptr) {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
  if (g_hook != nullptr) {
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
  }
}

// Pumps until stop_event is signaled or the interval passes. Returns true when stopping.
bool PumpHookMessages(HANDLE stop_event, DWORD interval_ms) {
  const ULONGLONG deadline = GetTickCount64() + interval_ms;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop_event, static_cast<DWORD>(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_OBJECT_0) return true;
    if (wait == WAIT_TIMEOUT) return false;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == kMsgSyncMouseHook) {
        SyncMouseHook();
      } else {
        DispatchMessageW(&message);
      }
    }
  }
}

}  // namespace

void SetHookMode(HookMode mode) {
  g_mode.store(mode, std::memory_order_relaxed);
  const DWORD thread = g_hook_thread.load(std::memory_order_acquire);
  if (thread != 0) PostThreadMessageW(thread, kMsgSyncMouseHook, 0, 0);
}

void EditorHeartbeat() { g_editor_beat.store(GetTickCount64(), std::memory_order_relaxed); }

void SetInputWatch(bool watch) {
  g_watch.store(watch, std::memory_order_relaxed);
  const DWORD thread = g_hook_thread.load(std::memory_order_acquire);
  if (thread != 0) PostThreadMessageW(thread, kMsgSyncMouseHook, 0, 0);
}

void RunKeyboardHook(HWND controller, HANDLE stop_event) {
  g_controller = controller;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  InstallHook();
  // Makes the message queue exist before anyone can post to it.
  MSG unused;
  PeekMessageW(&unused, nullptr, 0, 0, PM_NOREMOVE);
  g_hook_thread.store(GetCurrentThreadId(), std::memory_order_release);
  SyncMouseHook();
  while (!PumpHookMessages(stop_event, config::kHookRefreshMs)) {
    InstallHook();
    TrimWorkingSet();
  }
  g_hook_thread.store(0, std::memory_order_release);
  RemoveHooks();
}

}  // namespace sstk
