#include "capture.h"

namespace sstk {

void RunCaptureLoop(HWND controller, HANDLE trigger_event, HANDLE quit_event) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  const HANDLE waits[] = {trigger_event, quit_event};

  for (;;) {
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return;

    auto result = std::make_unique<CaptureResult>();
    result->trigger_time = NowSeconds();
    GetLocalTime(&result->local_time);

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

    // The flash goes first: it gives feedback within a frame while the screen is being read.
    // Test tooling makes the overlay capturable, so there the flash has to wait for the read.
    if (!OverlayVisibleToCapture()) PostMessageW(controller, kMsgFlash, reinterpret_cast<WPARAM>(monitor), 0);

    result->screen = AnalyzeScreen(monitor);
    const double analyzed = NowSeconds();
    result->frame = CaptureScreen(result->screen.bounds);
    Trace("capture: analyze %.2f ms, blit %.2f ms, regions %zu", (analyzed - result->trigger_time) * 1000.0,
          (NowSeconds() - analyzed) * 1000.0, result->screen.regions.size());

    if (OverlayVisibleToCapture()) PostMessageW(controller, kMsgFlash, reinterpret_cast<WPARAM>(monitor), 0);
    if (!PostMessageW(controller, kMsgCaptured, 0, reinterpret_cast<LPARAM>(result.get()))) continue;
    result.release();
  }
}

}  // namespace sstk
