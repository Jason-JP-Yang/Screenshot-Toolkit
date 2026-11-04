// Default build settings: what the program does and when.
#pragma once

namespace sstk::config {

// Whether screenshots show Screenshot Toolkit's own thumbnails and selector.
//   false: consecutive shots contain only what is really on the desktop.
//   true:  a shot taken while thumbnails are on screen shows them, in the bottom-left corner.
// The capture flash is never included either way.
inline constexpr bool kCaptureIncludesThumbnails = false;

// Where a screenshot dropped on the rubbish bin goes.
//   true:  the Windows Recycle Bin, so an accidental drop can be undone.
//   false: deleted for good straight away.
inline constexpr bool kDeleteToRecycleBin = true;

// Saved files. Takes the year, month, day, hour, minute, and second.
inline constexpr wchar_t kFileNameFormat[] = L"Screenshot %04d-%02d-%02d %02d%02d%02d";

// How long thumbnails stay.
inline constexpr double kDwellSeconds = 3.0;
inline constexpr double kDropDwellBonus = 2.0;

// Pressing and swiping a thumbnail. Lengths are device independent pixels.
inline constexpr float kPressSlop = 5.0f;
inline constexpr float kSwipeDismissFraction = 0.35f;
inline constexpr float kSwipeDismissVelocity = 900.0f;
inline constexpr float kRubberBand = 0.28f;

// How near the pointer must come to the rubbish bin. "Near" opens the capsule; "hot" draws the card in.
inline constexpr float kBinNearDistance = 170.0f;
inline constexpr float kBinNearHysteresis = 30.0f;
inline constexpr float kBinHotMargin = 16.0f;
inline constexpr float kBinHotMarginEngaged = 38.0f;

// Which windows become selector options. Lengths are physical pixels.
inline constexpr int kMinimumWindowExtent = 48;
inline constexpr double kMinimumVisibleFraction = 0.5;
inline constexpr int kSameRectTolerance = 2;
inline constexpr int kTaskbarHiddenThickness = 4;

// Scrolling screenshots. Times are in seconds, and lengths in physical pixels unless they say otherwise.
inline constexpr double kScrollStartSeconds = 8.0;     // Waiting for the first scroll.
inline constexpr double kScrollIdleSeconds = 5.0;      // No scrolling for this long finishes it.
inline constexpr double kScrollMismatchSeconds = 1.5;  // Content that cannot be stitched for this long ends it early.
inline constexpr double kScrollResultSeconds = 1.5;    // How long the selector shows the outcome.
inline constexpr double kScrollPartialShare = 0.10;    // An early end still keeps a capture grown by this share.
inline constexpr unsigned kScrollActiveMs = 40;        // Sampling while content moves,
inline constexpr unsigned kScrollRestingMs = 90;       // and while it rests.
inline constexpr double kScrollActiveHold = 0.6;
inline constexpr int kScrollMaxHeight = 30000;
inline constexpr int kScrollMaxPixels = 40'000'000;
inline constexpr float kScrollMatchShare = 0.55f;      // Of the comparable rows, how many must agree on a shift.
inline constexpr int kScrollMatchMinRows = 6;
inline constexpr int kScrollRepeatLimit = 6;           // A row repeating more often than this cannot place a shift.
inline constexpr double kScrollBandMinShare = 0.25;    // The moving part between a fixed header and footer.
inline constexpr float kScrollAreaMinWidth = 96.0f;    // Device independent pixels, as is the next one.
inline constexpr float kScrollAreaMinHeight = 72.0f;
inline constexpr unsigned kScrollDetectTimeoutMs = 1500;

// Housekeeping.
inline constexpr unsigned kTickIntervalMs = 40;
inline constexpr int kTaskbarPollTicks = 3;
inline constexpr unsigned kTrimDelayMs = 1500;
inline constexpr unsigned kClipboardRetryMs = 20;
inline constexpr int kClipboardMaxRetries = 25;

}  // namespace sstk::config
