#include "sstk/scroll/tracker.h"

#include "sstk/config/core.h"

#include <cstring>

namespace sstk {

namespace {

constexpr size_t kMaxSameRows = 6;       // A piece found in more rows than this says nothing of where it went.
constexpr int kMinVotes = 6;             // With fewer pieces agreeing on a shift, every shift is tried as well.
constexpr size_t kMaxTrials = 6;
constexpr size_t kSweepPoints = 4096;
constexpr uint32_t kMinExplained = 48;   // Changed pixels a shift must explain to be believed,
constexpr double kExplainedShare = 0.7;  // and the share of them,
constexpr double kClearMargin = 1.5;     // and how much better than any shift more than a row away.
constexpr double kSureShare = 0.9;
constexpr double kSureMargin = 2.5;
constexpr int kBehindSlack = 32;         // How far apart a background that holds still may show behind an edge.
constexpr uint32_t kBlockPixels = 8;     // Pixels a block must have compared to say whether it follows a shift.
constexpr int kFirmBlocks = 3;           // Blocks that must follow a shift exactly for it to count on them alone,
constexpr int kFirmRows = 8;             // and the rows they must do so in.
constexpr int64_t kGrain = 1 << 16;      // Pixels below which a part of a loop is not worth a thread.
constexpr int kBuckets = 64;             // Parts the earlier reading's pieces are sorted in, by the top bits of their hash.

// Each piece of a row as a number only the same pixels share, or zero for a piece all of one color, which could be
// anywhere.
void HashRow(const uint32_t* row, int width, uint32_t* out) {
  for (int from = 0, piece = 0; from < width; from += Tracker::kSegment, ++piece) {
    const int to = std::min(from + Tracker::kSegment, width);
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

size_t Bucket(uint32_t hash) { return static_cast<size_t>(hash >> 26); }

}  // namespace

void Tracker::Reset(int width, int height, Workers* workers) {
  workers_ = workers;
  width_ = width;
  height_ = height;
  segments_ = (width + kSegment - 1) / kSegment;
  words_ = (width + 63) / 64;
  changed_.assign(static_cast<size_t>(words_) * height, 0);
  changed_in_row_.assign(static_cast<size_t>(height), 0);
  for (auto& hashes : hash_) hashes.assign(static_cast<size_t>(segments_) * height, 0);
  ref_ = now_ = PixelView{};
  index_serial_ = -1;
  block_columns_ = (width + kBlock - 1) / kBlock;
  block_rows_ = (height + kBlock - 1) / kBlock;
  const size_t blocks = static_cast<size_t>(block_columns_) * block_rows_;
  block_compared_.assign(blocks, 0);
  block_explained_.assign(blocks, 0);
  block_lines_.assign(blocks, 0);
}

int Tracker::Parts(int rows) const { return workers_->Parts(static_cast<int64_t>(rows) * width_, kGrain); }

void Tracker::HashRows(const PixelView& view, std::vector<uint32_t>& hashes) {
  const Bands bands(0, height_, 1, Parts(height_));
  workers_->For(bands.count(), [&](int band) {
    for (int y = bands.begin(band); y < bands.end(band); ++y) HashRow(PixelRow(view, y), width_, hashes.data() + static_cast<size_t>(y) * segments_);
  });
}

void Tracker::SetRef(const PixelView& ref) {
  ref_ = ref;
  HashRows(ref, hash_[1 - now_slot_]);
  ++serial_;
}

uint32_t Tracker::Load(const PixelView& now, const RECT& window, int band_first, int band_last, uint32_t quiet) {
  now_ = now;
  window_ = window;
  band_first_ = band_first;
  band_last_ = band_last;
  Compare();
  // Most readings find the screen as it was, and are done with here.
  if (changed_count_ <= quiet) return changed_count_;
  HashRows(now, hash_[now_slot_]);
  key_ = 14695981039346656037ull;
  for (const uint32_t hash : hash_[now_slot_]) key_ = (key_ ^ hash) * 1099511628211ull;
  return changed_count_;
}

uint32_t Tracker::Focus(const RECT& window) {
  window_ = window;
  Compare();
  return changed_count_;
}

void Tracker::Promote() {
  ref_ = now_;
  now_slot_ = 1 - now_slot_;
  ++serial_;
}

void Tracker::Compare() {
  const __m128i slack = SlackOf(config::kScrollPixelSlack);
  const int left = window_.left;
  const int right = window_.right;
  struct Tally {
    uint32_t count = 0;
    int rows = 0;
    int left = INT_MAX;
    int right = 0;
  };
  const Bands bands(0, height_, 1, Parts(height_));
  std::vector<Tally> tallies(static_cast<size_t>(bands.count()));
  workers_->For(bands.count(), [&](int band) {
    Tally& tally = tallies[static_cast<size_t>(band)];
    for (int y = bands.begin(band); y < bands.end(band); ++y) {
      uint64_t* out = changed_.data() + static_cast<size_t>(y) * words_;
      std::fill(out, out + words_, 0ull);
      changed_in_row_[static_cast<size_t>(y)] = 0;
      if (y < window_.top || y >= window_.bottom) continue;
      const uint32_t* now = PixelRow(now_, y);
      const uint32_t* was = PixelRow(ref_, y);
      uint32_t count = 0;
      int first = width_;
      int last = -1;
      int x = left;
      for (; x + 4 <= right; x += 4) {
        const int same = Near4(now + x, was + x, slack);
        if (same == 0xF) continue;
        for (int k = 0; k < 4; ++k) {
          if (((same >> k) & 1) != 0) continue;
          const int at = x + k;
          out[at >> 6] |= 1ull << (at & 63);
          ++count;
          first = std::min(first, at);
          last = at;
        }
      }
      for (; x < right; ++x) {
        if (Near(now[x], was[x], config::kScrollPixelSlack)) continue;
        out[x >> 6] |= 1ull << (x & 63);
        ++count;
        first = std::min(first, x);
        last = x;
      }
      changed_in_row_[static_cast<size_t>(y)] = count;
      if (count == 0) continue;
      ++tally.rows;
      tally.count += count;
      tally.left = std::min(tally.left, first);
      tally.right = std::max(tally.right, last + 1);
    }
  });
  changed_count_ = 0;
  changed_rows_ = 0;
  changed_left_ = width_;
  changed_right_ = 0;
  for (const Tally& tally : tallies) {
    changed_count_ += tally.count;
    changed_rows_ += tally.rows;
    changed_left_ = std::min(changed_left_, tally.left);
    changed_right_ = std::max(changed_right_, tally.right);
  }
}

// The earlier reading's pieces in the window, sorted. Each band of rows counts its pieces per bucket, then writes them
// where the order of the buckets puts them, and each bucket is sorted by itself: a piece's number starts with the top
// bits of its hash, so the buckets lie in order.
void Tracker::Index(const std::vector<uint32_t>& hashes, int first_segment, int last_segment) {
  const Bands bands(window_.top, window_.bottom, 1, Parts(window_.bottom - window_.top));
  const int count = bands.count();
  std::vector<size_t> at(static_cast<size_t>(count) * kBuckets, 0);
  workers_->For(count, [&](int band) {
    size_t* sizes = at.data() + static_cast<size_t>(band) * kBuckets;
    for (int y = bands.begin(band); y < bands.end(band); ++y) {
      const uint32_t* row = hashes.data() + static_cast<size_t>(y) * segments_;
      for (int s = first_segment; s <= last_segment; ++s) {
        if (row[s] != 0) ++sizes[Bucket(row[s])];
      }
    }
  });
  std::vector<size_t> starts(kBuckets + 1, 0);
  size_t total = 0;
  for (int bucket = 0; bucket < kBuckets; ++bucket) {
    starts[static_cast<size_t>(bucket)] = total;
    for (int band = 0; band < count; ++band) {
      size_t& slot = at[static_cast<size_t>(band) * kBuckets + bucket];
      const size_t size = slot;
      slot = total;
      total += size;
    }
  }
  starts[kBuckets] = total;
  index_.resize(total);
  workers_->For(count, [&](int band) {
    size_t* next = at.data() + static_cast<size_t>(band) * kBuckets;
    for (int y = bands.begin(band); y < bands.end(band); ++y) {
      const uint32_t* row = hashes.data() + static_cast<size_t>(y) * segments_;
      for (int s = first_segment; s <= last_segment; ++s) {
        if (row[s] != 0) index_[next[Bucket(row[s])]++] = Key(row[s], s) | static_cast<uint64_t>(y);
      }
    }
  });
  workers_->For(kBuckets, [&](int bucket) {
    std::sort(index_.begin() + static_cast<ptrdiff_t>(starts[static_cast<size_t>(bucket)]), index_.begin() + static_cast<ptrdiff_t>(starts[static_cast<size_t>(bucket) + 1]));
  });
}

// Proposes the shifts most pieces of changed rows agree on, and says how many agree on the best.
int Tracker::Vote() {
  const int first_segment = (window_.left + kSegment - 1) / kSegment;
  const int last_segment = window_.right / kSegment - 1;
  if (last_segment < first_segment) return 0;
  const std::vector<uint32_t>& ref_hash = hash_[1 - now_slot_];
  const std::vector<uint32_t>& now_hash = hash_[now_slot_];
  if (index_serial_ != serial_ || !EqualRect(&index_window_, &window_)) {
    Index(ref_hash, first_segment, last_segment);
    index_serial_ = serial_;
    index_window_ = window_;
  }
  const size_t span = static_cast<size_t>(height_) * 2 + 1;
  const Bands bands(window_.top, window_.bottom, 1, Parts(window_.bottom - window_.top));
  std::vector<int> counts(span * static_cast<size_t>(bands.count()), 0);
  workers_->For(bands.count(), [&](int band) {
    int* votes = counts.data() + span * static_cast<size_t>(band);
    for (int y = bands.begin(band); y < bands.end(band); ++y) {
      if (changed_in_row_[static_cast<size_t>(y)] == 0) continue;
      const uint32_t* now = now_hash.data() + static_cast<size_t>(y) * segments_;
      const uint32_t* was = ref_hash.data() + static_cast<size_t>(y) * segments_;
      for (int s = first_segment; s <= last_segment; ++s) {
        const uint32_t hash = now[s];
        if (hash == 0 || hash == was[s]) continue;
        const uint64_t key = Key(hash, s);
        const auto from = std::lower_bound(index_.begin(), index_.end(), key);
        const auto to = std::lower_bound(from, index_.end(), key + 0x10000);
        if (from == to || to - from > static_cast<ptrdiff_t>(kMaxSameRows)) continue;
        for (auto it = from; it != to; ++it) {
          const int moved = static_cast<int>(*it & 0xFFFF) - y;
          if (moved != 0) ++votes[static_cast<size_t>(moved + height_)];
        }
      }
    }
  });
  votes_.assign(span, 0);
  for (int band = 0; band < bands.count(); ++band) {
    const int* votes = counts.data() + span * static_cast<size_t>(band);
    for (size_t i = 0; i < span; ++i) votes_[i] += votes[i];
  }
  const int limit = (window_.bottom - window_.top) - kMinOverlap;
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

// When pieces of rows are found nowhere else, as when nothing drawn matches exactly, every shift is tried on a sample
// of what changed, and the few that explain most of it are proposed.
void Tracker::Sweep() {
  // Edges of what is drawn, as when a shift is checked: in every other row, every third pixel, in reading order.
  const int first = window_.top + 1;
  const int lines = first < window_.bottom ? (window_.bottom - first + 1) / 2 : 0;
  const Bands bands(0, lines, 1, Parts(lines * 2));
  std::vector<std::vector<size_t>> found(static_cast<size_t>(bands.count()));
  workers_->For(bands.count(), [&](int band) {
    std::vector<size_t>& out = found[static_cast<size_t>(band)];
    for (int line = bands.begin(band); line < bands.end(band); ++line) {
      const int y = first + line * 2;
      if (changed_in_row_[static_cast<size_t>(y)] == 0 || Hovered(y)) continue;
      const size_t base = static_cast<size_t>(y) * width_;
      const uint64_t* changed = Changed(y);
      const uint32_t* now = PixelRow(now_, y);
      const uint32_t* above = PixelRow(now_, y - 1);
      for (int x = window_.left; x < window_.right; x += 3) {
        if (Bit(changed, x) && !Near(now[x], above[x], config::kScrollPixelSlack)) out.push_back(base + x);
      }
    }
  });
  points_.clear();
  for (const std::vector<size_t>& part : found) points_.insert(points_.end(), part.begin(), part.end());
  if (points_.size() > kSweepPoints) {
    const size_t step = points_.size() / kSweepPoints + 1;
    size_t kept = 0;
    for (size_t i = 0; i < points_.size(); i += step) points_[kept++] = points_[i];
    points_.resize(kept);
  }
  if (points_.size() < kMinExplained) return;
  const int limit = (window_.bottom - window_.top) - kMinOverlap;
  const size_t enough = std::max<size_t>(32, points_.size() / 4);
  // Each shift is tried by itself, and the best few are then taken in order, as trying them one after another would.
  const int shifts = std::max(limit * 2 + 1, 0);
  std::vector<double> shares(static_cast<size_t>(shifts), -1.0);
  const Bands tries(0, shifts, 1, workers_->Parts(static_cast<int64_t>(shifts) * static_cast<int64_t>(points_.size()), kGrain));
  workers_->For(tries.count(), [&](int part) {
    for (int i = tries.begin(part); i < tries.end(part); ++i) {
      const int moved = i - limit;
      if (moved == 0) continue;
      // The points are in reading order, so those the shift keeps in view are one run of them.
      const auto from = std::lower_bound(points_.begin(), points_.end(), static_cast<size_t>(std::max<int>(window_.top, window_.top - moved)) * width_);
      const auto to = std::lower_bound(from, points_.end(), static_cast<size_t>(std::min<int>(window_.bottom, window_.bottom - moved)) * width_);
      const size_t inside = static_cast<size_t>(to - from);
      if (inside < enough) continue;
      size_t matched = 0;
      for (auto it = from; it != to; ++it) {
        const int y = static_cast<int>(*it / width_);
        const int x = static_cast<int>(*it % width_);
        if (Near(PixelRow(now_, y)[x], PixelRow(ref_, y + moved)[x], kBehindSlack)) ++matched;
      }
      shares[static_cast<size_t>(i)] = static_cast<double>(matched) / static_cast<double>(inside);
    }
  });
  int picks[3] = {0, 0, 0};
  double best[3] = {0.0, 0.0, 0.0};
  for (int i = 0; i < shifts; ++i) {
    const double share = shares[static_cast<size_t>(i)];
    if (share < 0.5) continue;
    int place = 3;
    while (place > 0 && best[place - 1] < share) --place;
    if (place >= 3) continue;
    for (int move = 2; move > place; --move) {
      picks[move] = picks[move - 1];
      best[move] = best[move - 1];
    }
    picks[place] = i - limit;
    best[place] = share;
  }
  for (int i = 0; i < 3; ++i) {
    if (best[i] > 0.0 && std::find(candidates_.begin(), candidates_.end(), picks[i]) == candidates_.end()) candidates_.push_back(picks[i]);
  }
}

// How much of what changed a shift explains. Only pixels unlike the one above them count, each together with that
// one: an edge of something drawn says exactly where it went, while the inside of a plain shape looks the same under
// many shifts, and would let a look-alike pass. With `tally`, the same is counted block by block.
Tracker::Match Tracker::Verify(int shift, bool tally) {
  Match match;
  match.shift = shift;
  constexpr int slack = config::kScrollPixelSlack;
  if (tally) {
    std::fill(block_compared_.begin(), block_compared_.end(), 0u);
    std::fill(block_explained_.begin(), block_explained_.end(), 0u);
    std::fill(block_lines_.begin(), block_lines_.end(), static_cast<uint16_t>(0));
  }
  const int from = std::max<int>(window_.top + 1, window_.top + 1 - shift);
  const int to = std::min<int>(window_.bottom, window_.bottom - shift);
  struct Tally {
    uint32_t explained = 0;
    uint32_t compared = 0;
    int rows = 0;
  };
  // Bands keep to whole rows of blocks, so that only one of them counts in any block.
  const Bands bands(from, to, kBlock, Parts(to - from));
  std::vector<Tally> tallies(static_cast<size_t>(bands.count()));
  workers_->For(bands.count(), [&](int band) {
    Tally& sum = tallies[static_cast<size_t>(band)];
    std::vector<uint16_t> line(tally ? static_cast<size_t>(block_columns_) : 0);
    for (int y = bands.begin(band); y < bands.end(band); ++y) {
      if (changed_in_row_[static_cast<size_t>(y)] == 0 || Hovered(y)) continue;
      const uint32_t* now = PixelRow(now_, y);
      const uint32_t* above = PixelRow(now_, y - 1);
      const uint32_t* was = PixelRow(ref_, y + shift);
      const uint32_t* was_above = PixelRow(ref_, y + shift - 1);
      const size_t block_row = static_cast<size_t>(y / kBlock) * block_columns_;
      if (tally) std::fill(line.begin(), line.end(), static_cast<uint16_t>(0));
      uint32_t explained = 0;
      ForBits(Changed(y), words_, [&](int x) {
        if (Near(now[x], above[x], slack)) return;
        const int here = Apart(now[x], was[x]);
        const int over = Apart(above[x], was_above[x]);
        // One side of the edge must show just the same. The other may be a background behind it that holds still, and
        // shows a shade or a mark of its own at either place.
        const bool same = (here <= slack && over <= kBehindSlack) || (over <= slack && here <= kBehindSlack);
        if (same) ++explained;
        ++sum.compared;
        if (!tally) return;
        const int block = x / kBlock;
        ++block_compared_[block_row + block];
        if (same) {
          ++block_explained_[block_row + block];
          ++line[static_cast<size_t>(block)];
        }
      });
      sum.explained += explained;
      if (explained >= 2) ++sum.rows;
      if (!tally || explained == 0) continue;
      for (int block = 0; block < block_columns_; ++block) {
        if (line[static_cast<size_t>(block)] >= 2) ++block_lines_[block_row + block];
      }
    }
  });
  for (const Tally& sum : tallies) {
    match.explained += sum.explained;
    match.compared += sum.compared;
    match.rows += sum.rows;
  }
  return match;
}

// Whether enough whole blocks of the view follow the shift just tallied to be sure of it by them alone. Something
// that changes by itself, as a video or what a pointer lights up, fails in its own blocks and nowhere else, and has no
// say; a blurred reading fails a little everywhere, and is not sharp.
bool Tracker::Firm() {
  uint32_t explained = 0;
  uint32_t compared = 0;
  uint32_t half = 0;  // Compared in the blocks that follow at least by half.
  int blocks = 0;
  int rows = 0;
  for (int by = 0; by < block_rows_; ++by) {
    int lines = 0;
    for (int bx = 0; bx < block_columns_; ++bx) {
      const size_t block = static_cast<size_t>(by) * block_columns_ + bx;
      const uint32_t all = block_compared_[block];
      const uint32_t same = block_explained_[block];
      if (all < kBlockPixels) continue;
      if (same * 2 >= all) half += all;
      if (same * 10 < all * 9) continue;
      ++blocks;
      explained += same;
      compared += all;
      lines = std::max<int>(lines, block_lines_[block]);
    }
    rows += lines;
  }
  return blocks >= kFirmBlocks && rows >= kFirmRows && explained >= kMinExplained * 4 && compared * 2 >= half;
}

bool Tracker::Register(int expected, Match* match, bool* sure) {
  candidates_.clear();
  if (Vote() < kMinVotes) Sweep();
  const int rows = window_.bottom - window_.top;
  if (expected != 0 && std::abs(expected) <= rows - kMinOverlap && std::find(candidates_.begin(), candidates_.end(), expected) == candidates_.end()) {
    candidates_.push_back(expected);
  }
  Match best;
  std::vector<Match> trials;
  trials.reserve(candidates_.size());
  for (const int candidate : candidates_) {
    trials.push_back(Verify(candidate, false));
    if (trials.back().Score() > best.Score()) best = trials.back();
  }
  double rival = 0.0;
  for (const Match& trial : trials) {
    if (std::abs(trial.shift - best.shift) >= 2) rival = std::max(rival, trial.Score());
  }
  *match = best;
  *sure = false;
  if (best.explained < kMinExplained || best.Score() < rival * kClearMargin) return false;
  // A single line drawn across the view would match itself anywhere, so a match must hold over several rows, unless
  // it explains all there is and nothing else comes near.
  const bool thin = best.rows < 3 && !(best.rows >= 2 && best.Share() >= kSureShare && best.explained >= kMinExplained * 2);
  const bool plain = !thin && best.Share() >= kExplainedShare;
  if (best.Share() >= kSureShare) {
    match->sharp = plain;
  } else {
    Verify(best.shift, true);
    match->sharp = Firm();
  }
  // Short of explaining most of what changed, it must be firm in what it does explain.
  if (!plain) return match->sharp;
  *sure = best.Share() >= kSureShare && best.Score() >= rival * kSureMargin && std::abs(best.shift) <= rows / 2;
  return true;
}

RECT Tracker::MovedBox(int shift) const {
  constexpr int kBucket = 16;
  const size_t buckets = static_cast<size_t>(width_ / kBucket + 1);
  const int from = std::max<int>(window_.top, window_.top - shift);
  const int to = std::min<int>(window_.bottom, window_.bottom - shift);
  struct Tally {
    std::vector<uint32_t> columns;
    int top = INT_MAX;
    int bottom = -1;
  };
  const Bands bands(from, to, 1, Parts(to - from));
  std::vector<Tally> tallies(static_cast<size_t>(bands.count()));
  workers_->For(bands.count(), [&](int band) {
    Tally& tally = tallies[static_cast<size_t>(band)];
    tally.columns.assign(buckets, 0);
    for (int y = bands.begin(band); y < bands.end(band); ++y) {
      if (changed_in_row_[static_cast<size_t>(y)] == 0) continue;
      const uint32_t* now = PixelRow(now_, y);
      const uint32_t* was = PixelRow(ref_, y + shift);
      uint32_t explained = 0;
      ForBits(Changed(y), words_, [&](int x) {
        if (!Near(now[x], was[x], config::kScrollPixelSlack)) return;
        ++explained;
        ++tally.columns[static_cast<size_t>(x / kBucket)];
      });
      if (explained < 4) continue;
      tally.top = std::min(tally.top, y);
      tally.bottom = y;
    }
  });
  std::vector<uint32_t> columns(buckets, 0);
  int top = INT_MAX;
  int bottom = -1;
  for (const Tally& tally : tallies) {
    for (size_t i = 0; i < buckets; ++i) columns[i] += tally.columns[i];
    top = std::min(top, tally.top);
    bottom = std::max(bottom, tally.bottom);
  }
  if (bottom < 0) return RECT{};
  // A stray match at the side, as in a scroll bar, says nothing about the columns the content takes.
  uint32_t most = 0;
  for (const uint32_t count : columns) most = std::max(most, count);
  const uint32_t enough = std::max<uint32_t>(6, most / 16);
  int left = -1;
  int right = -1;
  for (int bucket = 0; bucket < static_cast<int>(columns.size()); ++bucket) {
    if (columns[static_cast<size_t>(bucket)] < enough) continue;
    if (left < 0) left = bucket;
    right = bucket;
  }
  if (left < 0) return RECT{};
  return RECT{left * kBucket, top, std::min((right + 1) * kBucket, width_), bottom + 1};
}

}  // namespace sstk
