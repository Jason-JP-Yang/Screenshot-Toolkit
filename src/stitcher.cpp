#include "stitcher.h"

#include "config/core.h"
#include "longimage.h"
#include "platform.h"

#include <cmath>
#include <cstring>

namespace sstk {

namespace {

constexpr uint32_t kQuietPixels = 24;  // So few pixels changing is nothing changing.
constexpr uint32_t kColor = 0xFFFFFFu;
constexpr size_t kPage = 4096;
constexpr int kGrid = 16;      // Pixels along a side of the squares things that change by themselves are gathered in.
constexpr int kFluxGap = 3;    // Squares that may lie between parts of one thing that changes by itself.
constexpr int kMinMoves = 4;   // Scrolls seen before anything is said to change by itself in place.
constexpr int kCell = 8;       // Pixels along a side of the squares fixed things are found in.
constexpr int kSpread = 32;    // Pixels along a side of the squares a background must be seen in enough of.
constexpr int kBarMoves = 8;   // Scrolls seen before a scroll bar at the side is looked for while recording.
constexpr int kReach = 48;     // How far one side of a gap in the background is carried into it.
constexpr int kSpan = 400;     // How wide a gap in the background may be to be filled by blending its two sides.
constexpr uint64_t kDeferBytes = 256ull << 20;  // The most rows held for the picture behind them may take, unpacked.
constexpr int kFinishRows = 256;   // The most rows handed on together as the capture finishes.
constexpr int kStartRows = 32;     // Rows of the first reading packed together.
constexpr int64_t kGrain = 1 << 16;  // Pixels below which a part of a loop is not worth a thread.
constexpr int kColumnAlign = 64;   // Columns the parts of the view start at, so that no two threads write one cache line.

// What a pixel of the view is, as of the last reading placed.
constexpr uint8_t kHeld = 1;   // It shows something that holds still.
constexpr uint8_t kTrue = 2;   // It showed content seen to scroll, the last time.
constexpr uint8_t kOther = 4;  // It changed by itself, the last time.
constexpr uint8_t kFlux = 8;   // It changed by itself, and so did the content it shows, where that was before.
constexpr uint8_t kSeen = 16;  // Content has been seen to scroll through it.
constexpr uint8_t kNew = 32;   // Nothing that could be seen scrolled to it: it came into view, or out from under something fixed.

// How a pixel of a reading compared with the reading before.
constexpr uint8_t kStayed = 1;
constexpr uint8_t kFollowed = 2;

// What a pixel of the view keeps over the capture besides its counts.
constexpr uint8_t kKind = 3;       // The end what is fixed there belongs at, if anything is.
constexpr uint8_t kFresh = 4;      // It just changed in place, to count as that once what came holds still.
constexpr uint8_t kFlipped = 8;    // What held still there came, went or changed.
constexpr uint8_t kCovered = 16;   // Content came in front of what held still there,
constexpr uint8_t kCovers = 32;    // and did so again.

// What a content pixel keeps above its color.
constexpr uint32_t kSure = 1u << 24;      // It was seen, showing this, in the row of the view its sight says.
constexpr uint32_t kFrozen = 2u << 24;    // It is part of something that changes by itself, taken whole.
constexpr uint32_t kUnseen = 4u << 24;    // Of a row made: no content known, as what was always behind something fixed.
constexpr uint32_t kBehind = 8u << 24;    // Of a row made: not content, so the background shows there if there is one,
constexpr uint32_t kUnsure = 16u << 24;   // or not known either way, seen too seldom to tell,
constexpr uint32_t kClear = 64u << 24;    // or surely see-through: unlike itself again and again, never alike.
constexpr uint32_t kDynamic = 32u << 24;  // It has been seen to change by itself as it scrolled, as a video does.
constexpr uint32_t kVoted = 1u << 31;     // Of a vote: something was seen. Below it, by how much the color leads.

// Which end something fixed belongs at.
constexpr uint8_t kTopKind = 1;
constexpr uint8_t kBottomKind = 2;

// What a pixel of the view was over the capture.
enum : uint8_t {
  kUnknown,   // Nothing told.
  kMoving,    // Content scrolls through it.
  kBack,      // It held still, and content came in front of it again and again: a background behind the content.
  kFixed,     // It held still, and content hardly ever showed there: something fixed in front of the content.
  kChanging,  // That, and it came, went or changed by itself.
  kBand,      // A row content never showed in, at the top or the bottom of the view.
};

// The tone of a row of more than one color.
constexpr uint32_t kMixed = 0xFFFFFFFFu;
constexpr uint32_t kNoTone = 0xFFFFFFFEu;

inline bool Same(uint32_t a, uint32_t b) { return ((a ^ b) & kColor) == 0; }

inline uint32_t Lead(uint32_t slot) { return (slot & kVoted) != 0 ? (slot >> 24) & 0x7F : 0; }

inline void Bump(uint8_t& count, int by = 1) { count = static_cast<uint8_t>(std::min(count + by, 255)); }

// A vote on what a pixel shows, by one sight of it. The color most sights agree on stays ahead.
inline void Vote(uint32_t& slot, uint32_t color, uint32_t weight) {
  const uint32_t lead = (slot >> 24) & 0x7F;
  if ((slot & kVoted) == 0) {
    slot = kVoted | weight << 24 | (color & kColor);
  } else if (Same(slot, color)) {
    slot = kVoted | std::min(lead + weight, 127u) << 24 | (slot & kColor);
  } else if (lead >= weight) {
    slot = kVoted | (lead - weight) << 24 | (slot & kColor);
  } else {
    slot = kVoted | (weight - lead) << 24 | (color & kColor);
  }
}

// The color most of those given agree on, one at a time.
inline void Majority(uint32_t& color, uint32_t& count, uint32_t pixel) {
  if (count == 0) {
    color = pixel & kColor;
    count = 1;
  } else {
    count = Near(color, pixel, config::kScrollPixelSlack) ? count + 1 : count - 1;
  }
}

// What scrolled through a pixel a time or two, as something fixed that slid in or moved before it stuck, while it
// held still far more often, is fixed all the same.
inline uint8_t TypeOf(uint8_t still, uint8_t moved, uint8_t marks) {
  if ((marks & kCovers) != 0 && still >= 2) return kBack;
  if (moved != 0 && moved * 3 >= still) return kMoving;
  if ((marks & kFlipped) != 0) return kChanging;
  return still != 0 ? kFixed : kUnknown;
}

inline uint8_t KindOf(uint8_t marks) { return static_cast<uint8_t>(marks & kKind); }

// `at` of `length` of the way from one color to the other.
inline uint32_t Mix(uint32_t a, uint32_t b, int at, int length) {
  uint32_t out = 0;
  for (int shift = 0; shift < 24; shift += 8) {
    const int from = static_cast<int>((a >> shift) & 0xFF);
    const int to = static_cast<int>((b >> shift) & 0xFF);
    out |= static_cast<uint32_t>(from + (to - from) * at / length) << shift;
  }
  return out;
}

// Rows of 32-bit pixels, packed as how each byte differs from the one in the row above.
std::vector<uint8_t> PackPixels(const uint32_t* rows, int width, int count) {
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  const size_t size = row_bytes * static_cast<size_t>(count);
  const uint8_t* in = reinterpret_cast<const uint8_t*>(rows);
  std::vector<uint8_t> delta(size);
  std::memcpy(delta.data(), in, std::min(row_bytes, size));
  size_t i = row_bytes;
  for (; i + 16 <= size; i += 16) {
    const __m128i here = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
    const __m128i above = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i - row_bytes));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(delta.data() + i), _mm_sub_epi8(here, above));
  }
  for (; i < size; ++i) delta[i] = static_cast<uint8_t>(in[i] - in[i - row_bytes]);
  return PackBytes(delta.data(), size);
}

void UnpackPixels(const std::vector<uint8_t>& packed, uint32_t* rows, int width, int count) {
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  const size_t size = row_bytes * static_cast<size_t>(count);
  uint8_t* out = reinterpret_cast<uint8_t*>(rows);
  if (!UnpackBytes(packed, out, size)) {
    std::memset(out, 0, size);
    return;
  }
  for (size_t row = row_bytes; row < size; row += row_bytes) {
    size_t i = 0;
    for (; i + 16 <= row_bytes; i += 16) {
      const __m128i here = _mm_loadu_si128(reinterpret_cast<const __m128i*>(out + row + i));
      const __m128i above = _mm_loadu_si128(reinterpret_cast<const __m128i*>(out + row + i - row_bytes));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + row + i), _mm_add_epi8(here, above));
    }
    for (; i < row_bytes; ++i) out[row + i] = static_cast<uint8_t>(out[row + i] + out[row + i - row_bytes]);
  }
}

// Rows a function makes, handed on with only their colors, as opaque pixels.
template <typename Fill>
class Maker final : public Stitcher::RowMaker {
 public:
  Maker(int width, const Fill& fill) : width_(width), fill_(fill) {}

  void Make(int first, int count, uint32_t* out) const override {
    fill_(first, count, out);
    const size_t pixels = static_cast<size_t>(count) * static_cast<size_t>(width_);
    for (size_t i = 0; i < pixels; ++i) out[i] = (out[i] & kColor) | 0xFF000000u;
  }

 private:
  int width_;
  const Fill& fill_;
};

}  // namespace

RowPlane::~RowPlane() {
  if (base_ != nullptr) VirtualFree(base_, 0, MEM_RELEASE);
}

bool RowPlane::Reserve(size_t stride, int rows, int origin) {
  stride_ = stride;
  origin_ = origin;
  base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, stride * rows, MEM_RESERVE, PAGE_READWRITE));
  return base_ != nullptr;
}

bool RowPlane::Ensure(int first, int last) {
  return first >= last || VirtualAlloc(Row(first), stride_ * (last - first), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void RowPlane::Release(int first, int last) {
  if (base_ == nullptr || first >= last) return;
  const size_t from = (static_cast<size_t>(Row(first) - base_) + kPage - 1) / kPage * kPage;
  const size_t to = static_cast<size_t>(Row(last) - base_) / kPage * kPage;
  if (to > from) VirtualFree(base_ + from, to - from, MEM_DECOMMIT);
}

void RowStore::Reset(int width, int rows, int origin, Workers* workers) {
  width_ = width;
  origin_ = origin;
  workers_ = workers;
  blocks_ = std::vector<Block>(static_cast<size_t>((rows + kRows - 1) / kRows));
}

void RowStore::Put(int row, const uint32_t* pixels) {
  const int at = row + origin_;
  Block& block = blocks_[static_cast<size_t>(at / kRows)];
  const size_t row_pixels = static_cast<size_t>(width_);
  if (block.plain.empty()) {
    block.plain.assign(row_pixels * kRows, 0);
    if (!block.packed.empty()) UnpackPixels(block.packed, block.plain.data(), width_, kRows);
    block.packed = std::vector<uint8_t>{};
  }
  std::memcpy(block.plain.data() + static_cast<size_t>(at % kRows) * row_pixels, pixels, row_pixels * 4);
  block.held |= 1u << (at % kRows);
  block.fresh = true;
}

void RowStore::Erase(int first, int last) {
  for (int row = first; row < last; ++row) {
    const int at = row + origin_;
    Block& block = blocks_[static_cast<size_t>(at / kRows)];
    block.held &= ~(1u << (at % kRows));
    if (block.held == 0) block = Block{};
  }
}

void RowStore::Pack() {
  std::vector<Block*> ready;
  for (Block& block : blocks_) {
    if (block.plain.empty()) continue;
    if (!block.fresh || block.held == 0xFFFFFFFFu) ready.push_back(&block);
    block.fresh = false;
  }
  workers_->For(static_cast<int>(ready.size()), [&](int part) {
    Block& block = *ready[static_cast<size_t>(part)];
    block.packed = PackPixels(block.plain.data(), width_, kRows);
    block.plain = std::vector<uint32_t>{};
  });
}

const uint32_t* RowStore::Reader::Row(int row) {
  const int at = row + store_.origin_;
  const int index = at / kRows;
  const Block& block = store_.blocks_[static_cast<size_t>(index)];
  const size_t offset = static_cast<size_t>(at % kRows) * static_cast<size_t>(store_.width_);
  if (!block.plain.empty()) return block.plain.data() + offset;
  if (index != block_) {
    plain_.resize(static_cast<size_t>(store_.width_) * kRows);
    UnpackPixels(block.packed, plain_.data(), store_.width_, kRows);
    block_ = index;
  }
  return plain_.data() + offset;
}

int Stitcher::Parts(int rows) const { return workers_->Parts(static_cast<int64_t>(std::max(rows, 0)) * width_, kGrain); }

bool Stitcher::Start(const std::shared_ptr<const Frame>& reading, float scale, int limit_rows, Sink* sink, Workers* workers) {
  const PixelView first = reading->view();
  width_ = first.width;
  height_ = first.height;
  scale_ = scale;
  limit_rows_ = limit_rows;
  sink_ = sink;
  workers_ = workers;
  if (width_ < Tracker::kSegment || width_ > 0xFFFF || height_ <= Tracker::kMinOverlap * 2 || height_ > 0x7FFF) return false;
  tracker_.Reset(width_, height_, workers);
  const int origin = limit_rows + height_;
  const int capacity = origin * 2 + height_;
  const size_t stride = static_cast<size_t>(width_) * 4;
  if (!truth_.Reserve(stride, capacity, origin) || !votes_.Reserve(stride, capacity, origin) || !backs_.Reserve(static_cast<size_t>(width_), capacity, origin) ||
      !sights_.Reserve(static_cast<size_t>(width_) * 2, capacity, origin)) {
    return false;
  }
  if (!truth_.Ensure(0, height_) || !votes_.Ensure(0, height_) || !backs_.Ensure(0, height_) || !sights_.Ensure(0, height_)) return false;
  row_base_ = origin;
  made_.Reset(width_, capacity, origin, workers);
  held_.assign(static_cast<size_t>(capacity), 0);
  tones_.assign(static_cast<size_t>(capacity), kMixed);
  const size_t pixels = static_cast<size_t>(width_) * height_;
  mask_.assign(pixels, 0);
  marks_.assign(pixels, 0);
  still_.assign(pixels, 0);
  moved_.assign(pixels, 0);
  odd_.assign(pixels, 0);
  wall_.assign(pixels, 0);
  back_.clear();
  column_true_.assign(static_cast<size_t>(width_), 0);
  column_odd_.assign(static_cast<size_t>(width_), 0);
  shades_.assign(4096, Shade{});
  strip_.clear();
  line_.assign(static_cast<size_t>(width_), 0);
  textured_ = false;
  tile_top_ = 0;
  tile_period_ = height_;
  next_segment_ = 1;
  bar_ = edge_ = width_;
  bar_ready_ = false;
  tracker_.SetRef(first);
  ref_ = reading;
  // Until content is made, the page's own color is what the first reading mostly shows.
  uint32_t count = 0;
  for (int y = 0; y < height_; ++y) {
    const uint32_t* pixels_in = PixelRow(first, y);
    for (int x = 0; x < width_; ++x) Majority(page_, count, pixels_in[x]);
  }
  position_ = min_position_ = max_position_ = rest_min_ = rest_max_ = 0;
  serial_ = 0;
  // Only the end may want the first reading again, so it is kept packed.
  start_.assign(static_cast<size_t>((height_ + kStartRows - 1) / kStartRows), std::vector<uint8_t>{});
  workers_->For(static_cast<int>(start_.size()), [&](int part) {
    const int top = part * kStartRows;
    start_[static_cast<size_t>(part)] = PackPixels(reading->pixels() + static_cast<size_t>(top) * width_, width_, std::min(kStartRows, height_ - top));
  });
  back_final_ = false;
  Capture(top_, reading, 0);
  Capture(bottom_, reading, 0);
  kept_ = true;
  return true;
}

// The rows around the pointer. A hover changes the whole of what is under it, which may reach well above and below.
void Stitcher::Band(POINT pointer, int* first, int* last) const {
  const int half = std::min(std::max(static_cast<int>(std::lround(config::kScrollPointerBand * scale_)), height_ / 8), height_ / 4);
  *first = INT_MAX;
  *last = -1;
  if (pointer.x >= -16 && pointer.x < width_ + 16 && pointer.y >= -half && pointer.y < height_ + half) {
    *first = std::max(static_cast<int>(pointer.y) - half, 0);
    *last = std::min(static_cast<int>(pointer.y) + half, height_ - 1);
  }
}

Stitcher::Step Stitcher::Feed(const std::shared_ptr<const Frame>& reading, POINT pointer) {
  kept_ = false;
  const PixelView frame = reading->view();
  if (finished_ || frame.width != width_ || frame.height != height_) return Step::kLost;
  Band(pointer, &band_first_, &band_last_);
  if (tracker_.Load(frame, RECT{0, 0, width_, height_}, band_first_, band_last_, kQuietPixels) <= kQuietPixels) {
    last_key_ = 0;
    pending_ = step_ = 0;
    Rested();
    return Step::kSame;
  }
  const bool settled = tracker_.key() == last_key_;
  last_key_ = tracker_.key();
  if (pending_ == 0 && Calm()) {
    Settle();
    Rested();
    return Step::kRest;
  }

  Tracker::Match match;
  bool sure = false;
  if (tracker_.Register(pending_ + step_, &match, &sure) && Scrolled(match.shift)) {
    lost_replaced_ = false;
    step_ = match.shift - pending_;
    // A few rows on, a reading is passed over until the view has gone further or stopped: a slow scroll is then read
    // just as a fast one is, in far fewer readings.
    if (step_ != 0 && std::abs(match.shift) < config::kScrollMinStep) {
      pending_ = match.shift;
      return Step::kMoved;
    }
    return Accept(reading, match.shift, match.sharp) ? Step::kMoved : Step::kLost;
  }
  // Change in a few rows only is something changing in place, as a hover, a picture loading, or an animation. Once it
  // holds still, and enough of what is drawn shows unchanged to be sure the view did not move, it is taken in.
  if (tracker_.changed_rows() * 3 <= height_ && static_cast<uint64_t>(tracker_.changed_count()) * 10 <= static_cast<uint64_t>(width_) * height_) {
    if (settled && Anchored()) Stay(reading);
    Rested();
    return Step::kRest;
  }
  lost_replaced_ = tracker_.changed_rows() >= height_ * config::kScrollReplacedShare && tracker_.changed_width() >= width_ * config::kScrollReplacedShare;
  return Step::kLost;
}

// Takes in the reading the tracker holds, `shift` rows on from the last one placed, and makes it the one the next is
// lined up with.
bool Stitcher::Accept(const std::shared_ptr<const Frame>& reading, int shift, bool sharp) {
  const PixelView frame = reading->view();
  const int from = position_;
  const int place = from + shift;
  if (std::abs(place) > limit_rows_) return false;
  const int low = std::min(from, place);
  const int high = std::max(from, place) + height_;
  if (!truth_.Ensure(low, high) || !votes_.Ensure(low, high) || !backs_.Ensure(low, high) || !sights_.Ensure(low, high)) return false;
  // Rows coming into view start with nothing seen of them.
  const int enter_first = shift > 0 ? std::max(from + height_, place) : place;
  const int enter_last = shift > 0 ? place + height_ : std::min(from, place + height_);
  const Bands entering(enter_first, enter_last, 1, Parts(enter_last - enter_first));
  workers_->For(entering.count(), [&](int part) {
    for (int row = entering.begin(part); row < entering.end(part); ++row) {
      if (Done(row)) continue;
      std::memset(truth_.Row(row), 0, static_cast<size_t>(width_) * 4);
      std::memset(votes_.Row(row), 0, static_cast<size_t>(width_) * 4);
      std::memset(backs_.Row(row), 0, static_cast<size_t>(width_));
      std::memset(sights_.Row(row), 0, static_cast<size_t>(width_) * 2);
    }
  });
  const PixelView ref = tracker_.ref();
  // What changes by itself is taken whole before any of it leaves the view, or goes under what is fixed at its edge.
  if (shift > 0) {
    Freeze(ref, from, 0, shift + top_band_);
  } else {
    Freeze(ref, from, height_ + shift - bottom_band_, height_);
  }
  const bool sorted = sharp && std::abs(shift) < height_;
  if (sorted) {
    Sort(ref, frame, shift, place);
  } else {
    Blur(ref, frame);
  }
  ++serial_;
  const int old_min = min_position_;
  const int old_max = max_position_;
  min_position_ = std::min(min_position_, place);
  max_position_ = std::max(max_position_, place);
  if (place == min_position_) Capture(top_, reading, place);
  if (place == max_position_) Capture(bottom_, reading, place);
  // The view is told apart after the first scroll, and again as more is seen: often at first, then less often. Nothing
  // counts for the content before, when what is fixed is not known yet.
  if (sorted && moves_ >= next_segment_) {
    Segment(false, frame);
    next_segment_ = moves_ + std::clamp(moves_ / 2, 1, 16);
  }
  const int leave_first = shift > 0 ? from : std::max(place + height_, from);
  const int leave_last = shift > 0 ? std::min(place, from + height_) : from + height_;
  Make(leave_first, leave_last);
  Flush(old_min, old_max);
  truth_.Release(leave_first, leave_last);
  votes_.Release(leave_first, leave_last);
  backs_.Release(leave_first, leave_last);
  sights_.Release(leave_first, leave_last);
  tracker_.Promote();
  ref_ = reading;
  kept_ = true;
  pending_ = 0;
  position_ = place;
  return true;
}

// Whether a reading that changed changed anything outside the rows the pointer lights up.
bool Stitcher::Changes() const {
  for (int y = 0; y < height_; ++y) {
    if (tracker_.ChangedInRow(y) != 0 && !tracker_.Hovered(y)) return true;
  }
  return false;
}

// Takes in a reading that changed in place, as where a picture loaded. What lights up around the pointer is passed
// over.
void Stitcher::Stay(const std::shared_ptr<const Frame>& reading) {
  const PixelView now = tracker_.now();
  const bool top = top_.serial == serial_ && top_.place == position_;
  const bool bottom = bottom_.serial == serial_ && bottom_.place == position_;
  const bool changes = Changes();
  uint32_t* top_pixels = top && changes ? Writable(top_) : nullptr;
  uint32_t* bottom_pixels = bottom && changes ? Writable(bottom_) : nullptr;
  const Bands bands(0, height_, 1, Parts(height_));
  workers_->For(bands.count(), [&](int part) {
    for (int y = bands.begin(part); y < bands.end(part); ++y) {
      if (tracker_.ChangedInRow(y) == 0 || tracker_.Hovered(y)) continue;
      const uint32_t* pixels = PixelRow(now, y);
      uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
      const int row = position_ + y;
      const bool wanted = !Done(row);
      uint32_t* truth = wanted ? reinterpret_cast<uint32_t*>(truth_.Row(row)) : nullptr;
      uint16_t* sights = wanted ? reinterpret_cast<uint16_t*>(sights_.Row(row)) : nullptr;
      ForBits(tracker_.Changed(y), tracker_.words(), [&](int x) {
        const size_t index = static_cast<size_t>(y) * width_ + x;
        const uint8_t was = state[x];
        if ((was & kHeld) != 0 && moved_[index] == 0) marks_[index] = static_cast<uint8_t>(marks_[index] | kFlipped);
        state[x] = static_cast<uint8_t>((was & (kHeld | kSeen)) | kOther);
        // Content changed in place, as a picture that loaded, is what the content there shows from now on.
        if (wanted && (was & kHeld) == 0 && KindOf(marks_[index]) == 0 && (truth[x] & (kSure | kFrozen)) == kSure) {
          truth[x] = kSure | (truth[x] & kDynamic) | (pixels[x] & kColor);
          sights[x] = static_cast<uint16_t>(y + 1);
        }
        if (top_pixels != nullptr) top_pixels[index] = pixels[x];
        if (bottom_pixels != nullptr) bottom_pixels[index] = pixels[x];
      });
    }
  });
  tracker_.Promote();
  ref_ = reading;
  ++serial_;
  if (top) top_.serial = serial_;
  if (bottom) bottom_.serial = serial_;
  if (top || bottom) ++revision_;
  kept_ = true;
  pending_ = 0;
}

// The view rests, and only what changes by itself or holds still changed, as a date shown while scrolling that goes
// once it stops: that is something fixed that came or went, and an end resting here shows it as it is now.
void Stitcher::Settle() {
  const PixelView now = tracker_.now();
  const bool top = top_.serial == serial_ && top_.place == position_;
  const bool bottom = bottom_.serial == serial_ && bottom_.place == position_;
  const bool changes = Changes();
  uint32_t* top_pixels = top && changes ? Writable(top_) : nullptr;
  uint32_t* bottom_pixels = bottom && changes ? Writable(bottom_) : nullptr;
  const Bands bands(0, height_, 1, Parts(height_));
  workers_->For(bands.count(), [&](int part) {
    for (int y = bands.begin(part); y < bands.end(part); ++y) {
      if (tracker_.ChangedInRow(y) == 0 || tracker_.Hovered(y)) continue;
      const uint32_t* pixels = PixelRow(now, y);
      ForBits(tracker_.Changed(y), tracker_.words(), [&](int x) {
        const size_t index = static_cast<size_t>(y) * width_ + x;
        if ((mask_[index] & kHeld) != 0 && moved_[index] == 0) marks_[index] = static_cast<uint8_t>(marks_[index] | kFlipped);
        if (top_pixels != nullptr) top_pixels[index] = pixels[x];
        if (bottom_pixels != nullptr) bottom_pixels[index] = pixels[x];
      });
    }
  });
  if (top || bottom) ++revision_;
}

// The view holds still. An elastic end shows empty space past the content for a moment: where the view comes to rest
// short of how far it went, and nothing but that lies beyond, the capture ends there.
void Stitcher::Rested() {
  rest_min_ = std::min(rest_min_, position_);
  rest_max_ = std::max(rest_max_, position_);
  if (position_ == rest_min_ && min_position_ < position_ && Blank(min_position_, position_)) {
    Drop(min_position_, position_);
    min_position_ = position_;
    Capture(top_, ref_, position_);
  }
  if (position_ == rest_max_ && max_position_ > position_ && Blank(position_ + height_, max_position_ + height_)) {
    Drop(position_ + height_, max_position_ + height_);
    max_position_ = position_;
    Capture(bottom_, ref_, position_);
  }
}

// Whether all that changed is what changes by itself or holds still where it is, as a video does while the view
// rests. Such a reading says nothing of where the view is, and is not worth looking for a shift in.
bool Stitcher::Calm() const {
  uint32_t elsewhere = 0;
  for (int y = 0; y < height_; ++y) {
    if (tracker_.ChangedInRow(y) == 0) continue;
    const uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
    bool over = false;
    ForBits(tracker_.Changed(y), tracker_.words(), [&](int x) {
      if (!over && (state[x] & (kOther | kHeld)) == 0 && ++elsewhere > kQuietPixels) over = true;
    });
    if (over) return false;
  }
  return true;
}

// Whether the shift is a scroll. The pointer's light passing from one thing to the next like it changes the screen
// just as a scroll of the distance between them would, but it leaves the content where it was: more of what was seen
// to scroll before now holds still, unlike what the shift would put there, than anything follows the shift. A scroll
// moves all of it.
bool Stitcher::Scrolled(int shift) const {
  const PixelView& now = tracker_.now();
  const PixelView& ref = tracker_.ref();
  // Every fourth row is enough to tell.
  const int first = std::max(0, -shift);
  const int last = std::min(height_, height_ - shift);
  const int lines = last > first ? (last - first + 3) / 4 : 0;
  struct Tally {
    uint32_t moved = 0;
    uint32_t held = 0;
  };
  const Bands bands(0, lines, 1, Parts(lines));
  std::vector<Tally> tallies(static_cast<size_t>(bands.count()));
  workers_->For(bands.count(), [&](int part) {
    Tally& tally = tallies[static_cast<size_t>(part)];
    for (int line = bands.begin(part); line < bands.end(part); ++line) {
      const int y = first + line * 4;
      if (tracker_.Hovered(y)) continue;
      const uint32_t* a = PixelRow(now, y);
      const uint32_t* s = PixelRow(ref, y);
      const uint32_t* m = PixelRow(ref, y + shift);
      const uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
      for (int x = 0; x < width_; ++x) {
        const bool stay = Same(a[x], s[x]);
        const bool follow = Same(a[x], m[x]);
        if (follow && !stay) {
          ++tally.moved;
        } else if (stay && !follow && (state[x] & kTrue) != 0) {
          ++tally.held;
        }
      }
    }
  });
  uint32_t moved = 0;
  uint32_t held = 0;
  for (const Tally& tally : tallies) {
    moved += tally.moved;
    held += tally.held;
  }
  return held <= moved;
}

// Whether enough of what is drawn shows unchanged where the last placed reading had it to be sure the view did not
// move: of the edges drawn in it, far more hold where they were than changed. Not counting what holds still whatever
// the content does, nor what changes by itself.
bool Stitcher::Anchored() const {
  const PixelView& now = tracker_.now();
  struct Tally {
    uint32_t held = 0;
    uint32_t moved = 0;
  };
  const Bands bands(1, height_, 1, Parts(height_ - 1));
  std::vector<Tally> tallies(static_cast<size_t>(bands.count()));
  workers_->For(bands.count(), [&](int part) {
    Tally& tally = tallies[static_cast<size_t>(part)];
    for (int y = bands.begin(part); y < bands.end(part); ++y) {
      if (tracker_.Hovered(y)) continue;
      const uint64_t* changed = tracker_.Changed(y);
      const uint32_t* pixels = PixelRow(now, y);
      const uint32_t* above = PixelRow(now, y - 1);
      const uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
      for (int x = 0; x < width_; ++x) {
        if (Same(pixels[x], above[x]) || (state[x] & (kOther | kHeld)) != 0) continue;
        ++(Bit(changed, x) ? tally.moved : tally.held);
      }
    }
  });
  uint32_t held = 0;
  uint32_t moved = 0;
  for (const Tally& tally : tallies) {
    held += tally.held;
    moved += tally.moved;
  }
  return held >= static_cast<uint32_t>(std::max(256, width_ / 4)) && held >= moved * 2;
}

// Whether a pixel of the view changes by itself in most scrolls: something fixed in the view that is never still, as
// a video in a corner, not content that changes as it passes. Content has never been seen to scroll through it: a
// video in the content, however long it stays in one part of the view, had other content pass there too.
bool Stitcher::Lively(size_t index) const {
  return moves_ >= kMinMoves && (mask_[index] & kSeen) == 0 && static_cast<uint32_t>(odd_[index]) * 4 >= static_cast<uint32_t>(moves_) * 3;
}

// Tells every pixel of the reading at `place` apart, by comparison with the reading `shift` rows before it, and lets
// the content rows it shows see it. Row y shows what row y + shift of `ref` did, so rows are gone through with the
// rows they came from still as they were.
// For telling fixed things apart: a pixel that shows what scrolled to it, and did not before, is content passing. One
// that holds still though something else scrolled to it shows what holds still there. One that did neither changed by
// itself, and when what it showed before did not scroll on either, something that holds still there came, went or
// changed. How each pixel of the view behaved is kept whatever it is thought to be, so that what was taken for
// something fixed can turn out otherwise. Only what scrolled out from under a band along the top or bottom says
// nothing of what holds still: lined up with the band, plain content would seem to hold still.
// For the content: each pixel of it is compared with itself as seen a quarter of the view away. Content looks the same
// wherever it is in the view. Where the content is see-through, what shows is the background behind it, which differs
// from place to place, and that is then what the background is, there and wherever it shows through after.
// Nothing here ties a pixel to any outside its column, so the columns are shared out, each gone through row by row.
void Stitcher::Sort(const PixelView& ref, const PixelView& now, int shift, int place) {
  ++moves_;
  max_step_ = std::max(max_step_, std::abs(shift));
  const Bands bands(0, width_, kColumnAlign, Parts(height_));
  workers_->For(bands.count(), [&](int part) { SortColumns(ref, now, shift, place, bands.begin(part), bands.end(part)); });
}

void Stitcher::SortColumns(const PixelView& ref, const PixelView& now, int shift, int place, int left, int right) {
  constexpr int kSlack = config::kScrollPixelSlack;
  const __m128i slack = SlackOf(kSlack);
  const bool down = shift > 0;
  const int apart = std::max(config::kScrollMinStep * 2, height_ / 4);
  std::vector<uint8_t> how(static_cast<size_t>(right - left));
  for (int i = 0; i < height_; ++i) {
    const int y = down ? i : height_ - 1 - i;
    const int source = y + shift;
    const bool entered = source < 0 || source >= height_;
    const bool banded = !entered && (source < top_band_ || source >= height_ - bottom_band_);
    const int gone = y - shift;  // Where what this row showed before would be now, had it scrolled.
    const uint32_t* a = PixelRow(now, y);
    const uint32_t* s = PixelRow(ref, y);
    const uint32_t* m = entered ? nullptr : PixelRow(ref, source);
    const uint32_t* on = gone >= 0 && gone < height_ && !tracker_.Hovered(gone) ? PixelRow(now, gone) : nullptr;
    const size_t base = static_cast<size_t>(y) * width_;
    int x = left;
    for (; x + 4 <= right; x += 4) {
      const int stays = Near4(a + x, s + x, slack);
      const int follows = entered ? 0 : Near4(a + x, m + x, slack);
      for (int k = 0; k < 4; ++k) how[static_cast<size_t>(x - left + k)] = static_cast<uint8_t>((stays >> k & 1) | (follows >> k & 1) << 1);
    }
    for (; x < right; ++x) {
      const bool follows = !entered && Near(a[x], m[x], kSlack);
      how[static_cast<size_t>(x - left)] = static_cast<uint8_t>((Near(a[x], s[x], kSlack) ? kStayed : 0) | (follows ? kFollowed : 0));
    }
    uint8_t* state = mask_.data() + base;
    const uint8_t* origin = entered ? nullptr : mask_.data() + static_cast<size_t>(source) * width_;
    uint8_t* marks = marks_.data() + base;
    const uint8_t* under = entered ? nullptr : marks_.data() + static_cast<size_t>(source) * width_;
    uint8_t* still = still_.data() + base;
    uint8_t* moved = moved_.data() + base;
    uint16_t* odd = odd_.data() + base;
    uint32_t* wall = wall_.data() + base;
    const int row = place + y;
    const bool hovered = tracker_.Hovered(y);
    // A row made already, or one lit up by the pointer, learns nothing here, and nothing is learned of the content
    // before what is fixed has been told apart once.
    const bool alive = !Done(row);
    const bool wanted = alive && !hovered;
    const bool learn = wanted && segments_ > 0;
    uint32_t* truth = alive ? reinterpret_cast<uint32_t*>(truth_.Row(row)) : nullptr;
    uint32_t* votes = alive ? reinterpret_cast<uint32_t*>(votes_.Row(row)) : nullptr;
    uint8_t* backs = alive ? backs_.Row(row) : nullptr;
    uint16_t* sights = alive ? reinterpret_cast<uint16_t*>(sights_.Row(row)) : nullptr;
    for (x = left; x < right; ++x) {
      const uint8_t compared = how[static_cast<size_t>(x - left)];
      const bool stay = (compared & kStayed) != 0;
      const bool follow = (compared & kFollowed) != 0;
      const bool open = KindOf(marks[x]) == 0;                // Content could show here, with nothing fixed in front of it.
      const bool hidden = !entered && KindOf(under[x]) != 0;  // What scrolled here was behind something fixed.
      const bool trusted = !hidden || !banded;
      const uint8_t was = state[x];
      if (!entered) {
        if (follow && !stay) {
          ++column_true_[static_cast<size_t>(x)];
          Bump(moved[x]);
          // Content came in front of what held still here.
          if ((was & kHeld) != 0 && Lead(wall[x]) >= 2 && !Near(a[x], wall[x], kSlack)) {
            marks[x] = static_cast<uint8_t>(marks[x] | ((marks[x] & kCovered) != 0 ? kCovers : kCovered));
          }
          marks[x] = static_cast<uint8_t>(marks[x] & ~kFresh);
        } else if (stay && !follow) {
          // What came in place holds still though content scrolled to it: something fixed came, went or changed there.
          if (trusted && (marks[x] & kFresh) != 0) marks[x] = static_cast<uint8_t>(marks[x] | kFlipped);
          marks[x] = static_cast<uint8_t>(marks[x] & ~kFresh);
          if (trusted) {
            Bump(still[x]);
            Vote(wall[x], a[x], 2);
          }
        } else if (!stay) {
          ++column_odd_[static_cast<size_t>(x)];
          if (!hovered && !hidden && odd[x] < 0xFFFF) ++odd[x];
          // Unless what showed here before scrolled on.
          if (trusted) {
            const bool fresh = moved[x] == 0 && on != nullptr && !Near(on[x], s[x], kSlack);
            marks[x] = static_cast<uint8_t>((marks[x] & ~kFresh) | (fresh ? kFresh : 0));
          }
        }
      }
      uint8_t is = static_cast<uint8_t>(was & (kHeld | kSeen));
      if (entered || hidden) {
        if (!stay) {
          is = static_cast<uint8_t>(is & kSeen);
        } else if (!open) {
          is = static_cast<uint8_t>(is | kHeld);
        }
        is = static_cast<uint8_t>(is | kNew);
      } else if (follow && !stay) {
        is = kTrue | kSeen;
      } else if (stay && !follow) {
        is = static_cast<uint8_t>(is | kHeld);
      } else if (stay) {
        if (!open) is = static_cast<uint8_t>(is | kHeld);
        if ((is & kHeld) == 0) is = static_cast<uint8_t>(is | (origin[x] & kTrue));
      } else {
        // Changing on from where it was, it goes with the content, as a video does. Changing where it is in the view,
        // it does not, as what a pointer lights up.
        is = static_cast<uint8_t>((is & kSeen) | kOther | ((origin[x] & kOther) != 0 && !hovered ? kFlux : 0));
      }
      state[x] = is;
      if (wanted) {
        // Content that changes as it scrolls is remembered as such, so that all of it is taken whole together.
        if ((is & kFlux) != 0) truth[x] |= kDynamic;
        // Over a picture, content shows only where content has been seen to scroll through: elsewhere, as where no
        // content ever passes, what shows is the picture.
        if (learn && open && (!textured_ || moved[x] != 0) && (truth[x] & kFrozen) == 0) {
          uint32_t& anchor = truth[x];  // The sight it is compared with, and in which row of the view that was.
          if ((anchor & kSure) == 0) {
            anchor = kSure | (anchor & kDynamic) | (a[x] & kColor);
            sights[x] = static_cast<uint16_t>(y + 1);
          } else if (std::abs(y + 1 - static_cast<int>(sights[x])) >= apart) {
            // Once each quarter of the view it moves, either way, so that neither counts more often than the other.
            // Alike at both places, it is content, unless it showed at each just what holds still there, as plain
            // parts of a picture behind it do.
            const size_t before = static_cast<size_t>(sights[x] - 1) * width_ + x;
            const bool behind = Through(before, anchor) && Through(base + x, a[x]);
            const bool content = Near(anchor, a[x], kSlack) && !behind;
            if (content) {
              Vote(votes[x], a[x], 2);
            } else {
              Bump(backs[x], 2);
            }
            // Only what is surely see-through, unlike itself again and again and never alike, as something animated
            // is now and then, tells what the background is, at both places.
            if (!content && backs[x] >= 4 && votes[x] == 0 && (anchor & kDynamic) == 0) Vote(wall_[before], anchor, 1);
            anchor = kSure | (anchor & kDynamic) | (a[x] & kColor);
            sights[x] = static_cast<uint16_t>(y + 1);
          }
          if (backs[x] >= 4 && votes[x] == 0 && (anchor & kDynamic) == 0) Vote(wall[x], a[x], 1);
        }
      }
    }
  }
}

// A reading that does not line up pixel for pixel tells nothing apart, and nothing of the content: it only says where
// the view went.
void Stitcher::Blur(const PixelView& ref, const PixelView& now) {
  const Bands bands(0, height_, 1, Parts(height_));
  workers_->For(bands.count(), [&](int part) {
    for (int y = bands.begin(part); y < bands.end(part); ++y) {
      const uint32_t* a = PixelRow(now, y);
      const uint32_t* s = PixelRow(ref, y);
      uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
      for (int x = 0; x < width_; ++x) {
        const bool stay = Near(a[x], s[x], config::kScrollPixelSlack);
        state[x] = static_cast<uint8_t>(state[x] & (stay ? kHeld | kSeen : kSeen));
      }
    }
  });
}

// Keeps the reading at `place` as the screen at an end.
void Stitcher::Capture(End& end, const std::shared_ptr<const Frame>& frame, int place) {
  end.frame = frame;
  end.own = std::vector<uint32_t>{};
  end.pixels = frame->pixels();
  end.place = place;
  end.serial = serial_;
  ++end.version;
  ++revision_;
}

// The screen at an end, to be changed in place: its own copy, from here on.
uint32_t* Stitcher::Writable(End& end) {
  if (end.own.empty()) {
    end.own.assign(end.pixels, end.pixels + static_cast<size_t>(width_) * height_);
    end.frame.reset();
    end.pixels = end.own.data();
  }
  ++end.version;
  return end.own.data();
}

// Content that changes by itself, as a video does, would be pieced together from different moments, were its rows
// made from what each saw. So once any of it reaches rows [first, last) of the reading at `place`, all of it that
// shows is taken from that one reading, the last to show it whole.
void Stitcher::Freeze(const PixelView& ref, int place, int first, int last) {
  first = std::max(first, 0);
  last = std::min(last, height_);
  // What changes by itself now, or was seen to at any time as it scrolled through the view: all of a video, not just
  // the parts that happened to change last.
  std::vector<const uint32_t*> truths(static_cast<size_t>(height_), nullptr);
  for (int y = 0; y < height_; ++y) {
    if (!Done(place + y)) truths[static_cast<size_t>(y)] = reinterpret_cast<const uint32_t*>(truth_.Row(place + y));
  }
  const auto dynamic = [&](int y, int x) {
    const size_t index = static_cast<size_t>(y) * width_ + x;
    const uint32_t* truth = truths[static_cast<size_t>(y)];
    return truth != nullptr && ((mask_[index] & kFlux) != 0 || (truth[x] & kDynamic) != 0) && !Lively(index);
  };
  const Bands reaching(first, last, 1, Parts(last - first));
  std::vector<uint8_t> found(static_cast<size_t>(reaching.count()), 0);
  workers_->For(reaching.count(), [&](int part) {
    for (int y = reaching.begin(part); y < reaching.end(part) && found[static_cast<size_t>(part)] == 0; ++y) {
      for (int x = 0; x < width_; ++x) {
        if (!dynamic(y, x)) continue;
        found[static_cast<size_t>(part)] = 1;
        break;
      }
    }
  });
  if (std::find(found.begin(), found.end(), static_cast<uint8_t>(1)) == found.end()) return;
  const int columns = (width_ + kGrid - 1) / kGrid;
  const int rows = (height_ + kGrid - 1) / kGrid;
  grid_.assign(static_cast<size_t>(columns) * rows, 0);
  const Bands bands(0, height_, kGrid, Parts(height_));
  workers_->For(bands.count(), [&](int part) {
    for (int y = bands.begin(part); y < bands.end(part); ++y) {
      uint8_t* cells = grid_.data() + static_cast<size_t>(y / kGrid) * columns;
      for (int x = 0; x < width_; ++x) {
        if (dynamic(y, x)) cells[x / kGrid] = 1;
      }
    }
  });
  for (int gy = first / kGrid; gy <= (last - 1) / kGrid; ++gy) {
    for (int gx = 0; gx < columns; ++gx) {
      if (grid_[static_cast<size_t>(gy) * columns + gx] != 1) continue;
      // Everything joined to it, as one box.
      stack_.clear();
      stack_.push_back(gy * columns + gx);
      grid_[static_cast<size_t>(gy) * columns + gx] = 2;
      RECT box{gx, gy, gx + 1, gy + 1};
      for (size_t head = 0; head < stack_.size(); ++head) {
        const int cx = stack_[head] % columns;
        const int cy = stack_[head] / columns;
        box.left = std::min<LONG>(box.left, cx);
        box.top = std::min<LONG>(box.top, cy);
        box.right = std::max<LONG>(box.right, cx + 1);
        box.bottom = std::max<LONG>(box.bottom, cy + 1);
        // Squares a little apart are one thing still: a video's still parts leave gaps in what changes.
        for (int ny = std::max(cy - kFluxGap, 0); ny <= std::min(cy + kFluxGap, rows - 1); ++ny) {
          for (int nx = std::max(cx - kFluxGap, 0); nx <= std::min(cx + kFluxGap, columns - 1); ++nx) {
            const int next = ny * columns + nx;
            if (grid_[static_cast<size_t>(next)] != 1) continue;
            grid_[static_cast<size_t>(next)] = 2;
            stack_.push_back(next);
          }
        }
      }
      const int left = box.left * kGrid;
      const int right = std::min<int>(box.right * kGrid, width_);
      for (int y = box.top * kGrid; y < std::min<int>(box.bottom * kGrid, height_); ++y) {
        const int row = place + y;
        if (Done(row)) continue;
        const uint32_t* pixels = PixelRow(ref, y);
        const size_t base = static_cast<size_t>(y) * width_;
        uint32_t* truth = reinterpret_cast<uint32_t*>(truth_.Row(row));
        for (int x = left; x < right; ++x) {
          if ((mask_[base + x] & kHeld) != 0 || KindOf(marks_[base + x]) != 0 || Lively(base + x) || (truth[x] & kFrozen) != 0) continue;
          truth[x] = kFrozen | (pixels[x] & kColor);
        }
      }
    }
  }
}

// Whether rows made are held until the capture finishes even away from the ends, so that all of them are painted from
// what is found of the view in the end: whether a picture is behind the content, and what is fixed. Within a bound
// on what they would take unpacked.
bool Stitcher::Deferring() const {
  return static_cast<uint64_t>(max_position_ - min_position_ + height_) * static_cast<uint64_t>(width_) * 4 <= kDeferBytes;
}

// Makes content rows [first, last), those not made yet. Rows within a view of either end, and over a picture all rows,
// are held for the ends and the picture to be painted on when the capture finishes, and meanwhile shown with them
// painted as they stand. All others are final.
void Stitcher::Make(int first, int last) {
  std::vector<Run> runs;
  for (int row = first; row < last; ++row) {
    if (Done(row)) continue;
    Compose(row, line_.data());
    Mark(row, row + 1);
    bool final = true;
    if (InEnds(row) || Deferring()) {
      made_.Put(row, line_.data());
      held_[static_cast<size_t>(row + row_base_)] = 1;
      Touched(row, row + 1);
      final = false;
    }
    if (!runs.empty() && runs.back().final == final && runs.back().first + runs.back().count == row) {
      ++runs.back().count;
    } else {
      runs.push_back(Run{row, 1, final});
    }
  }
  const auto fresh = [&](int from, int count, uint32_t* out) {
    for (int i = 0; i < count; ++i) {
      uint32_t* line = out + static_cast<size_t>(i) * width_;
      Blend(from + i, line);
      Render(from + i, line);
      Fill(line);
    }
  };
  const auto held = [&](int from, int count, uint32_t* out) { PaintHeld(from, count, out); };
  const Maker fresh_rows(width_, fresh);
  const Maker held_rows(width_, held);
  for (const Run& run : runs) {
    if (run.final) {
      sink_->Rows(run.first, run.count, fresh_rows, true);
    } else {
      sink_->Rows(run.first, run.count, held_rows, false);
    }
  }
  made_.Pack();
  Page();
}

// Tells the view apart, from how each of its pixels behaved over all the readings so far, and once the view has gone a
// little way, from what the screens at the two ends of the capture show alike: what holds still.
// - Rows the two ends show alike, nearly all across, from the top and from the bottom of the view, are a header or a
//   bar to type in. A few rows at the very edge may differ at the two ends with nothing seen to scroll in them, as a
//   sliver of a taskbar below a window's edge: they belong to the band the rows next to them make. Before the view has
//   gone that far, the bands are the rows content was never seen to scroll in.
// - What holds still where content came in front of it again and again, at many places across the view and in many
//   colors, is a picture behind the content, as a chat's wallpaper.
// - A column that holds still in a color other than the page's own nearly all the way down, as the edge of a window,
//   runs down the whole image as content does, and holds nothing fixed together.
// - What holds still where content never scrolled is the detail of something fixed in front of the content, unless
//   content scrolls over a picture: then only the bands are. So is what the two ends show alike, edge for edge.
// Detail near other detail, or above or below it in the same columns, as the icons down a side bar or a panel's
// heading and its list, is one thing, which belongs at the end of the view it reaches, the top if it reaches both, or
// else the nearer one. The thing is its detail and what holds still in a color other than the page's own, reached
// from it without crossing anything else: a panel's plain inside, its edge and its shadow, however far they are from
// what is drawn on it. What holds still in the page's own color looks the same whatever it is taken for, and content
// coming out from under the thing is never part of it. Detail all over the view is a background.
void Stitcher::Segment(bool final, const PixelView& now) {
  const double began = NowSeconds();
  ++segments_;
  ++revision_;
  ++fixed_version_;
  constexpr int kSlack = config::kScrollPixelSlack;
  const int columns = (width_ + kCell - 1) / kCell;
  const int rows = (height_ + kCell - 1) / kCell;
  const int inside = (edge_ + kCell - 1) / kCell;  // Columns of squares left of a scroll bar.
  const size_t cells = static_cast<size_t>(columns) * rows;
  const int gap = std::max(1, static_cast<int>(std::lround(config::kScrollClusterGap * scale_ / kCell)));
  const int reach = static_cast<int>(std::lround(config::kScrollBottomReach * scale_));
  const bool ends = bottom_.place - top_.place >= config::kScrollMinStep * 2;
  const uint32_t* tops = top_.pixels;
  const uint32_t* bottoms = bottom_.pixels;
  const auto type = [&](size_t i) { return TypeOf(still_[i], moved_[i], marks_[i]); };
  const auto alike = [&](size_t i) { return Near(tops[i], bottoms[i], kSlack); };
  // Per row: whether content was never seen to scroll or show a background anywhere in it, and whether the screens at
  // the two ends show it alike, nearly all across.
  std::vector<uint8_t> quiet_rows(static_cast<size_t>(height_), 0);
  std::vector<uint8_t> same_rows(static_cast<size_t>(height_), 0);
  {
    const Bands bands(0, height_, 1, Parts(height_));
    workers_->For(bands.count(), [&](int part) {
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        const size_t base = static_cast<size_t>(y) * width_;
        int moving = 0;
        int unlike = 0;
        for (int x = 0; x < edge_; ++x) {
          const uint8_t is = type(base + x);
          moving += is == kMoving || is == kBack ? 1 : 0;
          if (ends) unlike += alike(base + x) ? 0 : 1;
        }
        quiet_rows[static_cast<size_t>(y)] = moving <= std::max(2, edge_ / 64) ? 1 : 0;
        same_rows[static_cast<size_t>(y)] = unlike <= std::max(2, edge_ / 32) ? 1 : 0;
      }
    });
  }
  const auto quiet = [&](int y) { return quiet_rows[static_cast<size_t>(y)] != 0; };
  const auto same = [&](int y) { return same_rows[static_cast<size_t>(y)] != 0; };
  const auto band = [&](bool top) {
    const auto row = [&](int k) { return top ? k : height_ - 1 - k; };
    int length = 0;
    if (ends) {
      int skip = 0;
      while (skip < kCell * 2 && !same(row(skip)) && quiet(row(skip))) ++skip;
      length = skip;
      while (length < height_ / 3 && same(row(length))) ++length;
      if (length == skip) length = 0;
    } else {
      while (length < height_ / 3 && quiet(row(length))) ++length;
    }
    return length < height_ / 3 ? length : 0;
  };
  top_band_ = band(true);
  bottom_band_ = band(false);
  const int first = top_band_;
  const int last = height_ - bottom_band_;

  const auto held = [&](size_t i) { return type(i) != kMoving && (!ends || alike(i)); };
  const auto color = [&](size_t i) {
    return ends ? tops[i] : PixelRow(now, static_cast<int>(i / width_))[i % width_];
  };
  const auto own = [&](size_t i) { return Near(color(i), page_, kSlack); };  // In the page's own color.
  std::vector<uint8_t> strip(static_cast<size_t>(width_), 0);
  if (last > first) {
    const Bands bands(first, last, 1, Parts(last - first));
    std::vector<int> standing(static_cast<size_t>(width_) * bands.count(), 0);
    workers_->For(bands.count(), [&](int part) {
      int* counts = standing.data() + static_cast<size_t>(part) * width_;
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        const size_t base = static_cast<size_t>(y) * width_;
        for (int x = 0; x < edge_; ++x) counts[x] += held(base + x) && !own(base + x) ? 1 : 0;
      }
    });
    for (int x = 0; x < edge_; ++x) {
      int total = 0;
      for (int part = 0; part < bands.count(); ++part) total += standing[static_cast<size_t>(part) * width_ + x];
      strip[static_cast<size_t>(x)] = total * 10 >= (last - first) * 9 ? 1 : 0;
    }
  }

  // Squares showing the detail of something fixed. Not where content was seen to scroll through, as through a column
  // before it sticks, nor where the two ends differ, as where content comes out from under something fixed, nor what
  // shows the page's own color. And squares where content was seen passing in front of what both ends show alike.
  std::vector<uint16_t> holds(cells, 0);
  std::vector<uint8_t> passed(cells, 0);
  {
    // Each part keeps to whole rows of squares.
    const Bands bands(first, last, kCell, Parts(last - first));
    workers_->For(bands.count(), [&](int part) {
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        const size_t base = static_cast<size_t>(y) * width_;
        const size_t line = static_cast<size_t>(y / kCell) * columns;
        for (int x = 0; x < edge_; ++x) {
          const size_t i = base + x;
          if (ends && moved_[i] != 0 && alike(i) && !own(i)) passed[line + x / kCell] = 1;
          if (strip[static_cast<size_t>(x)] != 0 || moved_[i] != 0 || (ends && !alike(i)) || own(i)) continue;
          const uint8_t is = type(i);
          bool detail = is == kFixed || is == kChanging;
          if (!detail && ends && x + 1 < edge_ && y + 1 < last) {
            detail = (strip[static_cast<size_t>(x) + 1] == 0 && alike(i + 1) && !Near(tops[i], tops[i + 1], kSlack)) ||
                     (alike(i + width_) && !Near(tops[i], tops[i + width_], kSlack));
          }
          if (detail) ++holds[line + x / kCell];
        }
      }
    });
  }
  grid_.assign(cells, 0);
  for (size_t cell = 0; cell < cells; ++cell) grid_[cell] = static_cast<uint8_t>(holds[cell] >= 3 ? 1 : 0);

  if (!textured_) {
    const auto behind = [&](size_t i) { return type(i) == kBack && Lead(wall_[i]) >= 2; };
    // Each part keeps to whole rows of the squares the background is counted in.
    const Bands bands(first, last, kSpread, Parts(last - first));
    // What holds still behind content, in reading order, for the one color most of it shows.
    std::vector<std::vector<uint32_t>> walls(static_cast<size_t>(bands.count()));
    workers_->For(bands.count(), [&](int part) {
      std::vector<uint32_t>& seen_here = walls[static_cast<size_t>(part)];
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        for (int x = 0; x < edge_; ++x) {
          const size_t i = static_cast<size_t>(y) * width_ + x;
          if (behind(i)) seen_here.push_back(wall_[i]);
        }
      }
    });
    uint32_t base = 0;
    uint32_t count = 0;
    for (const std::vector<uint32_t>& seen_here : walls) {
      for (const uint32_t wall : seen_here) Majority(base, count, wall);
    }
    walls = std::vector<std::vector<uint32_t>>{};
    const int spread_columns = (width_ + kSpread - 1) / kSpread;
    std::vector<uint16_t> spread(static_cast<size_t>(spread_columns) * ((height_ + kSpread - 1) / kSpread), 0);
    struct Tally {
      uint64_t seen = 0;
      uint64_t apart = 0;
    };
    std::vector<Tally> tallies(static_cast<size_t>(bands.count()));
    workers_->For(bands.count(), [&](int part) {
      Tally& tally = tallies[static_cast<size_t>(part)];
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        for (int x = 0; x < edge_; ++x) {
          const size_t i = static_cast<size_t>(y) * width_ + x;
          if (!behind(i)) continue;
          ++tally.seen;
          if (!Near(wall_[i], base, config::kScrollPixelSlack)) ++tally.apart;
          uint16_t& place = spread[static_cast<size_t>(y / kSpread) * spread_columns + x / kSpread];
          if (place < 0xFFFF) ++place;
        }
      }
    });
    uint64_t seen = 0;
    uint64_t apart = 0;
    for (const Tally& tally : tallies) {
      seen += tally.seen;
      apart += tally.apart;
    }
    // A picture behind the content shows between the content all over the view, as a chat's wallpaper does. What
    // only seems to, as a column that slid and stuck, a panel that slid in, or what changed by itself, does so in a
    // few places only, and taken for a picture it would leave everything fixed in the view to show at both ends.
    int places = 0;
    for (const uint16_t place : spread) places += place >= kSpread ? 1 : 0;
    const uint64_t area = static_cast<uint64_t>(last - first) * static_cast<uint64_t>(edge_);
    const int area_places = ((edge_ + kSpread - 1) / kSpread) * ((last - first + kSpread - 1) / kSpread);
    textured_ = seen >= std::max<uint64_t>(1000, area / 400) && places >= std::max(12, area_places / 40) && apart * 5 >= seen;
    texture_seen_ = seen;
    texture_apart_ = apart;
  }

  // Over a picture, the bands are the only fixed things. Whatever else holds still is the picture itself where no
  // content passed, or shows as the screens at the ends had it, and nothing shows it elsewhere: the content comes from
  // where it was seen to move, and the picture from where it was seen through the content.
  if (textured_) std::fill(grid_.begin(), grid_.end(), static_cast<uint8_t>(0));

  std::vector<uint8_t> owner(cells, 0);
  std::vector<uint8_t> visited(cells, 0);
  const int stack = std::max(gap, (last - first) / (kCell * 3));  // Squares apart that things one above another join across.
  const size_t most = static_cast<size_t>(inside) * std::max(1, (last - first + kCell - 1) / kCell) / 4;
  elements_ = 0;
  for (int start = 0; start < static_cast<int>(cells); ++start) {
    if (grid_[static_cast<size_t>(start)] == 0 || visited[static_cast<size_t>(start)] != 0 || start % columns >= inside) continue;
    stack_.clear();
    stack_.push_back(start);
    visited[static_cast<size_t>(start)] = 1;
    RECT box{start % columns, start / columns, start % columns + 1, start / columns + 1};
    for (size_t head = 0; head < stack_.size(); ++head) {
      const int cx = stack_[head] % columns;
      const int cy = stack_[head] / columns;
      box.left = std::min<LONG>(box.left, cx);
      box.top = std::min<LONG>(box.top, cy);
      box.right = std::max<LONG>(box.right, cx + 1);
      box.bottom = std::max<LONG>(box.bottom, cy + 1);
      for (int ny = std::max(cy - stack, 0); ny <= std::min(cy + stack, rows - 1); ++ny) {
        for (int nx = std::max(cx - gap, 0); nx <= std::min(cx + gap, inside - 1); ++nx) {
          const size_t next = static_cast<size_t>(ny) * columns + nx;
          if (grid_[next] == 0 || visited[next] != 0) continue;
          visited[next] = 1;
          stack_.push_back(ny * columns + nx);
        }
      }
    }
    // Content seen passing in front of much of it makes it a picture behind the content, as a chat's wallpaper before
    // enough of it is seen to tell so all over the view. Nothing ever passes in front of something fixed, but for what
    // happens to look alike along its rim.
    size_t behind = 0;
    for (const int cell : stack_) behind += passed[static_cast<size_t>(cell)];
    if (stack_.size() > most || behind >= std::max<size_t>(2, stack_.size() / 20)) continue;
    const int top = box.top * kCell;
    const int bottom = std::min<int>(box.bottom * kCell, height_);
    const uint8_t end = top <= first + reach ? kTopKind : bottom >= last - reach || top + bottom >= height_ ? kBottomKind : kTopKind;
    ++elements_;
    if (final) Trace("stitch: fixed %ld,%d-%ld,%d at the %s end, %zu squares of detail", box.left * kCell, top, box.right * kCell, bottom,
                     end == kTopKind ? "top" : "bottom", stack_.size());
    for (const int cell : stack_) {
      uint8_t& slot = owner[static_cast<size_t>(cell)];
      if (slot == 0) slot = end;
    }
  }
  {
    const Bands bands(0, height_, 1, Parts(height_));
    workers_->For(bands.count(), [&](int part) {
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        const size_t base = static_cast<size_t>(y) * width_;
        uint8_t* marks = marks_.data() + base;
        const uint8_t* owners = owner.data() + static_cast<size_t>(y / kCell) * columns;
        const uint8_t banded = y < first ? kTopKind : y >= last ? kBottomKind : 0;
        for (int x = 0; x < width_; ++x) {
          uint8_t is = 0;
          if (x < edge_) {
            if (banded != 0) {
              is = banded;
            } else if (owners[x / kCell] != 0 && strip[static_cast<size_t>(x)] == 0 && held(base + x)) {
              is = owners[x / kCell];
            }
          }
          marks[x] = static_cast<uint8_t>((marks[x] & ~kKind) | is);
        }
      }
    });
  }
  {
    const Bands bands(first, last, 1, Parts(last - first));
    std::vector<std::vector<int>> found(static_cast<size_t>(bands.count()));
    workers_->For(bands.count(), [&](int part) {
      std::vector<int>& here = found[static_cast<size_t>(part)];
      for (int y = bands.begin(part); y < bands.end(part); ++y) {
        for (int x = 0; x < edge_; ++x) {
          const int i = y * width_ + x;
          if (KindOf(marks_[static_cast<size_t>(i)]) != 0) here.push_back(i);
        }
      }
    });
    stack_.clear();
    for (const std::vector<int>& here : found) stack_.insert(stack_.end(), here.begin(), here.end());
  }
  while (!stack_.empty()) {
    const int i = stack_.back();
    stack_.pop_back();
    const int x = i % width_;
    const int y = i / width_;
    const uint8_t kind = KindOf(marks_[static_cast<size_t>(i)]);
    const auto reach_to = [&](int next) {
      const size_t at = static_cast<size_t>(next);
      if (KindOf(marks_[at]) != 0 || strip[at % static_cast<size_t>(width_)] != 0 || !held(at) || own(at)) return;
      marks_[at] = static_cast<uint8_t>((marks_[at] & ~kKind) | kind);
      stack_.push_back(next);
    };
    if (x > 0) reach_to(i - 1);
    if (x + 1 < edge_) reach_to(i + 1);
    if (y > first) reach_to(i - width_);
    if (y + 1 < last) reach_to(i + width_);
  }
  if (textured_) Background(false);
  Trace("stitch: told apart after %d scrolls in %.1f ms: bands %d and %d, background %d, %d fixed things", moves_, (NowSeconds() - began) * 1000.0,
        top_band_, bottom_band_, textured_ ? 1 : 0, elements_);
}

// The one picture of the background, as the view shows it between the bands at its top and bottom. With `ends`, once
// every row is made: first the screen the capture began with, taken at rest, wherever the content there was seen to be
// see-through; then what was seen through the content as it scrolled; then the screens at the ends, wherever the
// content there is see-through. Before, only what was seen through the content. A gap left, where content always
// covered the picture, is filled from what was seen on either side of it: blended across, down its column where it is
// short that way, else along its row, and else the picture's commonest color. One color is never carried far.
void Stitcher::Background(bool ends) {
  const size_t pixels = static_cast<size_t>(width_) * height_;
  tile_top_ = top_band_;
  tile_period_ = height_ - top_band_ - bottom_band_;
  if (tile_period_ < height_ / 4) {
    tile_top_ = 0;
    tile_period_ = height_;
  }
  const int bottom = tile_top_ + tile_period_;
  std::vector<uint8_t> known(pixels, 0);
  back_.assign(pixels, 0);
  uint32_t base = 0;
  uint32_t count = 0;
  RowStore::Reader reader(made_);
  // Of a screen showing content rows from `place` on: its pixels where the content was found surely see-through.
  const auto take = [&](const uint32_t* screen, int place) {
    for (int y = tile_top_; y < bottom; ++y) {
      const int row = place + y;
      if (!Held(row)) continue;
      const uint32_t* line = reader.Row(row);
      for (int x = 0; x < width_; ++x) {
        const size_t i = static_cast<size_t>(y) * width_ + x;
        if (known[i] != 0 || KindOf(marks_[i]) != 0 || (line[x] & kClear) == 0) continue;
        known[i] = 1;
        back_[i] = screen[i] & kColor;
        Majority(base, count, back_[i]);
      }
    }
  };
  if (ends) {
    const std::vector<uint32_t> start = StartPixels();
    take(start.data(), 0);
  }
  // Not what came and went while holding still, as a button to jump to the end or a date shown while scrolling, even
  // if it went only as the capture ended: that floats in front of the picture, and is no part of it.
  const auto went = [&](size_t i) {
    return TypeOf(still_[i], moved_[i], marks_[i]) == kChanging || ((marks_[i] & kFresh) != 0 && moved_[i] == 0);
  };
  for (int y = tile_top_; y < bottom; ++y) {
    for (int x = 0; x < width_; ++x) {
      const size_t i = static_cast<size_t>(y) * width_ + x;
      if (known[i] != 0 || KindOf(marks_[i]) != 0 || Lead(wall_[i]) < 2 || went(i)) continue;
      known[i] = 1;
      back_[i] = wall_[i] & kColor;
      Majority(base, count, back_[i]);
    }
  }
  if (ends) {
    take(top_.pixels, top_.place);
    take(bottom_.pixels, bottom_.place);
    // Then what either end shows wherever its content is not opaque, as plain parts of the picture no content ever
    // passed: so long as the other end shows the same there, or content.
    constexpr int kSlack = config::kScrollPixelSlack;
    RowStore::Reader other(made_);
    for (int y = tile_top_; y < bottom; ++y) {
      const int top_row = top_.place + y;
      const int bottom_row = bottom_.place + y;
      const uint32_t* top_line = Held(top_row) ? reader.Row(top_row) : nullptr;
      const uint32_t* bottom_line = Held(bottom_row) ? other.Row(bottom_row) : nullptr;
      for (int x = 0; x < width_; ++x) {
        const size_t i = static_cast<size_t>(y) * width_ + x;
        if (known[i] != 0 || KindOf(marks_[i]) != 0 || went(i)) continue;
        const bool from_top = top_line != nullptr && (top_line[x] & kBehind) != 0;
        const bool from_bottom = bottom_line != nullptr && (bottom_line[x] & kBehind) != 0;
        if (!from_top && !from_bottom) continue;
        if (from_top && from_bottom && !Near(top_.pixels[i], bottom_.pixels[i], kSlack)) continue;
        known[i] = 1;
        back_[i] = (from_top ? top_.pixels[i] : bottom_.pixels[i]) & kColor;
        Majority(base, count, back_[i]);
      }
    }
  }
  back_final_ = ends;
  wall_base_ = base;
  // Fills the unknown run [from, to) of a line whose pixels lie `step` apart, `low` and `high` being its ends.
  const auto fill = [&](size_t origin, size_t step, int from, int to, int low, int high) {
    const auto at = [&](int k) { return origin + static_cast<size_t>(k) * step; };
    const bool before = from > low;
    const bool after = to < high;
    const int length = to - from;
    for (int k = from; k < to; ++k) {
      const int up = k - from + 1;
      const int down = to - k;
      uint32_t color = 0;
      if (before && after && length <= kSpan) {
        color = Mix(back_[at(from - 1)], back_[at(to)], up, length + 1);
      } else if (before && up <= kReach && (!after || up <= down)) {
        color = back_[at(from - 1)];
      } else if (after && down <= kReach) {
        color = back_[at(to)];
      } else {
        continue;
      }
      back_[at(k)] = color;
      known[at(k)] = 2;
    }
  };
  for (int x = 0; x < width_; ++x) {
    for (int y = tile_top_; y < bottom;) {
      if (known[static_cast<size_t>(y) * width_ + x] != 0) {
        ++y;
        continue;
      }
      int end = y;
      while (end < bottom && known[static_cast<size_t>(end) * width_ + x] == 0) ++end;
      fill(static_cast<size_t>(x), static_cast<size_t>(width_), y, end, tile_top_, bottom);
      y = end;
    }
  }
  for (int y = tile_top_; y < bottom; ++y) {
    const size_t origin = static_cast<size_t>(y) * width_;
    for (int x = 0; x < width_;) {
      if (known[origin + x] != 0) {
        ++x;
        continue;
      }
      int end = x;
      while (end < width_ && known[origin + end] == 0) ++end;
      fill(origin, 1, x, end, 0, width_);
      x = end;
    }
    for (int x = 0; x < width_; ++x) {
      if (known[origin + x] == 0) back_[origin + x] = base;
    }
  }
}

// The reading the capture began with, unpacked.
std::vector<uint32_t> Stitcher::StartPixels() const {
  std::vector<uint32_t> pixels(static_cast<size_t>(width_) * height_, 0);
  workers_->For(static_cast<int>(start_.size()), [&](int part) {
    const int top = part * kStartRows;
    UnpackPixels(start_[static_cast<size_t>(part)], pixels.data() + static_cast<size_t>(top) * width_, width_, std::min(kStartRows, height_ - top));
  });
  return pixels;
}

// The end whose screen a content row shows, if it lies within a view of one: of both, the one whose middle is nearer.
const Stitcher::End* Stitcher::Showing(int row) const {
  const bool top = top_.pixels != nullptr && row >= top_.place && row < top_.place + height_;
  const bool bottom = bottom_.pixels != nullptr && row >= bottom_.place && row < bottom_.place + height_;
  if (top && bottom) return row * 2 < top_.place + bottom_.place + height_ ? &top_ : &bottom_;
  return top ? &top_ : bottom ? &bottom_ : nullptr;
}

// The row of the background that content row `row` shows. It repeats one band of the view at a time, in step with the
// screen of the end the row lies by, or else with the view where the capture began.
int Stitcher::TileRow(int row) const {
  const End* end = Showing(row);
  const int offset = row - (end != nullptr ? end->place : 0) - tile_top_;
  return tile_top_ + (offset % tile_period_ + tile_period_) % tile_period_;
}

// One content row in view: each pixel as taken whole; or as it looked wherever it was in the view, if it looked alike
// far apart more often than not, or it was seen just once, as when the view scrolls fast. Otherwise what it is is not
// known, and the background shows there if there is one: it looked unlike itself far apart, as see-through content
// does, or what scrolled under something that stuck after it moved; or, seen just once, it showed just what holds still
// where it was seen; or it was never seen, as what was always behind something fixed. Content that changes by itself
// as it scrolls is the one exception, and shows as last seen.
void Stitcher::Blend(int row, uint32_t* out) const {
  const uint32_t* truth = reinterpret_cast<const uint32_t*>(truth_.Row(row));
  const uint32_t* votes = reinterpret_cast<const uint32_t*>(votes_.Row(row));
  const uint8_t* backs = backs_.Row(row);
  const uint16_t* sights = reinterpret_cast<const uint16_t*>(sights_.Row(row));
  for (int x = 0; x < width_; ++x) {
    const uint32_t anchor = truth[x];
    if ((anchor & kFrozen) != 0) {
      out[x] = anchor & (kColor | kFrozen);
      continue;
    }
    if ((anchor & kSure) == 0) {
      out[x] = page_ | kBehind | kUnsure | kUnseen;
      continue;
    }
    if (Lead(votes[x]) > backs[x]) {
      out[x] = votes[x] & kColor;
    } else if (backs[x] == 0) {
      const bool through = Through(static_cast<size_t>(std::max<int>(sights[x], 1) - 1) * width_ + x, anchor);
      out[x] = through ? page_ | kBehind | kUnsure | kUnseen : anchor & kColor;
    } else if ((anchor & kDynamic) != 0) {
      out[x] = (anchor & kColor) | kBehind;
    } else {
      out[x] = page_ | kBehind | kUnseen | (backs[x] >= 4 && votes[x] == 0 ? kClear : 0u);
    }
  }
}

// Whether a pixel of the view showing `color` showed just what holds still there: what it mostly showed while holding
// still, or where that was never seen, as on plain parts of a picture behind the content that look alike a scroll
// apart, the picture as filled in so far.
bool Stitcher::Through(size_t index, uint32_t color) const {
  constexpr int kSlack = config::kScrollPixelSlack;
  if (Lead(wall_[index]) >= 2) return Near(color, wall_[index], kSlack);
  return textured_ && back_.size() == wall_.size() && Near(color, back_[index], kSlack);
}

// Counts the content a row made shows toward the page's own color.
void Stitcher::Tally(const uint32_t* line) {
  for (int x = 0; x < edge_; ++x) {
    if ((line[x] & kUnseen) != 0) continue;
    const uint32_t color = line[x] & kColor;
    Shade& shade = shades_[(color >> 12 & 0xF00u) | (color >> 8 & 0xF0u) | (color >> 4 & 0xFu)];
    ++shade.count;
    Majority(shade.color, shade.lead, color);
  }
}

// The page's own color: the color most of the content made shows.
void Stitcher::Page() {
  const Shade* most = nullptr;
  for (const Shade& shade : shades_) {
    if (shade.count != 0 && (most == nullptr || shade.count > most->count)) most = &shade;
  }
  if (most != nullptr) page_ = most->color;
}

void Stitcher::Compose(int row, uint32_t* out) {
  Blend(row, out);
  Tally(out);
  uint32_t tone = kNoTone;
  for (int x = 0; x < width_ && tone != kMixed; ++x) {
    const uint32_t color = out[x] & kColor;
    tone = tone == kNoTone || tone == color ? color : kMixed;
  }
  tones_[static_cast<size_t>(row + row_base_)] = tone;
}

// Shows the one picture of the background behind a content row made, where the content is see-through or not known:
// as finally known once the capture finishes, and meanwhile as seen so far. Beneath a scroll bar drawn over the
// content too. With no picture, content never seen shows its column's one color, as a side bar's or a window edge's,
// and else the page's own color, as now known.
void Stitcher::Render(int row, uint32_t* line) const {
  if (!textured_ || back_.empty()) {
    const bool strips = strip_.size() == static_cast<size_t>(width_);
    for (int x = 0; x < bar_; ++x) {
      if ((line[x] & kUnseen) == 0) continue;
      const uint32_t strip = strips ? strip_[static_cast<size_t>(x)] : kNoTone;
      line[x] = (strip != kNoTone ? strip : page_) | (line[x] & ~kColor);
    }
    return;
  }
  const size_t base = static_cast<size_t>(TileRow(row)) * width_;
  for (int x = 0; x < bar_; ++x) {
    if ((line[x] & kBehind) == 0) continue;
    const size_t i = base + static_cast<size_t>(x);
    const bool live = !back_final_ && KindOf(marks_[i]) == 0 && Lead(wall_[i]) >= 2;
    line[x] = ((live ? wall_[i] : back_[i]) & kColor) | (line[x] & (kBehind | kUnsure));
  }
}

// Held rows [first, first + count), with the ends painted on.
void Stitcher::PaintHeld(int first, int count, uint32_t* out) const {
  RowStore::Reader reader(made_);
  for (int i = 0; i < count; ++i) {
    uint32_t* line = out + static_cast<size_t>(i) * width_;
    std::memcpy(line, reader.Row(first + i), static_cast<size_t>(width_) * 4);
    Paint(first + i, line);
  }
}

// Paints the ends on a content row. Within a view of an end, the row is that end's screen as it was, but where the
// screen shows something fixed that belongs at the other end: there the other end's screen shows the content behind
// it, if it reaches the row and shows content there, and else the content behind shows as made. Whatever is fixed and
// belongs at an end shows from that end's screen wherever the screen reaches, so that it shows whole.
void Stitcher::Paint(int row, uint32_t* line) const {
  Render(row, line);
  const int at_top = row - top_.place;
  const int at_bottom = row - bottom_.place;
  const bool top = top_.pixels != nullptr && at_top >= 0 && at_top < height_;
  const bool bottom = bottom_.pixels != nullptr && at_bottom >= 0 && at_bottom < height_;
  if (const End* end = Showing(row); end != nullptr) {
    const size_t at = static_cast<size_t>(row - end->place) * width_;
    const size_t top_at = top ? static_cast<size_t>(at_top) * width_ : 0;
    const size_t bottom_at = bottom ? static_cast<size_t>(at_bottom) * width_ : 0;
    const bool across = end == &top_ ? bottom : top;  // Whether the other end's screen reaches the row too.
    const End& other = end == &top_ ? bottom_ : top_;
    const size_t other_at = end == &top_ ? bottom_at : top_at;
    for (int x = 0; x < edge_; ++x) {
      if (bottom && KindOf(marks_[bottom_at + x]) == kBottomKind) {
        line[x] = bottom_.pixels[bottom_at + x] & kColor;
      } else if (top && KindOf(marks_[top_at + x]) == kTopKind) {
        line[x] = top_.pixels[top_at + x] & kColor;
      } else if (KindOf(marks_[at + x]) == 0) {
        line[x] = end->pixels[at + x] & kColor;
      } else if (across && KindOf(marks_[other_at + x]) == 0) {
        line[x] = other.pixels[other_at + x] & kColor;
      }
    }
  }
  // A scroll bar at the side shows its arrows at the two ends and its plain track between them.
  if (bar_ready_) {
    const bool top_cap = at_top >= 0 && at_top < cap_top_;
    const bool bottom_cap = at_bottom >= height_ - cap_bottom_ && at_bottom < height_;
    const uint32_t* pixels = top_cap ? top_.pixels + static_cast<size_t>(at_top) * width_
                             : bottom_cap ? bottom_.pixels + static_cast<size_t>(at_bottom) * width_ : nullptr;
    for (int x = bar_; x < width_; ++x) line[x] = pixels != nullptr ? pixels[x] & kColor : track_[static_cast<size_t>(x - bar_)];
  }
}

bool Stitcher::InEnds(int row) const {
  return (row >= min_position_ && row < min_position_ + height_) || (row >= max_position_ && row < max_position_ + height_);
}

// Rows held near an end that the end has moved away from are final as they are, unless they wait for the picture.
void Stitcher::Flush(int old_min, int old_max) {
  if (Deferring()) return;
  const auto render = [&](int from, int count, uint32_t* out) {
    RowStore::Reader reader(made_);
    for (int i = 0; i < count; ++i) {
      uint32_t* line = out + static_cast<size_t>(i) * width_;
      std::memcpy(line, reader.Row(from + i), static_cast<size_t>(width_) * 4);
      Render(from + i, line);
      Fill(line);
    }
  };
  const Maker rows(width_, render);
  const auto flush = [&](int first, int last) {
    std::vector<Run> runs;
    for (int row = first; row < last; ++row) {
      if (!Held(row) || InEnds(row)) continue;
      if (!runs.empty() && runs.back().first + runs.back().count == row) {
        ++runs.back().count;
      } else {
        runs.push_back(Run{row, 1, true});
      }
    }
    for (const Run& run : runs) sink_->Rows(run.first, run.count, rows, true);
    for (const Run& run : runs) {
      for (int row = run.first; row < run.first + run.count; ++row) held_[static_cast<size_t>(row + row_base_)] = 0;
      made_.Erase(run.first, run.first + run.count);
      Touched(run.first, run.first + run.count);
    }
  };
  flush(old_min, old_min + height_);
  flush(old_max, old_max + height_);
}

// Rows past where the capture now ends are let go, to be made again should they ever be seen.
void Stitcher::Drop(int first, int last) {
  for (int row = first; row < last; ++row) held_[static_cast<size_t>(row + row_base_)] = 0;
  made_.Erase(first, last);
  Touched(first, last);
  Undo(first, last);
}

// Rows [first, last) were made, held or let go, which changes how the ends whose views they lie in show.
void Stitcher::Touched(int first, int last) {
  if (first < top_.place + height_ && last > top_.place) ++top_.held;
  if (first < bottom_.place + height_ && last > bottom_.place) ++bottom_.held;
}

// A scroll bar at the side shows no content. What changes in its columns as the content scrolls is never what
// scrolled, while beside it nearly everything that changes is: its columns, up to where its track meets the content,
// are drawn as the bar. A bar drawn over the content keeps the content, and takes nothing fixed from an end, so its
// thumb never shows.
void Stitcher::FindBar() {
  bar_ = width_;
  const int reach = std::min(std::max(static_cast<int>(std::lround(config::kScrollBarWidth * scale_)), 8), width_ / 4);
  const int stop = width_ - reach;
  const uint32_t quiet = static_cast<uint32_t>(moves_) / 8u;
  const auto still = [&](int x) { return column_odd_[static_cast<size_t>(x)] + column_true_[static_cast<size_t>(x)] <= quiet; };
  const auto odd = [&](int x, uint32_t share) {
    const uint32_t changed = column_odd_[static_cast<size_t>(x)];
    return changed >= 4 && changed * share >= column_true_[static_cast<size_t>(x)];
  };
  // Whether columns x and x + 1 differ down most of the view, as content and a track beside it do.
  const auto border = [&](int x) {
    int differ = 0;
    for (int y = 0; y < height_; ++y) {
      const uint32_t* pixels = bottom_.pixels + static_cast<size_t>(y) * width_;
      differ += Near(pixels[x], pixels[x + 1], config::kScrollPixelSlack) ? 0 : 1;
    }
    return differ * 5 >= height_ * 3;
  };
  int outer = width_ - 1;
  while (outer >= stop && still(outer)) --outer;
  int x = outer;
  while (x >= stop && odd(x, 2)) --x;
  if (x < outer && x >= stop) {
    while (x >= stop && still(x) && !border(x)) --x;
    bar_ = x + 1;
    edge_ = bar_;
    return;
  }
  x = outer;
  while (x >= stop && odd(x, 4)) --x;
  edge_ = x < outer ? x + 1 : width_;
}

// The bar's track, per column the color both ends mostly show there, and the rows of the arrows at its two ends.
void Stitcher::Tracks() {
  track_.assign(static_cast<size_t>(width_ - bar_), 0);
  for (int x = bar_; x < width_; ++x) {
    uint32_t color = 0;
    uint32_t count = 0;
    for (const End* end : {&top_, &bottom_}) {
      for (int y = 0; y < height_; ++y) Majority(color, count, end->pixels[static_cast<size_t>(y) * width_ + x]);
    }
    track_[static_cast<size_t>(x - bar_)] = color;
  }
  cap_top_ = Cap(true);
  cap_bottom_ = Cap(false);
  bar_ready_ = bar_ < width_;
}

// How many rows at one end of the bar are its arrow: rows both ends show alike, from the edge through the first run
// that is not plain track, up to the plain row after it. Anything else there, as a thumb that has not moved off it,
// makes no arrow.
int Stitcher::Cap(bool top) const {
  const End& end = top ? top_ : bottom_;
  const int limit = std::min(height_ / 4, (width_ - bar_) * 2 + 4);
  bool inside = false;
  for (int i = 0; i < limit; ++i) {
    const int y = top ? i : height_ - 1 - i;
    const size_t base = static_cast<size_t>(y) * width_;
    bool alike = true;
    bool plain = true;
    for (int x = bar_; x < width_; ++x) {
      alike = alike && Near(top_.pixels[base + x], bottom_.pixels[base + x], config::kScrollPixelSlack);
      plain = plain && Near(end.pixels[base + x], track_[static_cast<size_t>(x - bar_)], config::kScrollPixelSlack);
    }
    if (!alike) return 0;
    if (!plain) {
      inside = true;
    } else if (inside) {
      return i;
    }
  }
  return 0;
}

// Per column, the one color it shows nearly all the way down in the screens at both ends, if it does, as the plain
// inside of a bar along the side or the edge of a window: it runs on down the image where content was never seen.
void Stitcher::Strips() {
  constexpr int kSlack = config::kScrollPixelSlack;
  strip_.assign(static_cast<size_t>(width_), 0);
  const Bands bands(0, width_, kColumnAlign, Parts(height_ * 2));
  workers_->For(bands.count(), [&](int part) {
    const int left = bands.begin(part);
    const int right = bands.end(part);
    std::vector<uint32_t> counts(static_cast<size_t>(right - left), 0);
    for (const End* end : {&top_, &bottom_}) {
      for (int y = 0; y < height_; ++y) {
        const uint32_t* pixels = end->pixels + static_cast<size_t>(y) * width_;
        for (int x = left; x < right; ++x) Majority(strip_[static_cast<size_t>(x)], counts[static_cast<size_t>(x - left)], pixels[x]);
      }
    }
    std::fill(counts.begin(), counts.end(), 0u);
    for (const End* end : {&top_, &bottom_}) {
      for (int y = 0; y < height_; ++y) {
        const uint32_t* pixels = end->pixels + static_cast<size_t>(y) * width_;
        for (int x = left; x < right; ++x) counts[static_cast<size_t>(x - left)] += Near(pixels[x], strip_[static_cast<size_t>(x)], kSlack) ? 1 : 0;
      }
    }
    for (int x = left; x < right; ++x) {
      if (static_cast<uint64_t>(counts[static_cast<size_t>(x - left)]) * 10 < static_cast<uint64_t>(height_) * 2 * 9) strip_[static_cast<size_t>(x)] = kNoTone;
    }
  });
}

// Draws the bar's plain track on a row of content.
void Stitcher::Fill(uint32_t* line) const {
  if (!bar_ready_) return;
  for (int x = bar_; x < width_; ++x) line[x] = track_[static_cast<size_t>(x - bar_)];
}

// Writes what was found of the view as pictures, for tracing: both ends' screens; what each pixel was (dark gray
// content, green a background behind it, red held still, magenta came, went or changed, blue a band content never
// showed in, black not known), tinted cyan where something fixed belongs at the top end and yellow where at the
// bottom end; what holds still; where content passed in front of it; and the background as it repeats.
void Stitcher::Dump() const {
  const size_t pixels = static_cast<size_t>(width_) * height_;
  const std::vector<uint32_t> start = StartPixels();
  TraceImage(L"start", start.data(), width_, height_);
  TraceImage(L"top", top_.pixels, width_, height_);
  TraceImage(L"bottom", bottom_.pixels, width_, height_);
  std::vector<uint32_t> picture(pixels, 0);
  constexpr uint32_t kColors[] = {0x000000, 0x404040, 0x20C040, 0xE02020, 0xFF00FF, 0x203070};
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const size_t i = static_cast<size_t>(y) * width_ + x;
      const bool banded = y < top_band_ || y >= height_ - bottom_band_;
      uint32_t color = kColors[banded ? kBand : TypeOf(still_[i], moved_[i], marks_[i])];
      if (KindOf(marks_[i]) != 0) {
        const uint32_t tint = KindOf(marks_[i]) == kBottomKind ? 0xFFFF00u : 0x00FFFFu;
        color = (((color & 0xFEFEFE) >> 1) + ((tint & 0xFEFEFE) >> 1)) & kColor;
      }
      picture[i] = color;
    }
  }
  TraceImage(L"kinds", picture.data(), width_, height_);
  for (size_t i = 0; i < pixels; ++i) picture[i] = Lead(wall_[i]) >= 2 ? wall_[i] & kColor : 0;
  TraceImage(L"wall", picture.data(), width_, height_);
  for (size_t i = 0; i < pixels; ++i) picture[i] = ((marks_[i] & kCovers) != 0 ? 0xFF0000u : 0) | (moved_[i] != 0 ? 0x0000FFu : 0) | (still_[i] != 0 ? 0x00FF00u : 0);
  TraceImage(L"behind", picture.data(), width_, height_);
  if (!back_.empty()) TraceImage(L"back", back_.data(), width_, height_);
}

// Hands on content rows [first, last) as they would be made now, as not final: those held, or those in view.
void Stitcher::Show(int first, int last, bool held) {
  std::vector<Run> runs;
  for (int row = first; row < last; ++row) {
    const bool shown = held ? Held(row) : !Held(row) && !finished_ && row >= position_ && row < position_ + height_ && !Done(row);
    if (!shown) continue;
    if (!runs.empty() && runs.back().first + runs.back().count == row) {
      ++runs.back().count;
    } else {
      runs.push_back(Run{row, 1, false});
    }
  }
  if (runs.empty()) return;
  const auto made = [&](int from, int count, uint32_t* out) { PaintHeld(from, count, out); };
  const auto blended = [&](int from, int count, uint32_t* out) {
    for (int i = 0; i < count; ++i) {
      uint32_t* line = out + static_cast<size_t>(i) * width_;
      Blend(from + i, line);
      Paint(from + i, line);
    }
  };
  const Maker made_rows(width_, made);
  const Maker blended_rows(width_, blended);
  const RowMaker& rows = held ? static_cast<const RowMaker&>(made_rows) : static_cast<const RowMaker&>(blended_rows);
  for (const Run& run : runs) sink_->Rows(run.first, run.count, rows, false);
}

// All that the view of an end shows depends on.
Stitcher::Shown Stitcher::Showable(bool top) const {
  const End& end = top ? top_ : bottom_;
  const End& other = top ? bottom_ : top_;
  Shown shown;
  shown.valid = !textured_;
  shown.place = end.place;
  shown.version = end.version;
  shown.held = end.held;
  if (std::abs(other.place - end.place) < height_) {
    shown.other_place = other.place;
    shown.other_version = other.version;
  }
  shown.fixed = fixed_version_;
  shown.page = page_;
  shown.strips = strip_.size();
  shown.edge = edge_;
  shown.bar = bar_;
  shown.bar_ready = bar_ready_;
  shown.cap_top = cap_top_;
  shown.cap_bottom = cap_bottom_;
  shown.track = track_;
  return shown;
}

// The held rows in the view of an end, shown again just as they were while nothing they are painted from has changed.
void Stitcher::ShowEnd(bool top) {
  const End& end = top ? top_ : bottom_;
  Shown& last = top ? shown_top_ : shown_bottom_;
  Shown now = Showable(top);
  const int slot = top ? 0 : 1;
  if (now.valid && now == last) {
    sink_->Replay(slot);
    return;
  }
  sink_->Record(slot);
  Show(end.place, end.place + height_, true);
  sink_->Stop();
  last = std::move(now);
}

void Stitcher::Preview() {
  if (finished_ || sink_ == nullptr) return;
  if (moves_ >= kBarMoves) {
    const int bar = bar_;
    const int edge = edge_;
    FindBar();
    Tracks();
    if (bar != bar_ || edge != edge_) ++revision_;
  }
  if (shown_ != revision_) {
    shown_ = revision_;
    ShowEnd(true);
    ShowEnd(false);
  }
  Show(position_, position_ + height_, false);
}

bool Stitcher::Done(int row) const {
  for (const auto& run : done_) {
    if (row < run.first) return false;
    if (row < run.second) return true;
  }
  return false;
}

void Stitcher::Mark(int first, int last) {
  if (first >= last) return;
  auto at = done_.begin();
  while (at != done_.end() && at->second < first) ++at;
  auto end = at;
  while (end != done_.end() && end->first <= last) {
    first = std::min(first, end->first);
    last = std::max(last, end->second);
    ++end;
  }
  at = done_.erase(at, end);
  done_.insert(at, {first, last});
}

void Stitcher::Undo(int first, int last) {
  if (first >= last) return;
  std::vector<std::pair<int, int>> left;
  left.reserve(done_.size() + 1);
  for (const auto& run : done_) {
    if (run.second <= first || run.first >= last) {
      left.push_back(run);
      continue;
    }
    if (run.first < first) left.push_back({run.first, first});
    if (run.second > last) left.push_back({last, run.second});
  }
  done_ = std::move(left);
}

// Whether rows made are all of one color and the same one.
bool Stitcher::Blank(int first, int last) const {
  uint32_t tone = kNoTone;
  for (int row = first; row < last; ++row) {
    const uint32_t here = tones_[static_cast<size_t>(row + row_base_)];
    if (!Done(row) || here == kMixed) return false;
    if (tone != kNoTone && tone != here) return false;
    tone = here;
  }
  return first < last;
}

void Stitcher::Finish() {
  if (finished_ || tracker_.ref().data == nullptr) return;
  finished_ = true;
  // Whatever changes by itself in view is taken whole, as the screen showed it last.
  Freeze(tracker_.ref(), position_, 0, height_);
  FindBar();
  Tracks();
  Segment(true, tracker_.ref());
  Strips();
  Make(position_, position_ + height_);
  if (textured_) Background(true);
  Trace("stitch: view %dx%d, %d scrolls, bands %d and %d, background %d, bar %d, edge %d, %d fixed things", width_, height_, moves_,
        top_band_, bottom_band_, textured_ ? 1 : 0, bar_, edge_, elements_);
  Trace("stitch: content came in front of what holds still again and again at %llu pixels, %llu unlike its commonest color",
        static_cast<unsigned long long>(texture_seen_), static_cast<unsigned long long>(texture_apart_));
  // Everything held is final now, with each end painted on as the screen showed it there, and the picture behind.
  std::vector<Run> runs;
  for (int row = min_position_; row < max_position_ + height_; ++row) {
    if (!Held(row)) continue;
    if (!runs.empty() && runs.back().first + runs.back().count == row && runs.back().count < kFinishRows) {
      ++runs.back().count;
    } else {
      runs.push_back(Run{row, 1, true});
    }
  }
  const auto made = [&](int from, int count, uint32_t* out) { PaintHeld(from, count, out); };
  const Maker rows(width_, made);
  for (const Run& run : runs) sink_->Rows(run.first, run.count, rows, true);
  for (const Run& run : runs) {
    for (int row = run.first; row < run.first + run.count; ++row) held_[static_cast<size_t>(row + row_base_)] = 0;
  }
  truth_.Release(position_, position_ + height_);
  votes_.Release(position_, position_ + height_);
  backs_.Release(position_, position_ + height_);
  sights_.Release(position_, position_ + height_);
  made_.Clear();
}

}  // namespace sstk
