// Low-level keyboard hook that takes PrintScreen and Win+Shift+S away from the shell.
#pragma once

#include "platform.h"

namespace sstk {

// Installs the hook and pumps messages on the calling thread until stop_event is signaled.
// Keys are swallowed only while the application's trigger event exists, so Windows keeps
// its native behavior whenever the application is not running.
void RunKeyboardHook(HANDLE stop_event);

}  // namespace sstk
