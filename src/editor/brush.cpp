#include "sstk/editor/brush.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kPieceLength = 0.75f;  // The path between points is walked in straight pieces this long.
constexpr float kLeast = 0.0005f;

const config::BrushTuning& TuningOf(Tool tool) {
  return IsInk(tool) ? config::kBrushTuning[static_cast<int>(tool)] : config::kEraserTuning;
}

// Some tools keep the strongest coverage a pixel got, so a stroke is even however often it crosses itself. The others
// build up, as graphite and wet ink do.
bool Builds(Tool tool) { return tool == Tool::kPencil || tool == Tool::kCrayon || tool == Tool::kBrush; }
bool Multiplies(Tool tool) { return tool == Tool::kHighlighter || tool == Tool::kBrush; }

float Smoothstep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float Lattice(int x, int y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

float Noise(float x, float y, uint32_t seed) {
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const int ix = static_cast<int>(fx);
  const int iy = static_cast<int>(fy);
  const float tx = x - fx;
  const float ty = y - fy;
  const float sx = tx * tx * (3.0f - 2.0f * tx);
  const float sy = ty * ty * (3.0f - 2.0f * ty);
  const float top = Lattice(ix, iy, seed) + (Lattice(ix + 1, iy, seed) - Lattice(ix, iy, seed)) * sx;
  const float bottom = Lattice(ix, iy + 1, seed) + (Lattice(ix + 1, iy + 1, seed) - Lattice(ix, iy + 1, seed)) * sx;
  return top + (bottom - top) * sy;
}

// Two sizes of paper tooth, fixed to the picture.
float Grain(float x, float y, float size, uint32_t seed) {
  return 0.62f * Noise(x / size, y / size, seed) + 0.38f * Noise(x / (size * 2.7f), y / (size * 2.7f), seed + 17u);
}

PointF CatmullRom(const InkPoint& a, const InkPoint& b, const InkPoint& c, const InkPoint& d, float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  const auto blend = [&](float p0, float p1, float p2, float p3) {
    return 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
  };
  return PointF{blend(a.x, b.x, c.x, d.x), blend(a.y, b.y, c.y, d.y)};
}

float Share(const Stroke& stroke, const StampTrail& trail, float pressure) {
  const config::BrushTuning& tuning = TuningOf(stroke.tool);
  const float p = stroke.mouse ? config::kMousePressure : std::clamp(pressure, 0.0f, 1.0f);
  float share = tuning.pressure_floor + (1.0f - tuning.pressure_floor) * std::pow(p, 0.8f);
  if (stroke.mouse && (stroke.tool == Tool::kPen || stroke.tool == Tool::kBrush)) {
    share *= std::clamp(1.0f - trail.speed * config::kMouseThinning, config::kMouseThinnest, 1.0f);
  }
  return share;
}

float Spacing(const Stroke& stroke, const StampTrail& trail, float pressure) {
  const float radius = std::max(stroke.width * 0.5f * Share(stroke, trail, pressure), 0.4f);
  return std::max(TuningOf(stroke.tool).spacing * 2.0f * radius, 0.35f);
}

Stamp MakeStamp(const Stroke& stroke, const StampTrail& trail, float x, float y, float pressure, float tilt, float azimuth) {
  const config::BrushTuning& tuning = TuningOf(stroke.tool);
  const float p = stroke.mouse ? config::kMousePressure : std::clamp(pressure, 0.0f, 1.0f);
  Stamp stamp{};
  stamp.x = x;
  stamp.y = y;
  stamp.radius = std::max(stroke.width * 0.5f * Share(stroke, trail, pressure), 0.4f);
  stamp.aspect = 1.0f;
  stamp.alpha = tuning.flow;
  switch (stroke.tool) {
    case Tool::kPen:
      stamp.aspect = config::kNibAspect;
      stamp.angle = config::kNibAngleDegrees * kPi / 180.0f;
      break;
    case Tool::kHighlighter:
      stamp.aspect = config::kChiselAspect;
      stamp.angle = config::kChiselAngleDegrees * kPi / 180.0f;
      break;
    case Tool::kPencil: {
      stamp.alpha = tuning.flow * (0.35f + 0.65f * p);
      stamp.cut = tuning.grain_bite * (0.95f - 0.8f * p);
      // Laid over on its side, a pencil shades: a wider, fainter mark along the way it leans.
      const float side = std::clamp((tilt - 0.35f) / 0.65f, 0.0f, 1.0f);
      if (side > 0.0f && !stroke.mouse) {
        stamp.aspect = 1.0f - 0.55f * side;
        stamp.radius *= 1.0f + 1.6f * side;
        stamp.alpha *= 1.0f - 0.45f * side;
        stamp.angle = azimuth;
      }
      break;
    }
    case Tool::kCrayon:
      stamp.cut = tuning.grain_bite * (0.9f - 0.55f * p);
      break;
    case Tool::kBrush:
      stamp.alpha = tuning.flow * (0.6f + 0.4f * p);
      break;
    default:
      break;
  }
  if (stroke.mouse && tuning.taper > 0.0f) {
    stamp.radius *= 0.35f + 0.65f * Smoothstep(0.0f, tuning.taper * stroke.width, trail.travelled);
  }
  return stamp;
}

float Ellipse(float u, float v, float radius, float aspect) {
  const float minor = std::max(radius * aspect, 0.3f);
  const float k = std::sqrt(u * u / (radius * radius) + v * v / (minor * minor));
  return (k - 1.0f) * minor;
}

float Chisel(float u, float v, float radius, float aspect) {
  const float half_x = radius;
  const float half_y = std::max(radius * aspect, 0.5f);
  const float corner = std::min(half_y, 1.2f) * 0.8f;
  const float qx = std::fabs(u) - half_x + corner;
  const float qy = std::fabs(v) - half_y + corner;
  const float ox = std::max(qx, 0.0f);
  const float oy = std::max(qy, 0.0f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - corner;
}

uint8_t ToByte(float value) { return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f)); }

}  // namespace

size_t LayStamps(Stroke& stroke, StampTrail& trail, bool finished) {
  const size_t first = stroke.stamps.size();
  const std::vector<InkPoint>& points = stroke.points;
  const size_t count = points.size();
  if (count == 0) return first;

  const auto emit = [&](float x, float y, float pressure, float tilt, float azimuth) {
    const Stamp stamp = MakeStamp(stroke, trail, x, y, pressure, tilt, azimuth);
    stroke.stamps.push_back(stamp);
    const float reach = stamp.radius + 2.0f;
    const RECT box{static_cast<LONG>(std::floor(x - reach)), static_cast<LONG>(std::floor(y - reach)), static_cast<LONG>(std::ceil(x + reach)),
                   static_cast<LONG>(std::ceil(y + reach))};
    stroke.bounds = Union(stroke.bounds, box);
  };

  if (!trail.started) {
    trail.started = true;
    emit(points[0].x, points[0].y, points[0].pressure, points[0].tilt, points[0].azimuth);
  }

  // A span needs the point after it to know how it bends, so the last one waits for the stroke to go on or end.
  const size_t settled = finished ? count - 1 : (count >= 3 ? count - 2 : 0);
  for (; trail.segment < settled; ++trail.segment) {
    const size_t k = trail.segment;
    const InkPoint& a = points[k == 0 ? 0 : k - 1];
    const InkPoint& b = points[k];
    const InkPoint& c = points[k + 1];
    const InkPoint& d = points[std::min(k + 2, count - 1)];
    const float chord = std::hypot(c.x - b.x, c.y - b.y);
    const double dt = c.time - b.time;
    if (dt > 1e-4) {
      const float speed = static_cast<float>(chord / dt);
      trail.speed = trail.speed == 0.0f ? speed : trail.speed * 0.6f + speed * 0.4f;
    }
    const int pieces = std::max(1, static_cast<int>(std::ceil(chord / kPieceLength)));
    PointF from{b.x, b.y};
    for (int i = 1; i <= pieces; ++i) {
      const PointF to = CatmullRom(a, b, c, d, static_cast<float>(i) / pieces);
      const float length = std::hypot(to.x - from.x, to.y - from.y);
      float along = 0.0f;
      for (;;) {
        const float t = (static_cast<float>(i - 1) + (length > 0.0f ? along / length : 0.0f)) / pieces;
        const float pressure = b.pressure + (c.pressure - b.pressure) * t;
        const float need = Spacing(stroke, trail, pressure) - trail.carry;
        if (along + need > length) {
          trail.carry += length - along;
          trail.travelled += length - along;
          break;
        }
        along += need;
        trail.travelled += need;
        trail.carry = 0.0f;
        const float f = length > 0.0f ? along / length : 0.0f;
        emit(from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, pressure, b.tilt + (c.tilt - b.tilt) * t,
             b.azimuth + (c.azimuth - b.azimuth) * t);
      }
      from = to;
    }
  }
  return first;
}

void RelayStamps(Stroke& stroke) {
  stroke.stamps.clear();
  stroke.bounds = RECT{};
  StampTrail trail;
  LayStamps(stroke, trail, true);
  const config::BrushTuning& tuning = TuningOf(stroke.tool);
  if (!stroke.mouse || tuning.taper <= 0.0f || stroke.stamps.size() < 3) return;
  const float reach = tuning.taper * stroke.width;
  float from_end = 0.0f;
  for (size_t i = stroke.stamps.size(); i-- > 0;) {
    Stamp& stamp = stroke.stamps[i];
    if (i + 1 < stroke.stamps.size()) from_end += std::hypot(stroke.stamps[i + 1].x - stamp.x, stroke.stamps[i + 1].y - stamp.y);
    if (from_end >= reach) break;
    stamp.radius *= 0.35f + 0.65f * Smoothstep(0.0f, reach, from_end);
  }
}

void DrawStamps(const Stroke& stroke, size_t first, size_t last, const RECT& area, float* mask, int stride) {
  const config::BrushTuning& tuning = TuningOf(stroke.tool);
  const bool builds = Builds(stroke.tool);
  const float grain_size = std::max(tuning.grain * stroke.scale, 0.5f);
  const uint32_t paper = 0x9E3779B9u * (static_cast<uint32_t>(stroke.tool) + 1u);
  last = std::min(last, stroke.stamps.size());
  for (size_t i = first; i < last; ++i) {
    const Stamp& stamp = stroke.stamps[i];
    const float reach = stamp.radius + 1.5f;
    const int x0 = std::max(static_cast<int>(std::floor(stamp.x - reach)), static_cast<int>(area.left));
    const int x1 = std::min(static_cast<int>(std::ceil(stamp.x + reach)), static_cast<int>(area.right));
    const int y0 = std::max(static_cast<int>(std::floor(stamp.y - reach)), static_cast<int>(area.top));
    const int y1 = std::min(static_cast<int>(std::ceil(stamp.y + reach)), static_cast<int>(area.bottom));
    if (x0 >= x1 || y0 >= y1) continue;
    const float cosine = std::cos(stamp.angle);
    const float sine = std::sin(stamp.angle);
    for (int y = y0; y < y1; ++y) {
      float* row = mask + static_cast<ptrdiff_t>(y - area.top) * stride - area.left;
      const float py = static_cast<float>(y) + 0.5f;
      const float dy = py - stamp.y;
      for (int x = x0; x < x1; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        const float dx = px - stamp.x;
        const float u = dx * cosine + dy * sine;
        const float v = dy * cosine - dx * sine;
        float cover = 0.0f;
        switch (stroke.tool) {
          case Tool::kPen:
            cover = std::clamp(0.5f - Ellipse(u, v, stamp.radius, stamp.aspect), 0.0f, 1.0f);
            break;
          case Tool::kHighlighter:
            cover = std::clamp(0.5f - Chisel(u, v, stamp.radius, stamp.aspect), 0.0f, 1.0f);
            break;
          case Tool::kPencil:
            cover = std::clamp(0.5f - Ellipse(u, v, stamp.radius, stamp.aspect) / std::max(1.0f, stamp.radius * 0.3f), 0.0f, 1.0f);
            break;
          case Tool::kCrayon: {
            const float rough = stamp.radius * (0.84f + 0.16f * Noise(px * 0.45f / stroke.scale, py * 0.45f / stroke.scale, paper + 3u));
            cover = std::clamp(rough - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            break;
          }
          case Tool::kBrush: {
            const float t = std::sqrt(dx * dx + dy * dy) / stamp.radius;
            const float body = 1.0f - Smoothstep(0.6f, 1.0f, t);
            const float rim = Smoothstep(0.55f, 0.9f, t) * (1.0f - Smoothstep(0.92f, 1.02f, t));
            cover = std::clamp(body * (1.0f - config::kBrushRim) + rim * config::kBrushRim, 0.0f, 1.0f);
            break;
          }
          default:
            cover = std::clamp(stamp.radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            break;
        }
        if (cover <= 0.0f) continue;
        if (tuning.grain > 0.0f) {
          const float grain = Grain(px, py, grain_size, paper);
          if (stroke.tool == Tool::kBrush) {
            cover *= 1.0f - tuning.grain_bite * (1.0f - grain);
          } else {
            const float soft = stroke.tool == Tool::kPencil ? 0.16f : 0.08f;
            cover *= Smoothstep(stamp.cut - soft, stamp.cut + soft, grain);
          }
        }
        cover *= stamp.alpha;
        if (cover <= 0.0f) continue;
        float& m = row[x];
        m = builds ? m + cover * (1.0f - m) : std::max(m, cover);
      }
    }
  }
}

void BlendStroke(const Stroke& stroke, const float* mask, int mask_stride, uint32_t* pixels, ptrdiff_t stride, const uint32_t* base,
                 ptrdiff_t base_stride, int width, int height) {
  const bool erase = stroke.tool == Tool::kEraser;
  const bool multiply = Multiplies(stroke.tool);
  const float opacity = std::clamp(stroke.opacity, 0.0f, 1.0f);
  const float color[3] = {static_cast<float>(stroke.color & 0xFF), static_cast<float>((stroke.color >> 8) & 0xFF),
                          static_cast<float>((stroke.color >> 16) & 0xFF)};
  for (int y = 0; y < height; ++y) {
    const float* coverage = mask + static_cast<ptrdiff_t>(y) * mask_stride;
    uint32_t* row = pixels + y * stride;
    const uint32_t* under = base + y * base_stride;
    for (int x = 0; x < width; ++x) {
      float a = coverage[x];
      if (a <= kLeast) continue;
      const uint32_t pixel = row[x];
      float channel[3] = {static_cast<float>(pixel & 0xFF), static_cast<float>((pixel >> 8) & 0xFF), static_cast<float>((pixel >> 16) & 0xFF)};
      if (erase) {
        const uint32_t back = under[x];
        for (int c = 0; c < 3; ++c) channel[c] += (static_cast<float>((back >> (c * 8)) & 0xFF) - channel[c]) * std::min(a, 1.0f);
      } else {
        a = std::min(a, 1.0f) * opacity;
        for (int c = 0; c < 3; ++c) {
          const float target = multiply ? channel[c] * color[c] / 255.0f : color[c];
          channel[c] += (target - channel[c]) * a;
        }
      }
      row[x] = 0xFF000000u | static_cast<uint32_t>(ToByte(channel[2])) << 16 | static_cast<uint32_t>(ToByte(channel[1])) << 8 | ToByte(channel[0]);
    }
  }
}

void PaintStroke(const Stroke& stroke, const float* mask, int mask_stride, uint32_t* pixels, ptrdiff_t stride, int width, int height) {
  if (stroke.tool == Tool::kEraser) return;
  const float opacity = std::clamp(stroke.opacity, 0.0f, 1.0f) * (Multiplies(stroke.tool) ? 0.85f : 1.0f);
  const float color[3] = {static_cast<float>(stroke.color & 0xFF), static_cast<float>((stroke.color >> 8) & 0xFF),
                          static_cast<float>((stroke.color >> 16) & 0xFF)};
  for (int y = 0; y < height; ++y) {
    const float* coverage = mask + static_cast<ptrdiff_t>(y) * mask_stride;
    uint32_t* row = pixels + y * stride;
    for (int x = 0; x < width; ++x) {
      const float a = std::min(coverage[x], 1.0f) * opacity;
      if (a <= kLeast) continue;
      const uint32_t pixel = row[x];
      float out[4];
      for (int c = 0; c < 4; ++c) out[c] = static_cast<float>((pixel >> (c * 8)) & 0xFF) * (1.0f - a);
      for (int c = 0; c < 3; ++c) out[c] += color[c] * a;
      out[3] += 255.0f * a;
      row[x] = static_cast<uint32_t>(ToByte(out[3])) << 24 | static_cast<uint32_t>(ToByte(out[2])) << 16 | static_cast<uint32_t>(ToByte(out[1])) << 8 |
               ToByte(out[0]);
    }
  }
}

bool Touches(const Stroke& stroke, float x, float y, float reach) {
  const RECT& b = stroke.bounds;
  if (x < static_cast<float>(b.left) - reach || x > static_cast<float>(b.right) + reach || y < static_cast<float>(b.top) - reach ||
      y > static_cast<float>(b.bottom) + reach) {
    return false;
  }
  for (const Stamp& stamp : stroke.stamps) {
    const float limit = stamp.radius + reach;
    const float dx = stamp.x - x;
    const float dy = stamp.y - y;
    if (dx * dx + dy * dy <= limit * limit) return true;
  }
  return false;
}

}  // namespace sstk
