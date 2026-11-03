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
                       float color_alpha, float faint_alpha) {
  const int width = std::max<int>(pitch * static_cast<int>(glyphs.size()), 1);
  Image strip = Image::Allocate(width, height);
  if (!strip) return strip;

  const float color[3] = {static_cast<float>(color_bgr & 0xFF), static_cast<float>((color_bgr >> 8) & 0xFF),
                          static_cast<float>((color_bgr >> 16) & 0xFF)};

  // Every length lands on whole pixels, so that strokes and seams stay crisp at any display scale.
  auto pixels = [scale](float dips, float minimum) { return std::max(std::round(dips * scale), minimum); };
  const float stroke = pixels(1.0f, 1.0f);
  const float gutter = pixels(1.0f, 1.0f);
  const float band_thickness = pixels(2.0f, 2.0f);
  const float seam = pixels(0.75f, 1.0f);
  const float corner = 3.0f * scale;
  const float fill_corner = 1.1f * scale;

  struct Shape {
    float left, top, right, bottom, radius, alpha;
  };

  for (size_t index = 0; index < glyphs.size(); ++index) {
    const GlyphSpec& glyph = glyphs[index];
    float screen_w = 22.0f * scale;
    float screen_h = screen_w / glyph.screen_aspect;
    if (screen_h > 16.0f * scale) {
      screen_h = 16.0f * scale;
      screen_w = screen_h * glyph.screen_aspect;
    }
    screen_w = std::round(screen_w);
    screen_h = std::round(screen_h);
    const float origin_x = std::round(pitch * static_cast<float>(index) + (pitch - screen_w) * 0.5f);
    const float origin_y = std::round((height - screen_h) * 0.5f);
    const float screen_r = origin_x + screen_w;
    const float screen_b = origin_y + screen_h;

    // The usable area and the taskbar band inside the outline.
    const float inset = stroke + gutter;
    float area_l = origin_x + inset;
    float area_t = origin_y + inset;
    float area_r = screen_r - inset;
    float area_b = screen_b - inset;
    Shape band{0.0f, 0.0f, 0.0f, 0.0f, fill_corner * 0.6f, 0.0f};
    switch (glyph.band) {
      case GlyphBand::kLeft: band = {area_l, area_t, area_l + band_thickness, area_b, band.radius, 1.0f}; area_l = band.right + seam; break;
      case GlyphBand::kTop: band = {area_l, area_t, area_r, area_t + band_thickness, band.radius, 1.0f}; area_t = band.bottom + seam; break;
      case GlyphBand::kRight: band = {area_r - band_thickness, area_t, area_r, area_b, band.radius, 1.0f}; area_r = band.left - seam; break;
      case GlyphBand::kBottom: band = {area_l, area_b - band_thickness, area_r, area_b, band.radius, 1.0f}; area_b = band.top - seam; break;
      case GlyphBand::kNone: break;
    }

    auto map_box = [&](const GlyphBox& box, float shrink) {
      Shape shape{};
      shape.left = std::round(area_l + box.left * (area_r - area_l));
      shape.top = std::round(area_t + box.top * (area_b - area_t));
      shape.right = std::round(area_l + box.right * (area_r - area_l));
      shape.bottom = std::round(area_t + box.bottom * (area_b - area_t));
      if (box.left > 0.001f) shape.left += shrink;
      if (box.top > 0.001f) shape.top += shrink;
      if (box.right < 0.999f) shape.right -= shrink;
      if (box.bottom < 0.999f) shape.bottom -= shrink;
      shape.right = std::max(shape.right, shape.left + stroke * 2.0f);
      shape.bottom = std::max(shape.bottom, shape.top + stroke * 2.0f);
      shape.radius = fill_corner;
      shape.alpha = 1.0f;
      return shape;
    };

    std::vector<Shape> shapes;
    std::vector<Shape> cuts;
    const bool outlined = glyph.style != GlyphStyle::kWholeScreen;
    if (!outlined) {
      shapes.push_back({origin_x, origin_y, screen_r, screen_b, corner, 1.0f});
      // A seam marks the taskbar's edge. It runs from the outline to the outline.
      switch (glyph.band) {
        case GlyphBand::kLeft: cuts.push_back({band.right, origin_y, band.right + seam, screen_b, 0.0f, 1.0f}); break;
        case GlyphBand::kTop: cuts.push_back({origin_x, band.bottom, screen_r, band.bottom + seam, 0.0f, 1.0f}); break;
        case GlyphBand::kRight: cuts.push_back({band.left - seam, origin_y, band.left, screen_b, 0.0f, 1.0f}); break;
        case GlyphBand::kBottom: cuts.push_back({origin_x, band.top - seam, screen_r, band.top, 0.0f, 1.0f}); break;
        case GlyphBand::kNone: break;
      }
    } else {
      if (glyph.band != GlyphBand::kNone) {
        band.alpha = faint_alpha;
        shapes.push_back(band);
      }
      for (const GlyphBox& neighbor : glyph.neighbors) {
        Shape shape = map_box(neighbor, seam * 0.5f);
        shape.alpha = faint_alpha;
        shapes.push_back(shape);
      }
      Shape main_shape = map_box(glyph.box, glyph.style == GlyphStyle::kZone ? seam * 0.5f : 0.0f);
      shapes.push_back(main_shape);
      // A title bar seam, when the window is tall enough to carry one.
      if (glyph.style == GlyphStyle::kWindow && main_shape.bottom - main_shape.top >= seam + pixels(4.0f, 4.0f)) {
        const float seam_top = main_shape.top + pixels(1.75f, 2.0f);
        cuts.push_back({main_shape.left, seam_top, main_shape.right, seam_top + seam, 0.0f, 1.0f});
      }
    }

    const float cx = origin_x + screen_w * 0.5f;
    const float cy = origin_y + screen_h * 0.5f;
    const int start_x = pitch * static_cast<int>(index);
    for (int y = 0; y < height; ++y) {
      uint8_t* out = strip.data() + static_cast<ptrdiff_t>(y) * strip.stride() + static_cast<ptrdiff_t>(start_x) * 4;
      for (int x = start_x; x < start_x + pitch; ++x, out += 4) {
        const float px = x + 0.5f;
        const float py = y + 0.5f;
        float coverage = 0.0f;
        if (outlined) {
          const float box = RoundedBoxDistance(px, py, cx, cy, screen_w * 0.5f, screen_h * 0.5f, corner);
          coverage = Coverage(std::fabs(box + stroke * 0.5f) - stroke * 0.5f);
        }
        auto distance_to = [px, py](const Shape& shape) {
          return RoundedBoxDistance(px, py, (shape.left + shape.right) * 0.5f, (shape.top + shape.bottom) * 0.5f,
                                    (shape.right - shape.left) * 0.5f, (shape.bottom - shape.top) * 0.5f, shape.radius);
        };
        for (const Shape& shape : shapes) coverage = std::max(coverage, Coverage(distance_to(shape)) * shape.alpha);
        for (const Shape& cut : cuts) coverage *= 1.0f - Coverage(distance_to(cut));
        const float alpha = coverage * color_alpha;
        for (int c = 0; c < 3; ++c) out[c] = ToByte(color[c] * alpha);
        out[3] = ToByte(alpha * 255.0f);
      }
    }
  }
  return strip;
}

}  // namespace sstk
