// Design tokens. Lengths are device independent pixels; springs follow response/damping notation.
//
// Visual language, "Graphite Glass": a monochrome palette of warm graphite and porcelain, so the
// screenshot itself is the only color on screen. Hue appears only where a drag asks a question: blue
// says a drop will land, red says it will delete. A thin white border and a soft shadow lift each
// thumbnail off the desktop, and everything moves on physically modeled springs.
#pragma once

#include <cstdint>

#include "motion.h"

namespace sstk::theme {

// Thumbnail deck.
inline constexpr float kCardMaxHeight = 160.0f;
inline constexpr float kCardMaxWidth = 340.0f;
inline constexpr float kEdgeMargin = 16.0f;
inline constexpr float kCardRadius = 12.0f;
inline constexpr float kCardBorder = 2.0f;
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
inline constexpr double kDropDwellBonus = 2.0;

// Thumbnail in the air.
inline constexpr float kDragScale = 0.72f;
inline constexpr float kDragTiltMaxDegrees = 2.6f;
inline constexpr float kDragTiltPerSpeed = 2.6f / 2600.0f;  // Degrees per DIP/s of horizontal speed.
inline constexpr float kDragShadowRest = 0.78f;
inline constexpr float kDragShadowDrop = 5.0f;
inline constexpr float kDragLandingZone = 44.0f;
inline constexpr float kDropVanishShrink = 0.20f;
inline constexpr float kDropBadgeSize = 22.0f;

// Rubbish bin.
inline constexpr float kBinHeight = 44.0f;
inline constexpr float kBinMargin = 18.0f;
inline constexpr float kBinIconSize = 24.0f;
inline constexpr float kBinFontSize = 14.0f;
inline constexpr float kBinLabelStart = 40.0f;
inline constexpr float kBinLabelEnd = 18.0f;
inline constexpr float kBinNearDistance = 170.0f;
inline constexpr float kBinNearHysteresis = 30.0f;
inline constexpr float kBinHotMargin = 16.0f;
inline constexpr float kBinHotMarginEngaged = 38.0f;
inline constexpr float kBinGrowNear = 1.03f;
inline constexpr float kBinGrowEngaged = 1.08f;
inline constexpr float kBinMagnet = 0.035f;
inline constexpr float kBinMagnetMax = 5.0f;
inline constexpr float kBinLidNearDegrees = 8.0f;
inline constexpr float kBinLidOpenDegrees = 34.0f;
inline constexpr float kBinLidLift = 2.0f;
inline constexpr float kBinHeldScale = 0.80f;
inline constexpr float kBinDockGap = 36.0f;
inline constexpr float kBinNeck = 12.0f;
inline constexpr float kBinInsideAt = 0.85f;
inline constexpr float kBinInsideImpulse = 1.1f;
inline constexpr float kBinOutsideImpulse = -0.7f;
inline constexpr float kBinGulpAt = 0.90f;
inline constexpr double kBinGulpHold = 0.24;

// Palette, 0xRRGGBB unless noted. Alpha is premultiplied when rendered.
inline constexpr uint32_t kGlassTint = 0x16171A;
inline constexpr float kGlassTintAlpha = 0.80f;
inline constexpr uint32_t kPorcelain = 0xF3F2EF;
inline constexpr uint32_t kGlyphOnGlass = 0xFFFFFF;
inline constexpr float kGlyphOnGlassAlpha = 0.74f;
inline constexpr uint32_t kGlyphOnPorcelain = 0x16171A;
inline constexpr float kCardBorderAlpha = 1.0f;
inline constexpr float kCapsuleHairlineAlpha = 0.14f;
inline constexpr float kHoverVeilAlpha = 0.10f;
inline constexpr float kFlashPeak = 0.36f;
inline constexpr float kSwipeEndOpacity = 0.90f;
inline constexpr float kExitOpacity = 0.86f;
inline constexpr uint32_t kDropBlue = 0x0A84FF;
inline constexpr uint32_t kBinRed = 0xC9352F;
inline constexpr float kBinEngagedVeil = 0.08f;

// Thumbnail shadow: a wide ambient layer and a tight contact layer. Shadows never fall on the taskbar.
inline constexpr float kShadowAmbientSigma = 9.0f;
inline constexpr float kShadowAmbientAlpha = 0.26f;
inline constexpr float kShadowAmbientOffset = 5.0f;
inline constexpr float kShadowContactSigma = 1.5f;
inline constexpr float kShadowContactAlpha = 0.14f;
inline constexpr float kShadowContactOffset = 1.0f;

// Motion.
inline constexpr SpringSpec kFlash{0.18f, 1.0f};
inline constexpr SpringSpec kFlight{0.50f, 0.86f};
inline constexpr SpringSpec kReflow{0.42f, 0.86f};
inline constexpr SpringSpec kMorph{0.36f, 0.86f};
inline constexpr SpringSpec kMorphFade{0.24f, 1.0f};
inline constexpr SpringSpec kExit{0.38f, 1.0f};
inline constexpr SpringSpec kSnapBack{0.34f, 0.82f};
inline constexpr SpringSpec kFade{0.16f, 1.0f};
inline constexpr SpringSpec kQuickFade{0.09f, 1.0f};
inline constexpr SpringSpec kCapsuleEnter{0.42f, 0.78f};
inline constexpr SpringSpec kPillLeading{0.24f, 0.80f};
inline constexpr SpringSpec kPillTrailing{0.34f, 0.84f};
inline constexpr SpringSpec kCapsuleLeave{0.26f, 1.0f};
inline constexpr SpringSpec kDragLift{0.36f, 0.72f};
inline constexpr SpringSpec kDragCatchUp{0.16f, 1.0f};
inline constexpr SpringSpec kDragTilt{0.46f, 0.50f};
inline constexpr SpringSpec kDropVanish{0.30f, 1.0f};
inline constexpr SpringSpec kDropBadgeIn{0.32f, 0.54f};
inline constexpr SpringSpec kDropBadgeOut{0.15f, 1.0f};
inline constexpr SpringSpec kFlyHome{0.52f, 0.80f};
inline constexpr SpringSpec kBinPullIn{0.56f, 0.96f};
inline constexpr SpringSpec kBinPullOut{0.48f, 0.84f};
inline constexpr SpringSpec kBinSwallow{0.30f, 1.0f};
inline constexpr SpringSpec kBinDock{0.24f, 1.0f};
inline constexpr SpringSpec kBinGrow{0.34f, 0.78f};
inline constexpr SpringSpec kBinLid{0.30f, 0.66f};
inline constexpr SpringSpec kBinLidSlam{0.20f, 0.70f};
inline constexpr SpringSpec kBinSquash{0.38f, 0.58f};
inline constexpr double kFlightHold = 0.02;
inline constexpr double kCapsuleEnterDelay = 0.14;
inline constexpr double kFlightLanding = 0.45;

}  // namespace sstk::theme
