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
constexpr int kPlateKnown = 2;  // Times a pixel must hold still while content moves past for it to count as still.

// Lining a reading up with the last placed one.
constexpr int kSegment = 32;              // Pixels of a row hashed as one piece, to find where rows went.
constexpr size_t kMaxSameRows = 6;        // A piece found in more rows than this says nothing of where it went.
constexpr int kMinVotes = 6;              // With fewer pieces agreeing on a shift, every shift is tried as well.
constexpr size_t kMaxTrials = 6;
constexpr size_t kSweepPoints = 4096;
constexpr uint32_t kMinExplained = 48;    // Changed pixels a shift must explain to be believed,
constexpr double kExplainedShare = 0.7;   // and the share of them, against the last placed reading,
constexpr double kRealignShare = 0.8;     // or against the image.
constexpr double kClearMargin = 1.25;     // How much better than any shift more than a row away.
constexpr uint32_t kQuietPixels = 24;     // So few pixels changing is nothing changing.

// Writing the image.
constexpr int kTopScore = 250;              // Rows this well placed in their reading are where the image comes from.
constexpr int kStaleScore = kTopScore / 2;  // The least placing for rows to be taken again, or for a seam to move through.
constexpr int kMovingWeight = 4;            // How much more a seam minds cutting what moved than what held still.
constexpr int kSeamSkip = 16;               // Rows a seam may leave out of a run, which the image then keeps as it was.

// What a row of a reading is to the image.
constexpr uint8_t kKeep = 0;     // The image keeps what it has.
constexpr uint8_t kTake = 1;     // The reading's row goes in.
constexpr uint8_t kMust = 2;     // The image has nothing there yet.
constexpr uint8_t kPending = 3;  // Better placed than the image's, and waits to go in with the rows around it.

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

inline PixelView ViewOf(const std::vector<uint32_t>& pixels, int width, int height) {
  return PixelView{reinterpret_cast<const uint8_t*>(pixels.data()), width, height, width * 4};
}

// The same color to within a few levels in every channel. Alpha says nothing on screen.
inline __m128i PixelSlack() {
  const uint32_t slack = static_cast<uint32_t>(config::kScrollPixelSlack);
  return _mm_set1_epi32(static_cast<int>(0xFF000000u | slack << 16 | slack << 8 | slack));
}

// One bit per pixel of the four, set where they are the same.
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

inline void Bump(uint8_t& count) {
  if (count < 255) ++count;
}

// Each piece of a row as a number only the same pixels share, or zero for a piece all of one color, which could be
// anywhere.
void HashRow(const uint32_t* row, int width, uint32_t* out) {
  for (int from = 0, piece = 0; from < width; from += kSegment, ++piece) {
    const int to = std::min(from + kSegment, width);
    const uint32_t first = row[from] & 0xFFFFFFu;
    uint32_t hash = 2166136261u;
    bool flat = true;
    for (int x = from; x < to; ++x) {
      const uint32_t pixel = row[x] & 0xFFFFFFu;
      flat = flat && pixel == first;
      hash = (hash ^ pixel) * 16777619u;
    }
    out[piece] = flat ? 0u : (hash | 1u);
  }
}

void HashRows(const std::vector<uint32_t>& pixels, int width, int height, int segments, std::vector<uint32_t>& hashes) {
  hashes.resize(static_cast<size_t>(segments) * height);
  for (int y = 0; y < height; ++y) HashRow(pixels.data() + static_cast<size_t>(y) * width, width, hashes.data() + static_cast<size_t>(y) * segments);
}

// A piece's hash and place along the row, with room below for the row it is in.
inline uint64_t Key(uint32_t hash, int segment) { return static_cast<uint64_t>(hash) << 32 | static_cast<uint64_t>(segment) << 16; }

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
      const size_t i = static_cast<size_t>(cy) * columns + cx;
      // What stayed where it was, as a background that holds still, says nothing of where the rest went, and would
      // make anything that moved look as if it had not.
      if (stays_[i] != 0) continue;
      const uint32_t total = energy_[i] + before.SliceEnergy(cx, other, other + kCell);
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
  limit_rows_ = limit_rows;
  hinted_ = hinted;
  box_ = box;
  // Rows and pieces are numbered in sixteen bits where shifts are looked up.
  if (width_ < kSegment || height_ <= kMinOverlap * 2 || height_ > 0xFFFF) return false;
  segments_ = (width_ + kSegment - 1) / kSegment;
  SetBand(pointer);
  const int origin = limit_rows + height_;
  const int capacity = origin * 2 + height_;
  if (!pixels_.Reserve(static_cast<size_t>(width_) * 4, capacity, origin) || !hashes_.Reserve(static_cast<size_t>(segments_) * 4, capacity, origin)) return false;
  row_score_.assign(static_cast<size_t>(capacity), 0);
  row_base_ = origin;
  evidence_.Reset(width_, height_);
  step_up_.assign(static_cast<size_t>(width_), 0);
  step_down_.assign(static_cast<size_t>(width_), 0);
  const size_t view = static_cast<size_t>(width_) * height_;
  ref_.resize(view);
  now_.resize(view);
  changed_.assign(view, 0);
  stay_n_.assign(view, 0);
  changed_in_row_.assign(static_cast<size_t>(height_), 0);
  score_.assign(static_cast<size_t>(height_), 0);
  want_.assign(static_cast<size_t>(height_), kKeep);
  difference_.assign(static_cast<size_t>(height_), -1);
  SetFollow(box.top + kCell, box.bottom - kCell - 1);
  crop_left_ = 0;
  crop_right_ = width_;
  SetCrop();
  Load(first);
  HashRows(now_, width_, height_, segments_, now_hash_);
  if (!pixels_.Commit(0, height_) || !hashes_.Commit(0, height_)) return false;
  dirty_first_ = INT_MAX;
  dirty_last_ = INT_MIN;
  Place(0, 0, true);
  now_edges_.Build(ViewOf(now_, width_, height_));
  std::swap(ref_, now_);
  std::swap(ref_hash_, now_hash_);
  std::swap(ref_edges_, now_edges_);
  ++ref_serial_;
  position_ = 0;
  min_position_ = 0;
  max_position_ = 0;
  rest_min_ = 0;
  rest_max_ = 0;
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

void Stitcher::SetFollow(int first, int last) {
  follow_first_ = first;
  follow_last_ = last;
}

// The columns compared are those the image keeps, so that what goes on beside them has no say.
void Stitcher::SetCrop() {
  const Crop crop = ViewCrop();
  if (crop.right - crop.left < kSegment) return;
  crop_left_ = crop.left;
  crop_right_ = crop.right;
}

void Stitcher::Load(const PixelView& frame) {
  for (int y = 0; y < height_; ++y) std::memcpy(now_.data() + static_cast<size_t>(y) * width_, PixelRow(frame, y), static_cast<size_t>(width_) * 4);
}

Stitcher::Step Stitcher::Feed(const PixelView& frame, POINT pointer) {
  if (frame.width != width_ || frame.height != height_) return Step::kLost;
  SetBand(pointer);
  Load(frame);
  if (Compare() <= kQuietPixels) {
    frame_key_ = 0;
    rest_min_ = std::min(rest_min_, position_);
    rest_max_ = std::max(rest_max_, position_);
    return Step::kSame;
  }
  HashRows(now_, width_, height_, segments_, now_hash_);
  uint64_t key = 14695981039346656037ull;
  for (const uint32_t hash : now_hash_) key = (key ^ hash) * 1099511628211ull;
  const bool settled = key == frame_key_;
  frame_key_ = key;

  int shift = 0;
  if (Register(&shift)) {
    last_lost_replaced_ = false;
    return Accept(position_ + shift, shift) ? Step::kMoved : Step::kLost;
  }
  // Change in a few rows only is something changing in place, as a hover, a picture loading, or an animation. Once it
  // holds still, the view as it now is takes the place of the last placed reading, where that one was.
  const int band = std::max(std::min(follow_last_ + 1, height_) - std::max(follow_first_, 0), 1);
  const uint64_t area = static_cast<uint64_t>(crop_right_ - crop_left_) * height_;
  if (changed_rows_ * 3 <= band && static_cast<uint64_t>(changed_count_) * 10 <= area) {
    if (settled) Accept(position_, 0);
    rest_min_ = std::min(rest_min_, position_);
    rest_max_ = std::max(rest_max_, position_);
    return Step::kRest;
  }
  // Too far from the last placed reading, or caught between two. The image reaches further.
  int place = 0;
  if (Realign(&place)) {
    last_lost_replaced_ = false;
    return Accept(place, place - position_) ? Step::kMoved : Step::kLost;
  }
  last_lost_replaced_ = changed_rows_ >= height_ * config::kScrollReplacedShare &&
                        changed_right_ - changed_left_ >= (crop_right_ - crop_left_) * config::kScrollReplacedShare;
  return Step::kLost;
}

// Marks what differs from the last placed reading at the same place, in the columns compared.
uint32_t Stitcher::Compare() {
  const __m128i slack = PixelSlack();
  changed_count_ = 0;
  changed_rows_ = 0;
  changed_left_ = width_;
  changed_right_ = 0;
  for (int y = 0; y < height_; ++y) {
    const size_t base = static_cast<size_t>(y) * width_;
    const uint32_t* now = now_.data() + base;
    const uint32_t* was = ref_.data() + base;
    uint8_t* out = changed_.data() + base;
    std::memset(out, 0, static_cast<size_t>(width_));
    uint32_t count = 0;
    int x = crop_left_;
    for (; x + 4 <= crop_right_; x += 4) {
      const int same = Near4(now + x, was + x, slack);
      if (same == 0xF) continue;
      for (int k = 0; k < 4; ++k) {
        if (((same >> k) & 1) != 0) continue;
        out[x + k] = 1;
        ++count;
      }
    }
    for (; x < crop_right_; ++x) {
      if (Near(now[x], was[x])) continue;
      out[x] = 1;
      ++count;
    }
    changed_in_row_[static_cast<size_t>(y)] = count;
    if (count == 0) continue;
    ++changed_rows_;
    changed_count_ += count;
    for (int k = crop_left_; k < crop_right_; ++k) {
      if (out[k] == 0) continue;
      changed_left_ = std::min(changed_left_, k);
      break;
    }
    for (int k = crop_right_ - 1; k >= crop_left_; --k) {
      if (out[k] == 0) continue;
      changed_right_ = std::max(changed_right_, k + 1);
      break;
    }
  }
  return changed_count_;
}

// The shift from the last placed reading, if one clearly explains what changed. Pieces of rows that changed are looked
// for among the last placed reading's rows, and each one found proposes a shift. Only what changed takes part, so a
// background or a bar that holds still cannot pull the shift toward none, and whatever moved counts, whatever its shape.
bool Stitcher::Register(int* shift) {
  candidates_.clear();
  if (Vote() < kMinVotes) Sweep();
  const int limit = height_ - kMinOverlap;
  if (last_shift_ != 0 && std::abs(last_shift_) <= limit && std::find(candidates_.begin(), candidates_.end(), last_shift_) == candidates_.end()) {
    candidates_.push_back(last_shift_);
  }
  std::vector<Trial> trials;
  trials.reserve(candidates_.size());
  Trial best;
  for (const int candidate : candidates_) {
    trials.push_back(Verify(candidate));
    if (trials.back().Score() > best.Score()) best = trials.back();
  }
  double rival = 0.0;
  for (const Trial& trial : trials) {
    if (std::abs(trial.shift - best.shift) >= 2) rival = std::max(rival, trial.Score());
  }
  if (best.explained < kMinExplained || best.rows < 3 || static_cast<double>(best.explained) < best.compared * kExplainedShare || best.Score() < rival * kClearMargin) {
    return false;
  }
  *shift = best.shift;
  return true;
}

// Proposes the shifts most pieces of changed rows agree on, and says how many agree on the best.
int Stitcher::Vote() {
  const int first_segment = crop_left_ / kSegment;
  const int last_segment = (crop_right_ - 1) / kSegment;
  if (index_serial_ != ref_serial_ || index_left_ != crop_left_ || index_right_ != crop_right_) {
    ref_index_.clear();
    for (int y = 0; y < height_; ++y) {
      const uint32_t* hashes = ref_hash_.data() + static_cast<size_t>(y) * segments_;
      for (int s = first_segment; s <= last_segment; ++s) {
        if (hashes[s] != 0) ref_index_.push_back(Key(hashes[s], s) | static_cast<uint64_t>(y));
      }
    }
    std::sort(ref_index_.begin(), ref_index_.end());
    index_serial_ = ref_serial_;
    index_left_ = crop_left_;
    index_right_ = crop_right_;
  }
  votes_.assign(static_cast<size_t>(height_) * 2 + 1, 0);
  for (int y = 0; y < height_; ++y) {
    if (changed_in_row_[static_cast<size_t>(y)] == 0) continue;
    const uint32_t* now = now_hash_.data() + static_cast<size_t>(y) * segments_;
    const uint32_t* was = ref_hash_.data() + static_cast<size_t>(y) * segments_;
    for (int s = first_segment; s <= last_segment; ++s) {
      const uint32_t hash = now[s];
      if (hash == 0 || hash == was[s]) continue;
      const uint64_t key = Key(hash, s);
      const auto from = std::lower_bound(ref_index_.begin(), ref_index_.end(), key);
      const auto to = std::lower_bound(from, ref_index_.end(), key + 0x10000);
      if (from == to || to - from > static_cast<ptrdiff_t>(kMaxSameRows)) continue;
      for (auto it = from; it != to; ++it) {
        const int moved = static_cast<int>(*it & 0xFFFF) - y;
        if (moved != 0) ++votes_[static_cast<size_t>(moved + height_)];
      }
    }
  }
  const int limit = height_ - kMinOverlap;
  int most = 0;
  for (size_t n = 0; n < kMaxTrials; ++n) {
    int pick = 0;
    int pick_votes = 1;
    for (int moved = -limit; moved <= limit; ++moved) {
      const int count = votes_[static_cast<size_t>(moved + height_)];
      if (count > pick_votes) {
        pick = moved;
        pick_votes = count;
      }
    }
    if (pick == 0) break;
    candidates_.push_back(pick);
    votes_[static_cast<size_t>(pick + height_)] = 0;
    most = std::max(most, pick_votes);
  }
  return most;
}

// When pieces of rows found nowhere else, as when nothing drawn matches exactly, every shift is tried on a sample of what
// changed, and the few that explain most of it are proposed.
void Stitcher::Sweep() {
  points_.clear();
  for (int y = 0; y < height_; y += 2) {
    if (changed_in_row_[static_cast<size_t>(y)] == 0) continue;
    const size_t base = static_cast<size_t>(y) * width_;
    for (int x = crop_left_; x < crop_right_; x += 3) {
      if (changed_[base + x] != 0) points_.push_back(base + x);
    }
  }
  if (points_.size() > kSweepPoints) {
    const size_t step = points_.size() / kSweepPoints + 1;
    size_t kept = 0;
    for (size_t i = 0; i < points_.size(); i += step) points_[kept++] = points_[i];
    points_.resize(kept);
  }
  if (points_.size() < kMinExplained) return;
  const int limit = height_ - kMinOverlap;
  const size_t enough = std::max<size_t>(32, points_.size() / 4);
  int picks[3] = {0, 0, 0};
  double shares[3] = {0.0, 0.0, 0.0};
  for (int moved = -limit; moved <= limit; ++moved) {
    if (moved == 0) continue;
    // The points are in reading order, so those the shift keeps in view are one run of them.
    const auto first = std::lower_bound(points_.begin(), points_.end(), static_cast<size_t>(std::max(0, -moved)) * width_);
    const auto last = std::lower_bound(first, points_.end(), static_cast<size_t>(std::min(height_, height_ - moved)) * width_);
    const size_t inside = static_cast<size_t>(last - first);
    if (inside < enough) continue;
    const ptrdiff_t offset = static_cast<ptrdiff_t>(moved) * width_;
    size_t matched = 0;
    for (auto it = first; it != last; ++it) {
      if (Near(now_[*it], ref_[static_cast<size_t>(static_cast<ptrdiff_t>(*it) + offset)])) ++matched;
    }
    const double share = static_cast<double>(matched) / static_cast<double>(inside);
    if (share < 0.5) continue;
    int place = 3;
    while (place > 0 && shares[place - 1] < share) --place;
    if (place >= 3) continue;
    for (int move = 2; move > place; --move) {
      picks[move] = picks[move - 1];
      shares[move] = shares[move - 1];
    }
    picks[place] = moved;
    shares[place] = share;
  }
  for (int i = 0; i < 3; ++i) {
    if (shares[i] > 0.0 && std::find(candidates_.begin(), candidates_.end(), picks[i]) == candidates_.end()) candidates_.push_back(picks[i]);
  }
}

// How much of what changed a shift explains: every changed pixel whose row stays in view, against the last placed
// reading's pixel the shift says it came from.
Stitcher::Trial Stitcher::Verify(int shift) const {
  Trial trial;
  trial.shift = shift;
  const int from = std::max(0, -shift);
  const int to = std::min(height_, height_ - shift);
  for (int y = from; y < to; ++y) {
    if (changed_in_row_[static_cast<size_t>(y)] == 0) continue;
    const size_t base = static_cast<size_t>(y) * width_;
    const uint8_t* changed = changed_.data() + base;
    const uint32_t* now = now_.data() + base;
    const uint32_t* was = ref_.data() + static_cast<size_t>(y + shift) * width_;
    uint32_t explained = 0;
    for (int x = crop_left_; x < crop_right_; ++x) {
      if (changed[x] == 0) continue;
      ++trial.compared;
      if (Near(now[x], was[x])) ++explained;
    }
    trial.explained += explained;
    if (explained >= 2) ++trial.rows;
  }
  return trial;
}

// Where in the image the reading is, if it can be found there: pieces of the reading that differ from the last placed
// one are looked for among the image's rows, and the place most agree on must explain nearly all of what changed.
bool Stitcher::Realign(int* position) {
  const int first_segment = crop_left_ / kSegment;
  const int last_segment = (crop_right_ - 1) / kSegment;
  now_index_.clear();
  for (int y = 0; y < height_; ++y) {
    const uint32_t* now = now_hash_.data() + static_cast<size_t>(y) * segments_;
    const uint32_t* was = ref_hash_.data() + static_cast<size_t>(y) * segments_;
    for (int s = first_segment; s <= last_segment; ++s) {
      if (now[s] != 0 && now[s] != was[s]) now_index_.push_back(Key(now[s], s) | static_cast<uint64_t>(y));
    }
  }
  if (now_index_.empty()) return false;
  std::sort(now_index_.begin(), now_index_.end());
  const int reach = height_ - kMinOverlap;
  const int low = min_position_ - reach;
  const int high = max_position_ + reach;
  votes_.assign(static_cast<size_t>(high - low + 1), 0);
  for (int row = min_position_; row < max_position_ + height_; ++row) {
    if (!Held(row)) continue;
    const uint32_t* hashes = reinterpret_cast<const uint32_t*>(hashes_.Row(row));
    for (int s = first_segment; s <= last_segment; ++s) {
      if (hashes[s] == 0) continue;
      const uint64_t key = Key(hashes[s], s);
      const auto from = std::lower_bound(now_index_.begin(), now_index_.end(), key);
      const auto to = std::lower_bound(from, now_index_.end(), key + 0x10000);
      if (from == to || to - from > static_cast<ptrdiff_t>(kMaxSameRows)) continue;
      for (auto it = from; it != to; ++it) {
        const int place = row - static_cast<int>(*it & 0xFFFF);
        if (place >= low && place <= high) ++votes_[static_cast<size_t>(place - low)];
      }
    }
  }
  std::vector<Trial> trials;
  Trial best;
  for (int n = 0; n < 4; ++n) {
    const auto most = std::max_element(votes_.begin(), votes_.end());
    if (*most < kMinVotes) break;
    const int place = low + static_cast<int>(most - votes_.begin());
    *most = 0;
    trials.push_back(VerifyAt(place));
    if (trials.back().Score() > best.Score()) best = trials.back();
  }
  double rival = 0.0;
  for (const Trial& trial : trials) {
    if (std::abs(trial.shift - best.shift) >= 2) rival = std::max(rival, trial.Score());
  }
  if (best.explained < kMinExplained * 4 || best.rows < height_ / 8 || static_cast<double>(best.explained) < best.compared * kRealignShare ||
      best.Score() < rival * kClearMargin) {
    return false;
  }
  *position = best.shift;
  return true;
}

// How much of what changed since the last placed reading the image shows alike, were the reading placed at `place`.
Stitcher::Trial Stitcher::VerifyAt(int place) const {
  Trial trial;
  trial.shift = place;
  for (int y = 0; y < height_; ++y) {
    if (changed_in_row_[static_cast<size_t>(y)] == 0 || !Held(place + y)) continue;
    const size_t base = static_cast<size_t>(y) * width_;
    const uint8_t* changed = changed_.data() + base;
    const uint32_t* now = now_.data() + base;
    const uint32_t* held = reinterpret_cast<const uint32_t*>(pixels_.Row(place + y));
    uint32_t explained = 0;
    for (int x = crop_left_; x < crop_right_; ++x) {
      if (changed[x] == 0) continue;
      ++trial.compared;
      if (Near(now[x], held[x])) ++explained;
    }
    trial.explained += explained;
    if (explained >= 2) ++trial.rows;
  }
  return trial;
}

// Puts the reading at `position`, `shift` rows on from the last placed one, and makes it the one the next is lined up
// with.
bool Stitcher::Accept(int position, int shift) {
  if (std::abs(position) > limit_rows_ || !pixels_.Commit(position, position + height_) || !hashes_.Commit(position, position + height_)) return false;
  Place(position, shift, false);
  now_edges_.Build(ViewOf(now_, width_, height_));
  const bool overlaps = shift != 0 && std::abs(shift) <= height_ - kMinOverlap;
  if (overlaps) Learn(shift);
  std::swap(ref_, now_);
  std::swap(ref_hash_, now_hash_);
  std::swap(ref_edges_, now_edges_);
  ++ref_serial_;
  if (shift != 0) last_shift_ = overlaps ? shift : 0;
  position_ = position;
  min_position_ = std::min(min_position_, position);
  max_position_ = std::max(max_position_, position);
  if (overlaps) SetCrop();
  return true;
}

// Writes the rows of the reading being taken in that the image needs: rows it has nothing for, rows the reading shows
// better placed, and rows whose content changed in place. Each run of them goes in whole, between seams at rows the
// image already shows alike.
void Stitcher::Place(int position, int shift, bool flush) {
  for (int y = 0; y < height_; ++y) {
    score_[static_cast<size_t>(y)] = Score(y);
    difference_[static_cast<size_t>(y)] = -1;
    const int row = position + y;
    uint8_t want = kKeep;
    if (!Held(row)) {
      want = kMust;
    } else if (score_[static_cast<size_t>(y)] >= kStaleScore && Stale(y, row)) {
      want = kTake;
    } else if (score_[static_cast<size_t>(y)] > RowScore(row)) {
      want = score_[static_cast<size_t>(y)] == kTopScore ? kPending : kTake;
    }
    want_[static_cast<size_t>(y)] = want;
  }
  // Rows waiting in the best placed part of the view go in together once the scroll is about to carry them out of it,
  // so that the image is made of few tall blocks rather than many thin ones, and a background that holds still behind
  // the content breaks only where they meet.
  const int ahead = 2 * std::abs(shift) + 8;
  for (int first = 0; first < height_;) {
    if (score_[static_cast<size_t>(first)] != kTopScore) {
      ++first;
      continue;
    }
    int last = first;
    while (last < height_ && score_[static_cast<size_t>(last)] == kTopScore) ++last;
    int pending = 0;
    bool due = flush || shift == 0;
    for (int y = first; y < last; ++y) {
      if (want_[static_cast<size_t>(y)] != kPending) continue;
      ++pending;
      if ((shift > 0 && y < first + ahead) || (shift < 0 && y >= last - ahead)) due = true;
    }
    if (pending * 4 >= (last - first) * 3) due = true;
    for (int y = first; y < last; ++y) {
      uint8_t& want = want_[static_cast<size_t>(y)];
      if (want == kPending) want = due ? kTake : kKeep;
    }
    first = last;
  }
  int floor = 0;
  for (int first = 0; first < height_;) {
    if (want_[static_cast<size_t>(first)] == kKeep) {
      ++first;
      continue;
    }
    int last = first;
    int must_first = height_;
    int must_last = -1;
    while (last < height_ && want_[static_cast<size_t>(last)] != kKeep) {
      if (want_[static_cast<size_t>(last)] == kMust) {
        must_first = std::min(must_first, last);
        must_last = last;
      }
      ++last;
    }
    const int top = SeamTop(position, first, last, must_first, floor);
    const int bottom = SeamBottom(position, top, last, must_last);
    if (bottom > top) WriteRows(position, top, bottom);
    floor = std::max(bottom, last);
    first = floor;
  }
}

// How well placed a row of the view is to go into the image: nothing outside where content moves, rising to the best a
// sixth of the way in from its ends, and low under the pointer, where a hover may change how things look.
int Stitcher::Score(int y) const {
  const int lo = std::clamp(follow_first_, 0, height_ - 1);
  const int hi = std::clamp(follow_last_ + 1, lo + 1, height_);
  if (y < lo || y >= hi) return 1;
  const int plateau = std::max((hi - lo) / 6, 1);
  const int reach = std::min({y - lo, hi - 1 - y, plateau});
  const int score = 2 + reach * (kTopScore - 2) / plateau;
  if (y >= band_first_ && y <= band_last_) return std::max(2, score / 3);
  return score;
}

bool Stitcher::Held(int row) const {
  const int index = row + row_base_;
  return index >= 0 && index < static_cast<int>(row_score_.size()) && row_score_[static_cast<size_t>(index)] != 0;
}

// Whether much of a row is shown otherwise than the image holds it, by pixels that did not hold still: a picture that
// loaded, or a change too wide to be a background showing between things that moved.
bool Stitcher::Stale(int y, int row) const {
  const uint32_t enough = static_cast<uint32_t>(std::max(16, (crop_right_ - crop_left_) / 6));
  if (changed_in_row_[static_cast<size_t>(y)] < enough) return false;
  const size_t base = static_cast<size_t>(y) * width_;
  const uint8_t* changed = changed_.data() + base;
  const uint32_t* now = now_.data() + base;
  const uint32_t* held = reinterpret_cast<const uint32_t*>(pixels_.Row(row));
  uint32_t count = 0;
  for (int x = crop_left_; x < crop_right_; ++x) {
    if (changed[x] != 0 && !Near(now[x], held[x]) && ++count >= enough) return true;
  }
  return false;
}

// How unlike the image's row a row of the reading is, minding most what moved.
int Stitcher::Difference(int position, int y) {
  if (y < 0 || y >= height_ || !Held(position + y)) return INT_MAX;
  int& known = difference_[static_cast<size_t>(y)];
  if (known >= 0) return known;
  const size_t base = static_cast<size_t>(y) * width_;
  const uint8_t* changed = changed_.data() + base;
  const uint32_t* now = now_.data() + base;
  const uint32_t* held = reinterpret_cast<const uint32_t*>(pixels_.Row(position + y));
  int sum = 0;
  for (int x = crop_left_; x < crop_right_; ++x) {
    if (!Near(now[x], held[x])) sum += changed[x] != 0 ? kMovingWeight : 1;
  }
  known = sum;
  return sum;
}

// A seam between rows `boundary` - 1 and `boundary` cannot be seen where either row is alike in both: the rows on
// either side then meet as they do in one of them.
int Stitcher::SeamCost(int position, int boundary) { return std::min(Difference(position, boundary - 1), Difference(position, boundary)); }

// Where a run of rows [first, last) starts going in. It may start higher, through rows the reading also shows well
// placed, or lower, through rows the image need not take, but never below `must`, the first it has nothing for.
int Stitcher::SeamTop(int position, int first, int last, int must, int floor) {
  if (!Held(position + first - 1)) return first;
  const int reach = std::max(32, height_ / 4);
  int high = first;
  while (high > floor && first - high < reach && score_[static_cast<size_t>(high - 1)] >= kStaleScore && Held(position + high - 1)) --high;
  const int low = std::min({must, last, first + kSeamSkip});
  int best = first;
  int best_cost = SeamCost(position, first);
  for (int step = 1; best_cost > 0 && (first - step >= high || first + step <= low); ++step) {
    for (const int y : {first - step, first + step}) {
      if (y < high || y > low) continue;
      const int cost = SeamCost(position, y);
      if (cost < best_cost) {
        best = y;
        best_cost = cost;
      }
    }
  }
  return best;
}

// Where a run of rows from `first` to `last` stops going in: never above `must` + 1, past the last it has nothing for.
int Stitcher::SeamBottom(int position, int first, int last, int must) {
  if (first >= last) return first;
  if (!Held(position + last)) return last;
  const int reach = std::max(32, height_ / 4);
  const int low = std::max({first, must + 1, last - kSeamSkip});
  int high = last;
  while (high < height_ && high - last < reach && score_[static_cast<size_t>(high)] >= kStaleScore && Held(position + high)) ++high;
  int best = last;
  int best_cost = SeamCost(position, last);
  for (int step = 1; best_cost > 0 && (last - step >= low || last + step <= high); ++step) {
    for (const int y : {last - step, last + step}) {
      if (y < low || y > high) continue;
      const int cost = SeamCost(position, y);
      if (cost < best_cost) {
        best = y;
        best_cost = cost;
      }
    }
  }
  return best;
}

void Stitcher::WriteRows(int position, int first, int last) {
  for (int y = first; y < last; ++y) {
    const int row = position + y;
    std::memcpy(pixels_.Row(row), now_.data() + static_cast<size_t>(y) * width_, static_cast<size_t>(width_) * 4);
    std::memcpy(hashes_.Row(row), now_hash_.data() + static_cast<size_t>(y) * segments_, static_cast<size_t>(segments_) * 4);
    RowScore(row) = static_cast<uint8_t>(std::max(score_[static_cast<size_t>(y)], 1));
    difference_[static_cast<size_t>(y)] = 0;
  }
  MarkChanged(position + first, position + last);
}

// What a reading `shift` rows on from the last placed one says about the view: which rows and columns follow the scroll,
// how the display is divided, and what holds still while content moves past.
void Stitcher::Learn(int shift) {
  evidence_.Add(ref_edges_, now_edges_, shift, std::min(follow_first_, static_cast<int>(box_.top)), std::max(follow_last_ + 1, static_cast<int>(box_.bottom)));
  // Rows where something really follows the scroll, by a share of what could have, as a stray match says nothing and
  // what shows through a frosted bar follows only a little. An edge reading looks one row down, so the row above the
  // first that moves seems to follow it too.
  const uint32_t enough = std::max<uint32_t>(kEvidenceMin, static_cast<uint32_t>(static_cast<uint64_t>(evidence_.frames) * width_ / kFollowEveryPixels));
  int first = 0;
  while (first < height_ && evidence_.row_follow[first] < enough) ++first;
  int last = height_ - 1;
  while (last > first && evidence_.row_follow[last] < enough) --last;
  if (first < height_) SetFollow(first == 0 ? 0 : first + 1, last);
  UpdateSteps();
  const int from = std::max(0, -shift);
  const int to = std::min(height_, height_ - shift);
  const int inside_first = std::max(follow_first_, 0);
  const int inside_last = std::min(follow_last_ + 1, height_);
  for (int y = from; y < to; ++y) {
    const uint32_t* now = now_.data() + static_cast<size_t>(y) * width_;
    const uint32_t* same = ref_.data() + static_cast<size_t>(y) * width_;
    const uint32_t* moved = ref_.data() + static_cast<size_t>(y + shift) * width_;
    uint8_t* stays = stay_n_.data() + static_cast<size_t>(y) * width_;
    const bool inside = y >= inside_first && y < inside_last;
    for (int x = 0; x < width_; ++x) {
      if (!Near(now[x], same[x]) || Near(now[x], moved[x])) continue;
      Bump(stays[x]);
      if (inside && x >= crop_left_ && x < crop_right_) ++behind_count_;
    }
  }
}

// Keeps track of the color steps along the rows, which is how the display's own divisions show. Static things do not
// need every reading, and the content moving through tells them from what merely lines up for a while.
void Stitcher::UpdateSteps() {
  const int first = std::max(follow_first_, 0);
  const int last = std::min(follow_last_ + 1, height_);
  const int phase = step_tick_++ % kStepRowEvery;
  for (int y = first + phase; y < last; y += kStepRowEvery) {
    const uint8_t* pixels = reinterpret_cast<const uint8_t*>(now_.data() + static_cast<size_t>(y) * width_);
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

void Stitcher::Finish() {
  if (ref_.empty()) return;
  // The last reading placed is the one the image is checked against from here on, and nothing changed in it.
  std::swap(now_, ref_);
  std::swap(now_hash_, ref_hash_);
  std::fill(changed_.begin(), changed_.end(), static_cast<uint8_t>(0));
  std::fill(changed_in_row_.begin(), changed_in_row_.end(), 0u);
  Place(position_, 0, true);
  std::swap(now_, ref_);
  std::swap(now_hash_, ref_hash_);
}

bool Stitcher::RowsBlank(int first, int last, int left, int right) const {
  for (int row = first; row < last; ++row) {
    if (!Held(row)) continue;
    const uint32_t* pixels = reinterpret_cast<const uint32_t*>(pixels_.Row(row));
    for (int x = left + 1; x < right; ++x) {
      if (!Near(pixels[x], pixels[left])) return false;
    }
  }
  return true;
}

// The first and last places of the view the image spans. An elastic end shows empty space past the content for a
// moment, so where the view came to rest short of how far it went, and nothing but that lies beyond, it ends there.
void Stitcher::Ends(const Crop& view, int* top, int* bottom) const {
  *top = min_position_;
  *bottom = max_position_;
  if (*top < rest_min_ && RowsBlank(*top + view.top, rest_min_ + view.top, view.left, view.right)) *top = rest_min_;
  if (*bottom > rest_max_ && RowsBlank(rest_max_ + view.bottom, *bottom + view.bottom, view.left, view.right)) *bottom = rest_max_;
}

int Stitcher::span() const {
  int top = 0;
  int bottom = 0;
  Ends(ViewCrop(), &top, &bottom);
  return bottom - top;
}

PixelView Stitcher::Composed(int first, int last) const {
  return PixelView{pixels_.Row(first), width_, std::max(last - first, 0), width_ * 4};
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
  int top = 0;
  int bottom = 0;
  Ends(crop, &top, &bottom);
  crop.top += top;
  crop.bottom += bottom;
  return crop;
}

void Stitcher::Copy(const Crop& crop, uint8_t* to, ptrdiff_t stride) const {
  const size_t bytes = static_cast<size_t>(crop.right - crop.left) * 4;
  for (int row = crop.top; row < crop.bottom; ++row) {
    if (Held(row)) {
      std::memcpy(to, pixels_.Row(row) + static_cast<ptrdiff_t>(crop.left) * 4, bytes);
    } else {
      std::memset(to, 0, bytes);
    }
    to += stride;
  }
}

void Stitcher::MarkChanged(int first, int last) {
  dirty_first_ = std::min(dirty_first_, first);
  dirty_last_ = std::max(dirty_last_, last);
}

bool Stitcher::TakeChanges(int* first, int* last) {
  if (dirty_first_ >= dirty_last_) return false;
  *first = dirty_first_;
  *last = dirty_last_;
  dirty_first_ = INT_MAX;
  dirty_last_ = INT_MIN;
  return true;
}

}  // namespace sstk
