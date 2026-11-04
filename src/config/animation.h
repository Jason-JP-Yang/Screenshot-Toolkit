// Default build settings: how everything moves. Springs are {response, damping}: response is the seconds
// for one undamped oscillation, so smaller is faster, and damping 1 never overshoots while less than 1 does.
// Times are in seconds, and lengths in device independent pixels.
#pragma once

#include "../motion.h"

namespace sstk::config {

// Capture flash.
inline constexpr double kFlashRise = 0.06;
inline constexpr SpringSpec kFlashFall{0.62f, 1.0f};

// The screenshot flying into the deck.
inline constexpr SpringSpec kFlight{0.50f, 0.86f};
inline constexpr double kFlightHold = 0.02;
inline constexpr double kFlightLanding = 0.45;
inline constexpr double kFlightDecorationDelay = 0.22;
inline constexpr double kBaseFadeDelay = 0.13;
inline constexpr double kCrispFlightDelay = 0.26;
inline constexpr double kFullReleaseDelay = 0.45;

// The deck.
inline constexpr SpringSpec kReflow{0.42f, 0.86f};
inline constexpr SpringSpec kMorph{0.36f, 0.86f};
inline constexpr SpringSpec kMorphFade{0.24f, 1.0f};
inline constexpr double kCrispMorphDelay = 0.02;
inline constexpr SpringSpec kExit{0.38f, 1.0f};
inline constexpr float kExitOvershoot = 48.0f;
inline constexpr SpringSpec kSnapBack{0.34f, 0.82f};
inline constexpr SpringSpec kFade{0.16f, 1.0f};
inline constexpr SpringSpec kQuickFade{0.09f, 1.0f};

// The region selector.
inline constexpr SpringSpec kCapsuleEnter{0.42f, 0.78f};
inline constexpr SpringSpec kCapsuleLeave{0.26f, 1.0f};
inline constexpr double kCapsuleEnterDelay = 0.14;
inline constexpr float kCapsuleRise = 10.0f;
inline constexpr SpringSpec kPillLeading{0.24f, 0.80f};
inline constexpr SpringSpec kPillTrailing{0.34f, 0.84f};

// The thumbnail in the air.
inline constexpr float kMaxFrameTime = 0.05f;
inline constexpr float kDragScale = 0.72f;
inline constexpr SpringSpec kDragLift{0.36f, 0.72f};
inline constexpr SpringSpec kDragCatchUp{0.16f, 1.0f};
inline constexpr SpringSpec kDragTilt{0.46f, 0.50f};
inline constexpr float kDragTiltMaxDegrees = 2.6f;
inline constexpr float kDragTiltPerSpeed = 2.6f / 2600.0f;  // Degrees per DIP/s of horizontal speed.
inline constexpr float kDragSpeedSmoothing = 0.035f;
inline constexpr float kDragShadowRest = 0.78f;
inline constexpr float kDragShadowDrop = 5.0f;
inline constexpr float kDragLandingZone = 44.0f;
inline constexpr SpringSpec kFlyHome{0.52f, 0.80f};

// A drop that lands.
inline constexpr SpringSpec kDropVanish{0.30f, 1.0f};
inline constexpr float kDropVanishShrink = 0.20f;
inline constexpr float kDropDriftDecay = 16.0f;
inline constexpr float kDropShadowFade = 1.8f;
inline constexpr SpringSpec kDropBadgeIn{0.32f, 0.54f};
inline constexpr SpringSpec kDropBadgeOut{0.15f, 1.0f};
inline constexpr float kDropBadgeGrow = 0.22f;

// Being drawn into the rubbish bin and back out.
inline constexpr float kBinStretch = 0.45f;  // 0 shrinks the card evenly into the bin; 1 draws it out into a long funnel.
inline constexpr float kBinNeck = 24.0f;     // Wider keeps the card from pinching on its way in.
inline constexpr SpringSpec kBinPull{0.42f, 0.88f};
inline constexpr SpringSpec kBinSwallow{0.30f, 1.0f};
inline constexpr float kBinPullKick = 3.0f;
inline constexpr float kBinPullKickPerSpeed = 0.0012f;  // Per DIP/s of pointer speed.
inline constexpr float kBinPullKickMax = 6.0f;
inline constexpr float kBinMagnetFrom = 0.35f;  // How close the pointer must be, from 0 to 1, for the card to feel the bin.
inline constexpr float kBinMagnetPull = 0.14f;  // How far that draws the card in by the time the pointer arrives.
inline constexpr float kBinInsideAt = 0.85f;
inline constexpr float kBinGulpAt = 0.90f;
inline constexpr double kBinGulpHold = 0.30;
inline constexpr float kBinHideMargin = 0.05f;
inline constexpr float kBinShadowFade = 7.0f;
inline constexpr float kBinMeshCells = 170.0f;

// The rubbish bin's capsule and icon.
inline constexpr SpringSpec kBinMorph{0.34f, 0.84f};
inline constexpr SpringSpec kBinLid{0.26f, 0.58f};
inline constexpr SpringSpec kBinLidAim{0.30f, 0.62f};
inline constexpr SpringSpec kBinLidSlam{0.18f, 0.62f};
inline constexpr float kBinLidNearOpen = 0.34f;
inline constexpr float kBinLidTiltDegrees = 40.0f;
inline constexpr float kBinLidFullAimDegrees = 55.0f;
inline constexpr float kBinLidRiseAbove = 3.2f;  // In the icon's 24-unit grid, as are the next two.
inline constexpr float kBinLidRiseAside = 0.6f;
inline constexpr float kBinLidSlide = 0.9f;
inline constexpr float kBinLidClosedPastMin = -0.12f;
inline constexpr SpringSpec kBinCan{0.30f, 0.52f};
inline constexpr float kBinCanWidening = 0.55f;
inline constexpr float kBinCanSquashMax = 0.16f;
inline constexpr float kBinInsideImpulse = 2.2f;
inline constexpr float kBinOutsideImpulse = -1.6f;
inline constexpr float kBinGulpImpulse = 3.4f;

}  // namespace sstk::config
