#include "hook.h"

#include <psapi.h>

namespace sstk {

namespace {

// Windows silently removes hooks that once ran too slowly, so the hook is refreshed periodically.
constexpr DWORD kReinstallIntervalMs = 5 * 60 * 1000;
constexpr DWORD kHostCheckIntervalMs = 15 * 1000;
constexpr int kHostChecksPerRefresh = 20;

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

namespace {

// A process cannot open the elevated host's mutex, but the refusal still proves that it exists.
bool HookHostIsRunning() {
  if (GetEnvironmentVariableW(L"SSTK_IGNORE_HOOK_HOST", nullptr, 0) > 0) return false;
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHookHostMutexName);
  if (mutex != nullptr) {
    CloseHandle(mutex);
    return true;
  }
  return GetLastError() == ERROR_ACCESS_DENIED;
}

// Settings broadcasts and the like slowly page things back in, so idle memory is handed back now and then.
void TrimWorkingSet() { EmptyWorkingSet(GetCurrentProcess()); }

void RemoveHook() {
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
      DispatchMessageW(&message);
    }
  }
}

}  // namespace

void RunKeyboardHook(HANDLE stop_event) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  InstallHook();
  while (!PumpHookMessages(stop_event, kReinstallIntervalMs)) {
    InstallHook();
    TrimWorkingSet();
  }
  RemoveHook();
}

void RunKeyboardHookUnlessHosted(HANDLE stop_event) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  int checks = 0;
  bool stopping = false;
  while (!stopping) {
    const bool refresh = checks % kHostChecksPerRefresh == 0;
    if (HookHostIsRunning()) {
      RemoveHook();
      stopping = WaitForSingleObject(stop_event, kHostCheckIntervalMs) == WAIT_OBJECT_0;
    } else {
      // Windows can drop a hook silently, so one held for a while is renewed.
      if (g_hook == nullptr || refresh) InstallHook();
      stopping = PumpHookMessages(stop_event, kHostCheckIntervalMs);
    }
    if (refresh && checks > 0) TrimWorkingSet();
    ++checks;
  }
  RemoveHook();
}

}  // namespace sstk
