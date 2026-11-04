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
inline constexpr int kFrostSegments = 8;

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
