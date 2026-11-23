// Low-level hooks: the capture keys always, and while a thumbnail is on screen the chords that act on it.
// Which keys and buttons those are is set in sstk/config/hooks.h.
#pragma once

#include "sstk/base/platform.h"

namespace sstk {

enum class HookMode {
  kIdle,         // Only the capture keys.
  kSelector,     // The selector chords step the region selector.
  kScrollReady,  // As kSelector, and the scroll chord starts a scrolling screenshot.
  kScrolling,    // The scroll and capture chords finish the scrolling screenshot, and wheel turns are reported.
  kDrag,         // The cancel key and button call off the drag, without reaching the application under the pointer.
  kEditor,       // Every key and wheel turn goes to the editing panel with kMsgEditorKey and kMsgEditorWheel, and to
                 // nothing else, as long as the panel keeps calling EditorHeartbeat.
};

// Any mode but idle also hooks the mouse, which is why it is not left on: that hook wakes this process on every move.
void SetHookMode(HookMode mode);

// The editing panel is still answering. Should it stop, keys go back to the system after a moment.
void EditorHeartbeat();

// While watching, which hooks the mouse too, every key and button pressed and every wheel turned is reported with
// kMsgKeyInput or kMsgPointerInput, but for the chords taken here and keys that only modify others.
void SetInputWatch(bool watch);

// Installs the keyboard hook and pumps messages on the calling thread until stop_event is signaled.
// Keys are swallowed only while the application's trigger event exists, so Windows keeps
// its native behavior whenever the application is not running.
void RunKeyboardHook(HWND controller, HANDLE stop_event);

}  // namespace sstk
