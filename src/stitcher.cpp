#include "stitcher.h"

#include "config/core.h"
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
constexpr int kReach = 24;     // How far one side of a gap in the background is carried into it.
constexpr int kSpan = 160;     // How wide a gap in the background may be to be filled by blending its two sides.

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

constexpr int kFlat = 2;  // Per color channel, what a flat color, as a message's inside, differs from itself by.

// What a content pixel keeps above its color.
constexpr uint32_t kSure = 1u << 24;      // It was seen to scroll, showing this,
constexpr uint32_t kAgreed = 4u << 24;    // and seen so twice in a row: one sight may be something fixed that moved once.
constexpr uint32_t kFrozen = 2u << 24;    // It is part of something that changes by itself, taken whole.
constexpr uint32_t kBehind = 8u << 24;    // Of a row made: not surely content, so the background shows there if there is one.
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
inline uint8_t TypeOf(uint8_t still, uint8_t moved, uint8_t flips, uint8_t covers) {
  if (covers >= 2 && still >= 2) return kBack;
  if (moved != 0 && moved * 3 >= still) return kMoving;
  if (flips != 0) return kChanging;
  return still != 0 ? kFixed : kUnknown;
}

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

bool Stitcher::Start(const PixelView& first, float scale, int limit_rows, Sink* sink) {
  width_ = first.width;
  height_ = first.height;
  scale_ = scale;
  limit_rows_ = limit_rows;
  sink_ = sink;
  if (width_ < Tracker::kSegment || width_ > 0xFFFF || height_ <= Tracker::kMinOverlap * 2 || height_ > 0x7FFF) return false;
  tracker_.Reset(width_, height_);
  const int origin = limit_rows + height_;
  const int capacity = origin * 2 + height_;
  const size_t stride = static_cast<size_t>(width_) * 4;
  if (!truth_.Reserve(stride, capacity, origin) || !votes_.Reserve(stride, capacity, origin) || !made_.Reserve(stride, capacity, origin) ||
      !backs_.Reserve(static_cast<size_t>(width_), capacity, origin)) {
    return false;
  }
  if (!truth_.Ensure(0, height_) || !votes_.Ensure(0, height_) || !backs_.Ensure(0, height_)) return false;
  row_base_ = origin;
  held_.assign(static_cast<size_t>(capacity), 0);
  tones_.assign(static_cast<size_t>(capacity), kMixed);
  const size_t pixels = static_cast<size_t>(width_) * height_;
  mask_.assign(pixels, 0);
  how_.assign(pixels, 0);
  still_.assign(pixels, 0);
  moved_.assign(pixels, 0);
  covers_.assign(pixels, 0);
  flips_.assign(pixels, 0);
  fresh_.assign(pixels, 0);
  odd_.assign(pixels, 0);
  wall_.assign(pixels, 0);
  fixed_.assign(pixels, 0);
  back_.clear();
  column_true_.assign(static_cast<size_t>(width_), 0);
  column_odd_.assign(static_cast<size_t>(width_), 0);
  backdrop_.assign(static_cast<size_t>(width_), 0);
  backdrop_count_.assign(static_cast<size_t>(width_), 0);
  line_.assign(static_cast<size_t>(width_), 0);
  textured_ = false;
  tile_top_ = 0;
  tile_period_ = height_;
  next_segment_ = 1;
  bar_ = edge_ = width_;
  bar_ready_ = false;
  tracker_.SetRef(first);
  for (int y = 0; y < height_; ++y) {
    const uint32_t* pixels_in = PixelRow(first, y);
    for (int x = 0; x < width_; ++x) {
      uint32_t& count = backdrop_count_[static_cast<size_t>(x)];
      if (count == 0) {
        backdrop_[static_cast<size_t>(x)] = pixels_in[x] & kColor;
        count = 1;
      } else {
        count = Same(backdrop_[static_cast<size_t>(x)], pixels_in[x]) ? count + 1 : count - 1;
      }
    }
  }
  position_ = min_position_ = max_position_ = rest_min_ = rest_max_ = 0;
  serial_ = 0;
  Capture(top_, first, 0);
  Capture(bottom_, first, 0);
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

Stitcher::Step Stitcher::Feed(const PixelView& frame, POINT pointer) {
  kept_ = false;
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
    return Accept(frame, match.shift, match.sharp) ? Step::kMoved : Step::kLost;
  }
  // Change in a few rows only is something changing in place, as a hover, a picture loading, or an animation. Once it
  // holds still, and enough of what is drawn shows unchanged to be sure the view did not move, it is taken in.
  if (tracker_.changed_rows() * 3 <= height_ && static_cast<uint64_t>(tracker_.changed_count()) * 10 <= static_cast<uint64_t>(width_) * height_) {
    if (settled && Anchored()) Stay();
    Rested();
    return Step::kRest;
  }
  lost_replaced_ = tracker_.changed_rows() >= height_ * config::kScrollReplacedShare && tracker_.changed_width() >= width_ * config::kScrollReplacedShare;
  return Step::kLost;
}

// Takes in the reading the tracker holds, `shift` rows on from the last one placed, and makes it the one the next is
// lined up with.
bool Stitcher::Accept(const PixelView& frame, int shift, bool sharp) {
  const int from = position_;
  const int place = from + shift;
  if (std::abs(place) > limit_rows_) return false;
  const int low = std::min(from, place);
  const int high = std::max(from, place) + height_;
  if (!truth_.Ensure(low, high) || !votes_.Ensure(low, high) || !backs_.Ensure(low, high)) return false;
  // Rows coming into view start with nothing seen of them.
  const int enter_first = shift > 0 ? std::max(from + height_, place) : place;
  const int enter_last = shift > 0 ? place + height_ : std::min(from, place + height_);
  for (int row = enter_first; row < enter_last; ++row) {
    if (Done(row)) continue;
    std::memset(truth_.Row(row), 0, static_cast<size_t>(width_) * 4);
    std::memset(votes_.Row(row), 0, static_cast<size_t>(width_) * 4);
    std::memset(backs_.Row(row), 0, static_cast<size_t>(width_));
  }
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
    Blur(ref, frame, place);
  }
  ++serial_;
  const int old_min = min_position_;
  const int old_max = max_position_;
  min_position_ = std::min(min_position_, place);
  max_position_ = std::max(max_position_, place);
  if (place == min_position_) Capture(top_, frame, place);
  if (place == max_position_) Capture(bottom_, frame, place);
  // The view is told apart after the first scroll, and again as more is seen: often at first, then less often. Nothing
  // counts for the content before, when what is fixed is not known yet.
  if (sorted && moves_ >= next_segment_) {
    Segment();
    next_segment_ = moves_ + std::clamp(moves_ / 2, 1, 16);
  }
  const int leave_first = shift > 0 ? from : std::max(place + height_, from);
  const int leave_last = shift > 0 ? std::min(place, from + height_) : from + height_;
  Make(leave_first, leave_last);
  Flush(old_min, old_max);
  truth_.Release(leave_first, leave_last);
  votes_.Release(leave_first, leave_last);
  backs_.Release(leave_first, leave_last);
  tracker_.Promote();
  kept_ = true;
  pending_ = 0;
  position_ = place;
  return true;
}

// Takes in a reading that changed in place, as where a picture loaded. What lights up around the pointer is passed
// over.
void Stitcher::Stay() {
  const PixelView now = tracker_.now();
  const bool top = top_.serial == serial_ && top_.place == position_;
  const bool bottom = bottom_.serial == serial_ && bottom_.place == position_;
  for (int y = 0; y < height_; ++y) {
    if (tracker_.ChangedInRow(y) == 0 || tracker_.Hovered(y)) continue;
    const uint8_t* changed = tracker_.Changed(y);
    const uint32_t* pixels = PixelRow(now, y);
    uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
    const int row = position_ + y;
    const bool wanted = !Done(row);
    uint32_t* votes = wanted ? reinterpret_cast<uint32_t*>(votes_.Row(row)) : nullptr;
    for (int x = 0; x < width_; ++x) {
      if (changed[x] == 0) continue;
      const size_t index = static_cast<size_t>(y) * width_ + x;
      const uint8_t was = state[x];
      if ((was & kHeld) != 0 && moved_[index] == 0) Bump(flips_[index]);
      state[x] = static_cast<uint8_t>((was & (kHeld | kSeen)) | kOther);
      // A change seen only at rest might be a scroll too small to tell, so it counts barely: content seen to scroll
      // stays as seen, and the end's own reading shows what changed.
      if (segments_ > 0 && wanted && (was & kHeld) == 0 && fixed_[index] == 0) Vote(votes[x], pixels[x], 1);
      if (top) top_.pixels[index] = pixels[x];
      if (bottom) bottom_.pixels[index] = pixels[x];
    }
  }
  tracker_.Promote();
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
  for (int y = 0; y < height_; ++y) {
    if (tracker_.ChangedInRow(y) == 0 || tracker_.Hovered(y)) continue;
    const uint8_t* changed = tracker_.Changed(y);
    const uint32_t* pixels = PixelRow(now, y);
    for (int x = 0; x < width_; ++x) {
      if (changed[x] == 0) continue;
      const size_t index = static_cast<size_t>(y) * width_ + x;
      if ((mask_[index] & kHeld) != 0 && moved_[index] == 0) Bump(flips_[index]);
      if (top) top_.pixels[index] = pixels[x];
      if (bottom) bottom_.pixels[index] = pixels[x];
    }
  }
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
    Capture(top_, tracker_.ref(), position_);
  }
  if (position_ == rest_max_ && max_position_ > position_ && Blank(position_ + height_, max_position_ + height_)) {
    Drop(position_ + height_, max_position_ + height_);
    max_position_ = position_;
    Capture(bottom_, tracker_.ref(), position_);
  }
}

// Whether all that changed is what changes by itself or holds still where it is, as a video does while the view
// rests. Such a reading says nothing of where the view is, and is not worth looking for a shift in.
bool Stitcher::Calm() const {
  uint32_t elsewhere = 0;
  for (int y = 0; y < height_; ++y) {
    if (tracker_.ChangedInRow(y) == 0) continue;
    const uint8_t* changed = tracker_.Changed(y);
    const uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
    for (int x = 0; x < width_; ++x) {
      if (changed[x] != 0 && (state[x] & (kOther | kHeld)) == 0 && ++elsewhere > kQuietPixels) return false;
    }
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
  uint32_t moved = 0;
  uint32_t held = 0;
  // Every fourth row is enough to tell.
  for (int y = std::max(0, -shift); y < std::min(height_, height_ - shift); y += 4) {
    if (tracker_.Hovered(y)) continue;
    const uint32_t* a = PixelRow(now, y);
    const uint32_t* s = PixelRow(ref, y);
    const uint32_t* m = PixelRow(ref, y + shift);
    const uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
    for (int x = 0; x < width_; ++x) {
      const bool stay = Same(a[x], s[x]);
      const bool follow = Same(a[x], m[x]);
      if (follow && !stay) {
        ++moved;
      } else if (stay && !follow && (state[x] & kTrue) != 0) {
        ++held;
      }
    }
  }
  return held <= moved;
}

// Whether enough of what is drawn shows unchanged where the last placed reading had it to be sure the view did not
// move: of the edges drawn in it, far more hold where they were than changed. Not counting what holds still whatever
// the content does, nor what changes by itself.
bool Stitcher::Anchored() const {
  const PixelView& now = tracker_.now();
  uint32_t held = 0;
  uint32_t moved = 0;
  for (int y = 1; y < height_; ++y) {
    if (tracker_.Hovered(y)) continue;
    const uint8_t* changed = tracker_.Changed(y);
    const uint32_t* pixels = PixelRow(now, y);
    const uint32_t* above = PixelRow(now, y - 1);
    const uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
    for (int x = 0; x < width_; ++x) {
      if (Same(pixels[x], above[x]) || (state[x] & (kOther | kHeld)) != 0) continue;
      ++(changed[x] != 0 ? moved : held);
    }
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
// A pixel that shows what scrolled to it, and did not before, is content passing. One that holds still though
// something else scrolled to it shows what holds still there: something fixed in front of the content, or a
// background seen through it. One plain enough to do both tells little. One that did neither changed by itself, and
// when what it showed before did not scroll on either, something that holds still there came, went or changed.
// How each pixel of the view behaved is kept whatever it is thought to be, so that what was taken for something fixed
// can turn out to be content or background. Only what scrolled out from under a band along the top or bottom says
// nothing of what holds still: lined up with the band, plain content would seem to hold still.
void Stitcher::Sort(const PixelView& ref, const PixelView& now, int shift, int place) {
  constexpr int kSlack = config::kScrollPixelSlack;
  ++moves_;
  max_step_ = std::max(max_step_, std::abs(shift));
  const __m128i slack = SlackOf(kSlack);
  std::fill(backdrop_count_.begin(), backdrop_count_.end(), 0u);
  const bool down = shift > 0;
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
    uint8_t* how = how_.data() + base;
    int x = 0;
    for (; x + 4 <= width_; x += 4) {
      const int stays = Near4(a + x, s + x, slack);
      const int follows = entered ? 0 : Near4(a + x, m + x, slack);
      for (int k = 0; k < 4; ++k) how[x + k] = static_cast<uint8_t>((stays >> k & 1) | (follows >> k & 1) << 1);
    }
    for (; x < width_; ++x) {
      const bool follows = !entered && Near(a[x], m[x], kSlack);
      how[x] = static_cast<uint8_t>((Near(a[x], s[x], kSlack) ? kStayed : 0) | (follows ? kFollowed : 0));
    }
    uint8_t* state = mask_.data() + base;
    const uint8_t* origin = entered ? nullptr : mask_.data() + static_cast<size_t>(source) * width_;
    const uint8_t* fixed = fixed_.data() + base;
    const uint8_t* under = entered ? nullptr : fixed_.data() + static_cast<size_t>(source) * width_;
    uint8_t* still = still_.data() + base;
    uint8_t* moved = moved_.data() + base;
    uint8_t* covers = covers_.data() + base;
    uint8_t* flips = flips_.data() + base;
    uint8_t* fresh = fresh_.data() + base;
    uint16_t* odd = odd_.data() + base;
    uint32_t* wall = wall_.data() + base;
    const int row = place + y;
    const bool hovered = tracker_.Hovered(y);
    // A row made already, or one lit up by the pointer, learns nothing here, and nothing is learned of the content
    // before what is fixed has been told apart once.
    const bool wanted = !hovered && !Done(row);
    const bool learn = wanted && segments_ > 0;
    uint32_t* truth = wanted ? reinterpret_cast<uint32_t*>(truth_.Row(row)) : nullptr;
    uint32_t* votes = wanted ? reinterpret_cast<uint32_t*>(votes_.Row(row)) : nullptr;
    uint8_t* backs = wanted ? backs_.Row(row) : nullptr;
    for (x = 0; x < width_; ++x) {
      const bool stay = (how[x] & kStayed) != 0;
      const bool follow = (how[x] & kFollowed) != 0;
      const bool open = fixed[x] == 0;                // Content could show here, with nothing fixed in front of it.
      const bool hidden = !entered && under[x] != 0;  // What scrolled here was behind something fixed.
      const bool trusted = !hidden || !banded;
      const uint8_t was = state[x];
      if (!entered) {
        if (follow && !stay) {
          ++column_true_[static_cast<size_t>(x)];
          Bump(moved[x]);
          // Content came in front of what held still here.
          if ((was & kHeld) != 0 && Lead(wall[x]) >= 2 && !Near(a[x], wall[x], kSlack)) Bump(covers[x]);
          fresh[x] = 0;
        } else if (stay) {
          // What came in place holds still: something fixed came, went or changed there.
          if (trusted && fresh[x] != 0) Bump(flips[x]);
          fresh[x] = 0;
          if (!follow) {
            if (trusted) {
              Bump(still[x]);
              Vote(wall[x], a[x], 2);
            }
          } else if (trusted && moved[x] == 0 && Apart(a[x], m[x]) > kFlat) {
            // A smooth picture that holds still differs a little from itself that far away, if not by as much as a
            // reading may vary by.
            Vote(wall[x], a[x], 2);
          }
        } else {
          ++column_odd_[static_cast<size_t>(x)];
          if (!hidden) {
            if (!hovered && odd[x] < 0xFFFF) ++odd[x];
            // Unless what showed here before scrolled on.
            fresh[x] = static_cast<uint8_t>(moved[x] == 0 && on != nullptr && !Near(on[x], s[x], kSlack) ? 1 : 0);
          }
        }
      }
      const bool take = learn && open;
      uint8_t is = static_cast<uint8_t>(was & (kHeld | kSeen));
      bool sure = false;
      if (entered || hidden) {
        // What changed is new content. What held still tells nothing.
        if (!stay) {
          is = static_cast<uint8_t>(is & kSeen);
          if (take) Vote(votes[x], a[x], 1);
        } else if (!open) {
          is = static_cast<uint8_t>(is | kHeld);
        }
        is = static_cast<uint8_t>(is | kNew);
      } else if (follow && !stay) {
        is = kTrue | kSeen;
        sure = take;
      } else if (stay && !follow) {
        // In front of the content, or behind it and seen through it: either way it shows nothing of the content.
        is = static_cast<uint8_t>(is | kHeld);
        if (take) Bump(backs[x], 2);
      } else if (stay) {
        if (!open) is = static_cast<uint8_t>(is | kHeld);
        if ((is & kHeld) == 0) is = static_cast<uint8_t>(is | (origin[x] & kTrue));
        if (take) {
          if (textured_ && Lead(wall[x]) >= 2 && Near(a[x], wall[x], kSlack)) {
            Bump(backs[x], 2);
          } else if (moved[x] != 0 && moved[x] * 3 >= still[x]) {
            // Only where content scrolls: a plain stretch of something fixed looks just as plain.
            Vote(votes[x], a[x], 2);
          }
        }
      } else {
        // Changing on from where it was, it goes with the content, as a video does. Changing where it is in the view,
        // it does not, as what a pointer lights up.
        is = static_cast<uint8_t>((is & kSeen) | kOther | ((origin[x] & kOther) != 0 && !hovered ? kFlux : 0));
      }
      state[x] = is;
      if (wanted) {
        if (sure && (truth[x] & kFrozen) == 0) {
          const uint32_t agreed = (truth[x] & kSure) != 0 && Near(truth[x], a[x], kSlack) ? kAgreed : 0;
          truth[x] = kSure | agreed | (truth[x] & kDynamic) | (a[x] & kColor);
          Vote(votes[x], a[x], 2);
        }
        // Content that changes as it scrolls is remembered as such, so that all of it is taken whole together.
        if ((is & kFlux) != 0) truth[x] |= kDynamic;
      }
      // A column's backdrop is what the page shows there, not something fixed over it.
      if (!open) continue;
      uint32_t& count = backdrop_count_[static_cast<size_t>(x)];
      if (count == 0) {
        backdrop_[static_cast<size_t>(x)] = a[x] & kColor;
        count = 1;
      } else {
        count = Same(backdrop_[static_cast<size_t>(x)], a[x]) ? count + 1 : count - 1;
      }
    }
  }
}

// A reading that does not line up pixel for pixel tells little apart: what holds still stays what it was and tells
// nothing of the content, and what changed counts only for content that is never seen better.
void Stitcher::Blur(const PixelView& ref, const PixelView& now, int place) {
  std::fill(backdrop_count_.begin(), backdrop_count_.end(), 0u);
  for (int y = 0; y < height_; ++y) {
    const uint32_t* a = PixelRow(now, y);
    const uint32_t* s = PixelRow(ref, y);
    uint8_t* state = mask_.data() + static_cast<size_t>(y) * width_;
    uint8_t* how = how_.data() + static_cast<size_t>(y) * width_;
    const int row = place + y;
    const bool wanted = segments_ > 0 && !tracker_.Hovered(y) && !Done(row);
    uint32_t* votes = wanted ? reinterpret_cast<uint32_t*>(votes_.Row(row)) : nullptr;
    for (int x = 0; x < width_; ++x) {
      const bool stay = Near(a[x], s[x], config::kScrollPixelSlack);
      how[x] = stay ? kStayed : 0;
      state[x] = static_cast<uint8_t>(state[x] & (stay ? kHeld | kSeen : kSeen));
      const bool open = fixed_[static_cast<size_t>(y) * width_ + x] == 0;
      if (wanted && !stay && open) Vote(votes[x], a[x], 1);
      if (!open) continue;
      uint32_t& count = backdrop_count_[static_cast<size_t>(x)];
      if (count == 0) {
        backdrop_[static_cast<size_t>(x)] = a[x] & kColor;
        count = 1;
      } else {
        count = Same(backdrop_[static_cast<size_t>(x)], a[x]) ? count + 1 : count - 1;
      }
    }
  }
}

// Keeps the reading at `place` as the screen at an end.
void Stitcher::Capture(End& end, const PixelView& frame, int place) {
  end.pixels.resize(static_cast<size_t>(width_) * height_);
  for (int y = 0; y < height_; ++y) {
    std::memcpy(end.pixels.data() + static_cast<size_t>(y) * width_, PixelRow(frame, y), static_cast<size_t>(width_) * 4);
  }
  end.place = place;
  end.serial = serial_;
  ++revision_;
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
  bool any = false;
  for (int y = first; y < last && !any; ++y) {
    for (int x = 0; x < width_ && !any; ++x) any = dynamic(y, x);
  }
  if (!any) return;
  const int columns = (width_ + kGrid - 1) / kGrid;
  const int rows = (height_ + kGrid - 1) / kGrid;
  grid_.assign(static_cast<size_t>(columns) * rows, 0);
  for (int y = 0; y < height_; ++y) {
    uint8_t* cells = grid_.data() + static_cast<size_t>(y / kGrid) * columns;
    for (int x = 0; x < width_; ++x) {
      if (dynamic(y, x)) cells[x / kGrid] = 1;
    }
  }
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
          if ((mask_[base + x] & kHeld) != 0 || fixed_[base + x] != 0 || Lively(base + x) || (truth[x] & kFrozen) != 0) continue;
          truth[x] = kFrozen | (pixels[x] & kColor);
        }
      }
    }
  }
}

// Makes content rows [first, last), those not made yet. Rows within a view of either end are held for the end to be
// painted on when the capture finishes, and meanwhile shown with it painted as it stands. All others are final.
void Stitcher::Make(int first, int last) {
  std::vector<uint32_t> block;
  int block_first = 0;
  int block_rows = 0;
  bool block_final = false;
  const auto send = [&] {
    if (block_rows > 0) Send(block_first, block, block_rows, block_final);
    block_rows = 0;
  };
  for (int row = first; row < last; ++row) {
    if (Done(row)) {
      send();
      continue;
    }
    Compose(row, line_.data());
    Mark(row, row + 1);
    bool final = true;
    if (InEnds(row) && made_.Ensure(row, row + 1)) {
      std::memcpy(made_.Row(row), line_.data(), static_cast<size_t>(width_) * 4);
      held_[static_cast<size_t>(row + row_base_)] = 1;
      Paint(row, line_.data());
      final = false;
    } else {
      Render(row, line_.data());
      Fill(line_.data());
    }
    if (block_rows > 0 && (final != block_final || row != block_first + block_rows)) send();
    if (block_rows == 0) {
      block.clear();
      block_first = row;
      block_final = final;
    }
    block.insert(block.end(), line_.begin(), line_.end());
    ++block_rows;
  }
  send();
}

// Tells the view apart, from how each of its pixels behaved over all the readings so far.
// - Rows content was never seen in, from the top and from the bottom of the view, are a header or a bar to type in.
// - What holds still where content came in front of it again and again, at many places across the view and in many
//   colors, is a picture behind the content, as a chat's wallpaper.
// - What holds still where content hardly ever showed is something fixed in front of the content. Over a picture,
//   only what came, went or changed by itself, as a date shown while scrolling, or what stands alone small, as a
//   button: anything larger is the picture itself, where no content happened to pass.
// Fixed things near each other are one, filled out to its box. Each belongs at the end of the view it reaches, the top
// if it reaches both, or else the nearer one. A thing that would cover much of the view is no thing in front of it,
// but a background not told for one, and is left alone.
void Stitcher::Segment() {
  const double began = NowSeconds();
  ++segments_;
  ++revision_;
  const size_t pixels = static_cast<size_t>(width_) * height_;
  const int columns = (width_ + kCell - 1) / kCell;
  const int rows = (height_ + kCell - 1) / kCell;
  const int inside = (edge_ + kCell - 1) / kCell;  // Columns of squares left of a scroll bar.
  const size_t cells = static_cast<size_t>(columns) * rows;
  const int gap = std::max(1, static_cast<int>(std::lround(config::kScrollClusterGap * scale_ / kCell)));
  const int reach = static_cast<int>(std::lround(config::kScrollBottomReach * scale_));
  const auto type = [&](size_t i) { return TypeOf(still_[i], moved_[i], flips_[i], covers_[i]); };
  const auto empty = [&](int y) {
    const size_t base = static_cast<size_t>(y) * width_;
    int count = 0;
    for (int x = 0; x < edge_; ++x) {
      const uint8_t is = type(base + x);
      count += is == kMoving || is == kBack ? 1 : 0;
    }
    return count <= std::max(2, edge_ / 64);
  };
  const auto band = [&](bool top) {
    int length = 0;
    while (length < height_ / 3 && empty(top ? length : height_ - 1 - length)) ++length;
    return length < height_ / 3 ? length : 0;
  };
  top_band_ = band(true);
  bottom_band_ = band(false);
  const int first = top_band_;
  const int last = height_ - bottom_band_;

  // Squares that may show something fixed, and those of them where it came, went or changed by itself.
  std::vector<uint16_t> holds(cells, 0);
  std::vector<uint16_t> changes(cells, 0);
  for (int y = first; y < last; ++y) {
    const size_t base = static_cast<size_t>(y) * width_;
    const size_t line = static_cast<size_t>(y / kCell) * columns;
    for (int x = 0; x < edge_; ++x) {
      const uint8_t is = type(base + x);
      if (is == kFixed) ++holds[line + x / kCell];
      if (is == kChanging) ++changes[line + x / kCell];
    }
  }
  grid_.assign(cells, 0);
  for (size_t cell = 0; cell < cells; ++cell) grid_[cell] = static_cast<uint8_t>(changes[cell] >= 3 ? 2 : holds[cell] + changes[cell] >= 3 ? 1 : 0);

  if (!textured_) {
    uint32_t base = 0;
    uint32_t count = 0;
    const auto behind = [&](size_t i) { return type(i) == kBack && Lead(wall_[i]) >= 2; };
    for (int y = first; y < last; ++y) {
      for (int x = 0; x < edge_; ++x) {
        const size_t i = static_cast<size_t>(y) * width_ + x;
        if (behind(i)) Majority(base, count, wall_[i]);
      }
    }
    const int spread_columns = (width_ + kSpread - 1) / kSpread;
    std::vector<uint16_t> spread(static_cast<size_t>(spread_columns) * ((height_ + kSpread - 1) / kSpread), 0);
    uint64_t seen = 0;
    uint64_t apart = 0;
    for (int y = first; y < last; ++y) {
      for (int x = 0; x < edge_; ++x) {
        const size_t i = static_cast<size_t>(y) * width_ + x;
        if (!behind(i)) continue;
        ++seen;
        if (!Near(wall_[i], base, config::kScrollPixelSlack)) ++apart;
        uint16_t& place = spread[static_cast<size_t>(y / kSpread) * spread_columns + x / kSpread];
        if (place < 0xFFFF) ++place;
      }
    }
    int places = 0;
    for (const uint16_t place : spread) places += place >= 4 ? 1 : 0;
    textured_ = seen >= std::max<uint64_t>(400, pixels / 300) && places >= 6 && apart * 5 >= seen;
    texture_seen_ = seen;
    texture_apart_ = apart;
  }

  if (textured_) {
    std::vector<uint8_t> keep(cells, 0);
    std::vector<uint8_t> visited(cells, 0);
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
        for (int ny = std::max(cy - 1, 0); ny <= std::min(cy + 1, rows - 1); ++ny) {
          for (int nx = std::max(cx - 1, 0); nx <= std::min(cx + 1, inside - 1); ++nx) {
            const size_t next = static_cast<size_t>(ny) * columns + nx;
            if (grid_[next] == 0 || visited[next] != 0) continue;
            visited[next] = 1;
            stack_.push_back(ny * columns + nx);
          }
        }
      }
      const bool alone = (box.right - box.left) * kCell * 4 <= edge_ && (box.bottom - box.top) * kCell * 4 <= last - first;
      for (const int cell : stack_) keep[static_cast<size_t>(cell)] = static_cast<uint8_t>(alone || grid_[static_cast<size_t>(cell)] == 2 ? 1 : 0);
    }
    grid_.swap(keep);
  }

  std::vector<uint8_t> ends(cells, 0);
  std::vector<uint8_t> visited(cells, 0);
  const LONG most = static_cast<LONG>(inside) * std::max(1, (last - first + kCell - 1) / kCell) * 2;
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
      for (int ny = std::max(cy - gap, 0); ny <= std::min(cy + gap, rows - 1); ++ny) {
        for (int nx = std::max(cx - gap, 0); nx <= std::min(cx + gap, inside - 1); ++nx) {
          const size_t next = static_cast<size_t>(ny) * columns + nx;
          if (grid_[next] == 0 || visited[next] != 0) continue;
          visited[next] = 1;
          stack_.push_back(ny * columns + nx);
        }
      }
    }
    if ((box.right - box.left) * (box.bottom - box.top) * 5 > most) continue;
    const int top = box.top * kCell;
    const int bottom = std::min<int>(box.bottom * kCell, height_);
    const uint8_t end = top <= first + reach ? kTopKind : bottom >= last - reach || top + bottom >= height_ ? kBottomKind : kTopKind;
    ++elements_;
    // With a margin for its edges and shadow, which may hold still too faintly to be told.
    for (int cy = std::max<int>(box.top - 2, 0); cy < std::min<int>(box.bottom + 2, rows); ++cy) {
      for (int cx = std::max<int>(box.left - 2, 0); cx < std::min<int>(box.right + 2, inside); ++cx) {
        uint8_t& cell = ends[static_cast<size_t>(cy) * columns + cx];
        if (cell == 0) cell = end;
      }
    }
  }
  for (int y = 0; y < height_; ++y) {
    uint8_t* fixed = fixed_.data() + static_cast<size_t>(y) * width_;
    const uint8_t* owners = ends.data() + static_cast<size_t>(y / kCell) * columns;
    const uint8_t banded = y < first ? kTopKind : y >= last ? kBottomKind : 0;
    for (int x = 0; x < width_; ++x) fixed[x] = static_cast<uint8_t>(x >= edge_ ? 0 : banded != 0 ? banded : owners[x / kCell]);
  }
  if (textured_) Background();
  Trace("stitch: told apart after %d scrolls in %.1f ms: bands %d and %d, background %d, %d fixed things", moves_, (NowSeconds() - began) * 1000.0,
        top_band_, bottom_band_, textured_ ? 1 : 0, elements_);
}

// The background behind the content, as the view shows it between the bands at its top and bottom: each pixel what
// it showed while holding still. A gap, where nothing was seen or something fixed stands in front, is filled from
// what was seen on either side of it: blended across, down its column where it is short that way, else along its row,
// and else the background's commonest color. One color is never carried far.
void Stitcher::Background() {
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
  for (int y = tile_top_; y < bottom; ++y) {
    for (int x = 0; x < edge_; ++x) {
      const size_t i = static_cast<size_t>(y) * width_ + x;
      if (fixed_[i] != 0 || Lead(wall_[i]) < 2) continue;
      // Only what is known to hold still behind the content: where content scrolls, what seemed to hold still may
      // have been content that happened to look alike.
      const uint8_t is = TypeOf(still_[i], moved_[i], flips_[i], covers_[i]);
      if (is != kBack && is != kFixed) continue;
      known[i] = 1;
      back_[i] = wall_[i] & kColor;
      Majority(base, count, back_[i]);
    }
  }
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
  for (int x = 0; x < edge_; ++x) {
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
    for (int x = 0; x < edge_;) {
      if (known[origin + x] != 0) {
        ++x;
        continue;
      }
      int end = x;
      while (end < edge_ && known[origin + end] == 0) ++end;
      fill(origin, 1, x, end, 0, edge_);
      x = end;
    }
    for (int x = 0; x < width_; ++x) {
      if (known[origin + x] == 0) back_[origin + x] = base;
    }
  }
}

// The end whose screen a content row shows, if it lies within a view of one: of both, the one whose middle is nearer.
const Stitcher::End* Stitcher::Showing(int row) const {
  const bool top = !top_.pixels.empty() && row >= top_.place && row < top_.place + height_;
  const bool bottom = !bottom_.pixels.empty() && row >= bottom_.place && row < bottom_.place + height_;
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

// One content row in view: each pixel as it showed when seen to scroll twice alike, or taken whole; or else as most of
// the clear places it passed showed it, if it was seen clear more often than the background through it, and over a
// picture, more than once; or else the background, which is, where there is no picture, what was seen there all the
// same, or what its column mostly is.
void Stitcher::Blend(int row, uint32_t* out) const {
  const uint32_t* truth = reinterpret_cast<const uint32_t*>(truth_.Row(row));
  const uint32_t* votes = reinterpret_cast<const uint32_t*>(votes_.Row(row));
  const uint8_t* backs = backs_.Row(row);
  for (int x = 0; x < width_; ++x) {
    if ((truth[x] & kFrozen) != 0 || (truth[x] & (kSure | kAgreed)) == (kSure | kAgreed)) {
      out[x] = truth[x] & (kColor | kFrozen);
      continue;
    }
    const uint32_t lead = Lead(votes[x]);
    if (lead > backs[x] && (!textured_ || lead >= 4)) {
      out[x] = votes[x] & kColor;
    } else {
      out[x] = ((votes[x] & kVoted) != 0 ? votes[x] & kColor : backdrop_[static_cast<size_t>(x)]) | kBehind;
    }
  }
}

void Stitcher::Compose(int row, uint32_t* out) {
  Blend(row, out);
  uint32_t tone = kNoTone;
  for (int x = 0; x < width_ && tone != kMixed; ++x) {
    const uint32_t color = out[x] & kColor;
    tone = tone == kNoTone || tone == color ? color : kMixed;
  }
  tones_[static_cast<size_t>(row + row_base_)] = tone;
}

// Shows the background behind a content row made, where the content is not surely known.
void Stitcher::Render(int row, uint32_t* line) const {
  if (!textured_ || back_.empty()) return;
  const uint32_t* tile = back_.data() + static_cast<size_t>(TileRow(row)) * width_;
  for (int x = 0; x < edge_; ++x) {
    if ((line[x] & kBehind) != 0) line[x] = tile[x] | kBehind;
  }
}

// A content row near an end or in view, as made or as it would be made now. False for any other.
bool Stitcher::Consensus(int row, uint32_t* out) const {
  if (Held(row)) {
    std::memcpy(out, made_.Row(row), static_cast<size_t>(width_) * 4);
    return true;
  }
  if (finished_ || row < position_ || row >= position_ + height_ || Done(row)) return false;
  Blend(row, out);
  return true;
}

void Stitcher::Send(int first, std::vector<uint32_t>& block, int rows, bool final) {
  for (uint32_t& pixel : block) pixel = (pixel & kColor) | 0xFF000000u;
  sink_->Rows(first, rows, std::move(block), final);
  block = std::vector<uint32_t>{};
}

// Paints the ends on a content row. Within a view of an end, the row is that end's screen as it was, but where the
// screen shows something fixed that belongs at the other end: the content behind it shows there instead. Whatever is
// fixed and belongs at an end shows from that end's screen wherever the screen reaches, so that it shows whole.
void Stitcher::Paint(int row, uint32_t* line) const {
  Render(row, line);
  const int at_top = row - top_.place;
  const int at_bottom = row - bottom_.place;
  const bool top = !top_.pixels.empty() && at_top >= 0 && at_top < height_;
  const bool bottom = !bottom_.pixels.empty() && at_bottom >= 0 && at_bottom < height_;
  if (const End* end = Showing(row); end != nullptr) {
    const size_t at = static_cast<size_t>(row - end->place) * width_;
    const size_t top_at = top ? static_cast<size_t>(at_top) * width_ : 0;
    const size_t bottom_at = bottom ? static_cast<size_t>(at_bottom) * width_ : 0;
    for (int x = 0; x < edge_; ++x) {
      if (bottom && fixed_[bottom_at + x] == kBottomKind) {
        line[x] = bottom_.pixels[bottom_at + x] & kColor;
      } else if (top && fixed_[top_at + x] == kTopKind) {
        line[x] = top_.pixels[top_at + x] & kColor;
      } else if (fixed_[at + x] == 0) {
        line[x] = end->pixels[at + x] & kColor;
      }
    }
  }
  // A scroll bar at the side shows its arrows at the two ends and its plain track between them.
  if (bar_ready_) {
    const bool top_cap = at_top >= 0 && at_top < cap_top_;
    const bool bottom_cap = at_bottom >= height_ - cap_bottom_ && at_bottom < height_;
    const uint32_t* pixels = top_cap ? top_.pixels.data() + static_cast<size_t>(at_top) * width_
                             : bottom_cap ? bottom_.pixels.data() + static_cast<size_t>(at_bottom) * width_ : nullptr;
    for (int x = bar_; x < width_; ++x) line[x] = pixels != nullptr ? pixels[x] & kColor : track_[static_cast<size_t>(x - bar_)];
  }
}

bool Stitcher::InEnds(int row) const {
  return (row >= min_position_ && row < min_position_ + height_) || (row >= max_position_ && row < max_position_ + height_);
}

// Rows held near an end that the end has moved away from are final as they are.
void Stitcher::Flush(int old_min, int old_max) {
  const auto flush = [&](int first, int last) {
    std::vector<uint32_t> block;
    int start = 0;
    int rows = 0;
    const auto send = [&] {
      if (rows == 0) return;
      Send(start, block, rows, true);
      made_.Release(start, start + rows);
      rows = 0;
    };
    for (int row = first; row < last; ++row) {
      if (!Held(row) || InEnds(row)) {
        send();
        continue;
      }
      held_[static_cast<size_t>(row + row_base_)] = 0;
      if (rows == 0) {
        block.clear();
        start = row;
      }
      const uint32_t* made = reinterpret_cast<const uint32_t*>(made_.Row(row));
      block.insert(block.end(), made, made + width_);
      Render(row, block.data() + block.size() - static_cast<size_t>(width_));
      Fill(block.data() + block.size() - static_cast<size_t>(width_));
      ++rows;
    }
    send();
  };
  flush(old_min, old_min + height_);
  flush(old_max, old_max + height_);
}

// Rows past where the capture now ends are let go, to be made again should they ever be seen.
void Stitcher::Drop(int first, int last) {
  for (int row = first; row < last; ++row) held_[static_cast<size_t>(row + row_base_)] = 0;
  made_.Release(first, last);
  Undo(first, last);
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
      const uint32_t* pixels = bottom_.pixels.data() + static_cast<size_t>(y) * width_;
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
  TraceImage(L"top", top_.pixels.data(), width_, height_);
  TraceImage(L"bottom", bottom_.pixels.data(), width_, height_);
  std::vector<uint32_t> picture(pixels, 0);
  constexpr uint32_t kColors[] = {0x000000, 0x404040, 0x20C040, 0xE02020, 0xFF00FF, 0x203070};
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const size_t i = static_cast<size_t>(y) * width_ + x;
      const bool banded = y < top_band_ || y >= height_ - bottom_band_;
      uint32_t color = kColors[banded ? kBand : TypeOf(still_[i], moved_[i], flips_[i], covers_[i])];
      if (fixed_[i] != 0) {
        const uint32_t tint = fixed_[i] == kBottomKind ? 0xFFFF00u : 0x00FFFFu;
        color = (((color & 0xFEFEFE) >> 1) + ((tint & 0xFEFEFE) >> 1)) & kColor;
      }
      picture[i] = color;
    }
  }
  TraceImage(L"kinds", picture.data(), width_, height_);
  for (size_t i = 0; i < pixels; ++i) picture[i] = Lead(wall_[i]) >= 2 ? wall_[i] & kColor : 0;
  TraceImage(L"wall", picture.data(), width_, height_);
  for (size_t i = 0; i < pixels; ++i) picture[i] = (covers_[i] >= 2 ? 0xFF0000u : 0) | (moved_[i] != 0 ? 0x0000FFu : 0) | (still_[i] != 0 ? 0x00FF00u : 0);
  TraceImage(L"behind", picture.data(), width_, height_);
  if (!back_.empty()) TraceImage(L"back", back_.data(), width_, height_);
}

// Hands on content rows [first, last) as they would be made now, as not final: those held, or those in view.
void Stitcher::Show(int first, int last, bool held) {
  std::vector<uint32_t> block;
  int start = 0;
  int rows = 0;
  const auto send = [&] {
    if (rows > 0) Send(start, block, rows, false);
    rows = 0;
  };
  for (int row = first; row < last; ++row) {
    if (held != Held(row) || !Consensus(row, line_.data())) {
      send();
      continue;
    }
    Paint(row, line_.data());
    if (rows == 0) {
      block.clear();
      start = row;
    }
    block.insert(block.end(), line_.begin(), line_.end());
    ++rows;
  }
  send();
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
    Show(top_.place, top_.place + height_, true);
    Show(bottom_.place, bottom_.place + height_, true);
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
  Segment();
  Make(position_, position_ + height_);
  Trace("stitch: view %dx%d, %d scrolls, bands %d and %d, background %d, bar %d, edge %d, %d fixed things", width_, height_, moves_,
        top_band_, bottom_band_, textured_ ? 1 : 0, bar_, edge_, elements_);
  Trace("stitch: content came in front of what holds still again and again at %llu pixels, %llu unlike its commonest color",
        static_cast<unsigned long long>(texture_seen_), static_cast<unsigned long long>(texture_apart_));
  if (Tracing()) Dump();
  // Everything held is final now, with each end painted on as the screen showed it there.
  std::vector<uint32_t> block;
  int start = 0;
  int rows = 0;
  const auto send = [&] {
    if (rows > 0) Send(start, block, rows, true);
    rows = 0;
  };
  const auto emit = [&](int first, int last) {
    for (int row = first; row < last; ++row) {
      if (!Held(row)) {
        send();
        continue;
      }
      held_[static_cast<size_t>(row + row_base_)] = 0;
      std::memcpy(line_.data(), made_.Row(row), static_cast<size_t>(width_) * 4);
      Paint(row, line_.data());
      if (rows == 0) {
        block.clear();
        start = row;
      }
      block.insert(block.end(), line_.begin(), line_.end());
      ++rows;
    }
    send();
  };
  if (max_position_ < min_position_ + height_) {
    emit(min_position_, max_position_ + height_);
  } else {
    emit(min_position_, min_position_ + height_);
    emit(max_position_, max_position_ + height_);
  }
  truth_.Release(position_, position_ + height_);
  votes_.Release(position_, position_ + height_);
  backs_.Release(position_, position_ + height_);
  made_.Release(min_position_, min_position_ + height_);
  made_.Release(max_position_, max_position_ + height_);
}

}  // namespace sstk
