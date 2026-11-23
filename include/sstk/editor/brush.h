// The markup tools' marks: stamps laid along a smooth path through a stroke's points, drawn into coverage masks, and
// laid into the picture. Pencil, crayon, and brush show a paper grain fixed to the picture, so going over a mark again
// finds the same grain, as on paper.
#pragma once

#include "sstk/editor/document.h"

namespace sstk {

// How far laying a stroke in progress has got.
struct StampTrail {
  size_t segment = 0;  // The next span between points.
  float carry = 0.0f;  // Distance along the path since the last stamp.
  float travelled = 0.0f;
  bool started = false;
  float speed = 0.0f;
};

// Lays the stamps for the spans its points settle so far, or for all of them once the stroke is finished. Returns the
// index of the first new stamp.
size_t LayStamps(Stroke& stroke, StampTrail& trail, bool finished);

// Lays every stamp again from the start, as a finished stroke has them: a mouse's stroke narrows at both ends.
void RelayStamps(Stroke& stroke);

// Strides count pixels, not bytes.

// Coverage of stamps [first, last) over `area`, plane pixels, into `mask`: one float a pixel.
void DrawStamps(const Stroke& stroke, size_t first, size_t last, const RECT& area, float* mask, int stride);
inline void DrawStroke(const Stroke& stroke, const RECT& area, float* mask, int stride) {
  DrawStamps(stroke, 0, stroke.stamps.size(), area, mask, stride);
}

// Lays a stroke's coverage into opaque pixels. `base` is the picture under every stroke, which rubbing out brings back.
void BlendStroke(const Stroke& stroke, const float* mask, int mask_stride, uint32_t* pixels, ptrdiff_t stride, const uint32_t* base,
                 ptrdiff_t base_stride, int width, int height);

// The same over clear premultiplied pixels, as nearly as a stroke that multiplies can be shown on nothing.
void PaintStroke(const Stroke& stroke, const float* mask, int mask_stride, uint32_t* pixels, ptrdiff_t stride, int width, int height);

// Whether any of its stamps comes within `reach` of a point.
bool Touches(const Stroke& stroke, float x, float y, float reach);

// How far from its center a stamp may leave a mark: a soft or ragged edge, a pencil on its side, and wet ink bleeding
// past it included.
inline float StampReach(const Stamp& stamp) { return stamp.radius * (1.35f + 1.4f * stamp.wet) + 2.0f; }

}  // namespace sstk
