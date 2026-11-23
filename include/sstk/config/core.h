// Default build settings: what the program does and when. The dwell time, what a shot contains, how deletion
// works, and the gallery folder can also be changed without a rebuild, in settings.ini (see base/settings.h).
#pragma once

#include <cstdint>

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

// Updates. A newer build is looked for at each start and every 12 hours, and again an hour after a check fails. One
// downloaded is installed once nothing is on screen and no key or button has been touched for kUpdateIdleSeconds.
inline constexpr bool kAutoUpdate = true;
inline constexpr unsigned kUpdateCheckHours = 12;
inline constexpr unsigned kUpdateRetryMinutes = 60;
inline constexpr unsigned kUpdateIdleSeconds = 60;

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

// Reading a window by itself, for its region and for scrolling screenshots: how long to wait for its first picture,
// and how long the capture device stays open after the last shot.
inline constexpr unsigned kWindowPictureWaitMs = 500;
inline constexpr unsigned kScrollPictureWaitMs = 1000;
inline constexpr unsigned kCaptureDeviceKeepSeconds = 30;

// Which windows become selector options. Lengths are physical pixels.
inline constexpr int kMinimumWindowExtent = 48;
inline constexpr double kMinimumVisibleFraction = 0.5;
inline constexpr int kSameRectTolerance = 2;
inline constexpr int kTaskbarHiddenThickness = 4;

// Scrolling screenshots. Times are in seconds, and lengths in physical pixels unless they say otherwise.
inline constexpr double kScrollStartSeconds = 8.0;     // Waiting for the first scroll.
inline constexpr double kScrollIdleSeconds = 5.0;      // No scrolling for this long finishes it.
inline constexpr double kScrollMismatchSeconds = 1.5;  // Content that cannot be stitched for this long ends it early,
inline constexpr double kScrollMismatchWheelSeconds = 4.0;  // or this long with a wheel still turning,
inline constexpr double kScrollWheelActiveSeconds = 0.5;    // as it counts as doing for this long after each notch.
inline constexpr double kScrollResultSeconds = 1.5;    // How long the selector shows the outcome.
inline constexpr double kScrollPartialShare = 0.10;    // An early end still keeps a capture grown by this share.
inline constexpr double kScrollGuardSeconds = 0.25;    // How often an app touched since its shot is checked on.
inline constexpr unsigned kScrollActiveMs = 25;        // Sampling while content moves,
inline constexpr unsigned kScrollRestingMs = 90;       // and while it rests.
inline constexpr unsigned kScrollCatchUpMs = 12;       // While it cannot be lined up.
inline constexpr double kScrollActiveHold = 0.6;
inline constexpr int kScrollMaxHeight = 30000;
inline constexpr int kScrollMaxPixels = 90'000'000;

// How readings are compared.
inline constexpr int kScrollPixelSlack = 6;             // Per color channel, for two readings of a pixel to show the same.
inline constexpr double kScrollReplacedShare = 0.9;     // Changes across this much of an area replace it, rather than animate in it.
inline constexpr int kScrollMinStep = 24;               // Rows a view must move, or stop short of, before it is read: slow and fast scrolls then read alike.

// Device independent pixels.
inline constexpr float kScrollPointerBand = 56.0f;      // Above and below the pointer, where hover effects are taken for granted.
inline constexpr float kScrollBarWidth = 24.0f;         // The widest a scroll bar may be.
inline constexpr float kScrollClusterGap = 48.0f;       // Fixed things nearer than this to each other are one cluster.
inline constexpr float kScrollBottomReach = 64.0f;      // A cluster reaching this near the bottom of the view sits at the bottom.
inline constexpr float kScrollAreaMinWidth = 120.0f;    // The smallest area worth following.
inline constexpr float kScrollAreaMinHeight = 96.0f;
inline constexpr float kScrollHiddenMinHeight = 48.0f;  // How much must be hidden to say content overflows without a scroller.
inline constexpr float kScrollEdgeReach = 8.0f;         // How near the edge of its box content must reach to run on past it.

// What the app says about its content. Nothing else counts, so a window that says nothing, such as a game, has no
// scrolling screenshot.
inline constexpr double kScrollMaxViewPercent = 99.0;   // A scroller showing more than this has nothing left to scroll to.
inline constexpr int kScrollHiddenSetItems = 3;         // Items a list's size must exceed what is rendered by.
inline constexpr double kScrollTextMinShare = 0.15;     // Of the window, for a text document's hidden text to count.
inline constexpr unsigned kScrollDetectTimeoutMs = 1500;
inline constexpr unsigned kScrollDetectRetryMs[] = {150, 350, 700, 1200};  // Some apps build what they tell only once asked.

// The editing panel.
inline constexpr double kEditorZoomStep = 1.2;     // Per wheel notch, and per Ctrl+ or Ctrl-.
inline constexpr float kEditorMaxZoom = 8.0f;      // Screen pixels per picture pixel.
inline constexpr float kEditorMaxFit = 2.0f;       // A small crop is enlarged no further than this to fill the space.
inline constexpr float kEditorEdgeZoom = 1.6f;     // How fast a crop handle held against the edge widens the view, per second.
inline constexpr float kEditorEdgeZone = 28.0f;    // How near the edge that starts, in device independent pixels.
inline constexpr unsigned kEditorHeartbeatMs = 400;
inline constexpr unsigned kEditorKeyTimeoutMs = 2000;  // Keys go back to the system once the panel has been silent this long.
inline constexpr float kRulerStepDegrees = 5.0f;   // Per wheel notch over the ruler, and 1 with Shift.
inline constexpr float kRulerSnapDegrees = 2.0f;   // To every 45 degrees.

// Housekeeping.
inline constexpr unsigned kTickIntervalMs = 40;
inline constexpr int kTaskbarPollTicks = 3;
inline constexpr unsigned kTrimDelayMs = 1500;
inline constexpr unsigned kClipboardRetryMs = 20;
inline constexpr int kClipboardMaxRetries = 25;

}  // namespace sstk::config
