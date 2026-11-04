// Default build settings: sizes, spacing, and type. Lengths are device independent pixels.
#pragma once

namespace sstk::config {

// Thumbnail deck.
inline constexpr float kCardMaxHeight = 160.0f;
inline constexpr float kCardMaxWidth = 340.0f;
inline constexpr float kEdgeMargin = 16.0f;
inline constexpr float kCardRadius = 12.0f;
inline constexpr float kCardBorder = 2.0f;
inline constexpr float kDeckPeek = 9.0f;
inline constexpr float kDeckShrinkPerLevel = 0.055f;
inline constexpr int kDeckVisibleBehind = 2;

// Region selector.
inline constexpr float kCapsuleGap = 10.0f;
inline constexpr float kSegmentWidth = 38.0f;
inline constexpr float kSegmentHeight = 28.0f;
inline constexpr float kCapsulePadding = 3.0f;
inline constexpr int kFrostDownscale = 8;
inline constexpr float kFrostWidth = 640.0f;  // Wide enough for the widest selector.

// The selected option's label, and the states of a scrolling screenshot. Gaps measure between what is visibly drawn.
inline constexpr float kCapsuleFontSize = 13.0f;
inline constexpr float kLabelGap = 6.0f;
inline constexpr float kKeycapHeight = 15.0f;
inline constexpr float kKeycapGap = 2.5f;
inline constexpr float kKeycapTextGap = 5.0f;
inline constexpr float kRecordingDot = 8.0f;
inline constexpr float kStatusIconSize = 14.0f;
inline constexpr float kStatusGap = 8.0f;
inline constexpr float kFinishHintGap = 14.0f;
inline constexpr wchar_t kLabelFullscreen[] = L"Fullscreen";
inline constexpr wchar_t kLabelWindow[] = L"Window";
inline constexpr wchar_t kLabelNoTaskbar[] = L"No Taskbar";
inline constexpr wchar_t kLabelScrolling[] = L"Scrolling Screenshot";
inline constexpr wchar_t kScrollHintBefore[] = L"Press again /";  // The keycaps of the scroll chord follow.
inline constexpr wchar_t kScrollHintAfter[] = L"to activate Scrolling Screenshot";
inline constexpr wchar_t kRecordingWaiting[] = L"Scroll to capture";
inline constexpr wchar_t kRecordingActive[] = L"Recording";
inline constexpr wchar_t kRecordingFinish[] = L"to finish";
inline constexpr wchar_t kResultSucceeded[] = L"Scrolling Screenshot Saved";
inline constexpr wchar_t kResultPartial[] = L"Stopped Early, Saved";
inline constexpr wchar_t kResultFailed[] = L"Scrolling Screenshot Failed";

// A thumbnail growing with a scrolling screenshot keeps its width, and stops growing at the lower of these.
inline constexpr float kScrollCardMaxHeight = 460.0f;
inline constexpr float kScrollCardMaxShare = 0.5f;  // Of the screen height left free by the taskbar.

// The badge on a thumbnail in the air.
inline constexpr float kDropBadgeSize = 22.0f;

// Rubbish bin. The insets measure to what is visibly drawn, not to the bitmaps carrying it.
inline constexpr float kBinHeight = 38.0f;
inline constexpr float kBinMargin = 18.0f;
inline constexpr float kBinIconSize = 20.0f;
inline constexpr float kBinFontSize = 14.0f;
inline constexpr float kBinTextGap = 7.5f;
inline constexpr float kBinTextInset = 13.5f;
inline constexpr wchar_t kBinLabel[] = L"Rubbish Bin";

// Type, tried in order. Windows 11's display cut is tighter and cleaner, and older systems have only the static face.
inline constexpr const wchar_t* kLabelFaces[] = {L"Segoe UI Variable Display Semib", L"Segoe UI Semibold"};
inline constexpr float kLabelWeight = 0.92f;  // Below 1 thickens light text on a saturated body.

}  // namespace sstk::config
