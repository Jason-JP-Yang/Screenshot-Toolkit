// Shared Win32 includes, process-wide names, and small utilities.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace sstk {

inline constexpr wchar_t kTriggerEventName[] = L"Local\\ScreenshotToolkit.Trigger";
inline constexpr wchar_t kQuitEventName[] = L"Local\\ScreenshotToolkit.Quit";
inline constexpr wchar_t kAppMutexName[] = L"Local\\ScreenshotToolkit.App";

// Messages handled by the hidden controller window on the UI thread.
enum Message : UINT {
  kMsgFlash = WM_APP + 1,      // wParam: HMONITOR, lParam: 0
  kMsgCaptured,                // lParam: CaptureResult*, ownership moves to the receiver
  kMsgClipboardReady,          // wParam: sequence number, lParam: HGLOBAL
  kMsgClearClipboard,          // Removes the screenshot this program last put on the clipboard, if it is still there.
  kMsgDragFrame,               // Sent once per composed frame while a thumbnail is in the air.
  kMsgDragFeedback,            // wParam: drag id, lParam: whether the window under the pointer accepts the drop.
  kMsgDragFinished,            // lParam: FileDragOutcome*, ownership moves to the receiver
  kMsgDragCancel,              // The keyboard or right button called off the drag.
  kMsgSelectorStep,            // lParam: -1 for the previous option, 1 for the next.
};

// Monotonic seconds on the same clock DirectComposition uses for its timeline.
double NowSeconds();
int64_t QpcFrequency();

// True when screenshots show the overlay: chosen in config.h, or forced by SSTK_DEBUG_VISIBLE so that
// test tooling can sample the overlay from the screen.
bool OverlayVisibleToCapture();

// Appends a line to %TEMP%\ScreenshotToolkit.log when SSTK_TRACE is set. No-op otherwise.
void Trace(const char* format, ...);

// Asks Windows never to apply EcoQoS throttling, which would add wake-up latency to a background process.
void DisablePowerThrottling();

}  // namespace sstk
