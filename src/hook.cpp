#include "hook.h"

namespace sstk {

namespace {

// Windows silently removes hooks that once ran too slowly, so the hook is refreshed periodically.
constexpr DWORD kReinstallIntervalMs = 5 * 60 * 1000;

// An unassigned virtual key. Injecting it while Win is held stops the Start menu from opening on release.
constexpr WORD kMaskKey = 0xFF;

HHOOK g_hook = nullptr;
bool g_snapshot_held = false;
bool g_s_held = false;

bool IsHeld(int virtual_key) { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; }

bool IsWinHeld() { return IsHeld(VK_LWIN) || IsHeld(VK_RWIN); }

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

void InstallHook() {
  HHOOK replacement = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
  if (replacement == nullptr) {
    Trace("SetWindowsHookEx failed: %lu", GetLastError());
    return;
  }
  if (g_hook != nullptr) UnhookWindowsHookEx(g_hook);
  g_hook = replacement;
}

}  // namespace

void RunKeyboardHook(HANDLE stop_event) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  InstallHook();
  for (;;) {
    DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop_event, kReinstallIntervalMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_TIMEOUT) {
      InstallHook();
      continue;
    }
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      DispatchMessageW(&message);
    }
  }
  if (g_hook != nullptr) {
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
  }
}

}  // namespace sstk
