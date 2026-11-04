// Default build settings: every color and shadow. Colors are 0xRRGGBB, and opacities run from 0 to 1.
//
// Visual language, "Graphite Glass": a monochrome palette of warm graphite and porcelain, so the
// screenshot itself is the only color on screen. Hue appears only where a drag asks a question: blue
// says a drop will land, red says it will delete. Flat colors only.
#pragma once

#include <cstdint>

namespace sstk::config {

// Capture flash.
inline constexpr uint32_t kFlashColor = 0xFFFFFF;
inline constexpr float kFlashPeak = 0.26f;

// Thumbnails.
inline constexpr uint32_t kCardBorderColor = 0xFFFFFF;
inline constexpr float kCardBorderAlpha = 1.0f;
inline constexpr float kDeckOpacityPerLevel = 0.14f;
inline constexpr float kSwipeEndOpacity = 0.90f;
inline constexpr float kExitOpacity = 0.86f;

// Region selector.
inline constexpr uint32_t kGlassTint = 0x16171A;
inline constexpr float kGlassTintAlpha = 0.80f;
inline constexpr uint32_t kPorcelain = 0xF3F2EF;
inline constexpr uint32_t kGlyphOnGlass = 0xFFFFFF;
inline constexpr float kGlyphOnGlassAlpha = 0.74f;
inline constexpr uint32_t kGlyphOnPorcelain = 0x16171A;
inline constexpr uint32_t kHoverVeilColor = 0xFFFFFF;
inline constexpr float kHoverVeilAlpha = 0.10f;

// The outcome of a scrolling screenshot, filling the selector for a moment, and the ink on it.
inline constexpr uint32_t kScrollSucceeded = 0x1C7C43;
inline constexpr uint32_t kScrollPartial = 0x0D4527;
inline constexpr uint32_t kScrollFailed = 0x8E1519;
inline constexpr uint32_t kStatusInk = 0xFFFFFF;
inline constexpr float kStatusInkAlpha = 0.94f;
inline constexpr float kFinishHintAlpha = 0.58f;

// The hairline around the selector and the rubbish bin.
inline constexpr uint32_t kCapsuleHairlineColor = 0xFFFFFF;
inline constexpr float kCapsuleHairlineAlpha = 0.14f;

// Dragging.
inline constexpr uint32_t kDropBlue = 0x0A84FF;

// Rubbish bin.
inline constexpr uint32_t kBinRed = 0x8E1519;
inline constexpr uint32_t kBinRedOver = 0x9E181D;
inline constexpr uint32_t kBinInk = 0xFFFFFF;

// Shadows are always neutral black. Blur and offset are in device independent pixels.
struct ShadowSpec {
  float sigma;
  float alpha;
  float offset;
};

// A resting thumbnail: a wide ambient layer and a tight contact layer. They never fall on the taskbar.
inline constexpr ShadowSpec kCardShadowAmbient{9.0f, 0.26f, 5.0f};
inline constexpr ShadowSpec kCardShadowContact{1.5f, 0.14f, 1.0f};
inline constexpr float kBehindShadowAlpha = 0.5f;

// The selector and the rubbish bin.
inline constexpr ShadowSpec kCapsuleShadow{9.0f, 0.34f, 4.0f};

// A thumbnail in the air.
inline constexpr ShadowSpec kDragShadowWide{15.0f, 0.34f, 11.0f};
inline constexpr ShadowSpec kDragShadowTight{3.5f, 0.16f, 2.5f};

}  // namespace sstk::config
