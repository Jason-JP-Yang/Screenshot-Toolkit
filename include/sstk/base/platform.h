// Shared Win32 includes, process-wide names, and small utilities.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace sstk {

inline constexpr wchar_t kTriggerEventName[] = L"Local\\ScreenshotToolkit.Trigger";
inline constexpr wchar_t kEditTriggerEventName[] = L"Local\\ScreenshotToolkit.EditTrigger";
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
  kMsgScrollAreas,             // lParam: ScrollAreas*, ownership moves to the receiver
  kMsgScrollChord,             // wParam: 1 when a capture chord sent it, 0 for the scroll chord.
  kMsgScrollWheel,             // lParam: where the wheel turned, in virtual screen coordinates, as two signed halves.
  kMsgScrollUpdate,            // lParam: ScrollUpdate*, ownership moves to the receiver
  kMsgOpenShot,                // lParam: std::wstring*, the saved file to open, ownership moves to the receiver
  kMsgWindowShots,             // lParam: WindowShots*, ownership moves to the receiver
  kMsgPointerInput,            // wParam: when, in microseconds of NowSeconds. lParam: where a button or wheel went, as two signed halves.
  kMsgKeyInput,                // wParam: when, in microseconds of NowSeconds.
  kMsgScrollMismatch,          // wParam: the shot whose app no longer shows what the shot did.
  kMsgEditorKey,               // wParam: virtual key, lParam: kEditorKey* flags. The editing panel has every key while open.
  kMsgEditorWheel,             // wParam: the delta in the low 16 bits, then kEditorKey* flags from bit 16. lParam: the point.
  kMsgEditorJob,               // lParam: EditorJobResult*, ownership moves to the receiver
};

// What comes with a key or wheel turn held for the editing panel.
inline constexpr LPARAM kEditorKeyDown = 1;
inline constexpr LPARAM kEditorKeyRepeat = 2;
inline constexpr LPARAM kEditorKeyControl = 4;
inline constexpr LPARAM kEditorKeyShift = 8;
inline constexpr LPARAM kEditorKeyAlt = 16;
inline constexpr LPARAM kEditorWheelHorizontal = 32;

// Monotonic seconds on the same clock DirectComposition uses for its timeline.
double NowSeconds();
int64_t QpcFrequency();

// True when screenshots show the overlay: chosen in the settings, or forced by SSTK_DEBUG_VISIBLE so that
// test tooling can sample the overlay from the screen.
bool OverlayVisibleToCapture();

// Appends a line to %TEMP%\ScreenshotToolkit.log when tracing. No-op otherwise.
void Trace(const char* format, ...);

// Whether to trace: SSTK_TRACE is set, or %TEMP%\ScreenshotToolkit.trace exists, which a running instance notices.
bool Tracing();

// Writes BGRX pixels as %TEMP%\ScreenshotToolkit-<name>.png when tracing. No-op otherwise.
void TraceImage(const wchar_t* name, const uint32_t* pixels, int width, int height);

// Asks Windows never to apply EcoQoS throttling, which would add wake-up latency to a background process.
void DisablePowerThrottling();

}  // namespace sstk
