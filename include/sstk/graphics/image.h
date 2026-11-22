// CPU pixel work: capture storage, resampling, and procedural UI rasterization.
// All images are 32-bit BGRA, top-down, tightly packed.
#pragma once

#include "sstk/base/platform.h"

#include <memory>
#include <vector>

namespace sstk {

// Rows of pixels that something else owns.
struct PixelView {
  const uint8_t* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
};

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
  PixelView view() const { return PixelView{data_, width_, height_, stride()}; }
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

// Area-weighted reduction of a source rectangle, made a few rows at a time from source rows given as they are needed.
// The rectangle may have fractional edges.
class Resampler {
 public:
  struct Span {
    int first;
    int last;
    float first_weight;
    float last_weight;
  };

  Resampler(int source_width, int source_height, double left, double top, double right, double bottom, int width, int height);

  Span Row(int y) const;  // The source rows output row y is made from.

  // Output rows [first, last) of `out`, from source rows that start with row `source_first` at `source`.
  void Make(int first, int last, const uint8_t* source, int source_first, ptrdiff_t stride, uint8_t* out, ptrdiff_t out_stride) const;

 private:
  int source_height_ = 0;
  double top_ = 0.0;
  double step_y_ = 0.0;
  int width_ = 0;
  std::vector<Span> columns_;
  std::vector<float> column_norm_;
};

// All of it at once.
Image ResampleArea(const PixelView& source, double left, double top, double right, double bottom, int width, int height);
inline Image ResampleArea(const Image& source, double left, double top, double right, double bottom, int width, int height) {
  return ResampleArea(source.view(), left, top, right, bottom, width, height);
}

// Tinted, heavily blurred copy of a screen rectangle, used as frosted glass.
Image RenderFrost(const Image& frame, const RECT& rect, int downscale, uint32_t tint_bgr, float tint_alpha);

struct ShadowLayer {
  float sigma;
  float alpha;
  float offset_y;
};

// Soft shadow of a rounded rectangle, premultiplied black. All lengths are in output pixels.
Image RenderShadow(int box_width, int box_height, float radius, float pad, const ShadowLayer* layers, int layer_count);

// The monitor edge a glyph leaves empty to say that the taskbar there is not included.
enum class GlyphGap { kNone, kLeft, kTop, kRight, kBottom };

struct GlyphSpec {
  float screen_aspect;  // Width over height of the monitor.
  float left, top, right, bottom;  // Region in normalized monitor coordinates.
  GlyphGap gap;
  bool scroll = false;  // A chevron pointing down cut out of the region, for scrolling screenshots.
};

// One option's glyph, centered on a cell. Color is 0xRRGGBB.
Image RenderGlyph(const GlyphSpec& glyph, int width, int height, float scale, uint32_t color, float color_alpha);

// A key's outline around its symbol, `height` pixels tall. Color is 0xRRGGBB.
enum class Keycap { kWindows, kEnter };
Image RenderKeycap(Keycap key, int height, float scale, uint32_t color);

// A check, cross, or exclamation mark on a `size` pixel square. Color is 0xRRGGBB.
enum class StatusIcon { kCheck, kCross, kExclamation };
Image RenderStatusIcon(StatusIcon icon, int size, float scale, uint32_t color);

// A thumbnail with rounded corners and border. The clear margin keeps transformed edges antialiased. Color is 0xRRGGBB.
Image RenderSprite(const Image& thumbnail, float radius, float border, uint32_t border_color, int margin);

// Disc of `size` pixels with a white plus and a soft shadow, on a canvas larger by `pad` per side. Color is 0xRRGGBB.
Image RenderPlusBadge(int size, int pad, float scale, uint32_t color);

// The bin icon in two pieces on one canvas, so that the lid can swing off the can. Drawn on a 24-unit grid. Color is 0xRRGGBB.
Image RenderBinCan(int size, float scale, uint32_t color);
Image RenderBinLid(int size, float scale, uint32_t color);

// One line of text on clear pixels, with what it takes to set it on a baseline. Color is 0xRRGGBB.
struct TextLine {
  Image image;
  float baseline = 0.0f;  // Pixels from the top.
  float cap_height = 0.0f;
  float origin = 0.0f;   // Pixels from the left edge to where the pen starts.
  float advance = 0.0f;  // How far the pen moves, which italics overhang.
};

struct TextStyle {
  const wchar_t* const* faces;  // Tried in order.
  int face_count;
  int weight;
  bool italic;
  float ink_power;  // Below 1 thickens light text on a dark body.
};

// In the selector's label face.
TextLine RenderText(const wchar_t* text, int font_pixels, uint32_t color);
TextLine RenderText(const wchar_t* text, int font_pixels, uint32_t color, const TextStyle& style);
inline Image RenderLabel(const wchar_t* text, int font_pixels, uint32_t color) { return RenderText(text, font_pixels, color).image; }

// The program's icon: three viewfinder brackets on a graphite tile, the fourth corner a screenshot. Each size is
// fitted to whole pixels on its own.
Image RenderLogo(int size);

// The ring of a rounded rectangle, `stroke` pixels wide inside its edge. Color is 0xRRGGBB.
Image RenderRoundedOutline(int width, int height, float radius, float stroke, uint32_t color);

// The box of a bitmap's visible shape, whatever clear space its canvas leaves around it.
RECT InkBounds(const Image& image);

}  // namespace sstk
