// Low-level hooks: PrintScreen and Win+Shift+S always, and while a thumbnail is on screen the chords that act on it.
#pragma once

#include "platform.h"

namespace sstk {

enum class HookMode {
  kIdle,      // Only the capture keys.
  kSelector,  // Win+Shift with Left, Right, or a side button steps the region selector.
  kDrag,      // Escape and the right button cancel the drag, without reaching the application under the pointer.
};

// Any mode but idle also hooks the mouse, which is why it is not left on: that hook wakes this process on every move.
void SetHookMode(HookMode mode);

// Installs the keyboard hook and pumps messages on the calling thread until stop_event is signaled.
// Keys are swallowed only while the application's trigger event exists, so Windows keeps
// its native behavior whenever the application is not running.
void RunKeyboardHook(HWND controller, HANDLE stop_event);

}  // namespace sstk
