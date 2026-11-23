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
inline constexpr float kShortcutInk = 0.46f;  // The keycaps beside an option's name, as a share of the name's ink.

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

// The selector's colors: over a screenshot, and in each theme of the editing panel.
struct CapsuleStyle {
  uint32_t glass;
  uint32_t hairline;
  float hairline_alpha;
  uint32_t glyph;  // On the glass.
  float glyph_alpha;
  uint32_t pill;
  uint32_t pill_glyph;
  uint32_t hover;
  float hover_alpha;
  bool shortcuts;  // Whether the scroll option's name carries the keys that start it.
};

inline constexpr CapsuleStyle kCapsuleOnScreen{kGlassTint, kCapsuleHairlineColor, kCapsuleHairlineAlpha, kGlyphOnGlass, kGlyphOnGlassAlpha,
                                               kPorcelain, kGlyphOnPorcelain, kHoverVeilColor, kHoverVeilAlpha, true};

// Dragging.
inline constexpr uint32_t kDropBlue = 0x0A84FF;

// Rubbish bin.
inline constexpr uint32_t kBinRed = 0x8E1519;
inline constexpr uint32_t kBinRedOver = 0x9E181D;
inline constexpr uint32_t kBinInk = 0xFFFFFF;

// The program's icon.
inline constexpr uint32_t kLogoTile = 0x16171A;
inline constexpr uint32_t kLogoInk = 0xF3F2EF;
inline constexpr float kLogoRimAlpha = 0.12f;  // A hairline that keeps the tile's edge on dark backgrounds.

// The setup and About window, in each Windows app theme. Buttons are a face and the ink on it; a veil of the hover
// color lightens or darkens the face under the pointer, twice as strong while pressed.
struct SetupButtonColors {
  uint32_t face;
  uint32_t ink;
  uint32_t hover;
  float hover_alpha;
};

struct SetupPalette {
  uint32_t body;
  uint32_t title;
  uint32_t text;
  SetupButtonColors primary;
  SetupButtonColors secondary;
  SetupButtonColors danger;
  uint32_t track;
  uint32_t progress;
  uint32_t focus;
};

inline constexpr SetupPalette kSetupDark{
    0x16171A, 0xF3F2EF, 0x8E8F94,
    {0xF3F2EF, 0x16171A, 0x000000, 0.08f},
    {0x2A2B30, 0xF3F2EF, 0xFFFFFF, 0.06f},
    {0x8E1519, 0xFFFFFF, 0xFFFFFF, 0.07f},
    0x2A2B30, 0xF3F2EF, 0x8E8F94,
};

inline constexpr SetupPalette kSetupLight{
    0xF3F2EF, 0x16171A, 0x6B6C70,
    {0x16171A, 0xF3F2EF, 0xFFFFFF, 0.12f},
    {0xE3E2DE, 0x16171A, 0x000000, 0.05f},
    {0x8E1519, 0xFFFFFF, 0xFFFFFF, 0.07f},
    0xE0DFDB, 0x16171A, 0x6B6C70,
};

// The editing panel, in each Windows app theme.
struct EditorPalette {
  uint32_t body;          // Behind the picture.
  float outside_alpha;    // The picture past the crop, over the body.
  uint32_t control;       // Round buttons and clusters.
  uint32_t control_ink;
  float disabled_alpha;   // Of the ink on a control with nothing to do.
  uint32_t hover;
  float hover_alpha;
  uint32_t done;
  uint32_t done_ink;
  uint32_t frame;         // The crop frame and its handles,
  uint32_t halo;          // and the soft edge that keeps them apart from the picture.
  float halo_alpha;
  uint32_t panel;         // The markup toolbar and its popovers.
  uint32_t hairline;
  float hairline_alpha;
  uint32_t barrel;        // The tools' bodies.
  uint32_t barrel_shade;
  uint32_t barrel_edge;
  uint32_t chosen;        // The chosen width in a popover, and the ink on it.
  uint32_t chosen_ink;
  uint32_t text;
  float shield_alpha;     // Black over the other monitors.
  CapsuleStyle capsule;
};

inline constexpr EditorPalette kEditorDark{
    0x0E0F11, 0.30f,
    0x232428, 0xF3F2EF, 0.30f, 0xFFFFFF, 0.08f,
    0xF3F2EF, 0x16171A,
    0xF3F2EF, 0x000000, 0.40f,
    0x1D1E22, 0xFFFFFF, 0.10f,
    0x34353B, 0x1B1C1F, 0x4A4B52,
    0xF3F2EF, 0x16171A, 0xF3F2EF,
    0.45f,
    {0x232428, 0xFFFFFF, 0.10f, 0xFFFFFF, 0.70f, 0xF3F2EF, 0x16171A, 0xFFFFFF, 0.08f, false},
};

inline constexpr EditorPalette kEditorLight{
    0xF3F2EF, 0.36f,
    0xE4E3DF, 0x16171A, 0.30f, 0x000000, 0.06f,
    0x16171A, 0xF3F2EF,
    0x16171A, 0xFFFFFF, 0.60f,
    0xFFFFFF, 0x000000, 0.08f,
    0xFFFFFF, 0xCFCEC9, 0xBDBCB7,
    0x16171A, 0xF3F2EF, 0x16171A,
    0.25f,
    {0xE4E3DF, 0x000000, 0.06f, 0x16171A, 0.62f, 0xFFFFFF, 0x16171A, 0x000000, 0.06f, false},
};

// Markup. The toolbar's five colors; the sixth swatch opens a grid of more.
inline constexpr uint32_t kInkColors[] = {0x000000, 0x0A84FF, 0x30D158, 0xFFD60A, 0xFF453A};
inline constexpr uint32_t kRulerBody = 0x2A2B30;
inline constexpr float kRulerAlpha = 0.84f;
inline constexpr uint32_t kRulerInk = 0xF3F2EF;
inline constexpr uint32_t kPencilWood = 0xE9C793;
inline constexpr uint32_t kNibSteel = 0xA9ADB4;
inline constexpr uint32_t kEraserRubber = 0xF4A3B4;

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

// The markup toolbar and its popovers.
inline constexpr ShadowSpec kToolbarShadow{14.0f, 0.22f, 5.0f};

}  // namespace sstk::config
