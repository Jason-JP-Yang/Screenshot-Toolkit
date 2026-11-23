#include "sstk/editor/art.h"

#include "sstk/config/colors.h"
#include "sstk/config/layout.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace sstk {

namespace {

constexpr float kPi = 3.14159265f;

uint8_t ToByte(float value) { return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f)); }

float Channel(uint32_t color, int index) { return static_cast<float>((color >> (index * 8)) & 0xFF); }

float Segment(float px, float py, float ax, float ay, float bx, float by) {
  const float abx = bx - ax;
  const float aby = by - ay;
  const float t = std::clamp(((px - ax) * abx + (py - ay) * aby) / std::max(abx * abx + aby * aby, 1e-6f), 0.0f, 1.0f);
  return std::hypot(px - (ax + abx * t), py - (ay + aby * t));
}

float Box(float px, float py, float cx, float cy, float half_w, float half_h, float radius) {
  radius = std::min({radius, half_w, half_h});
  const float qx = std::fabs(px - cx) - half_w + radius;
  const float qy = std::fabs(py - cy) - half_h + radius;
  return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
}

// A shape whose half width changes along its height, from `top` to `bottom`.
template <typename Half>
float Taper(float u, float v, float center, float top, float bottom, Half half) {
  return std::max({std::fabs(u - center) - half(std::clamp(v, top, bottom)), top - v, v - bottom});
}

// Round pen strokes on a 20-unit grid: an arc of `radius` about a center, over the angles a test accepts.
template <typename Accept>
float Arc(float x, float y, float cx, float cy, float radius, Accept accept, float ex0, float ey0, float ex1, float ey1) {
  const float angle = std::atan2(y - cy, x - cx) * 180.0f / kPi;
  if (accept(angle)) return std::fabs(std::hypot(x - cx, y - cy) - radius);
  return std::min(std::hypot(x - ex0, y - ey0), std::hypot(x - ex1, y - ey1));
}

// An arrowhead at `tip`, pointing along (dx, dy).
float Head(float x, float y, float tip_x, float tip_y, float dx, float dy, float length) {
  const float norm = std::max(std::hypot(dx, dy), 1e-6f);
  dx /= norm;
  dy /= norm;
  const float c = std::cos(0.72f);
  const float s = std::sin(0.72f);
  const float ax = -(dx * c - dy * s) * length;
  const float ay = -(dx * s + dy * c) * length;
  const float bx = -(dx * c + dy * s) * length;
  const float by = -(-dx * s + dy * c) * length;
  return std::min(Segment(x, y, tip_x, tip_y, tip_x + ax, tip_y + ay), Segment(x, y, tip_x, tip_y, tip_x + bx, tip_y + by));
}

// Premultiplied color in floats, painted one shape over another. Shapes take device independent pixels.
class Painter {
 public:
  Painter(int width, int height, float scale) : width_(width), height_(height), scale_(scale), pixels_(static_cast<size_t>(width) * height * 4, 0.0f) {}

  // `shade` darkens below zero and lightens above it.
  template <typename Shape, typename Shade>
  void Fill(uint32_t color, float alpha, Shape shape, Shade shade) {
    for (int y = 0; y < height_; ++y) {
      const float v = (static_cast<float>(y) + 0.5f) / scale_;
      for (int x = 0; x < width_; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / scale_;
        const float cover = std::clamp(0.5f - shape(u, v) * scale_, 0.0f, 1.0f) * alpha;
        if (cover <= 0.0f) continue;
        const float s = shade(u, v);
        float* p = &pixels_[(static_cast<size_t>(y) * width_ + x) * 4];
        for (int c = 0; c < 3; ++c) {
          float value = Channel(color, c);
          value = s < 0.0f ? value * (1.0f + s) : value + (255.0f - value) * s;
          p[c] = p[c] * (1.0f - cover) + value * cover;
        }
        p[3] = p[3] * (1.0f - cover) + 255.0f * cover;
      }
    }
  }

  template <typename Shape>
  void Fill(uint32_t color, float alpha, Shape shape) {
    Fill(color, alpha, shape, [](float, float) { return 0.0f; });
  }

  void Blend(const Image& part, int at_x, int at_y) {
    for (int y = 0; y < part.height(); ++y) {
      const int to_y = at_y + y;
      if (to_y < 0 || to_y >= height_) continue;
      for (int x = 0; x < part.width(); ++x) {
        const int to_x = at_x + x;
        if (to_x < 0 || to_x >= width_) continue;
        const uint8_t* from = part.data() + static_cast<ptrdiff_t>(y) * part.stride() + static_cast<ptrdiff_t>(x) * 4;
        float* p = &pixels_[(static_cast<size_t>(to_y) * width_ + to_x) * 4];
        const float keep = 1.0f - from[3] / 255.0f;
        for (int c = 0; c < 4; ++c) p[c] = p[c] * keep + from[c];
      }
    }
  }

  Image Finish() const {
    Image image = Image::Allocate(width_, height_);
    if (!image) return image;
    uint8_t* out = image.data();
    for (size_t i = 0; i < pixels_.size(); ++i) out[i] = ToByte(pixels_[i]);
    return image;
  }

 private:
  int width_;
  int height_;
  float scale_;
  std::vector<float> pixels_;
};

float IconDistance(EditorIcon icon, float x, float y) {
  switch (icon) {
    case EditorIcon::kClose:
      return std::min(Segment(x, y, 5.6f, 5.6f, 14.4f, 14.4f), Segment(x, y, 14.4f, 5.6f, 5.6f, 14.4f)) - 0.95f;
    case EditorIcon::kDone:
      return std::min(Segment(x, y, 4.6f, 10.5f, 8.4f, 14.3f), Segment(x, y, 8.4f, 14.3f, 15.6f, 6.0f)) - 1.05f;
    case EditorIcon::kFold:
      return std::min(Segment(x, y, 5.6f, 8.0f, 10.0f, 12.4f), Segment(x, y, 10.0f, 12.4f, 14.4f, 8.0f)) - 0.95f;
    case EditorIcon::kUndo:
    case EditorIcon::kRedo: {
      if (icon == EditorIcon::kRedo) x = 20.0f - x;
      const float cx = 11.6f;
      const float cy = 11.4f;
      const float r = 3.6f;
      const float arc = x >= cx ? std::fabs(std::hypot(x - cx, y - cy) - r) : 1e9f;
      return std::min({arc, Segment(x, y, 4.4f, 7.8f, cx, 7.8f), Segment(x, y, cx, 15.0f, 8.4f, 15.0f), Head(x, y, 4.4f, 7.8f, -1.0f, 0.0f, 4.2f)}) - 0.95f;
    }
    case EditorIcon::kReset: {
      const float cx = 10.0f;
      const float cy = 10.4f;
      const float r = 6.0f;
      const float gap_from = -112.0f;
      const float gap_to = -62.0f;
      const float a0 = gap_from * kPi / 180.0f;
      const float a1 = gap_to * kPi / 180.0f;
      const float x0 = cx + r * std::cos(a0);
      const float y0 = cy + r * std::sin(a0);
      const float x1 = cx + r * std::cos(a1);
      const float y1 = cy + r * std::sin(a1);
      const float arc = Arc(x, y, cx, cy, r, [&](float angle) { return angle < gap_from || angle > gap_to; }, x0, y0, x1, y1);
      // Turning counterclockwise, the arrow points along (sin, -cos) where it ends.
      return std::min(arc, Head(x, y, x0, y0, std::sin(a0), -std::cos(a0), 3.6f)) - 0.95f;
    }
    case EditorIcon::kRotateRight:
    case EditorIcon::kRotateLeft: {
      if (icon == EditorIcon::kRotateLeft) x = 20.0f - x;
      const float body = std::fabs(Box(x, y, 7.6f, 13.0f, 4.4f, 4.2f, 1.5f));
      const float cx = 12.2f;
      const float cy = 7.8f;
      const float r = 4.6f;
      const float end = -40.0f * kPi / 180.0f;
      const float ex = cx + r * std::cos(end);
      const float ey = cy + r * std::sin(end);
      const float arc = Arc(x, y, cx, cy, r, [](float angle) { return angle <= -40.0f || angle >= 179.0f; }, cx - r, cy, ex, ey);
      // Clockwise, along (-sin, cos).
      return std::min({body, arc, Head(x, y, ex, ey, -std::sin(end), std::cos(end), 3.4f)}) - 0.9f;
    }
    case EditorIcon::kMarkup: {
      const float outline = std::min({Segment(x, y, 10.0f, 3.4f, 5.2f, 13.6f), Segment(x, y, 10.0f, 3.4f, 14.8f, 13.6f), Segment(x, y, 5.2f, 13.6f, 14.8f, 13.6f),
                                      Segment(x, y, 5.2f, 13.6f, 5.2f, 17.2f), Segment(x, y, 14.8f, 13.6f, 14.8f, 17.2f)}) - 0.85f;
      const float tip = Taper(x, y, 10.0f, 3.4f, 7.6f, [](float v) { return (v - 3.4f) * 0.47f; });
      return std::min(outline, tip);
    }
  }
  return 1e9f;
}

float Cylinder(float u, float center, float half) {
  const float q = (u - center) / std::max(half, 0.5f);
  return -0.26f * q * q + 0.16f * std::exp(-((q + 0.42f) / 0.24f) * ((q + 0.42f) / 0.24f));
}

uint32_t Toward(uint32_t from, uint32_t to, float share) {
  uint32_t out = 0;
  for (int c = 0; c < 3; ++c) out |= static_cast<uint32_t>(ToByte(Channel(from, c) + (Channel(to, c) - Channel(from, c)) * share)) << (c * 8);
  return out;
}

float Hue(float p, float q, float t) {
  if (t < 0.0f) t += 1.0f;
  if (t > 1.0f) t -= 1.0f;
  if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
  if (t < 0.5f) return q;
  if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
  return p;
}

uint32_t FromHsl(float hue, float saturation, float lightness) {
  const float q = lightness < 0.5f ? lightness * (1.0f + saturation) : lightness + saturation - lightness * saturation;
  const float p = 2.0f * lightness - q;
  const float h = hue / 360.0f;
  const float r = Hue(p, q, h + 1.0f / 3.0f);
  const float g = Hue(p, q, h);
  const float b = Hue(p, q, h - 1.0f / 3.0f);
  return static_cast<uint32_t>(ToByte(r * 255.0f)) << 16 | static_cast<uint32_t>(ToByte(g * 255.0f)) << 8 | ToByte(b * 255.0f);
}

float Brightness(uint32_t color) { return (0.299f * Channel(color, 2) + 0.587f * Channel(color, 1) + 0.114f * Channel(color, 0)) / 255.0f; }

// The checks behind anything that can be see-through.
uint32_t Check(float u, float v) {
  const int cell = (static_cast<int>(std::floor(u / 5.0f)) + static_cast<int>(std::floor(v / 5.0f))) & 1;
  return cell ? 0xD5D5D5u : 0xFFFFFFu;
}

}  // namespace

Image RenderEditorIcon(EditorIcon icon, int size, float scale, uint32_t color) {
  Painter painter(size, size, scale);
  const float unit = static_cast<float>(size) / scale / 20.0f;
  painter.Fill(color, 1.0f, [&](float u, float v) { return IconDistance(icon, u / unit, v / unit) * unit; });
  return painter.Finish();
}

Image RenderBox(int width, int height, float radius, uint32_t color, float alpha) {
  Painter painter(width, height, 1.0f);
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  painter.Fill(color, alpha, [&](float u, float v) { return Box(u, v, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, radius); });
  return painter.Finish();
}

Image RenderPanel(int width, int height, float radius, uint32_t fill, uint32_t hairline, float hairline_alpha) {
  Painter painter(width, height, 1.0f);
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  painter.Fill(hairline, hairline_alpha, [&](float u, float v) { return Box(u, v, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, radius); });
  painter.Fill(fill, 1.0f, [&](float u, float v) { return Box(u, v, w * 0.5f, h * 0.5f, w * 0.5f - 1.0f, h * 0.5f - 1.0f, std::max(radius - 1.0f, 0.0f)); });
  return painter.Finish();
}

Image RenderToolArt(Tool tool, int width, int height, float scale, const ToolLook& look) {
  Painter painter(width, height, scale);
  const float c = static_cast<float>(width) / scale * 0.5f;
  const float bottom = static_cast<float>(height) / scale + 20.0f;
  const float half = 9.6f;
  const bool outlined = Brightness(look.barrel) > 0.6f;
  const auto cylinder = [&](float h) { return [&, h](float u, float) { return Cylinder(u, c, h); }; };

  // Painted twice where the body is light, the first time a little larger in the edge color, which keeps it apart
  // from a light toolbar.
  const auto body = [&](uint32_t color, auto shape, auto shade) {
    if (outlined) painter.Fill(look.edge, 0.55f, [&](float u, float v) { return shape(u, v) - 0.7f; });
    painter.Fill(color, 1.0f, shape, shade);
  };
  const auto barrel = [&](float top) {
    body(look.barrel, [&, top](float u, float v) { return Box(u, v, c, (top + bottom) * 0.5f, half, (bottom - top) * 0.5f, 2.2f); }, cylinder(half));
  };
  const auto band = [&](float top, float bottom_edge, uint32_t color) {
    painter.Fill(color, 1.0f, [&, top, bottom_edge](float u, float v) { return Box(u, v, c, (top + bottom_edge) * 0.5f, half, (bottom_edge - top) * 0.5f, 0.0f); },
                 cylinder(half));
  };

  float label_y = 58.0f;
  switch (tool) {
    case Tool::kPen: {
      barrel(40.0f);
      band(40.0f, 44.0f, look.ink);
      body(0x2B2C30, [&](float u, float v) { return Taper(u, v, c, 21.0f, 40.5f, [](float y) { return 5.2f + (y - 21.0f) / 19.5f * 4.4f; }); },
           [&](float u, float v) { return Cylinder(u, c, 5.2f + (std::clamp(v, 21.0f, 40.5f) - 21.0f) / 19.5f * 4.4f); });
      const auto nib_half = [](float y) { return 5.6f * std::pow(std::max(y - 3.0f, 0.0f) / 19.0f, 0.7f); };
      body(config::kNibSteel, [&](float u, float v) { return Taper(u, v, c, 3.0f, 22.0f, nib_half); },
           [&](float u, float v) { return Cylinder(u, c, std::max(nib_half(std::clamp(v, 3.0f, 22.0f)), 0.8f)) * 1.4f; });
      painter.Fill(0x3A3C40, 1.0f, [&](float u, float v) { return std::min(Segment(u, v, c, 8.6f, c, 17.0f) - 0.42f, std::hypot(u - c, v - 17.8f) - 1.15f); });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Taper(u, v, c, 3.0f, 6.6f, nib_half); });
      break;
    }
    case Tool::kMonoline: {
      barrel(36.0f);
      band(36.0f, 39.5f, look.ink);
      body(look.barrel, [&](float u, float v) { return Taper(u, v, c, 13.0f, 36.5f, [](float y) { return 2.6f + (y - 13.0f) / 23.5f * 7.0f; }); },
           [&](float u, float v) { return Cylinder(u, c, 2.6f + (std::clamp(v, 13.0f, 36.5f) - 13.0f) / 23.5f * 7.0f); });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return std::min(Box(u, v, c, 9.6f, 2.6f, 4.0f, 0.0f), std::hypot(u - c, v - 5.8f) - 2.6f); },
                   cylinder(2.6f));
      break;
    }
    case Tool::kHighlighter: {
      barrel(38.0f);
      band(38.0f, 44.0f, look.ink);
      body(look.barrel, [&](float u, float v) { return Taper(u, v, c, 28.0f, 38.5f, [](float y) { return 7.0f + (y - 28.0f) / 10.5f * 2.6f; }); },
           cylinder(8.3f));
      const auto chisel = [&](float u, float v) { return std::max(Box(u, v, c, 18.5f, 7.0f, 10.0f, 0.0f), (9.0f + (u - (c - 7.0f)) * 0.55f) - v); };
      painter.Fill(look.ink, 1.0f, chisel, cylinder(7.0f));
      painter.Fill(0xFFFFFF, 0.22f, [&](float u, float v) { return std::max(chisel(u, v), v - (11.2f + (u - (c - 7.0f)) * 0.55f)); });
      break;
    }
    case Tool::kPencil: {
      const auto wood_half = [](float y) { return 2.4f + (y - 12.0f) / 22.0f * 7.2f; };
      painter.Fill(config::kPencilWood, 1.0f, [&](float u, float v) { return Taper(u, v, c, 12.0f, 36.0f, wood_half); },
                   [&](float u, float v) { return Cylinder(u, c, wood_half(std::clamp(v, 12.0f, 36.0f))) * 0.8f; });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Taper(u, v, c, 3.6f, 12.4f, [](float y) { return 0.35f + (y - 3.6f) / 8.8f * 2.1f; }); },
                   cylinder(1.6f));
      // The paint ends in scallops where the wood was sharpened.
      const auto paint = [&](float u, float v) {
        const float edge = 32.6f + 1.5f * (0.5f + 0.5f * std::cos((u - c) * 0.95f));
        return std::max(Box(u, v, c, (edge + bottom) * 0.5f, half, (bottom - 30.0f) * 0.5f, 0.0f), edge - v);
      };
      body(look.barrel, paint, [&](float u, float) {
        const float q = (u - c) / half;
        return q < -0.33f ? -0.02f : (q < 0.33f ? 0.08f : -0.16f);
      });
      band(46.0f, 48.0f, look.ink);
      label_y = 60.0f;
      break;
    }
    case Tool::kCrayon: {
      barrel(31.0f);
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Box(u, v, c, 56.0f, half, 11.0f, 0.0f); }, cylinder(half));
      painter.Fill(look.barrel, 0.9f, [&](float u, float v) { return std::min(Box(u, v, c, 47.6f, half, 0.45f, 0.0f), Box(u, v, c, 64.4f, half, 0.45f, 0.0f)); });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Box(u, v, c, 24.5f, 9.0f, 7.5f, 0.0f); }, cylinder(9.0f));
      painter.Fill(look.ink, 1.0f,
                   [&](float u, float v) {
                     return std::min(Taper(u, v, c, 8.2f, 17.5f, [](float y) { return 3.4f + (y - 8.2f) / 9.3f * 5.6f; }), std::hypot(u - c, v - 8.4f) - 3.4f);
                   },
                   [&](float u, float v) { return Cylinder(u, c, 3.4f + (std::clamp(v, 8.2f, 17.5f) - 8.2f) / 9.3f * 5.6f) + 0.05f; });
      label_y = 74.0f;
      break;
    }
    case Tool::kBrush: {
      barrel(42.0f);
      body(config::kNibSteel, [&](float u, float v) { return Taper(u, v, c, 29.0f, 42.5f, [](float y) { return 8.2f + (y - 29.0f) / 13.5f * 1.4f; }); },
           cylinder(9.0f));
      const auto bristle_half = [](float y) { return 8.0f * std::pow(std::max(y - 3.0f, 0.0f) / 27.0f, 0.55f); };
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Taper(u, v, c, 3.0f, 30.0f, bristle_half); },
                   [&](float u, float v) { return Cylinder(u, c, std::max(bristle_half(std::clamp(v, 3.0f, 30.0f)), 0.8f)) + 0.07f * std::sin(u * 2.3f + v * 0.35f); });
      label_y = 60.0f;
      break;
    }
    case Tool::kEraser: {
      barrel(36.0f);
      painter.Fill(config::kNibSteel, 1.0f, [&](float u, float v) { return Box(u, v, c, 33.5f, half, 2.6f, 0.0f); }, cylinder(half));
      body(config::kEraserRubber, [&](float u, float v) { return Box(u, v, c, 19.5f, 9.2f, 12.5f, 4.6f); }, cylinder(9.2f));
      break;
    }
    case Tool::kLasso: {
      barrel(36.0f);
      const auto cone = [&](float u, float v) { return Taper(u, v, c, 5.0f, 36.5f, [](float y) { return 0.8f + (y - 5.0f) / 31.5f * 8.8f; }); };
      body(look.barrel, cone, [&](float u, float v) { return Cylinder(u, c, 0.8f + (std::clamp(v, 5.0f, 36.5f) - 5.0f) / 31.5f * 8.8f); });
      painter.Fill(look.edge, 0.9f, [&](float u, float v) {
        const float stripe = std::fmod(u * 0.9f + v * 0.75f + 100.0f, 5.2f) - 2.1f;
        return std::max(cone(u, v) + 0.6f, stripe * 0.6f);
      });
      painter.Fill(look.edge, 1.0f, [&](float u, float v) { return std::hypot(u - c, v - 5.6f) - 1.1f; });
      break;
    }
    case Tool::kRuler: {
      body(look.barrel, [&](float u, float v) { return Box(u, v, c, (6.0f + bottom) * 0.5f, half, (bottom - 6.0f) * 0.5f, 2.0f); },
           [](float, float) { return 0.0f; });
      painter.Fill(look.edge, 1.0f, [&](float u, float v) {
        float nearest = 1e9f;
        for (int k = 0; k < 24; ++k) {
          const float y = 10.0f + static_cast<float>(k) * 3.6f;
          const float length = k % 5 == 0 ? 7.0f : (k % 5 == 2 ? 4.8f : 3.4f);
          nearest = std::min(nearest, Segment(u, v, c - half + 1.2f, y, c - half + 1.2f + length, y));
        }
        return nearest - 0.36f;
      });
      break;
    }
  }

  if (look.percent >= 0 && look.percent < 100 && IsInk(tool)) {
    wchar_t text[8];
    swprintf_s(text, L"%d", look.percent);
    const TextLine line = RenderText(text, std::max(static_cast<int>(std::lround(config::kToolLabelSize * scale)), 6), look.label);
    const RECT ink = InkBounds(line.image);
    const int x = static_cast<int>(std::lround(c * scale - static_cast<float>(ink.left + ink.right) * 0.5f));
    const int y = static_cast<int>(std::lround(label_y * scale - static_cast<float>(ink.top + ink.bottom) * 0.5f));
    painter.Blend(line.image, x, y);
  }
  return painter.Finish();
}

Image RenderSwatch(int size, float scale, uint32_t color, bool chosen, uint32_t panel) {
  Painter painter(size, size, 1.0f);
  const float center = static_cast<float>(size) * 0.5f;
  const float r = center - 0.5f;
  const float ring = std::round(config::kSwatchRing * scale);
  const auto disc = [&](float radius) { return [&, radius](float u, float v) { return std::hypot(u - center, v - center) - radius; }; };
  // Colors close to the toolbar's own keep a faint edge.
  const bool faint = std::fabs(Brightness(color) - Brightness(panel)) < 0.12f;
  const uint32_t edge = Brightness(panel) > 0.5f ? 0x000000u : 0xFFFFFFu;
  if (chosen) {
    painter.Fill(color, 1.0f, disc(r));
    painter.Fill(panel, 1.0f, disc(r - ring));
    if (faint) painter.Fill(edge, 0.16f, disc(r - ring * 2.0f));
    painter.Fill(color, 1.0f, disc(r - ring * 2.0f - (faint ? 1.0f : 0.0f)));
  } else {
    if (faint) painter.Fill(edge, 0.16f, disc(r));
    painter.Fill(color, 1.0f, disc(r - (faint ? 1.0f : 0.0f)));
  }
  return painter.Finish();
}

Image RenderHueSwatch(int size, float scale, uint32_t center_color, bool filled, bool chosen, uint32_t panel) {
  Image image = Image::Allocate(size, size);
  if (!image) return image;
  const float center = static_cast<float>(size) * 0.5f;
  const float r = center - 0.5f;
  const float ring = std::round((chosen ? config::kSwatchRing : 3.5f) * scale);
  const float gap = chosen ? std::round(config::kSwatchRing * scale) : std::round(1.5f * scale);
  uint8_t* out = image.data();
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x, out += 4) {
      const float dx = static_cast<float>(x) + 0.5f - center;
      const float dy = static_cast<float>(y) + 0.5f - center;
      const float d = std::hypot(dx, dy);
      const float outer = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
      const float hue_cover = outer * std::clamp(d - (r - ring) + 0.5f, 0.0f, 1.0f);
      const float inner = std::clamp((r - ring - gap) - d + 0.5f, 0.0f, 1.0f);
      const float hue = std::fmod(std::atan2(dy, dx) * 180.0f / kPi + 450.0f, 360.0f);
      const uint32_t wheel = FromHsl(hue, 0.9f, 0.55f);
      const uint32_t fill = filled ? center_color : panel;
      float color[3];
      for (int c = 0; c < 3; ++c) color[c] = Channel(wheel, c) * hue_cover + Channel(panel, c) * (outer - hue_cover - inner * outer) + Channel(fill, c) * inner * outer;
      for (int c = 0; c < 3; ++c) out[c] = ToByte(color[c]);
      out[3] = ToByte(outer * 255.0f);
    }
  }
  return image;
}

Image RenderCornerHandle(int corner, int arm, int thickness, int pad, uint32_t color, uint32_t halo, float halo_alpha) {
  const int size = arm + pad * 2;
  Painter painter(size, size, 1.0f);
  const float p = static_cast<float>(pad);
  const float a = static_cast<float>(arm);
  const float t = static_cast<float>(thickness);
  const auto shape = [&](float u, float v) {
    if (corner == 1 || corner == 3) u = static_cast<float>(size) - u;
    if (corner == 2 || corner == 3) v = static_cast<float>(size) - v;
    return std::min(Box(u, v, p + a * 0.5f, p + t * 0.5f, a * 0.5f, t * 0.5f, t * 0.5f), Box(u, v, p + t * 0.5f, p + a * 0.5f, t * 0.5f, a * 0.5f, t * 0.5f));
  };
  for (int ring = pad; ring >= 1; --ring) {
    const float spread = static_cast<float>(ring);
    painter.Fill(halo, halo_alpha / static_cast<float>(pad) * 0.6f, [&](float u, float v) { return shape(u, v) - spread; });
  }
  painter.Fill(color, 1.0f, shape);
  return painter.Finish();
}

Image RenderEdgeHandle(bool across, int length, int thickness, int pad, uint32_t color, uint32_t halo, float halo_alpha) {
  const int width = (across ? length : thickness) + pad * 2;
  const int height = (across ? thickness : length) + pad * 2;
  Painter painter(width, height, 1.0f);
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const float t = static_cast<float>(thickness);
  const auto shape = [&](float u, float v) {
    return Box(u, v, w * 0.5f, h * 0.5f, across ? static_cast<float>(length) * 0.5f : t * 0.5f, across ? t * 0.5f : static_cast<float>(length) * 0.5f, t * 0.5f);
  };
  for (int ring = pad; ring >= 1; --ring) {
    const float spread = static_cast<float>(ring);
    painter.Fill(halo, halo_alpha / static_cast<float>(pad) * 0.6f, [&](float u, float v) { return shape(u, v) - spread; });
  }
  painter.Fill(color, 1.0f, shape);
  return painter.Finish();
}

Image RenderOpacityTrack(int width, int height, float scale, uint32_t color) {
  Image image = Image::Allocate(width, height);
  if (!image) return image;
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  uint8_t* out = image.data();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x, out += 4) {
      const float u = static_cast<float>(x) + 0.5f;
      const float v = static_cast<float>(y) + 0.5f;
      const float cover = std::clamp(0.5f - Box(u, v, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, h * 0.5f), 0.0f, 1.0f);
      const float share = std::clamp((u - h * 0.5f) / std::max(w - h, 1.0f), 0.0f, 1.0f);
      const uint32_t check = Check(u / scale, v / scale);
      for (int c = 0; c < 3; ++c) out[c] = ToByte((Channel(check, c) + (Channel(color, c) - Channel(check, c)) * share) * cover);
      out[3] = ToByte(cover * 255.0f);
    }
  }
  return image;
}

Image RenderOpacityKnob(int size, float scale, uint32_t color, float opacity) {
  Image image = Image::Allocate(size, size);
  if (!image) return image;
  const float center = static_cast<float>(size) * 0.5f;
  const float r = center - 0.5f;
  const float ring = std::round(3.0f * scale);
  uint8_t* out = image.data();
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x, out += 4) {
      const float u = static_cast<float>(x) + 0.5f;
      const float v = static_cast<float>(y) + 0.5f;
      const float d = std::hypot(u - center, v - center);
      const float outer = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
      const float rim = std::clamp(d - (r - 1.0f) + 0.5f, 0.0f, 1.0f) * 0.25f;
      const float inner = std::clamp((r - ring) - d + 0.5f, 0.0f, 1.0f);
      const uint32_t check = Check(u / scale, v / scale);
      for (int c = 0; c < 3; ++c) {
        const float within = Channel(check, c) + (Channel(color, c) - Channel(check, c)) * opacity;
        const float face = 255.0f + (within - 255.0f) * inner;
        out[c] = ToByte(face * (1.0f - rim) * outer);
      }
      out[3] = ToByte(outer * 255.0f);
    }
  }
  return image;
}

uint32_t GridColor(int column, int row) {
  if (row == 0) {
    const float value = 1.0f - static_cast<float>(column) / 11.0f;
    const uint8_t level = ToByte(value * 255.0f);
    return static_cast<uint32_t>(level) << 16 | static_cast<uint32_t>(level) << 8 | level;
  }
  const float hue = std::fmod(200.0f + static_cast<float>(column) * 30.0f, 360.0f);
  const float lightness = 0.22f + static_cast<float>(row - 1) * (0.62f / 8.0f);
  return FromHsl(hue, 0.88f, lightness);
}

Image RenderColorGrid(int columns, int rows, int cell, float scale, int chosen_column, int chosen_row) {
  const int width = columns * cell;
  const int height = rows * cell;
  Painter painter(width, height, 1.0f);
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const float radius = std::round(8.0f * scale);
  Image cells = Image::Allocate(width, height);
  if (!cells) return cells;
  for (int y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(cells.data() + static_cast<ptrdiff_t>(y) * cells.stride());
    for (int x = 0; x < width; ++x) {
      const float u = static_cast<float>(x) + 0.5f;
      const float v = static_cast<float>(y) + 0.5f;
      const float cover = std::clamp(0.5f - Box(u, v, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, radius), 0.0f, 1.0f);
      const uint32_t color = GridColor(x / cell, y / cell);
      row[x] = static_cast<uint32_t>(ToByte(cover * 255.0f)) << 24 | static_cast<uint32_t>(ToByte(Channel(color, 2) * cover)) << 16 |
               static_cast<uint32_t>(ToByte(Channel(color, 1) * cover)) << 8 | ToByte(Channel(color, 0) * cover);
    }
  }
  painter.Blend(cells, 0, 0);
  if (chosen_column >= 0 && chosen_row >= 0) {
    const float cx = (static_cast<float>(chosen_column) + 0.5f) * cell;
    const float cy = (static_cast<float>(chosen_row) + 0.5f) * cell;
    const float half = static_cast<float>(cell) * 0.5f;
    painter.Fill(0x000000, 0.55f, [&](float u, float v) { return std::fabs(Box(u, v, cx, cy, half + 0.5f, half + 0.5f, 3.0f)) - 1.6f; });
    painter.Fill(0xFFFFFF, 1.0f, [&](float u, float v) { return std::fabs(Box(u, v, cx, cy, half - 0.8f, half - 0.8f, 2.4f)) - 1.0f; });
  }
  return painter.Finish();
}

Image RenderRuler(int length, int thickness, float scale) {
  Painter painter(length, thickness, 1.0f);
  const float l = static_cast<float>(length);
  const float t = static_cast<float>(thickness);
  painter.Fill(config::kRulerBody, config::kRulerAlpha, [&](float u, float v) { return Box(u, v, l * 0.5f, t * 0.5f, l * 0.5f, t * 0.5f, 3.0f * scale); });
  // Marks every few pixels from both long edges, measured from the middle so that they meet the center line evenly.
  const float step = 4.0f * scale;
  const float line = std::max(0.5f * scale, 0.6f);
  painter.Fill(config::kRulerInk, 0.85f, [&](float u, float v) {
    const float from_center = u - l * 0.5f;
    const float index = std::round(from_center / step);
    const float x = l * 0.5f + index * step;
    const int k = static_cast<int>(std::fabs(index)) % 10;
    const float reach = (k == 0 ? 16.0f : (k == 5 ? 11.0f : 6.0f)) * scale;
    const float edge = std::min(v, t - v);
    return std::max(std::fabs(u - x) - line, edge - reach);
  });
  return painter.Finish();
}

}  // namespace sstk
