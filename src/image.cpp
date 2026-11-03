#include "image.h"

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sstk {

namespace {

float RoundedBoxDistance(float px, float py, float cx, float cy, float half_w, float half_h, float radius) {
  float qx = std::fabs(px - cx) - half_w + radius;
  float qy = std::fabs(py - cy) - half_h + radius;
  float ox = std::max(qx, 0.0f);
  float oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

float Coverage(float distance) { return std::clamp(0.5f - distance, 0.0f, 1.0f); }

uint8_t ToByte(float value) { return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f)); }

// Separable two-pass box blur over the color channels, clamped at the edges.
void BoxBlur(Image& image, int radius) {
  const int width = image.width();
  const int height = image.height();
  const int window = radius * 2 + 1;
  std::vector<uint8_t> line(static_cast<size_t>(std::max(width, height)) * 4);

  auto blur_line = [&](uint8_t* start, int count, int step) {
    for (int i = 0; i < count; ++i) std::copy_n(start + static_cast<ptrdiff_t>(i) * step, 4, line.begin() + i * 4);
    int sum[3] = {0, 0, 0};
    for (int i = -radius; i <= radius; ++i) {
      const uint8_t* p = line.data() + std::clamp(i, 0, count - 1) * 4;
      for (int c = 0; c < 3; ++c) sum[c] += p[c];
    }
    for (int i = 0; i < count; ++i) {
      uint8_t* out = start + static_cast<ptrdiff_t>(i) * step;
      for (int c = 0; c < 3; ++c) out[c] = static_cast<uint8_t>(sum[c] / window);
      const uint8_t* add = line.data() + std::min(i + radius + 1, count - 1) * 4;
      const uint8_t* remove = line.data() + std::max(i - radius, 0) * 4;
      for (int c = 0; c < 3; ++c) sum[c] += add[c] - remove[c];
    }
  };

  for (int y = 0; y < height; ++y) blur_line(image.data() + static_cast<ptrdiff_t>(y) * image.stride(), width, 4);
  for (int x = 0; x < width; ++x) blur_line(image.data() + static_cast<ptrdiff_t>(x) * 4, height, image.stride());
}

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
  const float abx = bx - ax;
  const float aby = by - ay;
  const float t = std::clamp(((px - ax) * abx + (py - ay) * aby) / std::max(abx * abx + aby * aby, 1e-6f), 0.0f, 1.0f);
  const float dx = px - (ax + abx * t);
  const float dy = py - (ay + aby * t);
  return std::sqrt(dx * dx + dy * dy);
}

// A white shape on clear pixels. The callback gets icon units and returns coverage, given how many pixels one unit is.
template <typename Shape>
Image RenderIcon(int size, float scale, Shape shape) {
  Image image = Image::Allocate(size, size);
  if (!image) return image;
  uint8_t* out = image.data();
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x, out += 4) {
      const uint8_t value = ToByte(shape((x + 0.5f) / scale, (y + 0.5f) / scale) * 255.0f);
      out[0] = out[1] = out[2] = out[3] = value;
    }
  }
  return image;
}

}  // namespace

Image::~Image() { Reset(); }

Image::Image(Image&& other) noexcept { *this = std::move(other); }

Image& Image::operator=(Image&& other) noexcept {
  if (this != &other) {
    Reset();
    width_ = std::exchange(other.width_, 0);
    height_ = std::exchange(other.height_, 0);
    data_ = std::exchange(other.data_, nullptr);
    dib_ = std::exchange(other.dib_, nullptr);
  }
  return *this;
}

void Image::Reset() {
  if (dib_ != nullptr) {
    DeleteObject(dib_);
  } else if (data_ != nullptr) {
    VirtualFree(data_, 0, MEM_RELEASE);
  }
  dib_ = nullptr;
  data_ = nullptr;
  width_ = height_ = 0;
}

Image Image::Allocate(int width, int height) {
  Image image;
  void* memory = VirtualAlloc(nullptr, static_cast<size_t>(width) * height * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (memory == nullptr) return image;
  image.width_ = width;
  image.height_ = height;
  image.data_ = static_cast<uint8_t*>(memory);
  return image;
}

std::shared_ptr<const Image> CaptureScreen(const RECT& bounds) {
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  HDC screen = GetDC(nullptr);
  HDC memory_dc = CreateCompatibleDC(screen);
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(memory_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  std::shared_ptr<Image> image;
  if (dib != nullptr) {
    HGDIOBJ previous = SelectObject(memory_dc, dib);
    BOOL copied = BitBlt(memory_dc, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);
    SelectObject(memory_dc, previous);
    if (copied) {
      image = std::make_shared<Image>();
      image->width_ = width;
      image->height_ = height;
      image->data_ = static_cast<uint8_t*>(bits);
      image->dib_ = dib;
    } else {
      DeleteObject(dib);
    }
  }
  DeleteDC(memory_dc);
  ReleaseDC(nullptr, screen);
  return image;
}

Image Halve(const Image& source) {
  const int width = std::max(source.width() / 2, 1);
  const int height = std::max(source.height() / 2, 1);
  Image result = Image::Allocate(width, height);
  if (!result) return result;

  for (int y = 0; y < height; ++y) {
    const uint8_t* row0 = source.data() + static_cast<ptrdiff_t>(y) * 2 * source.stride();
    const uint8_t* row1 = row0 + (source.height() > 1 ? source.stride() : 0);
    uint8_t* out = result.data() + static_cast<ptrdiff_t>(y) * result.stride();
    int x = 0;
    for (; x + 4 <= width; x += 4) {
      __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row0 + static_cast<ptrdiff_t>(x) * 8));
      __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row0 + static_cast<ptrdiff_t>(x) * 8 + 16));
      __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row1 + static_cast<ptrdiff_t>(x) * 8));
      __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row1 + static_cast<ptrdiff_t>(x) * 8 + 16));
      __m128i v0 = _mm_avg_epu8(a0, b0);
      __m128i v1 = _mm_avg_epu8(a1, b1);
      __m128 even = _mm_shuffle_ps(_mm_castsi128_ps(v0), _mm_castsi128_ps(v1), _MM_SHUFFLE(2, 0, 2, 0));
      __m128 odd = _mm_shuffle_ps(_mm_castsi128_ps(v0), _mm_castsi128_ps(v1), _MM_SHUFFLE(3, 1, 3, 1));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + static_cast<ptrdiff_t>(x) * 4),
                       _mm_avg_epu8(_mm_castps_si128(even), _mm_castps_si128(odd)));
    }
    for (; x < width; ++x) {
      const uint8_t* p0 = row0 + static_cast<ptrdiff_t>(x) * 8;
      const uint8_t* p1 = row1 + static_cast<ptrdiff_t>(x) * 8;
      const bool has_right = x * 2 + 1 < source.width();
      for (int c = 0; c < 3; ++c) {
        int r0 = has_right ? p0[c + 4] : p0[c];
        int r1 = has_right ? p1[c + 4] : p1[c];
        out[static_cast<ptrdiff_t>(x) * 4 + c] = static_cast<uint8_t>((p0[c] + p1[c] + r0 + r1 + 2) / 4);
      }
      out[static_cast<ptrdiff_t>(x) * 4 + 3] = 255;
    }
  }
  return result;
}

Image CopyRect(const Image& source, int left, int top, int width, int height) {
  Image result = Image::Allocate(width, height);
  if (!result) return result;
  for (int y = 0; y < height; ++y) {
    std::copy_n(source.data() + static_cast<ptrdiff_t>(top + y) * source.stride() + static_cast<ptrdiff_t>(left) * 4,
                result.stride(), result.data() + static_cast<ptrdiff_t>(y) * result.stride());
  }
  return result;
}

Image ResampleArea(const Image& source, double left, double top, double right, double bottom, int width, int height) {
  Image result = Image::Allocate(width, height);
  if (!result) return result;

  left = std::clamp(left, 0.0, static_cast<double>(source.width()));
  right = std::clamp(right, left, static_cast<double>(source.width()));
  top = std::clamp(top, 0.0, static_cast<double>(source.height()));
  bottom = std::clamp(bottom, top, static_cast<double>(source.height()));

  struct Span {
    int first;
    int last;
    float first_weight;
    float last_weight;
  };

  auto make_span = [](double start, double end, int limit) {
    Span span{};
    span.first = std::clamp(static_cast<int>(std::floor(start)), 0, limit - 1);
    span.last = std::clamp(static_cast<int>(std::ceil(end)) - 1, span.first, limit - 1);
    if (span.first == span.last) {
      span.first_weight = static_cast<float>(std::max(end - start, 1e-4));
      span.last_weight = 0.0f;
    } else {
      span.first_weight = static_cast<float>(span.first + 1 - start);
      span.last_weight = static_cast<float>(end - span.last);
    }
    return span;
  };

  const double step_x = (right - left) / width;
  const double step_y = (bottom - top) / height;
  std::vector<Span> columns(width);
  std::vector<float> column_norm(width);
  for (int x = 0; x < width; ++x) {
    columns[x] = make_span(left + x * step_x, left + (x + 1) * step_x, source.width());
    const Span& span = columns[x];
    column_norm[x] = span.first == span.last ? span.first_weight
                                             : span.first_weight + span.last_weight + (span.last - span.first - 1);
  }

  std::vector<float> accumulator(static_cast<size_t>(width) * 4);
  const __m128i zero = _mm_setzero_si128();

  for (int y = 0; y < height; ++y) {
    const Span rows = make_span(top + y * step_y, top + (y + 1) * step_y, source.height());
    std::fill(accumulator.begin(), accumulator.end(), 0.0f);
    float row_norm = 0.0f;

    for (int sy = rows.first; sy <= rows.last; ++sy) {
      float row_weight = 1.0f;
      if (sy == rows.first) row_weight = rows.first_weight;
      else if (sy == rows.last) row_weight = rows.last_weight;
      if (row_weight <= 0.0f) continue;
      row_norm += row_weight;

      const uint32_t* pixels = reinterpret_cast<const uint32_t*>(source.data() + static_cast<ptrdiff_t>(sy) * source.stride());
      float* acc = accumulator.data();
      for (int x = 0; x < width; ++x, acc += 4) {
        const Span& span = columns[x];
        __m128 sum = _mm_setzero_ps();
        for (int sx = span.first; sx <= span.last; ++sx) {
          float weight = 1.0f;
          if (sx == span.first) weight = span.first_weight;
          else if (sx == span.last) weight = span.last_weight;
          __m128i bytes = _mm_cvtsi32_si128(static_cast<int>(pixels[sx]));
          __m128i words = _mm_unpacklo_epi8(bytes, zero);
          __m128 values = _mm_cvtepi32_ps(_mm_unpacklo_epi16(words, zero));
          sum = _mm_add_ps(sum, _mm_mul_ps(values, _mm_set1_ps(weight)));
        }
        _mm_storeu_ps(acc, _mm_add_ps(_mm_loadu_ps(acc), _mm_mul_ps(sum, _mm_set1_ps(row_weight))));
      }
    }

    uint8_t* out = result.data() + static_cast<ptrdiff_t>(y) * result.stride();
    const float* acc = accumulator.data();
    for (int x = 0; x < width; ++x, acc += 4, out += 4) {
      const float norm = 1.0f / (column_norm[x] * row_norm);
      out[0] = ToByte(acc[0] * norm);
      out[1] = ToByte(acc[1] * norm);
      out[2] = ToByte(acc[2] * norm);
      out[3] = 255;
    }
  }
  return result;
}

Image RenderFrost(const Image& frame, const RECT& rect, int downscale, uint32_t tint_bgr, float tint_alpha) {
  const int width = std::max<int>((rect.right - rect.left) / downscale, 1);
  const int height = std::max<int>((rect.bottom - rect.top) / downscale, 1);
  Image frost = ResampleArea(frame, rect.left, rect.top, rect.right, rect.bottom, width, height);
  if (!frost) return frost;
  BoxBlur(frost, 2);
  BoxBlur(frost, 2);

  const float tint[3] = {static_cast<float>(tint_bgr & 0xFF), static_cast<float>((tint_bgr >> 8) & 0xFF),
                         static_cast<float>((tint_bgr >> 16) & 0xFF)};
  uint8_t* p = frost.data();
  for (int i = 0; i < width * height; ++i, p += 4) {
    for (int c = 0; c < 3; ++c) p[c] = ToByte(p[c] * (1.0f - tint_alpha) + tint[c] * tint_alpha);
    p[3] = 255;
  }
  return frost;
}

Image RenderShadow(int box_width, int box_height, float radius, float pad, const ShadowLayer* layers, int layer_count) {
  const int width = box_width + static_cast<int>(std::ceil(pad)) * 2;
  const int height = box_height + static_cast<int>(std::ceil(pad)) * 2;
  Image shadow = Image::Allocate(width, height);
  if (!shadow) return shadow;

  const float cx = width * 0.5f;
  const float half_w = box_width * 0.5f;
  const float half_h = box_height * 0.5f;
  uint8_t* out = shadow.data();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x, out += 4) {
      float transparency = 1.0f;
      for (int i = 0; i < layer_count; ++i) {
        const float cy = height * 0.5f + layers[i].offset_y;
        const float distance = RoundedBoxDistance(x + 0.5f, y + 0.5f, cx, cy, half_w, half_h, radius);
        const float density = 0.5f * static_cast<float>(std::erfc(distance / (layers[i].sigma * 1.41421356f)));
        transparency *= 1.0f - layers[i].alpha * density;
      }
      out[0] = out[1] = out[2] = 0;
      out[3] = ToByte((1.0f - transparency) * 255.0f);
    }
  }
  return shadow;
}

Image RenderGlyphStrip(const std::vector<GlyphSpec>& glyphs, int pitch, int height, float scale, uint32_t color_bgr,
                       float color_alpha) {
  const int width = std::max<int>(pitch * static_cast<int>(glyphs.size()), 1);
  Image strip = Image::Allocate(width, height);
  if (!strip) return strip;

  const float color[3] = {static_cast<float>(color_bgr & 0xFF), static_cast<float>((color_bgr >> 8) & 0xFF),
                          static_cast<float>((color_bgr >> 16) & 0xFF)};
  const float stroke = 1.1f * scale;
  const float corner = 2.8f * scale;
  const float gutter = 2.1f * scale;
  const float fill_corner = 1.0f * scale;
  const float minimum_fill = 2.2f * scale;
  const float taskbar_gap = std::round(2.6f * scale);

  for (size_t index = 0; index < glyphs.size(); ++index) {
    const GlyphSpec& glyph = glyphs[index];
    float screen_w = 20.0f * scale;
    float screen_h = screen_w / glyph.screen_aspect;
    if (screen_h > 15.0f * scale) {
      screen_h = 15.0f * scale;
      screen_w = screen_h * glyph.screen_aspect;
    }

    // Snap the outline to whole pixels so that the strokes stay crisp at any display scale.
    const float origin_x = std::round(pitch * static_cast<float>(index) + (pitch - screen_w) * 0.5f);
    const float origin_y = std::round((height - screen_h) * 0.5f);
    screen_w = std::round(screen_w);
    screen_h = std::round(screen_h);
    const float cx = origin_x + screen_w * 0.5f;
    const float cy = origin_y + screen_h * 0.5f;

    const float inner_x = origin_x + gutter;
    const float inner_y = origin_y + gutter;
    const float inner_w = screen_w - gutter * 2.0f;
    const float inner_h = screen_h - gutter * 2.0f;
    float fill_l = std::round(inner_x + glyph.left * inner_w);
    float fill_r = std::round(inner_x + glyph.right * inner_w);
    float fill_t = std::round(inner_y + glyph.top * inner_h);
    float fill_b = std::round(inner_y + glyph.bottom * inner_h);
    // At this size the real taskbar is a sliver, so its absence is drawn as a clear gap along its edge.
    switch (glyph.gap) {
      case GlyphGap::kLeft: fill_l = std::round(inner_x) + taskbar_gap; break;
      case GlyphGap::kTop: fill_t = std::round(inner_y) + taskbar_gap; break;
      case GlyphGap::kRight: fill_r = std::round(inner_x + inner_w) - taskbar_gap; break;
      case GlyphGap::kBottom: fill_b = std::round(inner_y + inner_h) - taskbar_gap; break;
      case GlyphGap::kNone: break;
    }
    if (fill_r - fill_l < minimum_fill) fill_r = fill_l + minimum_fill;
    if (fill_b - fill_t < minimum_fill) fill_b = fill_t + minimum_fill;
    const float fill_cx = (fill_l + fill_r) * 0.5f;
    const float fill_cy = (fill_t + fill_b) * 0.5f;
    const float fill_hw = (fill_r - fill_l) * 0.5f;
    const float fill_hh = (fill_b - fill_t) * 0.5f;

    const int start_x = pitch * static_cast<int>(index);
    for (int y = 0; y < height; ++y) {
      uint8_t* out = strip.data() + static_cast<ptrdiff_t>(y) * strip.stride() + static_cast<ptrdiff_t>(start_x) * 4;
      for (int x = start_x; x < start_x + pitch; ++x, out += 4) {
        const float px = x + 0.5f;
        const float py = y + 0.5f;
        const float box = RoundedBoxDistance(px, py, cx, cy, screen_w * 0.5f, screen_h * 0.5f, corner);
        const float outline = Coverage(std::fabs(box + stroke * 0.5f) - stroke * 0.5f);
        const float fill = Coverage(RoundedBoxDistance(px, py, fill_cx, fill_cy, fill_hw, fill_hh, fill_corner));
        const float alpha = std::max(outline, fill) * color_alpha;
        for (int c = 0; c < 3; ++c) out[c] = ToByte(color[c] * alpha);
        out[3] = ToByte(alpha * 255.0f);
      }
    }
  }
  return strip;
}

Image RenderSprite(const Image& thumbnail, float radius, float border, int margin) {
  const int width = thumbnail.width() + margin * 2;
  const int height = thumbnail.height() + margin * 2;
  Image sprite = Image::Allocate(width, height);
  if (!sprite) return sprite;

  const float cx = width * 0.5f;
  const float cy = height * 0.5f;
  const float half_w = thumbnail.width() * 0.5f;
  const float half_h = thumbnail.height() * 0.5f;
  uint8_t* out = sprite.data();
  for (int y = 0; y < height; ++y) {
    const int source_y = std::clamp(y - margin, 0, thumbnail.height() - 1);
    const uint8_t* row = thumbnail.data() + static_cast<ptrdiff_t>(source_y) * thumbnail.stride();
    for (int x = 0; x < width; ++x, out += 4) {
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float outer = Coverage(RoundedBoxDistance(px, py, cx, cy, half_w, half_h, radius));
      if (outer <= 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 0;
        continue;
      }
      const float inner = Coverage(RoundedBoxDistance(px, py, cx, cy, half_w - border, half_h - border, std::max(radius - border, 0.0f)));
      const uint8_t* source = row + static_cast<ptrdiff_t>(std::clamp(x - margin, 0, thumbnail.width() - 1)) * 4;
      for (int c = 0; c < 3; ++c) out[c] = ToByte((255.0f + (source[c] - 255.0f) * inner) * outer);
      out[3] = ToByte(outer * 255.0f);
    }
  }
  return sprite;
}

Image RenderPlusBadge(int size, int pad, float scale, uint32_t color) {
  const int canvas = size + pad * 2;
  Image badge = Image::Allocate(canvas, canvas);
  if (!badge) return badge;

  const float center = canvas * 0.5f;
  const float radius = size * 0.5f;
  const float ring = 1.5f * scale;
  const float arm = radius * 0.46f;
  const float stroke = 1.05f * scale;
  const float sigma = 2.6f * scale;
  const float shadow_offset = 1.4f * scale;
  const float fill[3] = {static_cast<float>(color & 0xFF), static_cast<float>((color >> 8) & 0xFF), static_cast<float>((color >> 16) & 0xFF)};
  uint8_t* out = badge.data();
  for (int y = 0; y < canvas; ++y) {
    for (int x = 0; x < canvas; ++x, out += 4) {
      const float px = x + 0.5f - center;
      const float py = y + 0.5f - center;
      const float distance = std::sqrt(px * px + py * py);
      const float shadow_distance = std::sqrt(px * px + (py - shadow_offset) * (py - shadow_offset));
      const float disc = Coverage(distance - radius);
      const float core = Coverage(distance - (radius - ring));
      const float shadow = 0.32f * 0.5f * static_cast<float>(std::erfc((shadow_distance - radius) / (sigma * 1.41421356f)));
      const float plus = std::max(Coverage(SegmentDistance(px, py, -arm, 0.0f, arm, 0.0f) - stroke),
                                  Coverage(SegmentDistance(px, py, 0.0f, -arm, 0.0f, arm) - stroke));
      const float alpha = disc + shadow * (1.0f - disc);
      for (int c = 0; c < 3; ++c) {
        const float face = 255.0f + (fill[c] - 255.0f) * core * (1.0f - plus);
        out[c] = ToByte(face * disc);
      }
      out[3] = ToByte(alpha * 255.0f);
    }
  }
  return badge;
}

Image RenderBinCan(int size, float scale) {
  return RenderIcon(size, scale, [scale](float x, float y) {
    // A solid body narrowing toward its base, with two slots cut out.
    const float taper = 1.0f + 0.17f * std::clamp((y - 9.5f) / 11.8f, 0.0f, 1.0f);
    const float body = RoundedBoxDistance(12.0f + (x - 12.0f) * taper, y, 12.0f, 15.4f, 6.3f, 5.9f, 1.8f);
    const float slots = std::min(SegmentDistance(x, y, 9.5f, 12.6f, 9.8f, 18.3f), SegmentDistance(x, y, 14.5f, 12.6f, 14.2f, 18.3f)) - 0.8f;
    return Coverage(body * scale) * (1.0f - Coverage(slots * scale));
  });
}

Image RenderBinLid(int size, float scale) {
  return RenderIcon(size, scale, [scale](float x, float y) {
    const float bar = RoundedBoxDistance(x, y, 12.0f, 7.2f, 8.1f, 1.3f, 1.3f);
    const float grip = RoundedBoxDistance(x, y, 12.0f, 4.8f, 3.0f, 1.8f, 1.1f);
    return Coverage(std::min(bar, grip) * scale);
  });
}

Image RenderLabel(const wchar_t* text, int font_pixels) {
  // Drawn large and reduced, which gives even, unhinted edges at any display scale.
  constexpr int kOversample = 4;
  Image label;
  HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) return label;
  HFONT font = CreateFontW(-font_pixels * kOversample, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                           L"Segoe UI Semibold");
  HGDIOBJ previous_font = SelectObject(dc, font);
  const int length = lstrlenW(text);
  SIZE extent{};
  GetTextExtentPoint32W(dc, text, length, &extent);
  const int width = std::max<int>((extent.cx + kOversample - 1) / kOversample + 2, 1);
  const int height = std::max<int>((extent.cy + kOversample - 1) / kOversample, 1);

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width * kOversample;
  info.bmiHeader.biHeight = -height * kOversample;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib != nullptr) {
    HGDIOBJ previous_bitmap = SelectObject(dc, dib);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, kOversample, 0, text, length);
    GdiFlush();

    label = Image::Allocate(width, height);
    if (label) {
      const auto* source = static_cast<const uint8_t*>(bits);
      const int source_stride = width * kOversample * 4;
      uint8_t* out = label.data();
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x, out += 4) {
          int sum = 0;
          for (int sy = 0; sy < kOversample; ++sy) {
            const uint8_t* row = source + static_cast<ptrdiff_t>(y * kOversample + sy) * source_stride + static_cast<ptrdiff_t>(x * kOversample) * 4;
            for (int sx = 0; sx < kOversample; ++sx) sum += row[sx * 4];
          }
          // Light text on a saturated body reads thin without a touch of weight.
          const float coverage = std::pow(sum / (255.0f * kOversample * kOversample), 0.86f);
          out[0] = out[1] = out[2] = out[3] = ToByte(coverage * 255.0f);
        }
      }
    }
    SelectObject(dc, previous_bitmap);
    DeleteObject(dib);
  }
  SelectObject(dc, previous_font);
  DeleteObject(font);
  DeleteDC(dc);
  return label;
}

}  // namespace sstk
