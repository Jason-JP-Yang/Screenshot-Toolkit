// Pixel helpers shared by the scrolling screenshot code. Pixels are BGRA, and what the screen gives as alpha says nothing.
#pragma once

#include "image.h"

#include <emmintrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace sstk {

inline const uint32_t* PixelRow(const PixelView& view, int y) {
  return reinterpret_cast<const uint32_t*>(view.data + static_cast<ptrdiff_t>(y) * view.stride);
}

inline PixelView CropView(const PixelView& view, const RECT& rect) {
  return PixelView{view.data + static_cast<ptrdiff_t>(rect.top) * view.stride + static_cast<ptrdiff_t>(rect.left) * 4, static_cast<int>(rect.right - rect.left),
                   static_cast<int>(rect.bottom - rect.top), view.stride};
}

// How far apart two colors are: the largest difference in any channel.
inline int Apart(uint32_t a, uint32_t b) {
  const __m128i x = _mm_cvtsi32_si128(static_cast<int>(a & 0xFFFFFFu));
  const __m128i y = _mm_cvtsi32_si128(static_cast<int>(b & 0xFFFFFFu));
  const uint32_t d = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_or_si128(_mm_subs_epu8(x, y), _mm_subs_epu8(y, x))));
  return static_cast<int>(std::max({d & 0xFF, (d >> 8) & 0xFF, (d >> 16) & 0xFF}));
}

inline bool Near(uint32_t a, uint32_t b, int slack) { return Apart(a, b) <= slack; }

inline __m128i SlackOf(int slack) {
  const uint32_t s = static_cast<uint32_t>(slack);
  return _mm_set1_epi32(static_cast<int>(0xFF000000u | s << 16 | s << 8 | s));
}

// One bit per pixel of four, set where the two are within the slack.
inline int Near4(const uint32_t* a, const uint32_t* b, __m128i slack) {
  const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
  const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
  const __m128i apart = _mm_or_si128(_mm_subs_epu8(x, y), _mm_subs_epu8(y, x));
  return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_subs_epu8(apart, slack), _mm_setzero_si128())));
}

}  // namespace sstk
