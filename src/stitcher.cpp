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
constexpr int kFixedScoreMax = 12;
constexpr uint32_t kRowBlank = 24;
constexpr uint8_t kMaskEdge = 1;
constexpr uint8_t kMaskInside = 2;
constexpr int kInsideMaxPercent = 45;
constexpr uint32_t kSharperMin = 64;
constexpr int kFixedRowFirm = 4;
constexpr int kStaticReadings = 2;
constexpr int kPageColorStep = 16;
constexpr int kPageColorSlack = 400;  // Over a slice's 64 channel values.
constexpr int kStaticColorSlack = 48;

// How far a held row can be believed.
constexpr uint8_t kEmpty = 0;
constexpr uint8_t kPlaceholder = 1;  // Taken from where the content cannot be seen, only so that there is something there.
constexpr uint8_t kTentative = 2;    // Seen once, and it may yet turn out to be something fixed passing over it.
constexpr uint8_t kConfirmed = 3;    // Seen as it scrolled.
constexpr int kMinOverlap = 48;

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
  const int rows_after = after.height();
  const int rows_before = before.height();
  const int overlap = std::max(kMinOverlap, rows_after / 8);
  for (int strip = 0; strip < after.strips(); ++strip) {
    if (after.ProfileSum(strip, 0, rows_after) < kStripMinEnergy) continue;
    const uint8_t* now = after.Profile(strip);
    const uint8_t* was = before.Profile(strip);
    {
      const int from = std::max(0, -rest);
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
      const int from = std::max(0, -at);
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
    const auto top = std::max_element(votes_.begin(), votes_.end());
    if (*top <= 0.0f) break;
    candidates_.push_back(low + static_cast<int>(top - votes_.begin()));
    *top = 0.0f;
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
  for (int cy = 0; cy < after.cell_rows(); ++cy) {
    const int y0 = cy * kCell;
    const int other = y0 + at;
    if (other < 0 || other + kCell > before.height()) continue;
    for (int cx = 0; cx < columns; ++cx) {
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
  for (int cy = 0; cy < after.cell_rows(); ++cy) {
    const int y0 = cy * kCell;
    const int other = y0 + at;
    if (other < 0 || other + kCell > before.height()) continue;
    for (int cx = 0; cx < columns; ++cx) {
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

void MotionFinder::Analyze(const EdgeFrame& before, const EdgeFrame& after, int rest, int expected, Motion& motion) {
  const int columns = after.slices();
  const int rows = after.cell_rows();
  const size_t count = static_cast<size_t>(columns) * rows;
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

  const int overlap = std::max(kMinOverlap, after.height() / 8);
  const int low = -(after.height() - overlap) - rest;
  const int high = before.height() - overlap - rest;
  const int reach = std::max(256, after.height() / 3);
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
      uint32_t total = energy_[i];
      CellState state = CellState::kBlank;
      if (inside) {
        total += before.SliceEnergy(cx, other, other + kCell);
        if (total >= 2 * config::kScrollBlankCell) {
          if (motion.moved) {
            const uint32_t sad = CellSad(after, cx * kCell, y0, before, other);
            const bool follows = WithinShare(sad, total, motion.blurred ? config::kScrollLooseShare : config::kScrollFollowShare);
            state = follows && stays ? CellState::kBoth : follows ? CellState::kMoving : stays ? CellState::kFixed : CellState::kChanged;
            if (state == CellState::kMoving) {
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
      if (state == CellState::kFixed) motion.fixed_energy += total;
      if (state == CellState::kChanged) {
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
  for (auto* column : {&col_follow, &col_stay, &col_other, &col_edge, &col_faint}) column->assign(static_cast<size_t>(width), 0);
  col_first.assign(static_cast<size_t>(width), INT_MAX);
  col_last.assign(static_cast<size_t>(width), -1);
  row_follow.assign(static_cast<size_t>(height), 0);
  row_edge.assign(static_cast<size_t>(height), 0);
  samples = 0;
  frames = 0;
  first_row = INT_MAX;
  last_row = -1;
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
    if (columns) {
      ++samples;
      first_row = std::min(first_row, y);
      last_row = std::max(last_row, y);
    }
    for (int x0 = 0; x0 + 16 <= width; x0 += 16) {
      const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(now + x0));
      if (_mm_movemask_epi8(_mm_cmpeq_epi8(block, blank)) == 0xFFFF) continue;
      for (int x = x0; x < x0 + 16; ++x) {
        const int value = now[x];
        const bool stays_put = [&] {
          if (std::abs(value - kNoEdge) <= kEdgeFaint) {
            // A faint pattern that stays while the content moves is a background that does not scroll.
            if (!columns || value == kNoEdge || std::abs(value - still[x]) > 3 || std::abs(value - moved[x]) <= 3) return false;
            ++col_faint[x];
            return true;
          }
          ++row_edge[y];
          const bool follows = std::abs(value - moved[x]) <= kEdgeSlack;
          const bool stays = std::abs(value - still[x]) <= kEdgeSlack;
          if (follows && !stays) ++row_follow[y];
          if (!columns) return false;
          ++col_edge[x];
          if (follows && !stays) {
            ++col_follow[x];
          } else if (stays && !follows) {
            ++col_stay[x];
            return true;
          } else if (!stays && !follows) {
            ++col_other[x];
          }
          return false;
        }();
        if (stays_put) {
          col_first[x] = std::min(col_first[x], y);
          col_last[x] = std::max(col_last[x], y);
        }
      }
    }
  }
}

bool Evidence::Barrier(int x) const {
  const uint64_t other = static_cast<uint64_t>(col_stay[x]) + col_other[x] + col_faint[x];
  if (static_cast<uint64_t>(col_follow[x]) * 8 > other) return false;
  const uint64_t tall = std::max<uint64_t>(8, static_cast<uint64_t>(static_cast<double>(samples) * config::kScrollBarrierShare));
  if (other >= tall) return true;
  // A little of it, but all the way down, is a pattern in what does not scroll. A little in one place is something on top.
  const uint64_t sparse = std::max<uint64_t>(4, static_cast<uint64_t>(static_cast<double>(samples) * config::kScrollSparseShare));
  return other >= sparse && last_row > first_row && (col_last[x] - col_first[x]) * 2 >= last_row - first_row;
}

bool Evidence::Line(int x) const {
  return samples > 0 && static_cast<double>(col_edge[x]) >= static_cast<double>(samples) * config::kScrollLineShare &&
         static_cast<uint64_t>(col_follow[x]) * 32 <= col_edge[x];
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
  int box_left = INT_MAX;
  int box_right = 0;
  for (int cy = 0; cy < rows; ++cy) {
    for (int cx = 0; cx < columns; ++cx) {
      const int x = cx * kCell;
      if (x < left || x + kCell > right || motion.cells[static_cast<size_t>(cy) * columns + cx] != CellState::kMoving) continue;
      ++within[cy];
      box_left = std::min(box_left, x);
      box_right = std::max(box_right, x + kCell);
    }
  }
  extent(within, &first_row, &last_row);
  if (first_row < 0) return plan;
  const RECT box{box_left, first_row * kCell, box_right, (last_row + 1) * kCell};
  if (!wheel && box.bottom - box.top < static_cast<int>(config::kScrollAreaMinHeight * scale)) return plan;

  // Of the rectangles the app reported, the smallest around the motion with nothing fixed inside it.
  const int bar = static_cast<int>(std::lround(config::kScrollBarWidth * scale));
  const RECT* chosen = nullptr;
  long chosen_area = LONG_MAX;
  for (const RECT& hint : hints) {
    // Cells reach a little past what they hold.
    if (hint.left >= box.left + kCell || hint.top >= box.top + kCell || hint.right <= box.right - kCell || hint.bottom <= box.bottom - kCell) continue;
    if (hint.left < left - kCell || hint.right > right + bar) continue;
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
    plan.rect = RECT{left, 0, right, after.height()};
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

bool Stitcher::Start(const PixelView& first, float scale, int limit_rows, bool hinted, const RECT& box) {
  width_ = first.width;
  height_ = first.height;
  scale_ = scale;
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
      !quality_.Reserve(static_cast<size_t>(slices_), capacity, origin)) {
    return false;
  }
  differences_.assign(static_cast<size_t>(capacity / kCell + 1) * slices_, 0);
  difference_origin_ = origin;
  evidence_.Reset(width_, height_);
  const size_t rows = static_cast<size_t>(height_) * slices_;
  row_fixed_.assign(rows, 0);
  row_static_.assign(rows, 0);
  row_color_.assign(rows, 0);
  page_colors_.assign(64 * 255 / kPageColorStep + 2, 0);
  follow_first_ = box.top + kCell;
  follow_last_ = box.bottom - kCell - 1;
  still_.cells.assign(static_cast<size_t>(slices_) * cell_rows_, CellState::kFixed);
  if (!Commit(0)) return false;
  for (int y = 0; y < height_; ++y) {
    std::memcpy(pixels_.Row(y), first.data + static_cast<ptrdiff_t>(y) * first.stride, static_cast<size_t>(width_) * 4);
    std::memcpy(edges_.Row(y), before_.Row(y), static_cast<size_t>(width_));
    std::memset(quality_.Row(y), 1, static_cast<size_t>(slices_));
  }
  after_.Build(first);
  UpdateColors(first, still_);
  KeepEnds(first, 0, true);
  changed_first_ = 0;
  changed_last_ = height_;
  return true;
}

// The readings at either end of the capture, kept until the end. Coming to rest there outranks only passing through,
// which on an elastic end goes past the content.
void Stitcher::KeepEnds(const PixelView& frame, int position, bool at_rest) {
  const int half = height_ / 2;
  const size_t stride = static_cast<size_t>(width_) * 4;
  const auto keep = [&](std::vector<uint8_t>& to, int first, int count) {
    to.resize(stride * count);
    for (int y = 0; y < count; ++y) std::memcpy(to.data() + stride * y, frame.data + static_cast<ptrdiff_t>(first + y) * frame.stride, stride);
  };
  if (at_rest ? position <= rest_min_ : position <= top_position_) {
    keep(top_end_, 0, half);
    top_position_ = position;
  }
  if (at_rest ? position >= rest_max_ : position >= bottom_position_) {
    keep(bottom_end_, half, height_ - half);
    bottom_position_ = position;
  }
}

// Something fixed in the view at either end hides what lies under it there, and bits of what passed under it on the way
// are of no use around it. So from the first fixed row on, each end of the capture shows just what the screen showed.
void Stitcher::Finish() {
  const int half = height_ / 2;
  const size_t stride = static_cast<size_t>(width_) * 4;
  const auto holds_fixed = [&](int y) {
    // What a hover draws around the pointer can stay put too.
    if (y / kCell >= band_first_ && y / kCell <= band_last_) return false;
    const int8_t* scores = row_fixed_.data() + static_cast<size_t>(y) * slices_;
    return std::any_of(scores, scores + slices_, [](int8_t score) { return score >= kFixedRowFirm; });
  };
  const auto restore = [&](const std::vector<uint8_t>& from, int kept_first, int first, int last, int position) {
    for (int y = first; y < last; ++y) {
      std::memcpy(pixels_.Row(position + y), from.data() + stride * (y - kept_first), stride);
      changed_first_ = std::min(changed_first_, position + y);
      changed_last_ = std::max(changed_last_, position + y + 1);
    }
  };
  int bottom = half;
  while (bottom < height_ && !holds_fixed(bottom)) ++bottom;
  if (!bottom_end_.empty()) restore(bottom_end_, half, bottom, height_, bottom_position_);
  int top = half - 1;
  while (top >= 0 && !holds_fixed(top)) --top;
  if (!top_end_.empty()) restore(top_end_, 0, 0, top + 1, top_position_);
}

// A sharp reading says what follows the scroll and what stays, measured from the last sharp one, however many blurred
// ones came between.
void Stitcher::Note(int position, bool sharp) {
  if (!sharp) {
    if (anchor_is_before_) {
      std::swap(anchor_, before_);
      anchor_is_before_ = false;
    }
    return;
  }
  const EdgeFrame& earlier = anchor_is_before_ ? before_ : anchor_;
  const int shift = position - anchor_position_;
  if (shift != 0) {
    evidence_.Add(earlier, after_, shift, std::min(follow_first_, static_cast<int>(box_.top)), std::max(follow_last_ + 1, static_cast<int>(box_.bottom)));
    UpdateFixedRows(earlier, shift);
    // An edge reading looks one row down, so the row above the first that moves seems to follow it too.
    for (int y = 0; y < std::min(follow_first_, height_); ++y) {
      if (evidence_.row_follow[y] >= kEvidenceMin) {
        follow_first_ = y == 0 ? 0 : std::min(y + 1, follow_first_);
        break;
      }
    }
    for (int y = height_ - 1; y > follow_last_; --y) {
      if (evidence_.row_follow[y] >= kEvidenceMin) {
        follow_last_ = y;
        break;
      }
    }
  }
  anchor_is_before_ = true;
  anchor_position_ = position;
}

bool Stitcher::Commit(int position) {
  return pixels_.Commit(position, position + height_) && edges_.Commit(position, position + height_) && quality_.Commit(position, position + height_);
}

Stitcher::Step Stitcher::Feed(const PixelView& frame, POINT pointer) {
  after_.Build(frame);
  if (after_.SameAs(before_)) {
    rest_min_ = std::min(rest_min_, position_);
    rest_max_ = std::max(rest_max_, position_);
    if (pending_) Merge(frame, after_, still_, position_, true, pointer);
    return Step::kSame;
  }

  finder_.Analyze(before_, after_, 0, last_shift_, motion_);
  bool from_held = false;
  if (!motion_.moved && !motion_.blurred && motion_.changed_energy > motion_.fixed_energy) {
    // Too far from the last reading, or caught between two. The rows held reach further, and are sharper.
    const RECT box = motion_.changed_box;
    last_lost_replaced_ = box.right - box.left >= slices_ * config::kScrollReplacedShare && box.bottom - box.top >= cell_rows_ * config::kScrollReplacedShare;
    const int first = std::max(position_ - height_, min_position_);
    const int last = std::min(position_ + height_ * 2, max_position_ + height_);
    reference_.BuildFromEdges(edges_.Row(first), static_cast<ptrdiff_t>(edges_.stride()), width_, last - first);
    finder_.Analyze(reference_, after_, position_ - first, last_shift_, motion_);
    if (!motion_.moved && !motion_.blurred && motion_.changed_energy > motion_.fixed_energy) return Step::kLost;
    from_held = true;
  }
  if (!motion_.moved) {
    rest_min_ = std::min(rest_min_, position_);
    rest_max_ = std::max(rest_max_, position_);
    // Coming to rest from a blurred reading is a sharper one, not a change. Going the other way adds nothing.
    const bool sharp = !motion_.blurred || after_.TotalEnergy() >= before_.TotalEnergy();
    Note(position_, sharp);
    Merge(frame, after_, motion_, position_, sharp, pointer);
    if (sharp) KeepEnds(frame, position_, true);
    std::swap(before_, after_);
    return Step::kRest;
  }

  bool clean = false;
  const int position = Settle(after_, motion_, position_ + motion_.shift, &clean);
  clean = clean && !motion_.blurred;
  if (std::abs(position) > limit_rows_ || !Commit(position)) return Step::kLost;
  Note(position, clean);
  Merge(frame, after_, motion_, position, clean, pointer);
  if (clean) KeepEnds(frame, position, false);
  position_ = position;
  last_shift_ = motion_.shift;
  min_position_ = std::min(min_position_, position);
  max_position_ = std::max(max_position_, position);
  std::swap(before_, after_);
  return Step::kMoved;
}

// Frames caught in motion can sit between two pixel rows, and rounding must not add up over a long capture. The rows
// already held say exactly where the frame belongs, and how well it fits says whether it is sharp.
int Stitcher::Settle(const EdgeFrame& edges, const Motion& motion, int position, bool* clean) const {
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
        if (motion.cells[static_cast<size_t>(cy) * slices_ + cx] != CellState::kMoving) continue;
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

// Row by row and slice by slice, what stays where it is while the content moves past it. This finds the edge of a fixed
// thing to the pixel, however much moving text shares a cell with it.
void Stitcher::UpdateFixedRows(const EdgeFrame& earlier, int shift) {
  for (int y = 0; y < height_; ++y) {
    const uint8_t* now = after_.Row(y);
    const uint8_t* still = earlier.Row(y);
    const int other = y + shift;
    const uint8_t* moved = other >= 0 && other < earlier.height() ? earlier.Row(other) : nullptr;
    int8_t* score = row_fixed_.data() + static_cast<size_t>(y) * slices_;
    for (int slice = 0; slice < slices_; ++slice) {
      const int x = slice * kCell;
      const uint32_t energy = Energy16(now + x);
      if (energy < kRowBlank) continue;
      const bool stays = Sad16(now + x, still + x) * 4 <= energy + Energy16(still + x);
      const bool follows = moved != nullptr && Sad16(now + x, moved + x) * 5 <= (energy + Energy16(moved + x)) * 2;
      if (stays && !follows) {
        score[slice] = static_cast<int8_t>(std::min(score[slice] + 2, kFixedScoreMax));
      } else if (follows && !stays) {
        score[slice] = static_cast<int8_t>(std::max(score[slice] - 1, 0));
      }
    }
  }
}

// What is known to be fixed. Plain rows and columns between its edges are the inside of it, unless they span so much of
// the view as to be a gap between two fixed things. Nothing held is replaced from them, and nothing is confirmed.
void Stitcher::BuildMask(const EdgeFrame& edges) {
  const size_t count = static_cast<size_t>(height_) * slices_;
  row_mask_.resize(count);
  row_blank_.resize(count);
  for (int y = 0; y < height_; ++y) {
    const uint8_t* now = edges.Row(y);
    for (int slice = 0; slice < slices_; ++slice) {
      const size_t i = static_cast<size_t>(y) * slices_ + slice;
      row_mask_[i] = row_fixed_[i] > 0 ? kMaskEdge : 0;
      row_blank_[i] = Energy16(now + static_cast<ptrdiff_t>(slice) * kCell) < kRowBlank ? 1 : 0;
    }
  }
  const auto close = [&](size_t start, size_t step, int length) {
    int masked_at = -1;
    int run = -1;
    for (int n = 0; n < length; ++n) {
      const size_t i = start + step * n;
      if (row_mask_[i] == kMaskEdge) {
        if (masked_at >= 0 && run == masked_at + 1 && (n - run) * 100 <= length * kInsideMaxPercent) {
          for (int fill = run; fill < n; ++fill) row_mask_[start + step * fill] = kMaskInside;
        }
        masked_at = n;
        run = -1;
      } else if (row_blank_[i] != 0) {
        if (run < 0) run = n;
      } else {
        masked_at = -1;
        run = -1;
      }
    }
  };
  for (int slice = 0; slice < slices_; ++slice) close(static_cast<size_t>(slice), static_cast<size_t>(slices_), height_);
  for (int y = 0; y < height_; ++y) close(static_cast<size_t>(y) * slices_, 1, slices_);
  page_color_ = static_cast<int>(std::max_element(page_colors_.begin(), page_colors_.end()) - page_colors_.begin()) * kPageColorStep;
}

// Keeps track of what each row of the view looks like, so that what does not change as the content moves can be told.
void Stitcher::UpdateColors(const PixelView& frame, const Motion& motion) {
  const __m128i zero = _mm_setzero_si128();
  for (int y = 0; y < height_; ++y) {
    const uint8_t* pixels = frame.data + static_cast<ptrdiff_t>(y) * frame.stride;
    const uint8_t* edges = after_.Row(y);
    const int cy = std::min(y / kCell, cell_rows_ - 1);
    for (int slice = 0; slice < slices_; ++slice) {
      const size_t i = static_cast<size_t>(y) * slices_ + slice;
      const uint8_t* at = pixels + static_cast<ptrdiff_t>(slice) * kCell * 4;
      uint32_t sum = 0;
      for (int block = 0; block < 4; ++block) {
        const __m128i part = _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(at + block * 16)), zero);
        sum += static_cast<uint32_t>(_mm_cvtsi128_si32(part)) + static_cast<uint32_t>(_mm_extract_epi16(part, 4));
      }
      const int color = static_cast<int>(sum);
      if (motion.moved) {
        row_static_[i] = std::abs(color - row_color_[i]) <= kStaticColorSlack ? static_cast<uint8_t>(std::min(row_static_[i] + 1, 255)) : uint8_t{0};
        if (motion.cells[static_cast<size_t>(cy) * slices_ + slice] == CellState::kMoving && Energy16(edges + static_cast<ptrdiff_t>(slice) * kCell) < kRowBlank) {
          ++page_colors_[static_cast<size_t>(color / kPageColorStep)];
        }
      }
      row_color_[i] = static_cast<uint16_t>(color);
    }
  }
}

// A slice beside the edge of something fixed may hold its side as well.
bool Stitcher::RowTrusted(int y, int slice) const {
  if (y < follow_first_ || y > follow_last_) return false;
  const uint8_t* mask = row_mask_.data() + static_cast<size_t>(y) * slices_ + slice;
  return mask[0] == 0 && (slice == 0 || mask[-1] != kMaskEdge) && (slice + 1 == slices_ || mask[1] != kMaskEdge);
}

bool Stitcher::RowClear(int y, int slice) const {
  const uint8_t* mask = row_mask_.data() + static_cast<size_t>(y) * slices_ + slice;
  return (slice == 0 || mask[-1] == 0) && (slice + 1 == slices_ || mask[1] == 0);
}

// That it changes as the content moves shows a reading is of the content. What never changes may be the inside of
// something fixed, or a plain margin, and only the page's own color says it is probably the second.
bool Stitcher::RowChanges(int y, int slice) const { return row_static_[static_cast<size_t>(y) * slices_ + slice] < kStaticReadings; }

bool Stitcher::RowLooksLikePage(int y, int slice) const { return std::abs(row_color_[static_cast<size_t>(y) * slices_ + slice] - page_color_) <= kPageColorSlack; }

// Whether a reading really shows something other than the rows held for the same place: other edges, or the same edges
// in another color, as when a tint has come or gone.
bool Stitcher::Differs(const PixelView& frame, const EdgeFrame& edges, int slice, int y0, int position) const {
  const int x = slice * kCell;
  uint32_t sad = 0;
  uint32_t energy_now = 0;
  uint32_t energy_held = 0;
  int compared = 0;
  for (int i = 0; i < kCell; ++i) {
    if (!RowTrusted(y0 + i, slice) || !RowClear(y0 + i, slice) || !RowChanges(y0 + i, slice)) continue;
    const int row = position + y0 + i;
    if (quality_.Row(row)[slice] != kConfirmed) return false;
    const uint8_t* held = edges_.Row(row) + x;
    const uint8_t* now = edges.Row(y0 + i) + x;
    sad += Sad16(held, now);
    energy_now += Energy16(now);
    energy_held += Energy16(held);
    ++compared;
  }
  if (compared < kCell / 2) return false;
  const uint32_t total = energy_now + energy_held;
  if (total >= 2 * config::kScrollBlankCell && !WithinShare(sad, total, config::kScrollDifferShare)) return true;
  for (int i = 0; i < kCell; ++i) {
    if (!RowTrusted(y0 + i, slice) || !RowClear(y0 + i, slice) || !RowChanges(y0 + i, slice)) continue;
    const uint8_t* held = pixels_.Row(position + y0 + i) + static_cast<ptrdiff_t>(x) * 4;
    const uint8_t* now = frame.data + static_cast<ptrdiff_t>(y0 + i) * frame.stride + static_cast<ptrdiff_t>(x) * 4;
    uint32_t color = 0;
    for (int block = 0; block < 4; ++block) color += Sad16(held + block * 16, now + block * 16);
    if (color > static_cast<uint32_t>(config::kScrollColorSlack) * kCell) return true;
  }
  return false;
}

void Stitcher::WriteCell(const PixelView& frame, const EdgeFrame& edges, int slice, int y, int position) {
  const int x = slice * kCell;
  const int width = slice == slices_ - 1 ? width_ - x : kCell;
  const int row = position + y;
  std::memcpy(pixels_.Row(row) + static_cast<ptrdiff_t>(x) * 4, frame.data + static_cast<ptrdiff_t>(y) * frame.stride + static_cast<ptrdiff_t>(x) * 4, static_cast<size_t>(width) * 4);
  std::memcpy(edges_.Row(row) + x, edges.Row(y) + x, static_cast<size_t>(width));
  changed_first_ = std::min(changed_first_, row);
  changed_last_ = std::max(changed_last_, row + 1);
}

// Every cell of the view goes where its content belongs. A reading that shows content as it really is, confirmed by
// following the scroll and away from the pointer, overrules one that does not. Anything else only fills what is empty,
// which is also how a fixed header or footer stays once, at the end it was last seen at.
void Stitcher::Merge(const PixelView& frame, const EdgeFrame& edges, const Motion& motion, int position, bool clean, POINT pointer) {
  const size_t count = static_cast<size_t>(slices_) * cell_rows_;
  UpdateColors(frame, motion);
  BuildMask(edges);

  const int half = std::min(static_cast<int>(std::lround(config::kScrollPointerBand * scale_)), height_ / 4);
  int band_first = INT_MAX;
  int band_last = -1;
  if (pointer.x >= -kCell && pointer.x < width_ + kCell && pointer.y >= -half && pointer.y < height_ + half) {
    band_first = std::clamp((static_cast<int>(pointer.y) - half) / kCell, 0, cell_rows_ - 1);
    band_last = std::clamp((static_cast<int>(pointer.y) + half) / kCell, 0, cell_rows_ - 1);
  }
  band_first_ = band_first;
  band_last_ = band_last;

  differ_.assign(count, 0);
  hover_.assign(count, 0);
  flood_.clear();
  if (clean) {
    for (int cy = 0; cy < cell_rows_; ++cy) {
      for (int cx = 0; cx < slices_; ++cx) {
        const size_t i = static_cast<size_t>(cy) * slices_ + cx;
        if (!Differs(frame, edges, cx, cy * kCell, position)) continue;
        differ_[i] = 1;
        if (cy >= band_first && cy <= band_last) {
          hover_[i] = 1;
          flood_.push_back(static_cast<int>(i));
        }
      }
    }
    // What differs and touches the pointer is the pointer's doing, however far it spreads.
    while (!flood_.empty()) {
      const int i = flood_.back();
      flood_.pop_back();
      const int cx = i % slices_;
      const int cy = i / slices_;
      const int neighbors[4] = {cx > 0 ? i - 1 : -1, cx + 1 < slices_ ? i + 1 : -1, cy > 0 ? i - slices_ : -1, cy + 1 < cell_rows_ ? i + slices_ : -1};
      for (const int next : neighbors) {
        if (next < 0 || differ_[next] == 0 || hover_[next] != 0) continue;
        hover_[next] = 1;
        flood_.push_back(next);
      }
    }
  }

  pending_ = false;
  for (int cy = 0; cy < cell_rows_; ++cy) {
    const int y0 = cy * kCell;
    const int y1 = cy == cell_rows_ - 1 ? height_ : y0 + kCell;
    const bool in_band = cy >= band_first && cy <= band_last;
    for (int cx = 0; cx < slices_; ++cx) {
      const size_t i = static_cast<size_t>(cy) * slices_ + cx;
      const bool free = !in_band && hover_[i] == 0;
      const bool newer = clean && free;
      const bool confirming = newer && motion.moved;
      const CellState state = motion.cells[i];
      uint8_t& count_seen = differences_[static_cast<size_t>((position + y0 + kCell / 2 + difference_origin_) / kCell) * slices_ + cx];
      // Seen from elsewhere in the view, a disagreement counts for more than the same reading again.
      bool replacing = false;
      if (differ_[i] == 0 || !newer) {
        count_seen = 0;
      } else if ((count_seen = static_cast<uint8_t>(count_seen + (motion.moved ? 2 : 1))) >= config::kScrollRefreshReadings) {
        replacing = true;
        count_seen = 0;
      } else {
        pending_ = true;
      }
      for (int y = y0; y < y1; ++y) {
        uint8_t& quality = quality_.Row(position + y)[cx];
        const bool usable = free && y >= follow_first_ && y <= follow_last_ && row_mask_[static_cast<size_t>(y) * slices_ + cx] == 0;
        const bool changes = RowChanges(y, cx);
        const bool blank = row_blank_[static_cast<size_t>(y) * slices_ + cx] != 0;
        // A plain row proves nothing by itself, unless it has the page's own color or moves with what it shares a cell with.
        const bool settled = clean && usable && RowTrusted(y, cx) && RowClear(y, cx) && (blank ? RowLooksLikePage(y, cx) || state == CellState::kMoving : changes);
        if (quality == kEmpty) {
          WriteCell(frame, edges, cx, y, position);
          quality = settled ? kTentative : kPlaceholder;
        } else if (!usable) {
          continue;
        } else if (quality == kPlaceholder || (quality == kTentative && settled)) {
          // Even a blurred reading of the content beats no sight of it at all.
          WriteCell(frame, edges, cx, y, position);
          quality = !settled ? kPlaceholder : confirming ? kConfirmed : kTentative;
        } else if (replacing && changes && settled) {
          WriteCell(frame, edges, cx, y, position);
        }
      }
    }
  }

  // At rest a reading is as sharp as it gets, so rows first taken in motion can be taken again.
  if (motion.moved || !clean) return;
  for (int y = 0; y < height_; ++y) {
    const int cell_row = std::min(y / kCell, cell_rows_ - 1);
    if (cell_row >= band_first && cell_row <= band_last) continue;
    const int row = position + y;
    uint32_t held = 0;
    uint32_t now = 0;
    const auto usable = [&](int slice) { return RowTrusted(y, slice) && RowClear(y, slice) && RowChanges(y, slice) && hover_[static_cast<size_t>(cell_row) * slices_ + slice] == 0; };
    for (int slice = 0; slice < slices_; ++slice) {
      if (!usable(slice)) continue;
      held += Energy16(edges_.Row(row) + static_cast<ptrdiff_t>(slice) * kCell);
      now += Energy16(edges.Row(y) + static_cast<ptrdiff_t>(slice) * kCell);
    }
    if (now <= held + kSharperMin || now * 10 <= held * 11) continue;
    for (int slice = 0; slice < slices_; ++slice) {
      if (usable(slice)) WriteCell(frame, edges, slice, y, position);
    }
  }
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

Stitcher::Crop Stitcher::FinalCrop() const {
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
  } else {
    if (!hinted_) {
      int first = 0;
      while (first < width_ && seen.col_follow[first] < kEvidenceMin) ++first;
      int last = width_ - 1;
      while (last > first && seen.col_follow[last] < kEvidenceMin) --last;
      if (first < width_) {
        left = first;
        right = last + 1;
        while (left > 0 && !seen.Barrier(left - 1) && !seen.Line(left - 1)) --left;
        while (right < width_ && !seen.Barrier(right) && !seen.Line(right)) ++right;
      }
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
    // The last columns may hold a scroll bar: drawn on, and not following.
    const int bar = static_cast<int>(std::lround(config::kScrollBarWidth * scale_));
    const int from = std::max(left, right - bar);
    for (int x = from; x < right; ++x) {
      const uint64_t other = static_cast<uint64_t>(seen.col_stay[x]) + seen.col_other[x];
      if (other < std::max<uint64_t>(8, seen.samples / 50) || static_cast<uint64_t>(seen.col_follow[x]) * 8 > other) continue;
      // A bar's track lies around its thumb. A line at the very edge is only a border.
      int cut = x;
      const int track = right - x > 3 ? std::max(from, x - static_cast<int>(std::lround(6.0f * scale_))) : x;
      while (cut > track && seen.col_follow[cut - 1] == 0 && seen.col_stay[cut - 1] == 0 && seen.col_other[cut - 1] == 0) --cut;
      right = cut;
      break;
    }
  }

  Crop crop{left, right, min_position_ + view_top, max_position_ + view_bottom};
  // An elastic end stretches past the content and springs back. What only showed then is not content.
  if (max_position_ > rest_max_ && RowsBlank(rest_max_ + view_bottom, crop.bottom, left, right)) crop.bottom = rest_max_ + view_bottom;
  if (min_position_ < rest_min_ && RowsBlank(crop.top, rest_min_ + view_top, left, right)) crop.top = rest_min_ + view_top;
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
