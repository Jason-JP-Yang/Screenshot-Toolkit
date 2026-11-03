// CPU pixel work: capture storage, resampling, and procedural UI rasterization.
// All images are 32-bit BGRA, top-down, tightly packed.
#pragma once

#include "platform.h"

#include <memory>
#include <vector>

namespace sstk {

class Image {
 public:
  Image() = default;
  ~Image();
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  Image(Image&& other) noexcept;
  Image& operator=(Image&& other) noexcept;

  // Memory comes straight from the OS so that freeing a frame returns it immediately.
  static Image Allocate(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }
  int stride() const { return width_ * 4; }
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  friend std::shared_ptr<const Image> CaptureScreen(const RECT& bounds);
  void Reset();

  int width_ = 0;
  int height_ = 0;
  uint8_t* data_ = nullptr;
  HBITMAP dib_ = nullptr;
};

// Copies a rectangle of the virtual screen. Windows marked as excluded from capture are not included.
std::shared_ptr<const Image> CaptureScreen(const RECT& bounds);

// Fast 2x box reduction.
Image Halve(const Image& source);

// Exact copy of a rectangle, keeping alpha.
Image CopyRect(const Image& source, int left, int top, int width, int height);

// Area-weighted reduction of a source rectangle. The rectangle may have fractional edges.
Image ResampleArea(const Image& source, double left, double top, double right, double bottom, int width, int height);

// Tinted, heavily blurred copy of a screen rectangle, used as frosted glass.
Image RenderFrost(const Image& frame, const RECT& rect, int downscale, uint32_t tint_bgr, float tint_alpha);

struct ShadowLayer {
  float sigma;
  float alpha;
  float offset_y;
};

// Soft shadow of a rounded rectangle, premultiplied black. All lengths are in output pixels.
Image RenderShadow(int box_width, int box_height, float radius, float pad, const ShadowLayer* layers, int layer_count);

enum class GlyphStyle {
  kWholeScreen,     // A solid slab: everything, taskbar included.
  kWithoutTaskbar,  // The screen filled, with the taskbar band left faint.
  kWindow,          // One window, recognizable by its title bar seam.
  kZone,            // One cell of a snap layout, drawn among its faint neighbors.
};

enum class GlyphBand { kNone, kLeft, kTop, kRight, kBottom };

struct GlyphBox {
  float left, top, right, bottom;  // Normalized to the part of the monitor outside the taskbar.
};

struct GlyphSpec {
  GlyphStyle style;
  GlyphBand band;       // The monitor edge holding the taskbar.
  float screen_aspect;  // Width over height of the monitor.
  GlyphBox box;
  std::vector<GlyphBox> neighbors;
};

// One mini-screen glyph per option, laid out left to right at a fixed pitch.
Image RenderGlyphStrip(const std::vector<GlyphSpec>& glyphs, int pitch, int height, float scale, uint32_t color_bgr,
                       float color_alpha, float faint_alpha);

}  // namespace sstk
