// What the editing panel makes of a screenshot: the markup drawn on it, the part kept, and which way up it is. Places
// are plane pixels, as card.h numbers them: the screen's own, reaching past it for a window or a scrolling screenshot.
#pragma once

#include "sstk/base/platform.h"
#include "sstk/config/markup.h"

#include <dcommon.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace sstk {

// In the toolbar's order. The first kInkTools lay down color.
enum class Tool { kPen, kMonoline, kHighlighter, kPencil, kCrayon, kBrush, kEraser, kLasso, kRuler };
inline constexpr int kToolCount = 9;
inline constexpr int kInkTools = 6;

inline bool IsInk(Tool tool) { return static_cast<int>(tool) < kInkTools; }

struct InkPoint {
  float x, y;
  float pressure;  // 0 to 1.
  float tilt;      // 0 upright, 1 lying flat.
  float azimuth;   // Radians, the way the pen leans.
  double time;
};

// One mark of the brush. Its shape is the tool's: round, a slanted nib, or a chisel.
struct Stamp {
  float x, y;
  float radius;  // Along the long axis.
  float aspect;  // The short axis over the long one.
  float angle;   // Radians, of the long axis.
  float alpha;
  float cut;      // The paper's grain below this level is left bare, for the tools that show it.
  float heading;  // Radians, the way the stroke was going.
  float along;    // How far along the stroke it lies, which lines up streaks from one mark to the next.
  float wet;      // How far wet ink bleeds past the mark, as a share of its radius.
  float dry;      // How much a fast brush breaks into bristle streaks.
  uint32_t grain_seed;  // Varies the marks of one stroke from each other.
};

struct Stroke {
  Tool tool = Tool::kPen;  // An ink tool, or the eraser for pixels rubbed out.
  uint32_t color = 0;      // 0xRRGGBB.
  float opacity = 1.0f;
  float width = 1.0f;      // Plane pixels.
  float scale = 1.0f;      // Plane pixels per device independent pixel, which sizes the paper's grain.
  bool mouse = false;      // Drawn with a mouse, which has no pressure.
  uint32_t seed = 0;
  std::vector<InkPoint> points;
  std::vector<Stamp> stamps;
  RECT bounds{};  // What the stamps reach.
};

using StrokeList = std::vector<std::shared_ptr<const Stroke>>;

struct EditDocument {
  StrokeList strokes;
  RECT crop{};     // The part kept.
  int option = 0;  // The panel's option it came from; 0 is a part chosen by hand.
  int source = 0;  // The picture it is cut from: the screen's, or one of a window or a scrolling screenshot.
  int turns = 0;   // Quarter turns clockwise.
};

// The tools as last left, which every editing panel starts with.
struct ToolBox {
  struct Ink {
    uint32_t color;
    float opacity;
    int width;
  };
  Tool tool = Tool::kPen;
  Tool last_ink = Tool::kPen;
  std::array<Ink, kInkTools> inks{};
  int eraser_width = config::kEraserWidth;
  bool object_eraser = false;
  uint32_t custom_color = 0x8E5CF7;
  bool custom_used = false;

  ToolBox();
  float WidthOf(Tool tool, int index) const;  // Device independent pixels.
  float CurrentWidth() const;
};

ToolBox& Tools();

// Plane to the turned picture and back, about the plane's origin. A quarter turn clockwise takes (x, y) to (-y, x).
struct PointF {
  float x, y;
};
PointF Turn(PointF point, int turns);
RECT TurnRect(const RECT& rect, int turns);
// The plane pixel a pixel of the turned picture shows.
POINT TurnedPixel(int x, int y, int turns);
D2D_MATRIX_3X2_F TurnMatrix(int turns);

RECT Union(const RECT& a, const RECT& b);
bool Overlaps(const RECT& a, const RECT& b);
bool Same(const RECT& a, const RECT& b);

}  // namespace sstk
