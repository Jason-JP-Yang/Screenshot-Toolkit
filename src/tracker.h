// Finds how far content scrolled between two readings of the same part of the screen.
// Only pixels that changed take part, so whatever holds still, as a background or a bar, cannot pull the answer toward
// "nothing moved", and whatever moved counts, whatever its shape. Pieces of changed rows are looked up among the earlier
// reading's rows and each one found proposes a shift; a shift counts only when it explains nearly everything that
// changed, or all of it outside what changes by itself, clearly better than any other.
// The loops are shared with the workers, each part looking at rows of its own.
#pragma once

#include "pixels.h"
#include "workers.h"

#include <climits>
#include <vector>

namespace sstk {

class Tracker {
 public:
  // A shift tried: how many changed pixels it explains, of how many it could, and in how many rows.
  struct Match {
    int shift = 0;
    uint32_t explained = 0;
    uint32_t compared = 0;
    int rows = 0;
    bool sharp = false;  // Whether what follows the shift does so pixel for pixel.
    double Share() const { return compared == 0 ? 0.0 : static_cast<double>(explained) / compared; }
    double Score() const { return static_cast<double>(explained) * Share(); }
  };

  void Reset(int width, int height, Workers* workers);

  // The reading to line up with. Its pixels must stay as they are until another takes its place.
  void SetRef(const PixelView& ref);

  // Takes a reading in and marks what differs from the earlier one inside `window`. Rows [band_first, band_last] lie
  // around the pointer, where a hover changes things, and have no say. Returns how many pixels changed; with no more
  // than `quiet` of them the reading is the earlier one again, and nothing more is made ready to look for a shift.
  uint32_t Load(const PixelView& now, const RECT& window, int band_first, int band_last, uint32_t quiet);
  // Looks at another part of the same reading.
  uint32_t Focus(const RECT& window);

  // The shift that explains what changed: row y now shows what row y + shift showed. `expected` is the shift a scroll
  // going on as before would give; `sure` says the shift is beyond doubt.
  // Something that changes by itself, as a video does, need not be explained: a shift also counts when whole blocks
  // of the view follow it exactly, whatever the rest does.
  bool Register(int expected, Match* match, bool* sure);

  // Around the changed pixels a shift explains.
  RECT MovedBox(int shift) const;

  // The reading taken in becomes the one to line up with.
  void Promote();

  const PixelView& ref() const { return ref_; }
  const PixelView& now() const { return now_; }
  const uint64_t* Changed(int y) const { return changed_.data() + static_cast<size_t>(y) * words_; }  // A bit a pixel.
  int words() const { return words_; }
  uint32_t ChangedInRow(int y) const { return changed_in_row_[static_cast<size_t>(y)]; }
  uint32_t changed_count() const { return changed_count_; }
  int changed_rows() const { return changed_rows_; }
  int changed_width() const { return std::max(changed_right_ - changed_left_, 0); }
  bool Hovered(int y) const { return y >= band_first_ && y <= band_last_; }
  uint64_t key() const { return key_; }  // The reading taken in, as one number.

  static constexpr int kSegment = 32;     // Pixels of a row hashed as one piece.
  static constexpr int kMinOverlap = 48;  // Rows two readings must share to be lined up.
  static constexpr int kBlock = 32;       // Pixels along a side of the blocks a shift is judged in.
  static uint64_t Key(uint32_t hash, int segment) { return static_cast<uint64_t>(hash) << 32 | static_cast<uint64_t>(segment) << 16; }

 private:
  int Parts(int rows) const;
  void HashRows(const PixelView& view, std::vector<uint32_t>& hashes);
  void Compare();
  void Index(const std::vector<uint32_t>& hashes, int first_segment, int last_segment);
  int Vote();
  void Sweep();
  Match Verify(int shift, bool tally);
  bool Firm();

  Workers* workers_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int segments_ = 0;
  int words_ = 0;
  PixelView ref_{};
  PixelView now_{};
  RECT window_{};
  int band_first_ = INT_MAX;
  int band_last_ = -1;

  std::vector<uint32_t> hash_[2];
  int now_slot_ = 0;
  uint64_t key_ = 0;
  std::vector<uint64_t> changed_;
  std::vector<uint32_t> changed_in_row_;
  uint32_t changed_count_ = 0;
  int changed_rows_ = 0;
  int changed_left_ = 0;
  int changed_right_ = 0;

  std::vector<uint64_t> index_;  // The earlier reading's pieces, sorted, and for which reading and window.
  int serial_ = 0;
  int index_serial_ = -1;
  RECT index_window_{};
  std::vector<int> votes_;
  std::vector<int> candidates_;
  std::vector<size_t> points_;

  // Per block of the view, for the shift found: the pixels compared, those explained, and the rows explained in.
  int block_columns_ = 0;
  int block_rows_ = 0;
  std::vector<uint32_t> block_compared_;
  std::vector<uint32_t> block_explained_;
  std::vector<uint16_t> block_lines_;
};

}  // namespace sstk
