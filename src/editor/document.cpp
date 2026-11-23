#include "sstk/editor/document.h"

#include <algorithm>

namespace sstk {

ToolBox::ToolBox() {
  for (int i = 0; i < kInkTools; ++i) {
    const config::InkDefaults& defaults = config::kInkDefaults[i];
    inks[i] = Ink{defaults.color, defaults.opacity, defaults.width};
  }
}

float ToolBox::WidthOf(Tool which, int index) const {
  index = std::clamp(index, 0, 4);
  if (which == Tool::kEraser) return config::kEraserWidths[index];
  if (!IsInk(which)) return 0.0f;
  return config::kInkDefaults[static_cast<int>(which)].widths[index];
}

float ToolBox::CurrentWidth() const {
  if (tool == Tool::kEraser) return WidthOf(Tool::kEraser, eraser_width);
  if (!IsInk(tool)) return 0.0f;
  return WidthOf(tool, inks[static_cast<int>(tool)].width);
}

ToolBox& Tools() {
  static ToolBox tools;
  return tools;
}

PointF Turn(PointF point, int turns) {
  switch (((turns % 4) + 4) % 4) {
    case 1: return PointF{-point.y, point.x};
    case 2: return PointF{-point.x, -point.y};
    case 3: return PointF{point.y, -point.x};
    default: return point;
  }
}

RECT TurnRect(const RECT& rect, int turns) {
  switch (((turns % 4) + 4) % 4) {
    case 1: return RECT{-rect.bottom, rect.left, -rect.top, rect.right};
    case 2: return RECT{-rect.right, -rect.bottom, -rect.left, -rect.top};
    case 3: return RECT{rect.top, -rect.right, rect.bottom, -rect.left};
    default: return rect;
  }
}

POINT TurnedPixel(int x, int y, int turns) {
  switch (((turns % 4) + 4) % 4) {
    case 1: return POINT{y, -x - 1};
    case 2: return POINT{-x - 1, -y - 1};
    case 3: return POINT{-y - 1, x};
    default: return POINT{x, y};
  }
}

D2D_MATRIX_3X2_F TurnMatrix(int turns) {
  switch (((turns % 4) + 4) % 4) {
    case 1: return D2D_MATRIX_3X2_F{0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    case 2: return D2D_MATRIX_3X2_F{-1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f};
    case 3: return D2D_MATRIX_3X2_F{0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    default: return D2D_MATRIX_3X2_F{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  }
}

RECT Union(const RECT& a, const RECT& b) {
  if (a.right <= a.left || a.bottom <= a.top) return b;
  if (b.right <= b.left || b.bottom <= b.top) return a;
  return RECT{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

bool Overlaps(const RECT& a, const RECT& b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

bool Same(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace sstk
