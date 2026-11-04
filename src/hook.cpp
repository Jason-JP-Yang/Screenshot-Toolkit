#include "hook.h"

#include <psapi.h>

#include <atomic>

namespace sstk {

namespace {

// Windows silently removes hooks that once ran too slowly, so the hook is refreshed periodically.
constexpr DWORD kReinstallIntervalMs = 5 * 60 * 1000;

// An unassigned virtual key. Injecting it while Win is held stops the Start menu from opening on release.
constexpr WORD kMaskKey = 0xFF;

constexpr UINT kMsgSyncMouseHook = WM_APP + 1;

HWND g_controller = nullptr;
HHOOK g_hook = nullptr;
HHOOK g_mouse_hook = nullptr;
std::atomic<HookMode> g_mode{HookMode::kIdle};
std::atomic<DWORD> g_hook_thread{0};
bool g_snapshot_held = false;
bool g_s_held = false;
bool g_left_held = false;
bool g_right_held = false;
bool g_escape_held = false;
bool g_right_button_held = false;
bool g_side_button_held[2] = {false, false};

bool IsHeld(int virtual_key) { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; }

bool IsWinHeld() { return IsHeld(VK_LWIN) || IsHeld(VK_RWIN); }

bool IsSelectorChordHeld() { return IsWinHeld() && IsHeld(VK_SHIFT) && !IsHeld(VK_CONTROL) && !IsHeld(VK_MENU); }

// Opened by name on every press: the event only exists while the application is alive,
// and a cached handle would keep signaling a dead instance after a restart.
bool FireTrigger() {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kTriggerEventName);
  if (event == nullptr) return false;
  SetEvent(event);
  CloseHandle(event);
  return true;
}

void MaskWinKeyRelease() {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = kMaskKey;
  input.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &input, sizeof(input));
}

void StepSelector(int direction) {
  PostMessageW(g_controller, kMsgSelectorStep, 0, direction);
  MaskWinKeyRelease();
}

// Returns true when the event must not reach the rest of the system. A press taken here keeps its release too,
// even if the mode has moved on: a stray right button release would open a context menu under the pointer.
bool HandleChord(bool& held, bool is_down, bool allowed, void (*fire)(int), int argument) {
  if (!is_down) {
    const bool swallow = held;
    held = false;
    return swallow;
  }
  if (held) return true;  // Auto-repeat of a press that already fired.
  if (!allowed) return false;
  held = true;
  fire(argument);
  return true;
}

void CancelDrag(int) { PostMessageW(g_controller, kMsgDragCancel, 0, 0); }

// Returns true when the key event must not reach the rest of the system.
bool HandleKey(DWORD virtual_key, bool is_down, bool injected) {
  if (virtual_key == VK_SNAPSHOT) {
    if (!is_down) {
      bool swallow = g_snapshot_held;
      g_snapshot_held = false;
      return swallow;
    }
    if (g_snapshot_held) return true;  // Auto-repeat of a press that already fired.
    // Modified variants such as Alt+PrintScreen keep their native meaning.
    if (IsHeld(VK_MENU) || IsHeld(VK_CONTROL) || IsHeld(VK_SHIFT) || IsWinHeld()) {
      Trace("hook: PrintScreen with a modifier held, left to Windows");
      return false;
    }
    g_snapshot_held = FireTrigger();
    Trace("hook: PrintScreen, injected=%d", injected);
    return g_snapshot_held;
  }

  if (virtual_key == 'S') {
    if (!is_down) {
      bool swallow = g_s_held;
      g_s_held = false;
      return swallow;
    }
    if (g_s_held) return true;
    if (!IsWinHeld() || !IsHeld(VK_SHIFT) || IsHeld(VK_CONTROL) || IsHeld(VK_MENU)) return false;
    g_s_held = FireTrigger();
    Trace("hook: Win+Shift+S, injected=%d", injected);
    if (g_s_held) MaskWinKeyRelease();
    return g_s_held;
  }

  const HookMode mode = g_mode.load(std::memory_order_relaxed);
  if (virtual_key == VK_LEFT) return HandleChord(g_left_held, is_down, mode == HookMode::kSelector && IsSelectorChordHeld(), StepSelector, -1);
  if (virtual_key == VK_RIGHT) return HandleChord(g_right_held, is_down, mode == HookMode::kSelector && IsSelectorChordHeld(), StepSelector, 1);
  if (virtual_key == VK_ESCAPE) return HandleChord(g_escape_held, is_down, mode == HookMode::kDrag, CancelDrag, 0);
  return false;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const bool is_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    if (HandleKey(key->vkCode, is_down, (key->flags & LLKHF_INJECTED) != 0)) return 1;
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
}

bool HandleButton(WPARAM message, const MSLLHOOKSTRUCT& info) {
  const HookMode mode = g_mode.load(std::memory_order_relaxed);
  switch (message) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      return HandleChord(g_right_button_held, message == WM_RBUTTONDOWN, mode == HookMode::kDrag, CancelDrag, 0);
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
      const bool back = HIWORD(info.mouseData) == XBUTTON1;
      return HandleChord(g_side_button_held[back ? 0 : 1], message == WM_XBUTTONDOWN, mode == HookMode::kSelector && IsSelectorChordHeld(),
                         StepSelector, back ? -1 : 1);
    }
    default:
      return false;
  }
}

void SyncMouseHook();

LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && HandleButton(wparam, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam))) {
    // The hook may have been outliving its mode only to catch this release.
    if (wparam == WM_RBUTTONUP || wparam == WM_XBUTTONUP) PostThreadMessageW(GetCurrentThreadId(), kMsgSyncMouseHook, 0, 0);
    return 1;
  }
  return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

void SyncMouseHook() {
  const bool wanted = g_mode.load(std::memory_order_relaxed) != HookMode::kIdle || g_right_button_held || g_side_button_held[0] || g_side_button_held[1];
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

void RunKeyboardHook(HWND controller, HANDLE stop_event) {
  g_controller = controller;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  InstallHook();
  // Makes the message queue exist before anyone can post to it.
  MSG unused;
  PeekMessageW(&unused, nullptr, 0, 0, PM_NOREMOVE);
  g_hook_thread.store(GetCurrentThreadId(), std::memory_order_release);
  SyncMouseHook();
  while (!PumpHookMessages(stop_event, kReinstallIntervalMs)) {
    InstallHook();
    TrimWorkingSet();
  }
  g_hook_thread.store(0, std::memory_order_release);
  RemoveHooks();
}

}  // namespace sstk
