// Dedicated thread that reacts to the hotkey trigger and takes the actual screenshot.
#pragma once

#include "sstk/base/platform.h"
#include "sstk/capture/screen.h"
#include "sstk/capture/window.h"
#include "sstk/graphics/image.h"

#include <memory>
#include <vector>

namespace sstk {

struct CaptureResult {
  uint64_t id = 0;
  double trigger_time = 0.0;
  ScreenInfo screen;
  std::shared_ptr<const Image> frame;  // Null when the screen could not be read, e.g. on a secure desktop.
  SYSTEMTIME local_time{};
  bool edit = false;  // Taken by the keys that open the editing panel.
};

// The own pictures of the windows a shot offers by themselves. They come a moment after the shot.
struct WindowShots {
  uint64_t capture = 0;  // The CaptureResult they belong to.
  std::vector<WindowShot> shots;
};

// Blocks until quit_event is signaled, posting kMsgFlash and kMsgCaptured to the controller window, and kMsgWindowShots
// once the windows are read by themselves. A kMsgFlash carries the monitor in wParam. A shot set off by edit_event is
// marked to open in the editing panel.
void RunCaptureLoop(HWND controller, HANDLE trigger_event, HANDLE edit_event, HANDLE quit_event);

}  // namespace sstk
