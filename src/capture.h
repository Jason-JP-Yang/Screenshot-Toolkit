// Dedicated thread that reacts to the hotkey trigger and takes the actual screenshot.
#pragma once

#include "image.h"
#include "platform.h"
#include "screen.h"

#include <memory>

namespace sstk {

struct CaptureResult {
  double trigger_time = 0.0;
  ScreenInfo screen;
  std::shared_ptr<const Image> frame;  // Null when the screen could not be read, e.g. on a secure desktop.
  SYSTEMTIME local_time{};
};

// Blocks until quit_event is signaled, posting kMsgFlash and kMsgCaptured to the controller window.
void RunCaptureLoop(HWND controller, HANDLE trigger_event, HANDLE quit_event);

}  // namespace sstk
