// Default build settings: the markup tools. Widths are device independent pixels of the screenshot as the screen showed
// it, five to a tool, and opacities run from 0 to 1.
#pragma once

#include <cstdint>

namespace sstk::config {

struct InkDefaults {
  float widths[5];
  int width;  // The one chosen at first.
  uint32_t color;  // 0xRRGGBB
  float opacity;
};

// In the toolbar's order: ink pen, monoline, highlighter, pencil, crayon, ink brush.
inline constexpr InkDefaults kInkDefaults[] = {
    {{1.2f, 2.0f, 3.2f, 5.0f, 8.0f}, 1, 0xFF453A, 1.0f},
    {{1.0f, 2.0f, 3.5f, 6.0f, 10.0f}, 1, 0x0A84FF, 1.0f},
    {{8.0f, 13.0f, 19.0f, 26.0f, 36.0f}, 2, 0xFFD60A, 0.80f},
    {{1.2f, 2.2f, 4.0f, 7.0f, 12.0f}, 1, 0x000000, 1.0f},
    {{4.0f, 7.0f, 11.0f, 16.0f, 24.0f}, 1, 0xFF453A, 1.0f},
    {{4.0f, 8.0f, 14.0f, 22.0f, 34.0f}, 1, 0x0A84FF, 0.85f},
};
inline constexpr float kEraserWidths[] = {6.0f, 12.0f, 20.0f, 32.0f, 50.0f};
inline constexpr int kEraserWidth = 2;

// How each tool lays down its marks.
struct BrushTuning {
  float spacing;         // Between stamps, as a share of their diameter.
  float pressure_floor;  // The share of the width left at the lightest touch; 1 ignores pressure.
  float flow;            // How much one stamp lays down, from 0 to 1.
  float grain;           // The paper's grain, in pixels at 100%, or 0 for none.
  float grain_bite;      // How much the grain decides: 0 not at all, 1 only its peaks take color.
  float taper;           // The length of the narrowing ends, in widths, or 0 for none.
};

inline constexpr BrushTuning kBrushTuning[] = {
    {0.08f, 0.30f, 1.00f, 0.0f, 0.00f, 1.6f},  // Ink pen: pressure and speed, with a slanted nib.
    {0.10f, 1.00f, 1.00f, 0.0f, 0.00f, 0.0f},  // Monoline: one width throughout.
    {0.07f, 1.00f, 1.00f, 0.0f, 0.00f, 0.0f},  // Highlighter: a chisel, multiplied into the picture.
    {0.16f, 0.55f, 0.55f, 1.4f, 0.70f, 0.0f},  // Pencil: graphite on grain, building up where it passes again.
    {0.18f, 0.70f, 0.85f, 2.6f, 0.85f, 0.0f},  // Crayon: wax that skips the grain's hollows.
    {0.06f, 0.18f, 0.28f, 1.8f, 0.22f, 1.2f},  // Ink brush: wet, soft, and pooling at its edges.
};
inline constexpr BrushTuning kEraserTuning{0.10f, 1.00f, 1.00f, 0.0f, 0.00f, 0.0f};

inline constexpr float kNibAspect = 0.72f;
inline constexpr float kNibAngleDegrees = -38.0f;
inline constexpr float kChiselAspect = 0.30f;
inline constexpr float kChiselAngleDegrees = 78.0f;
inline constexpr float kBrushRim = 0.30f;  // How much darker the ink brush's edges dry.

// A mouse has no pressure: it presses this hard, and the ink pen and the brush thin with its speed instead.
inline constexpr float kMousePressure = 0.62f;
inline constexpr float kMouseThinning = 0.00045f;  // Per pixel a second.
inline constexpr float kMouseThinnest = 0.55f;

// Each new point is moved this share of the way from the last one, which smooths the hand's tremor. 1 keeps it as is.
inline constexpr float kSmoothingMouse = 0.55f;
inline constexpr float kSmoothingPen = 0.82f;

// The lasso takes a stroke when at least this share of its points lie inside the loop.
inline constexpr float kLassoShare = 0.5f;

}  // namespace sstk::config
