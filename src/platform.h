// Shared Win32 includes, process-wide names, and small utilities.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace sstk {

inline constexpr wchar_t kTriggerEventName[] = L"Local\\ScreenshotToolkit.Trigger";
inline constexpr wchar_t kQuitEventName[] = L"Local\\ScreenshotToolkit.Quit";
inline constexpr wchar_t kHookHostQuitEventName[] = L"Local\\ScreenshotToolkit.HookHost.Quit";
inline constexpr wchar_t kAppMutexName[] = L"Local\\ScreenshotToolkit.App";
inline constexpr wchar_t kHookHostMutexName[] = L"Local\\ScreenshotToolkit.HookHost";

// Messages handled by the hidden controller window on the UI thread.
enum Message : UINT {
  kMsgFlash = WM_APP + 1,      // wParam: HMONITOR, lParam: 0
  kMsgCaptured,                // lParam: CaptureResult*, ownership moves to the receiver
  kMsgClipboardReady,          // wParam: sequence number, lParam: HGLOBAL
};

// Monotonic seconds on the same clock DirectComposition uses for its timeline.
double NowSeconds();
int64_t QpcFrequency();

// True when SSTK_DEBUG_VISIBLE is set, so test tooling can sample the overlay from the screen.
bool OverlayVisibleToCapture();

// Appends a line to %TEMP%\ScreenshotToolkit.log when SSTK_TRACE is set. No-op otherwise.
void Trace(const char* format, ...);

// Asks Windows never to apply EcoQoS throttling, which would add wake-up latency to a background process.
void DisablePowerThrottling();

}  // namespace sstk
