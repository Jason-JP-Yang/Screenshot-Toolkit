#include "stitcher.h"

#include "config/core.h"

#include <emmintrin.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sstk {

namespace {

constexpr int kCell = 16;
constexpr int kStripSlices = 6;
constexpr int kNoEdge = 0x80;
constexpr size_t kMaxCandidates = 6;
constexpr int kVotesPerStrip = 3;
constexpr float kVoteCost = 0.5f;
constexpr float kStillCost = 0.04f;
constexpr uint32_t kStripMinEnergy = 64;
constexpr uint32_t kCompareMinEnergy = 128;
constexpr int kEdgeFaint = 12;
constexpr int kEdgeSlack = 24;
constexpr int kEvidenceMin = 4;
constexpr int kStepMin = 12;  // Over blue + 2 * green + red, so three levels of brightness.
constexpr int kStepRowEvery = 8;
constexpr uint32_t kSeparatorMinRows = 12;
constexpr uint64_t kSeparatorPercent = 92;
constexpr int kMarginFilledPercent = 85;
constexpr uint32_t kFollowEveryPixels = 800;  // In a row, per reading, for it to count as following the scroll.
constexpr int kMinOverlap = 48;

// How far a pixel of the capture can be believed, in its low bits, and what else was seen of it.
constexpr uint8_t kEmpty = 0;
constexpr uint8_t kSuspect = 1;         // Seen blurred, by the pointer, or where something may lie over it.
constexpr uint8_t kSeen = 2;            // Seen clearly, but not yet following the scroll.
constexpr uint8_t kConfirmed = 3;       // Seen following the scroll, away from the pointer.
constexpr uint8_t kRankMask = 0x07;
constexpr uint8_t kChanging = 0x40;     // Seen changing as the content moved, so it was content.
constexpr uint8_t kBehind = 0x80;       // Seen holding still while content moved, so it was not.

constexpr int kPlateKnown = 2;          // Times in a row a still pixel must look the same to be known.
constexpr int kSettledVotes = 4;        // More readings agreeing than not, for a look of content to be settled.
constexpr int kOverlayMin = 3;          // Readings in which a pixel must tint what moves under it.
constexpr int kSeedMin = 2;             // Readings in which a pixel must hide content seen elsewhere.
constexpr int kGrowStep = 24;           // Over the color channels, between neighbors of one surface.
constexpr int kBackgroundSlack = 30;    // Over the color channels, from a plain background's color.
constexpr int kBridge = 3;              // How close two marks are to be one thing.
constexpr size_t kFrontMinPixels = 6;
constexpr double kPlainShare = 0.6;     // Of a column, for its most common color to be a plain background.
constexpr int kVersionTile = 64;
constexpr size_t kMaxPlates = 4;
constexpr uint64_t kUnsettledPercent = 50;  // Of what changed on its edges in a sharp reading, in neither way, for it to say nothing.
constexpr int kSharpEdge = 24;              // On the edge map, a step this large is something drawn sharply.
constexpr int kUnsettledReadings = 3;       // Such readings in a row before the view counts as changed for good.

alignas(16) constexpr uint8_t kNoEdgeBytes[16] = {kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge,
                                                  kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge};

inline uint32_t Sad16(const uint8_t* a, const uint8_t* b) {
  const __m128i sad = _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(sad)) + static_cast<uint32_t>(_mm_extract_epi16(sad, 4));
}

inline uint32_t Energy16(const uint8_t* a) { return Sad16(a, kNoEdgeBytes); }

uint32_t SadBytes(const uint8_t* a, const uint8_t* b, int count) {
  uint32_t sum = 0;
  int i = 0;
  for (; i + 16 <= count; i += 16) sum += Sad16(a + i, b + i);
  for (; i < count; ++i) sum += static_cast<uint32_t>(std::abs(a[i] - b[i]));
  return sum;
}

void LumaRow(const uint8_t* pixels, int width, uint8_t* out) {
  const __m128i mask = _mm_set1_epi32(0xFF);
  const __m128i zero = _mm_setzero_si128();
  const auto luma = [&](__m128i v) {
    const __m128i blue = _mm_and_si128(v, mask);
    const __m128i green = _mm_and_si128(_mm_srli_epi32(v, 8), mask);
    const __m128i red = _mm_and_si128(_mm_srli_epi32(v, 16), mask);
    return _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(blue, red), _mm_slli_epi32(green, 1)), 2);
  };
  int x = 0;
  for (; x + 8 <= width; x += 8) {
    const __m128i first = luma(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + static_cast<ptrdiff_t>(x) * 4)));
    const __m128i second = luma(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + static_cast<ptrdiff_t>(x) * 4 + 16)));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(out + x), _mm_packus_epi16(_mm_packs_epi32(first, second), zero));
  }
  for (; x < width; ++x) {
    const uint8_t* p = pixels + static_cast<ptrdiff_t>(x) * 4;
    out[x] = static_cast<uint8_t>((p[0] + 2 * p[1] + p[2]) >> 2);
  }
}

// Each value is how far a pixel stands out from its right and lower neighbors, and nothing at all below the dead zone.
void EdgeRow(const uint8_t* line, const uint8_t* below, int width, uint8_t* out) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i dead = _mm_set1_epi16(static_cast<short>(config::kScrollEdgeDeadZone));
  const __m128i neg_dead = _mm_set1_epi16(static_cast<short>(-config::kScrollEdgeDeadZone));
  const __m128i flip = _mm_set1_epi8(static_cast<char>(kNoEdge));
  int x = 0;
  for (; x + 9 <= width; x += 8) {
    const __m128i here = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(line + x)), zero);
    const __m128i right = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(line + x + 1)), zero);
    const __m128i down = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(below + x)), zero);
    __m128i value = _mm_sub_epi16(_mm_sub_epi16(_mm_slli_epi16(here, 1), right), down);
    value = _mm_and_si128(value, _mm_or_si128(_mm_cmpgt_epi16(value, dead), _mm_cmplt_epi16(value, neg_dead)));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(out + x), _mm_xor_si128(_mm_packs_epi16(value, value), flip));
  }
  for (; x < width - 1; ++x) {
    int value = 2 * line[x] - line[x + 1] - below[x];
    if (std::abs(value) <= config::kScrollEdgeDeadZone) value = 0;
    out[x] = static_cast<uint8_t>(std::clamp(value, -127, 127) + kNoEdge);
  }
  if (x < width) out[x] = kNoEdge;
}

uint32_t CellSad(const EdgeFrame& a, int x, int ya, const EdgeFrame& b, int yb) {
  uint32_t sum = 0;
  for (int i = 0; i < kCell; ++i) sum += Sad16(a.Row(ya + i) + x, b.Row(yb + i) + x);
  return sum;
}

bool WithinShare(uint64_t part, uint64_t whole, float share) { return static_cast<double>(part) <= static_cast<double>(whole) * share; }

inline const uint32_t* PixelRow(const PixelView& view, int y) { return reinterpret_cast<const uint32_t*>(view.data + static_cast<ptrdiff_t>(y) * view.stride); }

void CopyFrame(const PixelView& frame, std::vector<uint32_t>& to) {
  for (int y = 0; y < frame.height; ++y) std::memcpy(to.data() + static_cast<size_t>(y) * frame.width, PixelRow(frame, y), static_cast<size_t>(frame.width) * 4);
}

// The same color to within a few levels in every channel. Alpha says nothing on screen.
inline __m128i PixelSlack() {
  const uint32_t slack = static_cast<uint32_t>(config::kScrollPixelSlack);
  return _mm_set1_epi32(static_cast<int>(0xFF000000u | slack << 16 | slack << 8 | slack));
}

inline int Near4(const uint32_t* a, const uint32_t* b, __m128i slack) {
  const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
  const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
  const __m128i apart = _mm_or_si128(_mm_subs_epu8(x, y), _mm_subs_epu8(y, x));
  return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_subs_epu8(apart, slack), _mm_setzero_si128())));
}

inline bool Near(uint32_t a, uint32_t b) {
  constexpr int slack = config::kScrollPixelSlack;
  return std::abs(static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF)) <= slack &&
         std::abs(static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF)) <= slack &&
         std::abs(static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF)) <= slack;
}

inline int Distance(uint32_t a, uint32_t b) {
  return std::abs(static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF)) + std::abs(static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF)) +
         std::abs(static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF));
}

inline void Bump(uint8_t& count) {
  if (count < 255) ++count;
}

// One pixel of the capture, as the readings so far have it.
struct Held {
  uint32_t& color;
  uint8_t& edge;
  uint8_t& state;
  uint8_t& votes;

  void Take(uint32_t now, uint8_t now_edge, uint8_t now_state) {
    color = now;
    edge = now_edge;
    state = now_state;
    votes = 1;
  }

  // A clear sight of what is there, not yet seen following the scroll. What most such sights agree on stands, so a
  // first sight under something that went away gives way to what is seen later.
  void VoteColor(uint32_t now, uint8_t now_edge, bool changing) {
    const uint8_t flags = changing ? kChanging : 0;
    if ((state & kRankMask) < kSeen) {
      Take(now, now_edge, kSeen | flags);
    } else if ((state & kBehind) == 0 && Near(color, now)) {
      Bump(votes);
      state |= flags;
    } else if (--votes == 0) {
      Take(now, now_edge, kSeen | flags);
    }
  }

  // A sight of content already seen following the scroll. Whether it agrees decides between two ways it has looked,
  // and disagreeing time after time at one place in the view gives away something tinting it there.
  bool VoteConfirmed(uint32_t now, uint8_t now_edge, bool counts, uint8_t& dissent) {
    if (Near(color, now)) {
      if (counts) Bump(votes);
      return false;
    }
    // Only a settled look says the other is the odd one out, whichever was seen first.
    if (votes >= kSettledVotes) Bump(dissent);
    if (!counts || --votes != 0) return false;
    Take(now, now_edge, kConfirmed);
    return true;
  }

  // A sight of something holding still where it is, which is not what scrolls there.
  void VoteBehind(uint32_t now, uint8_t now_edge) {
    if ((state & kRankMask) < kSeen) {
      Take(now, now_edge, kSeen | kBehind);
    } else if ((state & kBehind) != 0) {
      Bump(votes);
    } else if (--votes == 0) {
      Take(now, now_edge, kSeen | kBehind);
    }
  }
};

inline uint32_t Bin(uint32_t color) { return ((color >> 3) & 0x1F) | (((color >> 11) & 0x1F) << 5) | (((color >> 19) & 0x1F) << 10); }

uint32_t Mix(uint32_t a, uint32_t b, int t, int total) {
  const auto channel = [&](int shift) {
    return static_cast<uint32_t>((static_cast<int>((a >> shift) & 0xFF) * (total - t) + static_cast<int>((b >> shift) & 0xFF) * t) / total);
  };
  return 0xFF000000u | channel(16) << 16 | channel(8) << 8 | channel(0);
}

// The most common color among some, to within a few levels, and how much of them it is.
uint32_t MostCommon(const std::vector<uint32_t>& colors, std::vector<uint32_t>& bins, double* share) {
  *share = 0.0;
  if (colors.empty()) return 0xFF000000u;
  bins.resize(colors.size());
  for (size_t i = 0; i < colors.size(); ++i) bins[i] = Bin(colors[i]);
  std::sort(bins.begin(), bins.end());
  uint32_t best = bins[0];
  size_t best_run = 0;
  for (size_t i = 0; i < bins.size();) {
    size_t j = i;
    while (j < bins.size() && bins[j] == bins[i]) ++j;
    if (j - i > best_run) {
      best_run = j - i;
      best = bins[i];
    }
    i = j;
  }
  uint64_t sums[3] = {0, 0, 0};
  uint64_t count = 0;
  for (const uint32_t color : colors) {
    if (Bin(color) != best) continue;
    sums[0] += color & 0xFF;
    sums[1] += (color >> 8) & 0xFF;
    sums[2] += (color >> 16) & 0xFF;
    ++count;
  }
  *share = static_cast<double>(best_run) / static_cast<double>(colors.size());
  return 0xFF000000u | static_cast<uint32_t>(sums[2] / count) << 16 | static_cast<uint32_t>(sums[1] / count) << 8 | static_cast<uint32_t>(sums[0] / count);
}

std::vector<uint8_t> Dilate(const std::vector<uint8_t>& mask, int width, int height, int radius) {
  std::vector<uint8_t> across(mask.size(), 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* in = mask.data() + static_cast<size_t>(y) * width;
    uint8_t* out = across.data() + static_cast<size_t>(y) * width;
    int last = INT_MIN / 2;
    for (int x = 0; x < width; ++x) {
      if (in[x] != 0) last = x;
      if (x - last <= radius) out[x] = 1;
    }
    last = INT_MAX / 2;
    for (int x = width - 1; x >= 0; --x) {
      if (in[x] != 0) last = x;
      if (last - x <= radius) out[x] = 1;
    }
  }
  std::vector<uint8_t> result(mask.size(), 0);
  std::vector<int> last(static_cast<size_t>(width), INT_MIN / 2);
  for (int y = 0; y < height; ++y) {
    const uint8_t* in = across.data() + static_cast<size_t>(y) * width;
    uint8_t* out = result.data() + static_cast<size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      if (in[x] != 0) last[x] = y;
      if (y - last[x] <= radius) out[x] = 1;
    }
  }
  std::fill(last.begin(), last.end(), INT_MAX / 2);
  for (int y = height - 1; y >= 0; --y) {
    const uint8_t* in = across.data() + static_cast<size_t>(y) * width;
    uint8_t* out = result.data() + static_cast<size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      if (in[x] != 0) last[x] = y;
      if (last[x] - y <= radius) out[x] = 1;
    }
  }
  return result;
}

// The fewest rows after which a background repeats itself, if it does so throughout.
int FindPeriod(const std::vector<uint32_t>& color, const std::vector<uint8_t>& known, int width, int left, int right, int top, int bottom) {
  std::vector<size_t> marks;
  for (int y = top; y + 1 < bottom; ++y) {
    for (int x = left; x + 1 < right; ++x) {
      const size_t i = static_cast<size_t>(y) * width + x;
      if (known[i] == 0) continue;
      if ((known[i + 1] != 0 && Distance(color[i], color[i + 1]) > kGrowStep) ||
          (known[i + width] != 0 && Distance(color[i], color[i + width]) > kGrowStep)) {
        marks.push_back(i);
      }
    }
  }
  if (marks.size() < 200) return 0;
  if (marks.size() > 6000) {
    const size_t step = marks.size() / 6000 + 1;
    size_t kept = 0;
    for (size_t i = 0; i < marks.size(); i += step) marks[kept++] = marks[i];
    marks.resize(kept);
  }
  for (int period = 8; period * 2 <= bottom - top; ++period) {
    size_t compared = 0;
    size_t matched = 0;
    for (const size_t i : marks) {
      if (static_cast<int>(i / width) + period >= bottom) continue;
      const size_t j = i + static_cast<size_t>(period) * width;
      if (known[j] == 0) continue;
      ++compared;
      if (Distance(color[i], color[j]) <= kBackgroundSlack / 2) ++matched;
    }
    if (compared * 3 >= marks.size() && matched * 100 >= compared * 97) return period;
  }
  return 0;
}

// Fills what was never seen of a background: from where a repeating one brings it round, or else blended from what was
// seen nearest along its row, or else from the nearest row with anything seen.
void Inpaint(std::vector<uint32_t>& color, std::vector<uint8_t>& known, int width, int height, int left, int right, int period, const std::vector<uint32_t>& plain) {
  if (period > 0) {
    for (int y = 0; y < height; ++y) {
      for (int x = left; x < right; ++x) {
        const size_t i = static_cast<size_t>(y) * width + x;
        if (known[i] != 0) continue;
        for (int other = y % period; other < height; other += period) {
          const size_t j = static_cast<size_t>(other) * width + x;
          if (other == y || known[j] != 1) continue;
          color[i] = color[j];
          known[i] = 2;
          break;
        }
      }
    }
  }
  std::vector<uint8_t> has(static_cast<size_t>(height), 0);
  for (int y = 0; y < height; ++y) {
    uint32_t* line = color.data() + static_cast<size_t>(y) * width;
    const uint8_t* seen = known.data() + static_cast<size_t>(y) * width;
    int last = -1;
    for (int x = left; x < right; ++x) {
      if (seen[x] == 0) continue;
      if (last < 0) {
        std::fill(line + left, line + x, line[x]);
      } else {
        for (int k = last + 1; k < x; ++k) line[k] = Mix(line[last], line[x], k - last, x - last);
      }
      last = x;
    }
    if (last < 0) continue;
    std::fill(line + last + 1, line + right, line[last]);
    has[y] = 1;
  }
  std::vector<int> above(static_cast<size_t>(height), -1);
  std::vector<int> below(static_cast<size_t>(height), -1);
  for (int y = 0, seen = -1; y < height; ++y) {
    if (has[y] != 0) seen = y;
    above[y] = seen;
  }
  for (int y = height - 1, seen = -1; y >= 0; --y) {
    if (has[y] != 0) seen = y;
    below[y] = seen;
  }
  for (int y = 0; y < height; ++y) {
    if (has[y] != 0) continue;
    int from = above[y];
    if (from < 0 || (below[y] >= 0 && below[y] - y < y - from)) from = below[y];
    uint32_t* line = color.data() + static_cast<size_t>(y) * width;
    if (from < 0) {
      std::copy(plain.begin() + left, plain.begin() + right, line + left);
    } else {
      std::copy_n(color.data() + static_cast<size_t>(from) * width + left, right - left, line + left);
    }
  }
}

}  // namespace

void EdgeFrame::Allocate(int width, int height) {
  if (width == width_ && height == height_) return;
  width_ = width;
  height_ = height;
  slices_ = width / kCell;
  cell_rows_ = height / kCell;
  strips_ = slices_ == 0 ? 0 : std::max(slices_ / kStripSlices, 1);
  data_.resize(static_cast<size_t>(width) * height);
  slice_sums_.resize(static_cast<size_t>(height + 1) * slices_);
  profile_.resize(static_cast<size_t>(strips_) * height);
  profile_sums_.resize(static_cast<size_t>(strips_) * (height + 1));
}

void EdgeFrame::Summarize() {
  std::fill_n(slice_sums_.begin(), slices_, 0u);
  for (int strip = 0; strip < strips_; ++strip) profile_sums_[static_cast<size_t>(strip) * (height_ + 1)] = 0;
  for (int y = 0; y < height_; ++y) {
    const uint8_t* row = Row(y);
    const uint32_t* above = slice_sums_.data() + static_cast<size_t>(y) * slices_;
    uint32_t* sums = slice_sums_.data() + static_cast<size_t>(y + 1) * slices_;
    for (int slice = 0; slice < slices_; ++slice) sums[slice] = above[slice] + Energy16(row + static_cast<ptrdiff_t>(slice) * kCell);
    for (int strip = 0; strip < strips_; ++strip) {
      const int first = strip * kStripSlices;
      const int last = strip == strips_ - 1 ? slices_ : first + kStripSlices;
      uint32_t sum = 0;
      for (int slice = first; slice < last; ++slice) sum += sums[slice] - above[slice];
      const uint32_t value = std::min<uint32_t>(sum * 3 / static_cast<uint32_t>((last - first) * 8), 255u);
      profile_[static_cast<size_t>(strip) * height_ + y] = static_cast<uint8_t>(value);
      const size_t at = static_cast<size_t>(strip) * (height_ + 1) + y;
      profile_sums_[at + 1] = profile_sums_[at] + value;
    }
  }
}

void EdgeFrame::Build(const PixelView& pixels) {
  Allocate(pixels.width, pixels.height);
  for (auto& line : line_) line.resize(static_cast<size_t>(width_) + 16);
  uint8_t* here = line_[0].data();
  uint8_t* below = line_[1].data();
  LumaRow(pixels.data, width_, here);
  for (int y = 0; y + 1 < height_; ++y) {
    LumaRow(pixels.data + static_cast<ptrdiff_t>(y + 1) * pixels.stride, width_, below);
    EdgeRow(here, below, width_, data_.data() + static_cast<size_t>(y) * width_);
    std::swap(here, below);
  }
  // The last row has nothing below it to compare with, so it says nothing.
  if (height_ > 0) std::memset(data_.data() + static_cast<size_t>(height_ - 1) * width_, kNoEdge, static_cast<size_t>(width_));
  Summarize();
}

void EdgeFrame::BuildFromEdges(const uint8_t* rows, ptrdiff_t stride, int width, int height) {
  Allocate(width, height);
  for (int y = 0; y < height; ++y) std::memcpy(data_.data() + static_cast<size_t>(y) * width, rows + y * stride, static_cast<size_t>(width));
  Summarize();
}

uint32_t EdgeFrame::SliceEnergy(int slice, int first, int last) const {
  return slice_sums_[static_cast<size_t>(last) * slices_ + slice] - slice_sums_[static_cast<size_t>(first) * slices_ + slice];
}

uint32_t EdgeFrame::ProfileSum(int strip, int first, int last) const {
  const size_t base = static_cast<size_t>(strip) * (height_ + 1);
  return profile_sums_[base + last] - profile_sums_[base + first];
}

uint64_t EdgeFrame::TotalEnergy() const {
  uint64_t total = 0;
  for (int slice = 0; slice < slices_; ++slice) total += slice_sums_[static_cast<size_t>(height_) * slices_ + slice];
  return total;
}

bool EdgeFrame::SameAs(const EdgeFrame& other) const {
  return width_ == other.width_ && height_ == other.height_ && !data_.empty() && std::memcmp(data_.data(), other.data_.data(), data_.size()) == 0;
}

// Each strip of columns proposes the shifts that line its rows up best, so a strip holding fixed content costs only itself.
void MotionFinder::Collect(const EdgeFrame& before, const EdgeFrame& after, int rest, int low, int high, int expected) {
  candidates_.clear();
  if (high < low) return;
  const int span = high - low + 1;
  votes_.assign(static_cast<size_t>(span), 0.0f);
  costs_.resize(static_cast<size_t>(span));
  const int rows_after = focus_.bottom;
  const int rows_before = before.height();
  const int top = focus_.top;
  const int overlap = std::max(kMinOverlap, (rows_after - top) / 8);
  const int first_strip = std::min(static_cast<int>(focus_.left) / (kCell * kStripSlices), after.strips() - 1);
  const int last_strip = std::min((static_cast<int>(focus_.right) - 1) / (kCell * kStripSlices), after.strips() - 1);
  for (int strip = first_strip; strip <= last_strip; ++strip) {
    if (after.ProfileSum(strip, top, rows_after) < kStripMinEnergy) continue;
    const uint8_t* now = after.Profile(strip);
    const uint8_t* was = before.Profile(strip);
    {
      const int from = std::max(top, -rest);
      const int to = std::min(rows_after, rows_before - rest);
      if (to > from) {
        const uint32_t sum = after.ProfileSum(strip, from, to) + before.ProfileSum(strip, from + rest, to + rest);
        if (WithinShare(SadBytes(now + from, was + from + rest, to - from), sum, kStillCost)) continue;
      }
    }
    for (int i = 0; i < span; ++i) {
      const int shift = low + i;
      costs_[i] = 2.0f;
      if (shift == 0) continue;
      const int at = rest + shift;
      const int from = std::max(top, -at);
      const int to = std::min(rows_after, rows_before - at);
      if (to - from < overlap) continue;
      const uint32_t sum = after.ProfileSum(strip, from, to) + before.ProfileSum(strip, from + at, to + at);
      if (sum < kCompareMinEnergy) continue;
      costs_[i] = static_cast<float>(SadBytes(now + from, was + from + at, to - from)) / static_cast<float>(sum);
    }
    int picks[kVotesPerStrip];
    int picked = 0;
    for (int i = 0; i < span; ++i) {
      const float cost = costs_[i];
      if (cost >= kVoteCost || (i > 0 && costs_[i - 1] < cost) || (i + 1 < span && costs_[i + 1] < cost)) continue;
      int place = picked;
      while (place > 0 && costs_[picks[place - 1]] > cost) --place;
      if (place >= kVotesPerStrip) continue;
      const int end = std::min(picked, kVotesPerStrip - 1);
      for (int move = end; move > place; --move) picks[move] = picks[move - 1];
      picks[place] = i;
      picked = std::min(picked + 1, kVotesPerStrip);
    }
    for (int p = 0; p < picked; ++p) votes_[picks[p]] += kVoteCost - costs_[picks[p]];
  }
  for (size_t n = 0; n < kMaxCandidates; ++n) {
    const auto most = std::max_element(votes_.begin(), votes_.end());
    if (*most <= 0.0f) break;
    candidates_.push_back(low + static_cast<int>(most - votes_.begin()));
    *most = 0.0f;
  }
  if (expected != 0 && expected >= low && expected <= high && std::find(candidates_.begin(), candidates_.end(), expected) == candidates_.end()) {
    candidates_.push_back(expected);
  }
}

MotionFinder::Trial MotionFinder::Evaluate(const EdgeFrame& before, const EdgeFrame& after, int rest, int shift) const {
  Trial trial;
  trial.shift = shift;
  const int at = rest + shift;
  const int columns = after.slices();
  int last_row = -1;
  for (int cy = cells_.top; cy < cells_.bottom; ++cy) {
    const int y0 = cy * kCell;
    const int other = y0 + at;
    if (other < 0 || other + kCell > before.height()) continue;
    for (int cx = cells_.left; cx < cells_.right; ++cx) {
      const size_t i = static_cast<size_t>(cy) * columns + cx;
      const uint32_t total = energy_[i] + before.SliceEnergy(cx, other, other + kCell);
      if (total < 2 * config::kScrollBlankCell) continue;
      const uint32_t sad = CellSad(after, cx * kCell, y0, before, other);
      const bool follows = WithinShare(sad, total, config::kScrollFollowShare);
      if (follows && stays_[i] == 0) {
        if (last_row != cy) ++trial.rows;
        last_row = cy;
        ++trial.moving;
        trial.score += total;
        trial.sad += sad;
      } else if (!follows && stays_[i] == 0) {
        ++trial.changed;
      }
    }
  }
  return trial;
}

int MotionFinder::CountLoose(const EdgeFrame& before, const EdgeFrame& after, int rest, int shift, int* comparable) const {
  int count = 0;
  const int at = rest + shift;
  const int columns = after.slices();
  for (int cy = cells_.top; cy < cells_.bottom; ++cy) {
    const int y0 = cy * kCell;
    const int other = y0 + at;
    if (other < 0 || other + kCell > before.height()) continue;
    for (int cx = cells_.left; cx < cells_.right; ++cx) {
      const uint32_t total = energy_[static_cast<size_t>(cy) * columns + cx] + before.SliceEnergy(cx, other, other + kCell);
      if (total < 2 * config::kScrollBlankCell) continue;
      ++*comparable;
      if (WithinShare(CellSad(after, cx * kCell, y0, before, other), total, config::kScrollLooseShare)) ++count;
    }
  }
  return count;
}

MotionFinder::Trial MotionFinder::Search(const EdgeFrame& before, const EdgeFrame& after, int rest, int low, int high, int expected) {
  Collect(before, after, rest, low, high, expected);
  Trial best;
  const auto distance = [&](int shift) { return std::abs(expected != 0 ? shift - expected : shift); };
  for (const int shift : candidates_) {
    const Trial trial = Evaluate(before, after, rest, shift);
    const bool clearly = static_cast<double>(trial.score) > static_cast<double>(best.score) * 1.05;
    const bool equally = static_cast<double>(trial.score) * 1.05 >= static_cast<double>(best.score);
    if (clearly || (equally && distance(shift) < distance(best.shift)) || best.score == 0) {
      if (trial.score > 0) best = trial;
    }
  }
  return best;
}

void MotionFinder::Analyze(const EdgeFrame& before, const EdgeFrame& after, int rest, int expected, Motion& motion, const RECT* focus) {
  const int columns = after.slices();
  const int rows = after.cell_rows();
  const size_t count = static_cast<size_t>(columns) * rows;
  focus_ = focus != nullptr ? *focus : RECT{0, 0, after.width(), after.height()};
  cells_ = RECT{focus_.left / kCell, focus_.top / kCell, std::min<LONG>((focus_.right + kCell - 1) / kCell, columns), std::min<LONG>((focus_.bottom + kCell - 1) / kCell, rows)};
  motion.moved = false;
  motion.blurred = false;
  motion.shift = 0;
  motion.moving = 0;
  motion.moving_energy = motion.moving_sad = motion.fixed_energy = motion.changed_energy = 0;
  motion.cells.assign(count, CellState::kBlank);
  motion.changed_box = RECT{columns, rows, 0, 0};
  energy_.resize(count);
  stays_.assign(count, 0);

  for (int cy = 0; cy < rows; ++cy) {
    const int y0 = cy * kCell;
    const int other = y0 + rest;
    const bool inside = other >= 0 && other + kCell <= before.height();
    for (int cx = 0; cx < columns; ++cx) {
      const size_t i = static_cast<size_t>(cy) * columns + cx;
      energy_[i] = after.SliceEnergy(cx, y0, y0 + kCell);
      if (!inside) continue;
      const uint32_t total = energy_[i] + before.SliceEnergy(cx, other, other + kCell);
      if (total < 2 * config::kScrollBlankCell) continue;
      stays_[i] = WithinShare(CellSad(after, cx * kCell, y0, before, other), total, config::kScrollStayShare) ? 1 : 0;
    }
  }

  const int overlap = std::max(kMinOverlap, static_cast<int>(focus_.bottom - focus_.top) / 8);
  const int low = -(static_cast<int>(focus_.bottom) - overlap) - rest;
  const int high = before.height() - static_cast<int>(focus_.top) - overlap - rest;
  const int reach = std::max(256, static_cast<int>(focus_.bottom - focus_.top) / 3);
  Trial best = Search(before, after, rest, std::max(low, -reach), std::min(high, reach), expected);
  const bool strong = best.moving >= 8 && best.moving >= best.changed * 2;
  if (!strong && (low < -reach || high > reach)) {
    const Trial wide = Search(before, after, rest, low, high, expected);
    if (wide.score > best.score) best = wide;
  }
  // A single line drawn across the view would match itself anywhere, so a match must hold over several rows, the more
  // so the further it reaches.
  const bool far_away = std::abs(best.shift) > reach;
  motion.moved = best.moving >= (far_away ? config::kScrollMinMovingCellsAlone : config::kScrollMinMovingCells) && best.rows >= (far_away ? 4 : 2) &&
                 static_cast<double>(best.moving) >= static_cast<double>(best.moving + best.changed) * config::kScrollMovingShare;
  motion.shift = motion.moved ? best.shift : 0;
  if (!motion.moved) {
    // Caught between two pixel rows, a reading is too blurred for its cells to be trusted, but still says where it is.
    int best_count = 0;
    int best_shift = 0;
    candidates_.push_back(0);
    for (const int shift : candidates_) {
      int comparable = 0;
      const int loose = CountLoose(before, after, rest, shift, &comparable);
      if (loose > best_count && loose >= config::kScrollMinMovingCellsAlone && loose * 2 >= comparable) {
        best_count = loose;
        best_shift = shift;
      }
    }
    if (best_count > 0) {
      motion.blurred = true;
      motion.moved = best_shift != 0;
      motion.shift = best_shift;
    }
  }

  const int at = rest + motion.shift;
  for (int cy = 0; cy < rows; ++cy) {
    const int y0 = cy * kCell;
    const int other = y0 + at;
    const bool inside = other >= 0 && other + kCell <= before.height();
    for (int cx = 0; cx < columns; ++cx) {
      const size_t i = static_cast<size_t>(cy) * columns + cx;
      const bool stays = stays_[i] != 0;
      const bool counted = cx >= cells_.left && cx < cells_.right && cy >= cells_.top && cy < cells_.bottom;
      uint32_t total = energy_[i];
      CellState state = CellState::kBlank;
      if (inside) {
        total += before.SliceEnergy(cx, other, other + kCell);
        if (total >= 2 * config::kScrollBlankCell) {
          if (motion.moved) {
            const uint32_t sad = CellSad(after, cx * kCell, y0, before, other);
            const bool follows = WithinShare(sad, total, motion.blurred ? config::kScrollLooseShare : config::kScrollFollowShare);
            state = follows && stays ? CellState::kBoth : follows ? CellState::kMoving : stays ? CellState::kFixed : CellState::kChanged;
            if (state == CellState::kMoving && counted) {
              ++motion.moving;
              motion.moving_energy += total;
              motion.moving_sad += sad;
            }
          } else {
            state = stays ? CellState::kFixed : CellState::kChanged;
          }
        }
      } else if (stays) {
        state = CellState::kFixed;
      } else if (energy_[i] >= config::kScrollBlankCell) {
        state = motion.moved ? CellState::kEntered : CellState::kChanged;
      }
      if (state == CellState::kFixed && counted) motion.fixed_energy += total;
      if (state == CellState::kChanged && counted) {
        motion.changed_energy += total;
        motion.changed_box.left = std::min<LONG>(motion.changed_box.left, cx);
        motion.changed_box.top = std::min<LONG>(motion.changed_box.top, cy);
        motion.changed_box.right = std::max<LONG>(motion.changed_box.right, cx + 1);
        motion.changed_box.bottom = std::max<LONG>(motion.changed_box.bottom, cy + 1);
      }
      motion.cells[i] = state;
    }
  }
}

void Evidence::Reset(int width, int height) {
  for (auto* column : {&col_follow, &col_stay, &col_other, &col_faint}) column->assign(static_cast<size_t>(width), 0);
  row_follow.assign(static_cast<size_t>(height), 0);
  row_edge.assign(static_cast<size_t>(height), 0);
  samples = 0;
  frames = 0;
}

// Rows are all counted, and columns only for rows in [first, last), so that what lies above and below the area stays out.
void Evidence::Add(const EdgeFrame& before, const EdgeFrame& after, int shift, int first, int last) {
  const int from = std::max(0, -shift);
  const int to = std::min(after.height(), before.height() - shift);
  const int width = after.width();
  const __m128i blank = _mm_set1_epi8(static_cast<char>(kNoEdge));
  ++frames;
  // Every other row, a different half each time, so that a thin line is not missed for good.
  for (int y = from + (frames & 1); y < to; y += 2) {
    const uint8_t* now = after.Row(y);
    const uint8_t* moved = before.Row(y + shift);
    const uint8_t* still = before.Row(y);
    const bool columns = y >= first && y < last;
    if (columns) ++samples;
    for (int x0 = 0; x0 + 16 <= width; x0 += 16) {
      const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(now + x0));
      if (_mm_movemask_epi8(_mm_cmpeq_epi8(block, blank)) == 0xFFFF) continue;
      for (int x = x0; x < x0 + 16; ++x) {
        const int value = now[x];
        if (std::abs(value - kNoEdge) <= kEdgeFaint) {
          if (columns && value != kNoEdge && std::abs(value - still[x]) <= 3 && std::abs(value - moved[x]) > 3) ++col_faint[x];
          continue;
        }
        ++row_edge[y];
        const bool follows = std::abs(value - moved[x]) <= kEdgeSlack;
        const bool stays = std::abs(value - still[x]) <= kEdgeSlack;
        if (follows && !stays) ++row_follow[y];
        if (!columns) continue;
        if (follows && !stays) {
          ++col_follow[x];
        } else if (stays && !follows) {
          ++col_stay[x];
        } else if (!stays && !follows) {
          ++col_other[x];
        }
      }
    }
  }
}

uint64_t Evidence::Still(int x) const { return static_cast<uint64_t>(col_stay[x]) + col_other[x] + col_faint[x]; }

bool Evidence::Barrier(int x) const {
  const uint64_t other = Still(x);
  return other >= std::max<uint64_t>(8, static_cast<uint64_t>(static_cast<double>(samples) * config::kScrollBarrierShare)) && static_cast<uint64_t>(col_follow[x]) * 8 <= other;
}

ScrollPlan PlanScrollArea(const EdgeFrame& before, const EdgeFrame& after, const Motion& motion, POINT pointer, bool wheel, float scale,
                          const std::vector<RECT>& hints) {
  ScrollPlan plan;
  if (!motion.moved) return plan;
  if (!wheel && motion.moving < config::kScrollMinMovingCellsAlone) return plan;
  const int columns = after.slices();
  const int rows = after.cell_rows();
  std::vector<int> per_slice(static_cast<size_t>(columns), 0);
  std::vector<int> per_row(static_cast<size_t>(rows), 0);
  for (int cy = 0; cy < rows; ++cy) {
    for (int cx = 0; cx < columns; ++cx) {
      if (motion.cells[static_cast<size_t>(cy) * columns + cx] != CellState::kMoving) continue;
      ++per_slice[cx];
      ++per_row[cy];
    }
  }
  const auto extent = [&](const std::vector<int>& row_counts, int* first, int* last) {
    *first = -1;
    *last = -1;
    const auto supported = [&](int cy) {
      if (row_counts[cy] == 0) return false;
      if (row_counts[cy] >= 2) return true;
      for (int other = std::max(cy - 2, 0); other <= std::min(cy + 2, rows - 1); ++other) {
        if (other != cy && row_counts[other] > 0) return true;
      }
      return false;
    };
    for (int cy = 0; cy < rows; ++cy) {
      if (!supported(cy)) continue;
      if (*first < 0) *first = cy;
      *last = cy;
    }
  };
  int first_row = 0;
  int last_row = 0;
  extent(per_row, &first_row, &last_row);
  if (first_row < 0) return plan;

  Evidence evidence;
  evidence.Reset(after.width(), after.height());
  evidence.Add(before, after, motion.shift, first_row * kCell, (last_row + 1) * kCell);

  int seed = -1;
  if (wheel && pointer.x >= 0 && pointer.x < after.width()) {
    const int from = std::min(static_cast<int>(pointer.x) / kCell, columns - 1);
    for (int step = 0; step < columns && seed < 0; ++step) {
      if (from - step >= 0 && per_slice[from - step] > 0) {
        seed = from - step;
      } else if (from + step < columns && per_slice[from + step] > 0) {
        seed = from + step;
      }
    }
  }
  if (seed < 0) seed = static_cast<int>(std::max_element(per_slice.begin(), per_slice.end()) - per_slice.begin());

  int left = seed * kCell;
  int right = std::min((seed + 1) * kCell, after.width());
  while (left > 0 && !evidence.Barrier(left - 1)) --left;
  while (right < after.width() && !evidence.Barrier(right)) ++right;
  if (right - left < static_cast<int>(config::kScrollAreaMinWidth * scale)) return plan;

  std::vector<int> within(static_cast<size_t>(rows), 0);
  int moving_within = 0;
  for (int cy = 0; cy < rows; ++cy) {
    for (int cx = 0; cx < columns; ++cx) {
      const int x = cx * kCell;
      if (x < left || x + kCell > right || motion.cells[static_cast<size_t>(cy) * columns + cx] != CellState::kMoving) continue;
      ++within[cy];
      ++moving_within;
    }
  }
  extent(within, &first_row, &last_row);
  if (first_row < 0) return plan;
  // A stray match at the side, as in a scroll bar, says nothing about the columns the content takes.
  const int enough = std::max(1, moving_within / columns / 8);
  int first_slice = left / kCell;
  while (first_slice < columns - 1 && per_slice[first_slice] < enough) ++first_slice;
  int last_slice = std::min((right - 1) / kCell, columns - 1);
  while (last_slice > first_slice && per_slice[last_slice] < enough) --last_slice;
  const RECT box{first_slice * kCell, first_row * kCell, (last_slice + 1) * kCell, (last_row + 1) * kCell};
  if (!wheel && box.bottom - box.top < static_cast<int>(config::kScrollAreaMinHeight * scale)) return plan;

  // Of the rectangles the app reported, the smallest around the motion. What is read from then on is that or the whole
  // window, and what the capture keeps of it is settled by how the view is divided, not by how wide the content runs.
  const RECT* chosen = nullptr;
  long chosen_area = LONG_MAX;
  for (const RECT& hint : hints) {
    // Cells reach a little past what they hold.
    if (hint.left >= box.left + kCell || hint.top >= box.top + kCell || hint.right <= box.right - kCell || hint.bottom <= box.bottom - kCell) continue;
    const long area = (hint.right - hint.left) * (hint.bottom - hint.top);
    if (area < chosen_area) {
      chosen = &hint;
      chosen_area = area;
    }
  }
  plan.found = true;
  if (chosen != nullptr) {
    plan.hinted = true;
    plan.rect = RECT{std::max<LONG>(chosen->left, 0), std::max<LONG>(chosen->top, 0), std::min<LONG>(chosen->right, after.width()),
                     std::min<LONG>(chosen->bottom, after.height())};
  } else {
    plan.rect = RECT{0, 0, after.width(), after.height()};
  }
  plan.box = box;
  OffsetRect(&plan.box, -plan.rect.left, -plan.rect.top);
  return plan;
}

RowPlane::~RowPlane() {
  if (base_ != nullptr) VirtualFree(base_, 0, MEM_RELEASE);
}

bool RowPlane::Reserve(size_t stride, int rows, int origin) {
  stride_ = stride;
  origin_ = origin;
  base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, stride * rows, MEM_RESERVE, PAGE_READWRITE));
  return base_ != nullptr;
}

bool RowPlane::Commit(int first, int last) {
  if (first >= last) return true;
  const auto span = [&](int from, int to) { return VirtualAlloc(Row(from), stride_ * (to - from), MEM_COMMIT, PAGE_READWRITE) != nullptr; };
  if (!any_) {
    if (!span(first, last)) return false;
    first_ = first;
    last_ = last;
    any_ = true;
    return true;
  }
  if (first < first_) {
    if (!span(first, first_)) return false;
    first_ = first;
  }
  if (last > last_) {
    if (!span(last_, last)) return false;
    last_ = last;
  }
  return true;
}

bool Stitcher::Start(const PixelView& first, float scale, int limit_rows, bool hinted, const RECT& box, POINT pointer) {
  width_ = first.width;
  height_ = first.height;
  scale_ = scale;
  SetBand(pointer);
  limit_rows_ = limit_rows;
  hinted_ = hinted;
  box_ = box;
  before_.Build(first);
  slices_ = before_.slices();
  cell_rows_ = before_.cell_rows();
  if (slices_ == 0 || cell_rows_ == 0) return false;
  const int origin = limit_rows + height_;
  const int capacity = origin * 2 + height_;
  if (!pixels_.Reserve(static_cast<size_t>(width_) * 4, capacity, origin) || !edges_.Reserve(static_cast<size_t>(width_), capacity, origin) ||
      !states_.Reserve(static_cast<size_t>(width_), capacity, origin) || !votes_.Reserve(static_cast<size_t>(width_), capacity, origin) ||
      !sources_.Reserve(static_cast<size_t>(slices_) * sizeof(uint16_t), capacity, origin)) {
    return false;
  }
  evidence_.Reset(width_, height_);
  step_up_.assign(static_cast<size_t>(width_), 0);
  step_down_.assign(static_cast<size_t>(width_), 0);
  const size_t view = static_cast<size_t>(width_) * height_;
  for (auto* counts : {&follow_n_, &stay_n_, &neither_n_, &occluded_n_, &dissent_n_}) counts->assign(view, 0);
  plates_.assign(1, Plate{std::vector<uint32_t>(view, 0), std::vector<uint8_t>(view, 0)});
  tile_columns_ = (width_ + kVersionTile - 1) / kVersionTile;
  const size_t tiles = static_cast<size_t>(tile_columns_) * ((height_ + kVersionTile - 1) / kVersionTile);
  tile_compared_.assign(tiles, 0);
  tile_differ_.assign(tiles, 0);
  clean_.resize(view);
  top_frame_.resize(view);
  bottom_frame_.resize(view);
  SetFollow(box.top + kCell, box.bottom - kCell - 1);
  if (!Commit(0)) return false;
  for (int y = 0; y < height_; ++y) {
    std::memcpy(pixels_.Row(y), PixelRow(first, y), static_cast<size_t>(width_) * 4);
    std::memcpy(edges_.Row(y), before_.Row(y), static_cast<size_t>(width_));
    std::memset(states_.Row(y), y >= band_first_ && y <= band_last_ ? kSuspect : kSeen, static_cast<size_t>(width_));
    std::memset(votes_.Row(y), 1, static_cast<size_t>(width_));
  }
  CopyFrame(first, clean_);
  clean_position_ = 0;
  history_.assign(1, {0, 0});
  KeepEnds(first, 0, true);
  changed_first_ = 0;
  changed_last_ = height_;
  return true;
}

bool Stitcher::Commit(int position) {
  const int first = position;
  const int last = position + height_;
  if (!pixels_.Commit(first, last) || !edges_.Commit(first, last) || !states_.Commit(first, last) || !votes_.Commit(first, last) || !sources_.Commit(first, last)) {
    return false;
  }
  // Rows never seen say nothing about where edges are.
  const auto clear = [&](int from, int to) {
    for (int row = from; row < to; ++row) std::memset(edges_.Row(row), kNoEdge, static_cast<size_t>(width_));
  };
  if (!committed_) {
    clear(first, last);
    committed_ = true;
    committed_first_ = first;
    committed_last_ = last;
    return true;
  }
  if (first < committed_first_) {
    clear(first, committed_first_);
    committed_first_ = first;
  }
  if (last > committed_last_) {
    clear(committed_last_, last);
    committed_last_ = last;
  }
  return true;
}

void Stitcher::SetBand(POINT pointer) {
  const int half = std::min(static_cast<int>(std::lround(config::kScrollPointerBand * scale_)), height_ / 4);
  band_first_ = INT_MAX;
  band_last_ = -1;
  if (pointer.x >= -kCell && pointer.x < width_ + kCell && pointer.y >= -half && pointer.y < height_ + half) {
    band_first_ = std::max(static_cast<int>(pointer.y) - half, 0);
    band_last_ = std::min(static_cast<int>(pointer.y) + half, height_ - 1);
  }
}

// The readings at either end of the capture, kept until the end. Coming to rest short of where it passed through
// outranks that only where what it passed was the empty space an elastic end shows.
void Stitcher::KeepEnds(const PixelView& frame, int position, bool at_rest) {
  const int first = std::clamp(follow_first_, 0, height_ - 1);
  const int last = std::clamp(follow_last_ + 1, first + 1, height_);
  bool top = position <= top_position_;
  if (!top && at_rest && position <= rest_min_) top = RowsBlank(top_position_ + first, position + first, 0, width_);
  if (top) {
    CopyFrame(frame, top_frame_);
    top_position_ = position;
    top_band_first_ = band_first_;
    top_band_last_ = band_last_;
  }
  bool bottom = position >= bottom_position_;
  if (!bottom && at_rest && position >= rest_max_) bottom = RowsBlank(position + last, bottom_position_ + last, 0, width_);
  if (bottom) {
    CopyFrame(frame, bottom_frame_);
    bottom_position_ = position;
    bottom_band_first_ = band_first_;
    bottom_band_last_ = band_last_;
  }
}

// How the view is divided: a color step that runs all the way down, on the far side of which something holds still,
// is where one part of the display ends and another begins, however narrow the content scrolling in it.
bool Stitcher::Separator(int x, int far_first, int far_last) const {
  if (step_rows_ < kSeparatorMinRows) return false;
  const uint32_t most = std::max(step_up_[x], step_down_[x]);
  if (static_cast<uint64_t>(most) * 100 < static_cast<uint64_t>(step_rows_) * kSeparatorPercent) return false;
  const uint64_t enough = std::max<uint64_t>(8, evidence_.samples / 100);
  for (int column = far_first; column < far_last; ++column) {
    if (evidence_.Still(column) >= enough) return true;
  }
  return false;
}

// The part of the display content scrolls in, around every column it has been seen moving in.
void Stitcher::PaneColumns(bool panels, int* left, int* right) const {
  *left = 0;
  *right = width_;
  int from = std::clamp(static_cast<int>(box_.left), 0, width_ - 1);
  int to = std::clamp(static_cast<int>(box_.right), from + 1, width_);
  for (int x = 0; x < from; ++x) {
    if (evidence_.col_follow[x] < kEvidenceMin) continue;
    from = x;
    break;
  }
  for (int x = width_ - 1; x >= to; --x) {
    if (evidence_.col_follow[x] < kEvidenceMin) continue;
    to = x + 1;
    break;
  }
  const auto bounds = [&](int x, int far_first, int far_last) { return Separator(x, far_first, far_last) || (panels && evidence_.Barrier(x)); };
  for (int x = from - 1; x >= 0; --x) {
    if (!bounds(x, 0, x)) continue;
    *left = x + 1;
    break;
  }
  for (int x = to - 1; x + 1 < width_; ++x) {
    if (!bounds(x, x + 1, width_)) continue;
    *right = x + 1;
    break;
  }
  // A column of content standing on a pattern looks divided from it too, but a pattern fills everything on both sides.
  if (*left > 0 && *right < width_ && StaticFilled(0, *left) && StaticFilled(*right, width_)) {
    *left = 0;
    *right = width_;
  }
}

bool Stitcher::StaticFilled(int from, int to) const {
  const int first = std::clamp(follow_first_, 0, height_ - 1);
  const int last = std::clamp(follow_last_ + 1, first + 1, height_);
  uint64_t pixels = 0;
  uint64_t still = 0;
  for (int y = first; y < last; y += 2) {
    const uint8_t* stays = stay_n_.data() + static_cast<size_t>(y) * width_;
    for (int x = from; x < to; ++x) {
      ++pixels;
      if (stays[x] >= kPlateKnown) ++still;
    }
  }
  return pixels > 0 && still * 100 >= pixels * kMarginFilledPercent;
}

// Something seems to lie behind the content that does not scroll, so columns holding still need not be panels.
bool Stitcher::BackgroundLikely() const { return behind_count_ >= std::max<uint64_t>(500, static_cast<uint64_t>(width_) * height_ / 200); }

// Keeps track of the color steps along the rows, which is how the display's own divisions show. Static things do not
// need every reading, and the content moving through tells them from what merely lines up for a while.
void Stitcher::UpdateSteps(const PixelView& frame) {
  const int first = std::max(follow_first_, 0);
  const int last = std::min(follow_last_ + 1, height_);
  const int phase = step_tick_++ % kStepRowEvery;
  for (int y = first + phase; y < last; y += kStepRowEvery) {
    const uint8_t* pixels = frame.data + static_cast<ptrdiff_t>(y) * frame.stride;
    int here = pixels[0] + 2 * pixels[1] + pixels[2];
    for (int x = 0; x + 1 < width_; ++x) {
      pixels += 4;
      const int next = pixels[0] + 2 * pixels[1] + pixels[2];
      if (next - here >= kStepMin) {
        ++step_up_[x];
      } else if (here - next >= kStepMin) {
        ++step_down_[x];
      }
      here = next;
    }
    ++step_rows_;
  }
}

// A sharp reading says what follows the scroll and what stays, measured from the last sharp one, however many blurred
// ones came between.
void Stitcher::Note(const PixelView& frame, int position, bool sharp) {
  if (!sharp) {
    if (anchor_is_before_) {
      std::swap(anchor_, before_);
      anchor_is_before_ = false;
    }
    return;
  }
  UpdateSteps(frame);
  const EdgeFrame& earlier = anchor_is_before_ ? before_ : anchor_;
  const int shift = position - anchor_position_;
  if (shift != 0) {
    evidence_.Add(earlier, after_, shift, std::min(follow_first_, static_cast<int>(box_.top)), std::max(follow_last_ + 1, static_cast<int>(box_.bottom)));
    // Rows where something really follows the scroll, by a share of what could have, as a stray match says nothing and
    // what shows through a frosted bar follows only a little. An edge reading looks one row down, so the row above the
    // first that moves seems to follow it too.
    const uint32_t enough = std::max<uint32_t>(kEvidenceMin, static_cast<uint32_t>(static_cast<uint64_t>(evidence_.frames) * width_ / kFollowEveryPixels));
    int first = 0;
    while (first < height_ && evidence_.row_follow[first] < enough) ++first;
    int last = height_ - 1;
    while (last > first && evidence_.row_follow[last] < enough) --last;
    if (first < height_) SetFollow(first == 0 ? 0 : first + 1, last);
  }
  anchor_is_before_ = true;
  anchor_position_ = position;
}

void Stitcher::SetFollow(int first, int last) {
  follow_first_ = first;
  follow_last_ = last;
}

Stitcher::Step Stitcher::Feed(const PixelView& frame, POINT pointer) {
  after_.Build(frame);
  SetBand(pointer);
  if (after_.SameAs(before_)) {
    rest_min_ = std::min(rest_min_, position_);
    rest_max_ = std::max(rest_max_, position_);
    // Held still, a reading that could not be compared shows how the view now is.
    if (unsettled_ > 0 && Observe(frame, position_, true)) KeepEnds(frame, position_, true);
    return Step::kSame;
  }

  // Only the part of the display that scrolls has a say. Static text around it would only propose shifts of its own.
  int pane_left = 0;
  int pane_right = width_;
  PaneColumns(!BackgroundLikely(), &pane_left, &pane_right);
  const int slack = height_ / 8;
  const RECT focus{pane_left, std::clamp(std::min(follow_first_, static_cast<int>(box_.top)) - slack, 0, height_ - 1), pane_right,
                   std::clamp(std::max(follow_last_ + 1, static_cast<int>(box_.bottom)) + slack, 1, height_)};
  finder_.Analyze(before_, after_, 0, last_shift_, motion_, &focus);
  if (!motion_.moved && !motion_.blurred && motion_.changed_energy > motion_.fixed_energy) {
    // Too far from the last reading, or caught between two. The rows held reach further, and are sharper.
    const RECT box = motion_.changed_box;
    last_lost_replaced_ = (box.right - box.left) * kCell >= (focus.right - focus.left) * config::kScrollReplacedShare && (box.bottom - box.top) * kCell >= (focus.bottom - focus.top) * config::kScrollReplacedShare;
    const int first = std::max(position_ - height_, min_position_);
    const int last = std::min(position_ + height_ * 2, max_position_ + height_);
    reference_.BuildFromEdges(edges_.Row(first), static_cast<ptrdiff_t>(edges_.stride()), width_, last - first);
    finder_.Analyze(reference_, after_, position_ - first, last_shift_, motion_, &focus);
    if (!motion_.moved && !motion_.blurred && motion_.changed_energy > motion_.fixed_energy) return Step::kLost;
  }
  if (!motion_.moved) {
    // Coming to rest from a blurred reading is a sharper one, not a change. Going the other way adds nothing.
    const bool sharp = !motion_.blurred || after_.TotalEnergy() >= before_.TotalEnergy();
    // Placed by a blurred reading, it may sit a row off. A sharp one says where it really is.
    if (sharp && !settled_) {
      bool clean = false;
      const int position = Settle(after_, motion_, position_, &clean, true);
      if (Commit(position)) {
        position_ = position;
        min_position_ = std::min(min_position_, position);
        max_position_ = std::max(max_position_, position);
      }
    }
    settled_ = sharp;
    rest_min_ = std::min(rest_min_, position_);
    rest_max_ = std::max(rest_max_, position_);
    Note(frame, position_, sharp);
    if (!sharp) {
      Fill(frame, position_);
    } else if (Observe(frame, position_, true)) {
      KeepEnds(frame, position_, true);
    }
    std::swap(before_, after_);
    return Step::kRest;
  }

  bool clean = false;
  const int position = Settle(after_, motion_, position_ + motion_.shift, &clean, false);
  clean = clean && !motion_.blurred;
  if (std::abs(position) > limit_rows_ || !Commit(position)) return Step::kLost;
  settled_ = clean;
  Note(frame, position, clean);
  if (!clean) {
    Fill(frame, position);
  } else if (Observe(frame, position, false)) {
    KeepEnds(frame, position, false);
  }
  position_ = position;
  last_shift_ = motion_.shift;
  min_position_ = std::min(min_position_, position);
  max_position_ = std::max(max_position_, position);
  std::swap(before_, after_);
  return Step::kMoved;
}

bool Stitcher::Observe(const PixelView& frame, int position, bool at_rest) {
  if (position == clean_position_) {
    RestUpdate(frame, position);
  } else if (!Classify(frame, position)) {
    Fill(frame, position);
    // The last sharp reading stays what others are compared with, unless the view has changed for good, as it has
    // when it keeps looking so while nothing moves.
    unsettled_ = at_rest ? unsettled_ + 1 : 1;
    if (unsettled_ < kUnsettledReadings) return false;
  } else {
    history_.emplace_back(position, static_cast<int>(plates_.size()) - 1);
  }
  unsettled_ = 0;
  CopyFrame(frame, clean_);
  clean_position_ = position;
  return true;
}

bool Stitcher::Overlay(size_t index) const {
  return (neither_n_[index] >= kOverlayMin && neither_n_[index] >= 2 * (follow_n_[index] + stay_n_[index])) ||
         (dissent_n_[index] >= kOverlayMin && dissent_n_[index] * 2 >= follow_n_[index]);
}

bool Stitcher::Suspect(size_t index, bool band) const { return band || occluded_n_[index] >= kSeedMin || Overlay(index); }

void Stitcher::MarkChanged(int row) {
  changed_first_ = std::min(changed_first_, row);
  changed_last_ = std::max(changed_last_, row + 1);
}

// Compares a sharp reading with the last one, pixel by pixel. Where it shows what the scroll brought there and not what
// was there before, content follows the scroll. Where it shows what was there before and not what the scroll brought,
// something holds still: a background behind the content, or something fixed in front of it. What changed in neither
// way changed by itself, or shows the content through something tinted.
bool Stitcher::Classify(const PixelView& frame, int position) {
  const int shift = position - clean_position_;
  const __m128i slack = PixelSlack();
  // Where something is drawn sharply, most of it changed in neither way: blurred after all, or shaded anew all over,
  // it says nothing pixel by pixel. A tint coming and going over plain parts says nothing of that.
  {
    uint64_t telling = 0;
    uint64_t unsettled = 0;
    for (int y = 0; y < height_; y += 4) {
      const int other = y + shift;
      if (other < 0 || other >= height_ || (y >= band_first_ && y <= band_last_)) continue;
      const uint32_t* now = PixelRow(frame, y);
      const uint32_t* was = clean_.data() + static_cast<size_t>(y) * width_;
      const uint32_t* moved = clean_.data() + static_cast<size_t>(other) * width_;
      const uint8_t* edges = after_.Row(y);
      for (int x = 0; x < width_; ++x) {
        if (std::abs(edges[x] - kNoEdge) < kSharpEdge) continue;
        const bool stay = Near(now[x], was[x]);
        const bool follow = Near(now[x], moved[x]);
        if (stay && follow) continue;
        ++telling;
        if (!stay && !follow) ++unsettled;
      }
    }
    if (telling >= 64 && unsettled * 100 > telling * kUnsettledPercent) return false;
  }
  std::fill(tile_compared_.begin(), tile_compared_.end(), 0u);
  std::fill(tile_differ_.begin(), tile_differ_.end(), 0u);
  uint64_t compared = 0;
  uint64_t differ = 0;
  for (int y = 0; y < height_; ++y) {
    const uint32_t* now = PixelRow(frame, y);
    const uint32_t* was = clean_.data() + static_cast<size_t>(y) * width_;
    const int other = y + shift;
    const uint32_t* moved = other >= 0 && other < height_ ? clean_.data() + static_cast<size_t>(other) * width_ : nullptr;
    const int row = position + y;
    uint32_t* canvas = reinterpret_cast<uint32_t*>(pixels_.Row(row));
    uint8_t* edges = edges_.Row(row);
    uint8_t* states = states_.Row(row);
    uint8_t* votes = votes_.Row(row);
    uint16_t* sources = reinterpret_cast<uint16_t*>(sources_.Row(row));
    const uint8_t* edges_now = after_.Row(y);
    // By the pointer, hover effects come and go. Nothing seen there says what anything is, and it only fills gaps.
    const bool band = y >= band_first_ && y <= band_last_;
    // Nor does anything seen above or below where content moves, such as a bar it slides under.
    const bool aside = y < follow_first_ || y > follow_last_;
    const size_t base = static_cast<size_t>(y) * width_;
    bool wrote = false;
    for (int x0 = 0; x0 < width_; x0 += 4) {
      const int count = std::min(4, width_ - x0);
      int stays = 0;
      int follows = 0;
      if (count == 4) {
        stays = Near4(now + x0, was + x0, slack);
        follows = moved != nullptr ? Near4(now + x0, moved + x0, slack) : 0;
        // Plain, over content already seen following the scroll: nothing to learn or to keep.
        if ((stays & follows) == 0xF && (states[x0] & kRankMask) == kConfirmed && (states[x0 + 1] & kRankMask) == kConfirmed &&
            (states[x0 + 2] & kRankMask) == kConfirmed && (states[x0 + 3] & kRankMask) == kConfirmed) {
          continue;
        }
      } else {
        for (int b = 0; b < count; ++b) {
          if (Near(now[x0 + b], was[x0 + b])) stays |= 1 << b;
          if (moved != nullptr && Near(now[x0 + b], moved[x0 + b])) follows |= 1 << b;
        }
      }
      for (int b = 0; b < count; ++b) {
        const int x = x0 + b;
        const size_t v = base + x;
        const bool stay = ((stays >> b) & 1) != 0;
        const bool follow = ((follows >> b) & 1) != 0;
        const uint32_t color = now[x];
        Held held{canvas[x], edges[x], states[x], votes[x]};
        const int rank = held.state & kRankMask;
        uint16_t& source = sources[std::min(x / kCell, slices_ - 1)];
        if (band) {
          if (rank == kEmpty || (rank == kSuspect && follow && !stay)) {
            held.Take(color, edges_now[x], kSuspect);
            wrote = true;
          }
          continue;
        }
        // Where something may lie over the content, what is seen there only fills gaps.
        const bool suspect = aside || Suspect(v, false);
        const auto fill = [&] {
          if (rank != kEmpty) return;
          held.Take(color, edges_now[x], kSuspect);
          wrote = true;
        };
        // Content held there, seen moving, is hidden when something holds still over it.
        const bool content = rank == kConfirmed || (rank == kSeen && (held.state & (kChanging | kBehind)) == kChanging);
        if (follow && !stay) {
          Bump(follow_n_[v]);
          if (follow_n_[v] == 1 && stay_n_[v] >= kPlateKnown) ++behind_count_;
          // A see-through bar tints content alike in two readings, so it seems to follow the scroll too.
          if (rank == kConfirmed) {
            wrote = held.VoteConfirmed(color, edges_now[x], !suspect, dissent_n_[v]) || wrote;
            continue;
          }
          if (suspect) {
            fill();
            continue;
          }
          // Seen holding still there before, it was hidden by something in front, unless it merely looked the same.
          if ((held.state & kBehind) != 0 && !Near(held.color, color) && source < height_) Bump(occluded_n_[static_cast<size_t>(source) * width_ + x]);
          held.Take(color, edges_now[x], kConfirmed);
          wrote = true;
        } else if (stay && !follow && moved != nullptr) {
          Bump(stay_n_[v]);
          if (stay_n_[v] == kPlateKnown && follow_n_[v] >= 1) ++behind_count_;
          ObservePlate(x, y, color, &compared, &differ);
          if (content) {
            if (!Near(held.color, color)) Bump(occluded_n_[v]);
            continue;
          }
          if (suspect) {
            fill();
            continue;
          }
          held.VoteBehind(color, edges_now[x]);
          source = static_cast<uint16_t>(y);
          wrote = true;
        } else {
          const bool changing = !stay;
          if (changing && moved != nullptr) Bump(neither_n_[v]);
          // Plain, and just what has been seen holding still there, it is that rather than content.
          const Plate& plate = plates_.back();
          const bool still = !changing && plate.count[v] >= kPlateKnown && Near(plate.color[v], color);
          if (rank == kConfirmed) {
            // Plain, it still says what the content looks like, as when a picture loading has filled in.
            if (!changing && !still && moved != nullptr) wrote = held.VoteConfirmed(color, edges_now[x], !suspect, dissent_n_[v]) || wrote;
            continue;
          }
          if (suspect) {
            fill();
            continue;
          }
          if (still) {
            held.VoteBehind(color, edges_now[x]);
            source = static_cast<uint16_t>(y);
          } else {
            held.VoteColor(color, edges_now[x], changing);
          }
          wrote = true;
        }
      }
    }
    if (wrote) MarkChanged(row);
  }

  // A background that changes as a whole, all over the view, starts a version of its own.
  bool changed = false;
  if (compared >= std::max<uint64_t>(2000, static_cast<uint64_t>(width_) * height_ / 100) && differ * 100 >= compared * 30) {
    int tiles = 0;
    int differing = 0;
    for (size_t t = 0; t < tile_compared_.size(); ++t) {
      if (tile_compared_[t] < 32) continue;
      ++tiles;
      if (static_cast<uint64_t>(tile_differ_[t]) * 100 >= static_cast<uint64_t>(tile_compared_[t]) * 30) ++differing;
    }
    changed = tiles >= 4 && differing * 2 >= tiles;
  }
  version_votes_ = changed ? version_votes_ + 1 : 0;
  if (version_votes_ >= 2 && plates_.size() < kMaxPlates) {
    const size_t view = static_cast<size_t>(width_) * height_;
    plates_.push_back(Plate{std::vector<uint32_t>(view, 0), std::vector<uint8_t>(view, 0)});
    version_votes_ = 0;
  }
  return true;
}

void Stitcher::ObservePlate(int x, int y, uint32_t color, uint64_t* compared, uint64_t* differ) {
  Plate& plate = plates_.back();
  const size_t index = static_cast<size_t>(y) * width_ + x;
  uint8_t& count = plate.count[index];
  uint32_t& held = plate.color[index];
  if (count == 0) {
    held = color;
    count = 1;
    return;
  }
  const bool same = Near(held, color);
  if (count >= kPlateKnown) {
    const size_t tile = static_cast<size_t>(y / kVersionTile) * tile_columns_ + x / kVersionTile;
    ++*compared;
    ++tile_compared_[tile];
    if (!same) {
      ++*differ;
      ++tile_differ_[tile];
    }
  }
  if (same) {
    Bump(count);
  } else if (--count == 0) {
    held = color;
    count = 1;
  }
}

// At rest a reading is as sharp as it gets. It takes the place of anything seen less well, and of content changed in
// place, as a picture that has loaded, once two readings in a row agree on it.
void Stitcher::RestUpdate(const PixelView& frame, int position) {
  const __m128i slack = PixelSlack();
  for (int y = 0; y < height_; ++y) {
    const uint32_t* now = PixelRow(frame, y);
    const uint32_t* was = clean_.data() + static_cast<size_t>(y) * width_;
    const int row = position + y;
    uint32_t* canvas = reinterpret_cast<uint32_t*>(pixels_.Row(row));
    uint8_t* edges = edges_.Row(row);
    uint8_t* states = states_.Row(row);
    uint8_t* votes = votes_.Row(row);
    const uint8_t* edges_now = after_.Row(y);
    const bool band = y >= band_first_ && y <= band_last_;
    const size_t base = static_cast<size_t>(y) * width_;
    bool wrote = false;
    for (int x0 = 0; x0 < width_; x0 += 4) {
      const int count = std::min(4, width_ - x0);
      if (count == 4 && Near4(now + x0, canvas + x0, slack) == 0xF && (states[x0] & kRankMask) >= kSeen && (states[x0 + 1] & kRankMask) >= kSeen &&
          (states[x0 + 2] & kRankMask) >= kSeen && (states[x0 + 3] & kRankMask) >= kSeen) {
        continue;
      }
      for (int b = 0; b < count; ++b) {
        const int x = x0 + b;
        const size_t v = base + x;
        const uint32_t color = now[x];
        Held held{canvas[x], edges[x], states[x], votes[x]};
        const int rank = held.state & kRankMask;
        if (rank <= kSuspect) {
          const bool suspect = Suspect(v, band);
          if (rank == kSuspect && suspect) continue;
          held.Take(color, edges_now[x], suspect ? kSuspect : kSeen);
          wrote = true;
        } else if (rank == kConfirmed && follow_n_[v] > 0 && !Suspect(v, band) && Near(color, was[x]) && !Near(color, held.color)) {
          held.Take(color, edges_now[x], kConfirmed);
          wrote = true;
        }
      }
    }
    if (wrote) MarkChanged(row);
  }
}

// A blurred reading only fills what nothing better has been seen of.
void Stitcher::Fill(const PixelView& frame, int position) {
  for (int y = 0; y < height_; ++y) {
    const uint32_t* now = PixelRow(frame, y);
    const int row = position + y;
    uint32_t* canvas = reinterpret_cast<uint32_t*>(pixels_.Row(row));
    uint8_t* edges = edges_.Row(row);
    uint8_t* states = states_.Row(row);
    uint8_t* votes = votes_.Row(row);
    const uint8_t* edges_now = after_.Row(y);
    bool wrote = false;
    for (int x = 0; x < width_; ++x) {
      if ((states[x] & kRankMask) != kEmpty) continue;
      Held{canvas[x], edges[x], states[x], votes[x]}.Take(now[x], edges_now[x], kSuspect);
      wrote = true;
    }
    if (wrote) MarkChanged(row);
  }
}

// Frames caught in motion can sit between two pixel rows, and rounding must not add up over a long capture. The rows
// already held say exactly where the frame belongs, and how well it fits says whether it is sharp. At rest, nothing
// moved to compare, so everything drawn takes part.
int Stitcher::Settle(const EdgeFrame& edges, const Motion& motion, int position, bool* clean, bool at_rest) const {
  int best = position;
  double best_cost = 2.0;
  bool compared = false;
  for (const int nudge : {0, -1, 1, -2, 2}) {
    const int at = position + nudge;
    uint64_t sad = 0;
    uint64_t total = 0;
    for (int cy = 0; cy < cell_rows_; ++cy) {
      const int y0 = cy * kCell;
      if (at + y0 < min_position_ || at + y0 + kCell > max_position_ + height_) continue;
      for (int cx = 0; cx < slices_; ++cx) {
        const CellState state = motion.cells[static_cast<size_t>(cy) * slices_ + cx];
        if (at_rest ? state == CellState::kBlank : state != CellState::kMoving) continue;
        const int x = cx * kCell;
        for (int i = 0; i < kCell; ++i) {
          const uint8_t* held = edges_.Row(at + y0 + i) + x;
          const uint8_t* now = edges.Row(y0 + i) + x;
          sad += Sad16(held, now);
          total += Energy16(held) + Energy16(now);
        }
      }
    }
    if (total == 0) continue;
    compared = true;
    const double cost = static_cast<double>(sad) / static_cast<double>(total);
    if (cost < best_cost * 0.98) {
      best_cost = cost;
      best = at;
    }
  }
  *clean = compared ? best_cost <= config::kScrollCleanShare
                    : motion.moving_energy > 0 && WithinShare(motion.moving_sad, motion.moving_energy, config::kScrollCleanShare);
  return best;
}

bool Stitcher::RowsBlank(int first, int last, int left, int right) const {
  const int from = left / kCell;
  const int to = std::min(right / kCell, slices_);
  for (int row = first; row < last; ++row) {
    const uint8_t* edges = edges_.Row(row);
    for (int slice = from; slice < to; ++slice) {
      if (Energy16(edges + static_cast<ptrdiff_t>(slice) * kCell) > 32) return false;
    }
  }
  return true;
}

// Which version of the background was on screen when rows of the capture first passed the middle of the view.
int Stitcher::VersionAt(int first, int last) const {
  const int center = (first + last) / 2;
  int nearest = static_cast<int>(plates_.size()) - 1;
  int distance = INT_MAX;
  for (const auto& [position, version] : history_) {
    const int offset = center - position;
    if (offset >= height_ / 4 && offset < height_ * 3 / 4) return version;
    if (std::abs(offset - height_ / 2) < distance) {
      distance = std::abs(offset - height_ / 2);
      nearest = version;
    }
  }
  return nearest;
}

void Stitcher::Finish() {
  const Crop crop = FinalCrop();
  const Crop kept = ViewCrop();
  const int width = width_;
  const int height = height_;
  const size_t view = static_cast<size_t>(width) * height;
  const auto index = [&](int x, int y) { return static_cast<size_t>(y) * width + x; };

  // The rows of the view content was seen moving in, from the first to the last.
  std::vector<uint32_t> row_follow(static_cast<size_t>(height), 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* follows = follow_n_.data() + index(0, y);
    for (int x = 0; x < width; ++x) row_follow[y] += follows[x];
  }
  const uint32_t row_least = std::max<uint32_t>(4, static_cast<uint32_t>(width / 64));
  const int top = std::clamp(static_cast<int>(kept.top), 0, height);
  const int bottom = std::clamp(static_cast<int>(kept.bottom), top, height);
  int band_top = top;
  while (band_top < bottom && row_follow[band_top] < row_least) ++band_top;
  int band_bottom = bottom;
  while (band_bottom > band_top && row_follow[band_bottom - 1] < row_least) --band_bottom;
  const int left = crop.left;
  const int right = crop.right;
  if (band_top >= band_bottom || right <= left) return;

  // What lies behind, column by column: the color most of it shows, and whether that is most of it.
  std::vector<uint32_t> column_color(static_cast<size_t>(width), 0xFF000000u);
  std::vector<uint8_t> column_plain(static_cast<size_t>(width), 0);
  {
    std::vector<uint32_t> values;
    std::vector<uint32_t> bins;
    for (int x = left; x < right; ++x) {
      values.clear();
      for (int y = band_top; y < band_bottom; ++y) values.push_back(bottom_frame_[index(x, y)]);
      double share = 0.0;
      column_color[x] = MostCommon(values, bins, &share);
      column_plain[x] = share >= kPlainShare ? 1 : 0;
    }
  }
  const Plate& latest = plates_.back();
  const int middle = (top + bottom) / 2;
  const auto still_color = [&](size_t v, int y) { return latest.count[v] > 0 ? latest.color[v] : y < middle ? top_frame_[v] : bottom_frame_[v]; };
  const auto shown = [&](size_t v, int y) { return y < middle ? top_frame_[v] : bottom_frame_[v]; };

  // A background holding still behind the content shows between and beside it, all the way down the view. What it
  // shows between the content says what colors it is made of.
  size_t behind = 0;
  size_t behind_apart = 0;
  size_t beside = 0;
  int behind_rows = 0;
  int beside_rows = 0;
  std::vector<uint32_t> palette(4096, 0);
  const auto coarse = [](uint32_t color) { return ((color >> 4) & 0xF) | (((color >> 12) & 0xF) << 4) | (((color >> 20) & 0xF) << 8); };
  for (int y = band_top; y < band_bottom; ++y) {
    bool any_behind = false;
    bool any_beside = false;
    for (int x = left; x < right; ++x) {
      const size_t v = index(x, y);
      if (latest.count[v] < kPlateKnown || occluded_n_[v] >= kSeedMin) continue;
      if (follow_n_[v] > 0) {
        ++behind;
        any_behind = true;
        ++palette[coarse(latest.color[v])];
        if (Distance(latest.color[v], column_color[x]) > kBackgroundSlack) ++behind_apart;
      } else if (column_plain[x] == 0) {
        ++beside;
        any_beside = true;
      }
    }
    behind_rows += any_behind ? 1 : 0;
    beside_rows += any_beside ? 1 : 0;
  }
  const int band_height = band_bottom - band_top;
  const bool seen_behind = behind >= std::max<size_t>(500, view / 500) && behind_apart * 5 >= behind && behind_rows * 10 >= band_height * 3;
  fixed_background_ = seen_behind || (beside >= std::max<size_t>(2000, view / 100) && beside_rows * 2 >= band_height);
  // A color it never shows between the content, there to be seen often enough, is no part of it.
  const auto foreign = [&](uint32_t color) {
    const uint32_t bin = coarse(color);
    return static_cast<uint64_t>(palette[bin]) * 2000 < behind;
  };

  // What gives away something fixed in front: hiding content seen elsewhere, tinting what moves under it, or never
  // moving while it stands out from a plain background or from the colors of a fixed one.
  const size_t band_area = static_cast<size_t>(band_bottom - band_top) * (right - left);
  size_t overlays = 0;
  for (size_t v = 0; v < view; ++v) overlays += Overlay(v) ? 1 : 0;
  // As much tinted as that is a background changing by itself instead.
  const bool tints = overlays * 4 <= band_area;
  std::vector<uint8_t> front(view, 0);
  std::vector<size_t> queue;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const size_t v = index(x, y);
      const bool inert = follow_n_[v] == 0;
      const bool hides = occluded_n_[v] >= kSeedMin && occluded_n_[v] * 4 >= stay_n_[v];
      const bool tinted = tints && Overlay(v);
      const bool stands_out = inert && stay_n_[v] > 0 &&
                              (seen_behind ? foreign(still_color(v, y)) : column_plain[x] != 0 && Distance(still_color(v, y), column_color[x]) > kBackgroundSlack);
      if (!hides && !tinted && !stands_out) continue;
      front[v] = 1;
      queue.push_back(v);
    }
  }
  // Plain parts of the same thing count too: what never moved and continues smoothly from it.
  size_t grown = 0;
  const size_t grow_limit = view / 2;
  for (size_t head = 0; head < queue.size() && grown <= grow_limit; ++head) {
    const size_t v = queue[head];
    const int x = static_cast<int>(v % width);
    const int y = static_cast<int>(v / width);
    const uint32_t color = shown(v, y);
    const int neighbors[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
    for (const auto& [nx, ny] : neighbors) {
      if (nx < left || nx >= right || ny < top || ny >= bottom) continue;
      const size_t n = index(nx, ny);
      if (front[n] != 0 || follow_n_[n] != 0) continue;
      const uint32_t next = shown(n, ny);
      if (Distance(color, next) > kGrowStep || (column_plain[nx] != 0 && Distance(next, column_color[nx]) <= kBackgroundSlack)) continue;
      front[n] = 2;
      ++grown;
      queue.push_back(n);
    }
  }
  if (grown > grow_limit) {
    for (uint8_t& mark : front) mark = mark == 2 ? 0 : mark;
  }

  // Each thing fixed in front belongs to the end of the capture nearer to it, and shows only there. One reaching over
  // most of the view, as a rail down its side does, is split between the two.
  const std::vector<uint8_t> joined = Dilate(front, width, height, kBridge);
  std::vector<uint8_t> not_top(view, 0);     // What the reading at the top must not show,
  std::vector<uint8_t> not_bottom(view, 0);  // and the one at the bottom.
  int top_rows = band_top;
  int bottom_start = band_bottom;
  {
    std::vector<uint8_t> visited(view, 0);
    std::vector<size_t> members;
    std::vector<size_t> stack;
    for (size_t start = 0; start < view; ++start) {
      if (joined[start] == 0 || visited[start] != 0) continue;
      members.clear();
      stack.assign(1, start);
      visited[start] = 1;
      int box_left = width;
      int box_top = height;
      int box_right = 0;
      int box_bottom = 0;
      size_t pixels = 0;
      while (!stack.empty()) {
        const size_t v = stack.back();
        stack.pop_back();
        members.push_back(v);
        const int x = static_cast<int>(v % width);
        const int y = static_cast<int>(v / width);
        if (front[v] != 0) {
          ++pixels;
          box_left = std::min(box_left, x);
          box_top = std::min(box_top, y);
          box_right = std::max(box_right, x + 1);
          box_bottom = std::max(box_bottom, y + 1);
        }
        for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, height - 1); ++ny) {
          for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, width - 1); ++nx) {
            const size_t n = index(nx, ny);
            if (joined[n] == 0 || visited[n] != 0) continue;
            visited[n] = 1;
            stack.push_back(n);
          }
        }
      }
      if (pixels < kFrontMinPixels) continue;
      if ((box_bottom - box_top) * 10 > (bottom - top) * 6) {
        for (const size_t v : members) {
          const int y = static_cast<int>(v / width);
          if (y < middle) {
            not_bottom[v] = 1;
            top_rows = std::max(top_rows, y + 1);
          } else {
            not_top[v] = 1;
            bottom_start = std::min(bottom_start, y);
          }
        }
        continue;
      }
      const int area_left = std::max(box_left - kBridge, 0);
      const int area_top = std::max(box_top - kBridge, 0);
      const int area_right = std::min(box_right + kBridge, width);
      const int area_bottom = std::min(box_bottom + kBridge, height);
      const bool upper = box_top + box_bottom <= top + bottom;
      std::vector<uint8_t>& keep_out = upper ? not_bottom : not_top;
      for (int y = area_top; y < area_bottom; ++y) std::fill_n(keep_out.begin() + static_cast<ptrdiff_t>(index(area_left, y)), area_right - area_left, uint8_t{1});
      if (upper) {
        top_rows = std::max(top_rows, area_bottom);
      } else {
        bottom_start = std::min(bottom_start, area_top);
      }
    }
  }

  // The background as the view showed it, for each version of it, repeated a view at a time below the first.
  std::vector<std::vector<uint32_t>> tiles;
  int span = band_height;
  if (fixed_background_) {
    tiles.resize(plates_.size());
    std::vector<uint8_t> known(view);
    int period = 0;
    for (size_t version = plates_.size(); version-- > 0;) {
      const Plate& plate = plates_[version];
      for (size_t v = 0; v < view; ++v) known[v] = plate.count[v] >= kPlateKnown && joined[v] == 0 ? 1 : 0;
      tiles[version] = plate.color;
      if (version + 1 == plates_.size()) period = FindPeriod(tiles[version], known, width, left, right, band_top, band_bottom);
      Inpaint(tiles[version], known, width, height, left, right, period, column_color);
    }
    // Whole repeats of a pattern join up without a seam.
    if (period > 0 && period <= span) span = period * (span / period);
  }
  std::vector<int> slot_versions;
  const auto version_of_slot = [&](int slot) {
    if (slot >= static_cast<int>(slot_versions.size())) slot_versions.resize(static_cast<size_t>(slot) + 1, -1);
    int& version = slot_versions[static_cast<size_t>(slot)];
    if (version < 0) {
      const int first = top_position_ + band_top + slot * span;
      version = VersionAt(first, first + span);
    }
    return version;
  };
  const int bottom_version = fixed_background_ ? VersionAt(bottom_position_ + band_top, bottom_position_ + band_bottom) : 0;

  for (int row = crop.top; row < crop.bottom; ++row) {
    uint32_t* out = reinterpret_cast<uint32_t*>(pixels_.Row(row));
    const uint8_t* states = states_.Row(row);
    const int from_top = row - top_position_;
    const int from_bottom = row - bottom_position_;
    const bool top_end = from_top >= 0 && from_top < top_rows;
    const bool bottom_end = from_bottom >= bottom_start && from_bottom < height;
    const uint32_t* behind_row = nullptr;
    if (fixed_background_) {
      int plate_row = 0;
      int version = 0;
      if (from_bottom >= band_top) {
        plate_row = from_bottom;
        version = bottom_version;
      } else if (from_top < band_top) {
        plate_row = from_top;
        version = version_of_slot(0);
      } else {
        plate_row = band_top + (from_top - band_top) % span;
        version = version_of_slot((from_top - band_top) / span);
      }
      behind_row = tiles[static_cast<size_t>(version)].data() + index(0, std::clamp(plate_row, 0, height - 1));
    }
    // Within the view at either end, the screen there shows what is not fixed in front as well as anything can: all
    // of it on the near half, and on the far half, where what belongs to the other end may lie, what was not seen
    // clearly. Around the pointer, content seen clearly elsewhere is free of hover effects.
    const bool top_view = from_top < height;
    const bool bottom_view = from_bottom >= 0;
    const bool top_near = from_top < middle;
    const bool bottom_near = from_bottom >= middle;
    const bool top_hover = from_top >= top_band_first_ && from_top <= top_band_last_;
    const bool bottom_hover = from_bottom >= bottom_band_first_ && from_bottom <= bottom_band_last_;
    for (int x = left; x < right; ++x) {
      const size_t at_bottom = bottom_view ? index(x, from_bottom) : 0;
      const size_t at_top = top_view ? index(x, from_top) : 0;
      if (bottom_end && not_bottom[at_bottom] == 0) {
        out[x] = bottom_frame_[at_bottom];
        continue;
      }
      if (top_end && not_top[at_top] == 0) {
        out[x] = top_frame_[at_top];
        continue;
      }
      const uint8_t state = states[x];
      const int rank = state & kRankMask;
      const bool seen = rank == kConfirmed || (rank == kSeen && (state & kBehind) == 0);
      const bool use_bottom = bottom_view && not_bottom[at_bottom] == 0 && !(seen && bottom_hover);
      const bool use_top = top_view && not_top[at_top] == 0 && !(seen && top_hover);
      if (use_bottom && bottom_near) {
        out[x] = bottom_frame_[at_bottom];
      } else if (use_top && top_near) {
        out[x] = top_frame_[at_top];
      } else if (seen) {
      } else if (use_bottom) {
        out[x] = bottom_frame_[at_bottom];
      } else if (use_top) {
        out[x] = top_frame_[at_top];
      } else if (rank < kSuspect || (state & kBehind) != 0) {
        out[x] = behind_row != nullptr ? behind_row[x] : column_color[x];
      }
    }
  }
  changed_first_ = std::min(changed_first_, crop.top);
  changed_last_ = std::max(changed_last_, crop.bottom);
}

Stitcher::Crop Stitcher::ViewCrop() const {
  int left = 0;
  int right = width_;
  int view_top = 0;
  int view_bottom = height_;
  const Evidence& seen = evidence_;
  if (seen.samples == 0) {
    if (!hinted_) {
      left = box_.left;
      right = box_.right;
      view_top = box_.top;
      view_bottom = box_.bottom;
    }
    return Crop{left, right, view_top, view_bottom};
  }
  // What the app reports as scrolling is the area, whole, however its content is laid out. Otherwise the display's own
  // divisions bound it, around everything seen to move.
  if (!hinted_) {
    PaneColumns(!BackgroundLikely(), &left, &right);
    int top = 0;
    while (top < height_ && seen.row_follow[top] < kEvidenceMin) ++top;
    int bottom = height_ - 1;
    while (bottom > top && seen.row_follow[bottom] < kEvidenceMin) --bottom;
    if (top < height_) {
      const uint32_t plain = static_cast<uint32_t>(seen.frames) * static_cast<uint32_t>(right - left) / 256 + 2;
      view_top = top == 0 ? 0 : top + 1;
      view_bottom = bottom + 1;
      while (view_top > 0 && seen.row_edge[view_top - 1] <= plain) --view_top;
      while (view_bottom < height_ && seen.row_edge[view_bottom] <= plain) ++view_bottom;
    }
  }
  // The last columns may hold a scroll bar: drawn on, and not following. Columns further in doing the same are a
  // pattern that does not scroll, of which a bar could not be told apart, and is not worth losing it for.
  const int bar = static_cast<int>(std::lround(config::kScrollBarWidth * scale_));
  const int from = std::max(left, right - bar);
  const auto holds_still = [&](int x) {
    const uint64_t other = static_cast<uint64_t>(seen.col_stay[x]) + seen.col_other[x];
    return other >= std::max<uint64_t>(8, seen.samples / 50) && static_cast<uint64_t>(seen.col_follow[x]) * 8 <= other;
  };
  bool pattern = false;
  for (int x = std::max(left, from - bar); x < from && !pattern; ++x) pattern = holds_still(x);
  for (int x = from; x < right && !pattern; ++x) {
    if (!holds_still(x)) continue;
    // A bar's track lies around its thumb. A line at the very edge is only a border.
    int cut = x;
    const int track = right - x > 3 ? std::max(from, x - static_cast<int>(std::lround(6.0f * scale_))) : x;
    while (cut > track && seen.col_follow[cut - 1] == 0 && seen.col_stay[cut - 1] == 0 && seen.col_other[cut - 1] == 0) --cut;
    right = cut;
    break;
  }
  return Crop{left, right, view_top, view_bottom};
}

Stitcher::Crop Stitcher::FinalCrop() const {
  Crop crop = ViewCrop();
  crop.top += top_position_;
  crop.bottom += bottom_position_;
  return crop;
}

void Stitcher::Copy(const Crop& crop, uint8_t* to, ptrdiff_t stride) const {
  for (int row = crop.top; row < crop.bottom; ++row) {
    std::memcpy(to, pixels_.Row(row) + static_cast<ptrdiff_t>(crop.left) * 4, static_cast<size_t>(crop.right - crop.left) * 4);
    to += stride;
  }
}

bool Stitcher::replaced() const { return last_lost_replaced_; }

PixelView Stitcher::Rows(int first, int last) const { return PixelView{pixels_.Row(first), width_, last - first, width_ * 4}; }

bool Stitcher::TakeChanges(int* first, int* last) {
  if (changed_first_ >= changed_last_) return false;
  *first = changed_first_;
  *last = changed_last_;
  changed_first_ = INT_MAX;
  changed_last_ = INT_MIN;
  return true;
}

}  // namespace sstk
