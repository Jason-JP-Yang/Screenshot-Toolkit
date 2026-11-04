// Low-level hooks: the capture keys always, and while a thumbnail is on screen the chords that act on it.
// Which keys and buttons those are is set in config/hooks.h.
#pragma once

#include "platform.h"

namespace sstk {

enum class HookMode {
  kIdle,         // Only the capture keys.
  kSelector,     // The selector chords step the region selector.
  kScrollReady,  // As kSelector, and the scroll chord starts a scrolling screenshot.
  kScrolling,    // The scroll and capture chords finish the scrolling screenshot, and wheel turns are reported.
  kDrag,         // The cancel key and button call off the drag, without reaching the application under the pointer.
};

// Any mode but idle also hooks the mouse, which is why it is not left on: that hook wakes this process on every move.
void SetHookMode(HookMode mode);

// Installs the keyboard hook and pumps messages on the calling thread until stop_event is signaled.
// Keys are swallowed only while the application's trigger event exists, so Windows keeps
// its native behavior whenever the application is not running.
void RunKeyboardHook(HWND controller, HANDLE stop_event);

}  // namespace sstk
