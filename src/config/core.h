// Default build settings: what the program does and when.
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
inline constexpr double kScrollMismatchSeconds = 1.5;  // Content that cannot be stitched for this long ends it early,
inline constexpr double kScrollMismatchWheelSeconds = 4.0;  // or this long with a wheel still turning,
inline constexpr double kScrollWheelActiveSeconds = 0.5;    // as it counts as doing for this long after each notch.
inline constexpr double kScrollResultSeconds = 1.5;    // How long the selector shows the outcome.
inline constexpr double kScrollPartialShare = 0.10;    // An early end still keeps a capture grown by this share.
inline constexpr unsigned kScrollActiveMs = 25;        // Sampling while content moves,
inline constexpr unsigned kScrollRestingMs = 90;       // and while it rests.
inline constexpr unsigned kScrollCatchUpMs = 12;       // While it cannot be lined up.
inline constexpr double kScrollActiveHold = 0.6;
inline constexpr int kScrollMaxHeight = 30000;
inline constexpr int kScrollMaxPixels = 60'000'000;

// How readings are compared, on an edge map whose values run to 127 either way, in cells of 16 by 16 pixels.
inline constexpr int kScrollEdgeDeadZone = 6;           // Steps this small are shade rather than edges.
inline constexpr uint32_t kScrollBlankCell = 160;       // A cell with less edge energy than this has nothing to compare.
inline constexpr float kScrollFollowShare = 0.4f;       // Differences within this share of a cell's energy still match
inline constexpr float kScrollStayShare = 0.2f;         // the scroll, or say it stayed where it was.
inline constexpr float kScrollLooseShare = 0.62f;       // Blurred by motion, this is still enough to place a reading.
inline constexpr float kScrollCleanShare = 0.22f;       // A reading this close to the rows held is sharp enough to overrule them.
inline constexpr float kScrollDifferShare = 0.35f;      // Beyond this share a cell shows something else than the rows held,
inline constexpr int kScrollColorSlack = 16;            // or beyond this per pixel, summed over the color channels.
inline constexpr int kScrollRefreshReadings = 3;        // Readings in a row it must do so to replace them.
inline constexpr int kScrollMinMovingCells = 3;         // Cells that must follow a shift for it to count as a scroll,
inline constexpr int kScrollMinMovingCellsAlone = 12;   // and with no wheel turning.
inline constexpr double kScrollMovingShare = 0.35;      // Of the cells that do not stay put, how many must follow it.
inline constexpr double kScrollReplacedShare = 0.9;     // Changes across this much of an area replace it, rather than animate in it.
inline constexpr double kScrollBarrierShare = 0.12;     // Of the rows compared, how many a column must hold still to bound the area,
inline constexpr double kScrollSparseShare = 0.02;      // or this many if they are spread over at least half of them,
inline constexpr double kScrollLineShare = 0.6;         // or how many a line must span, if it follows nothing.

// Device independent pixels.
inline constexpr float kScrollPointerBand = 56.0f;      // Above and below the pointer, where hover effects are taken for granted.
inline constexpr float kScrollBarWidth = 24.0f;         // The widest a scroll bar may be.
inline constexpr float kScrollAreaMinWidth = 120.0f;    // The smallest area worth following.
inline constexpr float kScrollAreaMinHeight = 96.0f;
inline constexpr float kScrollHiddenMinHeight = 48.0f;  // How much must be hidden to say content overflows without a scroller.
inline constexpr float kScrollContentTile = 32.0f;      // Tiles for telling drawn content from what the app describes.

// What the app says about its content.
inline constexpr double kScrollMaxViewPercent = 99.0;   // A scroller showing more than this has nothing left to scroll to.
inline constexpr int kScrollHiddenSetItems = 3;         // Items a list's size must exceed what is rendered by.
inline constexpr int kScrollContentMinTiles = 8;        // Tiles of content nobody describes that may hide more of it.
inline constexpr long kScrollCanvasPercent = 40;        // A picture covering this much of the window describes nothing.
inline constexpr unsigned kScrollDetectTimeoutMs = 1500;
inline constexpr unsigned kScrollDetectRetryMs[] = {150, 350, 700, 1200};  // Some apps build what they tell only once asked.
inline constexpr int kScrollDetectContentAttempt = 3;   // Content nobody describes counts only once the app has had this many tries.

// Housekeeping.
inline constexpr unsigned kTickIntervalMs = 40;
inline constexpr int kTaskbarPollTicks = 3;
inline constexpr unsigned kTrimDelayMs = 1500;
inline constexpr unsigned kClipboardRetryMs = 20;
inline constexpr int kClipboardMaxRetries = 25;

}  // namespace sstk::config
