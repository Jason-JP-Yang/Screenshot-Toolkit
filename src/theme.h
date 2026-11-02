// Design tokens. Lengths are device independent pixels; springs follow response/damping notation.
//
// Visual language, "Graphite Glass": a monochrome palette of warm graphite and porcelain with no
// accent hue, so the screenshot itself is the only color on screen. Depth comes from soft shadows
// and hairlines rather than borders, and everything moves on physically modeled springs.
#pragma once

#include <cstdint>

#include "motion.h"

namespace sstk::theme {

// Thumbnail deck.
inline constexpr float kCardMaxHeight = 160.0f;
inline constexpr float kCardMaxWidth = 340.0f;
inline constexpr float kEdgeMargin = 16.0f;
inline constexpr float kCardRadius = 12.0f;
inline constexpr float kDeckPeek = 9.0f;
inline constexpr float kDeckShrinkPerLevel = 0.055f;
inline constexpr int kDeckVisibleBehind = 2;
inline constexpr float kDeckOpacityPerLevel = 0.14f;
inline constexpr float kExitOvershoot = 48.0f;

// Capsule selector.
inline constexpr float kCapsuleGap = 10.0f;
inline constexpr float kSegmentWidth = 38.0f;
inline constexpr float kSegmentHeight = 28.0f;
inline constexpr float kCapsulePadding = 3.0f;
inline constexpr float kCapsuleRise = 10.0f;
inline constexpr int kFrostDownscale = 8;
inline constexpr int kFrostSegments = 8;

// Interaction.
inline constexpr double kDwellSeconds = 3.0;
inline constexpr float kPressSlop = 5.0f;
inline constexpr float kSwipeDismissFraction = 0.35f;
inline constexpr float kSwipeDismissVelocity = 900.0f;
inline constexpr float kRubberBand = 0.28f;
inline constexpr float kDragPreviewWidth = 200.0f;

// Palette, 0xRRGGBB unless noted. Alpha is premultiplied when rendered.
inline constexpr uint32_t kGlassTint = 0x16171A;
inline constexpr float kGlassTintAlpha = 0.80f;
inline constexpr uint32_t kPorcelain = 0xF3F2EF;
inline constexpr uint32_t kGlyphOnGlass = 0xFFFFFF;
inline constexpr float kGlyphOnGlassAlpha = 0.74f;
inline constexpr uint32_t kGlyphOnPorcelain = 0x16171A;
inline constexpr float kHairlineAlpha = 0.20f;
inline constexpr float kCapsuleHairlineAlpha = 0.14f;
inline constexpr float kHoverVeilAlpha = 0.10f;
inline constexpr float kFlashPeak = 0.82f;

// Motion.
inline constexpr SpringSpec kFlash{0.22f, 1.0f};
inline constexpr SpringSpec kFlight{0.50f, 0.86f};
inline constexpr SpringSpec kReflow{0.42f, 0.86f};
inline constexpr SpringSpec kMorph{0.36f, 0.86f};
inline constexpr SpringSpec kExit{0.38f, 1.0f};
inline constexpr SpringSpec kSnapBack{0.34f, 0.82f};
inline constexpr SpringSpec kFade{0.16f, 1.0f};
inline constexpr SpringSpec kQuickFade{0.09f, 1.0f};
inline constexpr SpringSpec kCapsuleEnter{0.42f, 0.78f};
inline constexpr SpringSpec kPillLeading{0.24f, 0.80f};
inline constexpr SpringSpec kPillTrailing{0.34f, 0.84f};
inline constexpr double kFlightHold = 0.02;
inline constexpr double kCapsuleEnterDelay = 0.14;
inline constexpr double kFlightLanding = 0.45;

}  // namespace sstk::theme
