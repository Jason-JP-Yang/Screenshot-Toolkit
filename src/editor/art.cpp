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
      // A circle drawn anticlockwise from the upper left all the way round to the top, where it turns into an arrow
      // pointing left, back toward where it began.
      const float cx = 10.0f;
      const float cy = 10.6f;
      const float r = 6.3f;
      const float tail = -132.0f * kPi / 180.0f;
      const float tail_x = cx + r * std::cos(tail);
      const float tail_y = cy + r * std::sin(tail);
      const float arc = Arc(x, y, cx, cy, r, [](float angle) { return angle <= -132.0f || angle >= -90.0f; }, tail_x, tail_y, cx, cy - r);
      return std::min(arc, Head(x, y, cx, cy - r, -1.0f, 0.0f, 3.1f)) - 0.92f;
    }
    case EditorIcon::kRotateRight:
    case EditorIcon::kRotateLeft: {
      // A square, and an arrow that leaves its top edge and swings round its top corner to point down past it.
      if (icon == EditorIcon::kRotateLeft) x = 20.0f - x;
      const float body = std::fabs(Box(x, y, 7.2f, 13.4f, 4.2f, 4.2f, 2.0f));
      const float cx = 11.4f;
      const float cy = 9.2f;
      const float r = 4.4f;
      const float arc = Arc(x, y, cx, cy, r, [](float angle) { return angle <= 0.0f || angle >= 179.9f; }, cx - r, cy, cx + r, cy);
      return std::min({body, arc, Head(x, y, cx + r, cy + 0.4f, 0.0f, 1.0f, 3.1f)}) - 0.92f;
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

// A lit cylinder: darker toward its sides, a broad sheen and a sharp glint left of center, and a faint light
// reflected back along its right edge.
float Cylinder(float u, float center, float half) {
  const float q = (u - center) / std::max(half, 0.5f);
  const auto bump = [&](float at, float width) { return std::exp(-((q - at) / width) * ((q - at) / width)); };
  return -0.28f * q * q + 0.14f * bump(-0.42f, 0.26f) + 0.30f * bump(-0.52f, 0.07f) + 0.06f * bump(0.80f, 0.08f);
}

float ArtHash(int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0f;
}

float ArtNoise(float x, float y) {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const int ix = static_cast<int>(fx);
  const int iy = static_cast<int>(fy);
  const float tx = (x - fx) * (x - fx) * (3.0f - 2.0f * (x - fx));
  const float ty = (y - fy) * (y - fy) * (3.0f - 2.0f * (y - fy));
  const float top = ArtHash(ix, iy) + (ArtHash(ix + 1, iy) - ArtHash(ix, iy)) * tx;
  const float bottom = ArtHash(ix, iy + 1) + (ArtHash(ix + 1, iy + 1) - ArtHash(ix, iy + 1)) * tx;
  return top + (bottom - top) * ty;
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
  // Drawn 26 units wide whatever the size, the barrel 19.2 of them.
  const float unit = static_cast<float>(width) / 26.0f;
  Painter painter(width, height, unit);
  const float c = 13.0f;
  const float bottom = static_cast<float>(height) / unit + 20.0f;
  const float half = 9.6f;
  const bool outlined = Brightness(look.barrel) > 0.6f;
  const uint32_t steel = config::kNibSteel;
  const uint32_t dark = 0x26272B;
  const uint32_t gold = 0xC9A24C;
  const auto cylinder = [&](float h) { return [&, h](float u, float) { return Cylinder(u, c, h); }; };
  const auto flat = [](float, float) { return 0.0f; };
  const auto linear = [](float y0, float y1, float h0, float h1) {
    return [=](float y) { return h0 + (std::clamp(y, y0, y1) - y0) / (y1 - y0) * (h1 - h0); };
  };

  // Painted twice where the body is light, the first time a little larger in the edge color, which keeps it apart
  // from a light toolbar.
  const auto body = [&](uint32_t color, auto shape, auto shade) {
    if (outlined) painter.Fill(look.edge, 0.55f, [&](float u, float v) { return shape(u, v) - 0.7f; });
    painter.Fill(color, 1.0f, shape, shade);
  };
  const auto barrel = [&](float top) {
    body(look.barrel, [&, top](float u, float v) { return Box(u, v, c, (top + bottom) * 0.5f, half, (bottom - top) * 0.5f, 2.2f); }, cylinder(half));
  };
  const auto band = [&](float top, float bottom_edge, uint32_t color, float h = 9.6f) {
    painter.Fill(color, 1.0f, [&, top, bottom_edge, h](float u, float v) { return Box(u, v, c, (top + bottom_edge) * 0.5f, h, (bottom_edge - top) * 0.5f, 0.0f); },
                 cylinder(h));
  };
  // A thin line around the barrel, darker or lighter, as a seam or a groove.
  const auto ring = [&](float at, float thickness, uint32_t color, float alpha, float h = 9.6f) {
    painter.Fill(color, alpha, [&, at, thickness, h](float u, float v) { return Box(u, v, c, at, h, thickness * 0.5f, 0.0f); }, cylinder(h));
  };
  // A pocket clip down the barrel's right side, ending in a ball.
  const auto clip = [&](float top, float length) {
    const float x = c + 5.4f;
    const auto shape = [&, top, length](float u, float v) {
      return std::min(Box(u, v, x, top + length * 0.5f, 1.05f, length * 0.5f, 1.0f), std::hypot(u - x, v - (top + length)) - 1.6f);
    };
    painter.Fill(0x000000, 0.18f, [&](float u, float v) { return shape(u - 0.5f, v - 0.6f) - 0.3f; });
    painter.Fill(steel, 1.0f, shape, [&](float u, float) { return 0.25f - 0.5f * std::fabs(u - x); });
  };

  float label_y = 60.0f;
  switch (tool) {
    case Tool::kPen: {
      barrel(42.0f);
      clip(47.0f, 26.0f);
      band(42.0f, 45.6f, look.ink);
      ring(41.8f, 0.7f, steel, 1.0f);
      ring(45.9f, 0.7f, steel, 1.0f);
      // The grip, ridged where the fingers hold it, under a steel collar.
      const auto grip = linear(22.0f, 42.0f, 5.1f, 9.6f);
      body(dark, [&](float u, float v) { return Taper(u, v, c, 22.0f, 42.3f, grip); },
           [&](float u, float v) { return Cylinder(u, c, grip(v)) + 0.05f * std::sin(v * 2.6f); });
      band(20.6f, 23.0f, steel, 5.4f);
      // The nib: plated steel with a slit, a breather hole, engraved scrolls, and the ink at its point.
      const auto nib_half = [](float y) { return 5.6f * std::pow(std::max(y - 3.0f, 0.0f) / 18.0f, 0.68f); };
      body(steel, [&](float u, float v) { return Taper(u, v, c, 3.0f, 21.0f, nib_half); },
           [&](float u, float v) { return Cylinder(u, c, std::max(nib_half(std::clamp(v, 3.0f, 21.0f)), 0.8f)) * 1.5f + (v > 15.0f ? 0.06f : 0.0f); });
      painter.Fill(0x6E7178, 0.7f, [&](float u, float v) {
        const float left = std::fabs(std::hypot(u - (c - 4.6f), v - 19.2f) - 3.0f) - 0.28f;
        const float right = std::fabs(std::hypot(u - (c + 4.6f), v - 19.2f) - 3.0f) - 0.28f;
        return std::max(std::min(left, right), Taper(u, v, c, 3.0f, 21.0f, nib_half) + 0.6f);
      });
      painter.Fill(0x2E3034, 1.0f, [&](float u, float v) { return std::min(Segment(u, v, c, 7.8f, c, 16.6f) - 0.36f, std::hypot(u - c, v - 17.4f) - 1.05f); });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Taper(u, v, c, 3.0f, 6.0f, nib_half); });
      label_y = 64.0f;
      break;
    }
    case Tool::kMonoline: {
      barrel(38.0f);
      clip(44.0f, 24.0f);
      band(38.0f, 41.6f, look.ink);
      ring(42.0f, 0.6f, dark, 0.35f);
      const auto cone = linear(14.0f, 38.0f, 2.4f, 9.6f);
      body(look.barrel, [&](float u, float v) { return Taper(u, v, c, 14.0f, 38.5f, cone); }, [&](float u, float v) { return Cylinder(u, c, cone(v)); });
      for (const float at : {28.5f, 31.0f, 33.5f}) {
        painter.Fill(dark, 0.16f, [&, at](float u, float v) { return std::max(Taper(u, v, c, 14.0f, 38.5f, cone), std::fabs(v - at) - 0.3f); });
      }
      band(9.6f, 14.6f, steel, 2.1f);
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return std::min(Box(u, v, c, 7.6f, 1.55f, 2.4f, 0.0f), std::hypot(u - c, v - 5.4f) - 1.55f); },
                   cylinder(1.55f));
      painter.Fill(0xFFFFFF, 0.5f, [&](float u, float v) { return std::hypot(u - (c - 0.6f), v - 5.2f) - 0.45f; });
      label_y = 58.0f;
      break;
    }
    case Tool::kHighlighter: {
      barrel(40.0f);
      // A window down the barrel shows the ink inside.
      painter.Fill(look.ink, 0.42f, [&](float u, float v) { return Box(u, v, c - 0.8f, 61.0f, 5.6f, 10.0f, 2.6f); }, cylinder(half));
      band(40.0f, 46.0f, look.ink);
      ring(46.3f, 0.7f, dark, 0.3f);
      const auto shoulder = linear(29.0f, 40.0f, 7.2f, 9.6f);
      body(look.barrel, [&](float u, float v) { return Taper(u, v, c, 29.0f, 40.5f, shoulder); }, [&](float u, float v) { return Cylinder(u, c, shoulder(v)); });
      for (const float at : {32.0f, 34.4f, 36.8f}) ring(at, 0.55f, dark, 0.18f, 9.0f);
      // A felt chisel, cut on the slant, its top face catching the light.
      const auto chisel = [&](float u, float v) { return std::max(Box(u, v, c, 19.5f, 7.0f, 10.0f, 0.0f), (10.0f + (u - (c - 7.0f)) * 0.55f) - v); };
      painter.Fill(look.ink, 1.0f, chisel, [&](float u, float v) { return Cylinder(u, c, 7.0f) * 0.7f - 0.10f * ArtNoise(u * 2.2f, v * 2.2f); });
      painter.Fill(0xFFFFFF, 0.26f, [&](float u, float v) { return std::max(chisel(u, v), v - (12.2f + (u - (c - 7.0f)) * 0.55f)); });
      label_y = 78.0f;
      break;
    }
    case Tool::kPencil: {
      // Sharpened wood with its grain running up to the lead, and the paint ending in scallops.
      const auto wood = linear(12.0f, 36.0f, 2.4f, 9.6f);
      painter.Fill(config::kPencilWood, 1.0f, [&](float u, float v) { return Taper(u, v, c, 12.0f, 36.0f, wood); },
                   [&](float u, float v) {
                     const float q = (u - c) / std::max(wood(v), 0.5f);
                     return Cylinder(u, c, wood(v)) * 0.8f - 0.09f * std::pow(std::fabs(std::sin(q * 7.0f + v * 0.18f)), 6.0f);
                   });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Taper(u, v, c, 3.4f, 12.6f, linear(3.4f, 12.6f, 0.3f, 2.5f)); }, cylinder(1.6f));
      painter.Fill(0xFFFFFF, 0.35f, [&](float u, float v) { return Segment(u, v, c - 0.4f, 5.0f, c - 1.2f, 11.4f) - 0.25f; });
      const auto paint = [&](float u, float v) {
        const float edge = 33.4f + 1.6f * (0.5f + 0.5f * std::cos((u - c) * 0.95f));
        return std::max(Box(u, v, c, (edge + bottom) * 0.5f, half, (bottom - 30.0f) * 0.5f, 0.0f), edge - v);
      };
      // Six sides, three of them seen, each lit on its own.
      body(look.barrel, paint, [&](float u, float) {
        const float q = (u - c) / half;
        return q < -0.33f ? -0.03f : (q < 0.33f ? 0.10f : -0.17f);
      });
      painter.Fill(dark, 0.14f, [&](float u, float v) { return std::max(paint(u, v), std::min(std::fabs(u - (c - 3.2f)), std::fabs(u - (c + 3.2f))) - 0.25f); });
      band(47.0f, 49.0f, look.ink);
      ring(51.2f, 0.6f, gold, 0.9f);
      label_y = 62.0f;
      break;
    }
    case Tool::kCrayon: {
      // The paper wrapper, cut in a zigzag, printed with the color and its marks.
      const auto wrapper = [&](float u, float v) {
        const float tooth = 31.0f + 1.3f * std::fabs(std::fmod(u + 100.0f, 2.6f) - 1.3f);
        return std::max(Box(u, v, c, (31.0f + bottom) * 0.5f, half, (bottom - 31.0f) * 0.5f, 0.0f), tooth - v);
      };
      body(look.barrel, wrapper, cylinder(half));
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Box(u, v, c, 58.0f, half, 13.0f, 0.0f); }, cylinder(half));
      painter.Fill(look.barrel, 0.9f, [&](float u, float v) { return std::min(Box(u, v, c, 47.8f, half, 0.45f, 0.0f), Box(u, v, c, 68.2f, half, 0.45f, 0.0f)); });
      painter.Fill(look.barrel, 0.85f, [&](float u, float v) {
        float nearest = 1e9f;
        for (int k = -2; k <= 2; ++k) nearest = std::min(nearest, std::hypot(u - (c + static_cast<float>(k) * 3.4f), v - 58.0f) - 0.8f);
        return nearest;
      });
      painter.Fill(dark, 0.18f, [&](float u, float v) { return std::max(wrapper(u, v), std::fabs(u - (c + 6.2f)) - 0.22f); });
      // The wax, blunt from use, with a waxy sheen.
      const auto wax = linear(8.0f, 17.6f, 3.6f, 9.0f);
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Box(u, v, c, 24.5f, 9.0f, 7.4f, 0.0f); },
                   [&](float u, float v) { return Cylinder(u, c, 9.0f) - 0.06f * ArtNoise(u * 1.4f, v * 1.4f); });
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return std::min(Taper(u, v, c, 8.0f, 17.6f, wax), Box(u, v, c, 8.2f, 3.6f, 1.2f, 1.2f)); },
                   [&](float u, float v) { return Cylinder(u, c, wax(v)) + 0.05f; });
      painter.Fill(0xFFFFFF, 0.28f, [&](float u, float v) { return Box(u, v, c, 7.6f, 3.0f, 0.6f, 0.6f); });
      label_y = 76.0f;
      break;
    }
    case Tool::kBrush: {
      barrel(44.0f);
      for (const float at : {60.0f, 78.0f}) {
        ring(at, 1.2f, dark, 0.20f);
        ring(at + 0.9f, 0.5f, 0xFFFFFF, 0.25f);
      }
      // A crimped ferrule, then a tuft of hairs, darker and wetter toward the point.
      const auto ferrule = linear(29.0f, 44.0f, 8.0f, 9.6f);
      body(steel, [&](float u, float v) { return Taper(u, v, c, 29.0f, 44.5f, ferrule); },
           [&](float u, float v) { return Cylinder(u, c, ferrule(v)) * 1.3f; });
      for (const float at : {37.0f, 39.6f}) ring(at, 0.6f, dark, 0.35f, 9.2f);
      const auto tuft = [](float y) { return 7.8f * std::pow(std::max(y - 3.0f, 0.0f) / 27.0f, 0.55f); };
      painter.Fill(look.ink, 1.0f, [&](float u, float v) { return Taper(u, v, c, 3.0f, 30.0f, tuft); },
                   [&](float u, float v) {
                     const float strands = std::pow(std::fabs(std::sin((u - c) * 3.1f + v * 0.12f)), 10.0f);
                     const float dry = std::clamp((v - 18.0f) / 12.0f, 0.0f, 1.0f);
                     return Cylinder(u, c, std::max(tuft(v), 0.8f)) + 0.14f * strands * dry - 0.08f * (1.0f - dry);
                   });
      painter.Fill(0xFFFFFF, 0.30f, [&](float u, float v) { return Segment(u, v, c - 1.2f, 7.0f, c - 3.2f, 22.0f) - 0.35f; });
      label_y = 66.0f;
      break;
    }
    case Tool::kEraser: {
      barrel(38.0f);
      clip(44.0f, 22.0f);
      band(32.6f, 38.6f, steel);
      for (const float at : {34.4f, 36.8f}) ring(at, 0.55f, dark, 0.35f);
      // Soft rubber, bevelled on top and flecked.
      const auto rubber = [&](float u, float v) { return Box(u, v, c, 20.0f, 9.2f, 12.6f, 4.6f); };
      body(config::kEraserRubber, rubber, [&](float u, float v) {
        const float flecks = ArtNoise(u * 3.0f, v * 3.0f) > 0.78f ? -0.10f : 0.0f;
        return Cylinder(u, c, 9.2f) + (v < 11.0f ? 0.16f : 0.0f) + flecks;
      });
      break;
    }
    case Tool::kLasso: {
      barrel(38.0f);
      const auto taper = linear(5.0f, 38.0f, 0.8f, 9.6f);
      const auto cone = [&](float u, float v) { return Taper(u, v, c, 5.0f, 38.5f, taper); };
      body(look.barrel, cone, [&](float u, float v) { return Cylinder(u, c, taper(v)); });
      painter.Fill(look.edge, 0.9f, [&](float u, float v) {
        const float stripe = std::fmod(u * 0.9f + v * 0.75f + 100.0f, 5.2f) - 2.1f;
        return std::max(cone(u, v) + 0.6f, stripe * 0.6f);
      });
      painter.Fill(0xFFFFFF, 0.30f, [&](float u, float v) { return std::max(cone(u, v), Segment(u, v, c - 0.5f, 8.0f, c - 4.6f, 36.0f) - 0.5f); });
      painter.Fill(look.edge, 1.0f, [&](float u, float v) { return std::hypot(u - c, v - 5.6f) - 1.1f; });
      break;
    }
    case Tool::kRuler: {
      const auto ruler = [&](float u, float v) { return Box(u, v, c, (6.0f + bottom) * 0.5f, half, (bottom - 6.0f) * 0.5f, 2.0f); };
      body(look.barrel, ruler, flat);
      painter.Fill(0xFFFFFF, 0.35f, [&](float u, float v) { return std::max(ruler(u, v), std::fabs(u - (c + half - 1.0f)) - 0.4f); });
      painter.Fill(look.edge, 1.0f, [&](float u, float v) {
        float nearest = 1e9f;
        for (int k = 0; k < 26; ++k) {
          const float y = 10.0f + static_cast<float>(k) * 3.4f;
          const float length = k % 5 == 0 ? 7.6f : (k % 5 == 2 ? 5.0f : 3.4f);
          nearest = std::min(nearest, Segment(u, v, c - half + 1.2f, y, c - half + 1.2f + length, y));
        }
        return nearest - 0.34f;
      });
      painter.Fill(look.edge, 0.55f, [&](float u, float v) {
        float nearest = 1e9f;
        for (int k = 0; k < 6; ++k) nearest = std::min(nearest, std::hypot(u - (c + 3.6f), v - (10.0f + static_cast<float>(k) * 17.0f)) - 0.7f);
        return nearest;
      });
      break;
    }
  }

  if (look.percent >= 0 && look.percent < 100 && IsInk(tool)) {
    wchar_t text[8];
    swprintf_s(text, L"%d", look.percent);
    const TextLine line = RenderText(text, std::max(static_cast<int>(std::lround(config::kToolLabelSize * scale)), 6), look.label);
    const RECT ink = InkBounds(line.image);
    const int x = static_cast<int>(std::lround(c * unit - static_cast<float>(ink.left + ink.right) * 0.5f));
    const int y = static_cast<int>(std::lround(label_y * unit - static_cast<float>(ink.top + ink.bottom) * 0.5f));
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
