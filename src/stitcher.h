// The pixel work of a scrolling screenshot, apart from the screen and the clock.
// - A reading is placed by what changed since the last placed one. Whatever is the same at the same place in both, as a
//   background or a bar that holds still, is left out, and what moved decides the shift, whatever its shape. A shift
//   counts only when it explains nearly everything that changed, clearly better than any other. A reading that cannot
//   be placed that way writes nothing.
// - The long image is made of whole rows, each a row of one reading, so nothing in a row is ever pieced together.
//   Rows come from where a reading showed them best: away from the ends of where content moves and from the pointer.
//   They go in as tall blocks, and a block meets the rows around it at a row the image already shows alike, so nothing
//   drawn is cut between readings.
// - Content seen changed in place since, as a picture that loaded, is taken again whole.
// What shows while recording is the image as it will be kept.
#pragma once

#include "image.h"

#include <climits>
#include <cstdint>
#include <utility>
#include <vector>

namespace sstk {

// A reading reduced to where its brightness steps. Shades that drift, as a see-through window's do while content
// moves over it, leave no trace, and text and lines keep their place even when blurred.
class EdgeFrame {
 public:
  void Build(const PixelView& pixels);
  void BuildFromEdges(const uint8_t* rows, ptrdiff_t stride, int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }
  int slices() const { return slices_; }        // Columns of cells.
  int cell_rows() const { return cell_rows_; }  // Rows of cells.
  int strips() const { return strips_; }
  const uint8_t* Row(int y) const { return data_.data() + static_cast<size_t>(y) * width_; }
  uint32_t SliceEnergy(int slice, int first, int last) const;
  const uint8_t* Profile(int strip) const { return profile_.data() + static_cast<size_t>(strip) * height_; }
  uint32_t ProfileSum(int strip, int first, int last) const;
  uint64_t TotalEnergy() const;
  bool SameAs(const EdgeFrame& other) const;

 private:
  void Allocate(int width, int height);
  void Summarize();

  int width_ = 0;
  int height_ = 0;
  int slices_ = 0;
  int cell_rows_ = 0;
  int strips_ = 0;
  std::vector<uint8_t> data_;
  std::vector<uint32_t> slice_sums_;
  std::vector<uint8_t> profile_;
  std::vector<uint32_t> profile_sums_;
  std::vector<uint8_t> line_[2];
};

enum class CellState : uint8_t {
  kBlank,    // Nothing to compare.
  kMoving,   // Follows the scroll, and does not stay.
  kFixed,    // Stays, and does not follow the scroll.
  kBoth,     // Looks the same either way.
  kChanged,  // Neither.
  kEntered,  // Just came into view, so there is nothing to follow.
};

// How two readings differ.
struct Motion {
  bool moved = false;
  bool blurred = false;  // It lines up only loosely, so it says where the content is and nothing more.
  int shift = 0;  // Row y of the later reading shows what row y + rest + shift of the earlier one did.
  int moving = 0;
  uint64_t moving_energy = 0;
  uint64_t moving_sad = 0;
  uint64_t fixed_energy = 0;
  uint64_t changed_energy = 0;
  std::vector<CellState> cells;
  RECT changed_box{};  // In cells, around what changed.
};

class MotionFinder {
 public:
  // `rest` is the shift at which nothing has moved, and `expected` the shift to prefer among equals. With a focus, only
  // what happens inside it decides the outcome, though every cell of the reading gets a state.
  void Analyze(const EdgeFrame& before, const EdgeFrame& after, int rest, int expected, Motion& motion, const RECT* focus = nullptr);

 private:
  struct Trial {
    int shift = 0;
    int moving = 0;
    int rows = 0;
    uint64_t score = 0;
    uint64_t sad = 0;
    uint64_t changed = 0;
  };

  void Collect(const EdgeFrame& before, const EdgeFrame& after, int rest, int low, int high, int expected);
  Trial Evaluate(const EdgeFrame& before, const EdgeFrame& after, int rest, int shift) const;
  int CountLoose(const EdgeFrame& before, const EdgeFrame& after, int rest, int shift, int* comparable) const;
  Trial Search(const EdgeFrame& before, const EdgeFrame& after, int rest, int low, int high, int expected);

  RECT focus_{};
  RECT cells_{};  // The focus, in cells.
  std::vector<uint32_t> energy_;
  std::vector<uint8_t> stays_;
  std::vector<float> votes_;
  std::vector<float> costs_;
  std::vector<int> candidates_;
};

// Pixel by pixel, what each column and row did across the readings compared so far.
struct Evidence {
  std::vector<uint32_t> col_follow, col_stay, col_other, col_faint;
  std::vector<uint32_t> row_follow, row_edge;
  uint64_t samples = 0;
  int frames = 0;

  void Reset(int width, int height);
  void Add(const EdgeFrame& before, const EdgeFrame& after, int shift, int first, int last);
  uint64_t Still(int x) const;  // How much of a column is drawn on without following the scroll.
  bool Barrier(int x) const;    // Enough of it that the column holds a panel of its own.
};

// Where a scroll was seen, in the readings' own pixels.
struct ScrollPlan {
  bool found = false;
  bool hinted = false;  // The rectangle is one the app reported, so what surrounds the motion in it belongs too.
  RECT rect{};          // What to follow.
  RECT box{};           // What moved, within it.
};

ScrollPlan PlanScrollArea(const EdgeFrame& before, const EdgeFrame& after, const Motion& motion, POINT pointer, bool wheel, float scale,
                          const std::vector<RECT>& hints);

// Rows reserved up front in address space and made usable as the capture grows, in either direction.
class RowPlane {
 public:
  RowPlane() = default;
  ~RowPlane();
  RowPlane(const RowPlane&) = delete;
  RowPlane& operator=(const RowPlane&) = delete;

  bool Reserve(size_t stride, int rows, int origin);
  bool Commit(int first, int last);
  uint8_t* Row(int row) const { return base_ + stride_ * static_cast<ptrdiff_t>(row + origin_); }
  size_t stride() const { return stride_; }

 private:
  uint8_t* base_ = nullptr;
  size_t stride_ = 0;
  int origin_ = 0;
  int first_ = 0;
  int last_ = 0;
  bool any_ = false;
};

class Stitcher {
 public:
  enum class Step {
    kSame,   // Nothing changed.
    kRest,   // It did not scroll, though something may have changed in place.
    kMoved,  // It scrolled.
    kLost,   // The reading cannot be placed.
  };

  // The part of the capture that makes the image: columns, and rows numbered as content rows.
  struct Crop {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
  };

  // `pointer` is in the reading's own pixels, here and when fed.
  bool Start(const PixelView& first, float scale, int limit_rows, bool hinted, const RECT& box, POINT pointer);
  Step Feed(const PixelView& frame, POINT pointer);

  // Puts in what the last reading placed shows better than the image, as nothing came after it.
  void Finish();

  Crop ViewCrop() const;   // The columns, and the rows of the view, that the image keeps.
  Crop FinalCrop() const;  // The same, from the reading at the top of the capture to the one at the bottom.
  void Copy(const Crop& crop, uint8_t* to, ptrdiff_t stride) const;
  PixelView Composed(int first, int last) const;  // Whole rows of the image as it stands, good until the next call.

  int width() const { return width_; }
  int height() const { return height_; }
  int position() const { return position_; }
  int first_row() const { return min_position_; }
  int last_row() const { return max_position_ + height_; }
  int grown() const { return max_position_ - min_position_; }
  int span() const;  // How far the image reaches beyond one view.
  bool replaced() const { return last_lost_replaced_; }  // Whether a lost reading changed nearly everywhere.
  bool fixed_background() const { return BackgroundLikely(); }
  int follow_first() const { return follow_first_; }
  int follow_last() const { return follow_last_; }

  // The content rows whose image changed since the last call.
  bool TakeChanges(int* first, int* last);

 private:
  // A shift, or a place in the image, tried for the reading being taken in: how many of the pixels that changed since
  // the last placed reading it explains, of how many it could, and in how many rows.
  struct Trial {
    int shift = 0;
    uint32_t explained = 0;
    uint32_t compared = 0;
    int rows = 0;
    double Score() const { return compared == 0 ? 0.0 : static_cast<double>(explained) * explained / compared; }
  };

  void Load(const PixelView& frame);
  uint32_t Compare();
  bool Register(int* shift);
  int Vote();
  void Sweep();
  Trial Verify(int shift) const;
  bool Realign(int* position);
  Trial VerifyAt(int position) const;
  bool Accept(int position, int shift);
  void Place(int position, int shift, bool flush);
  void Learn(int shift);
  int Score(int y) const;
  bool Held(int row) const;
  uint8_t& RowScore(int row) { return row_score_[static_cast<size_t>(row + row_base_)]; }
  bool Stale(int y, int row) const;
  int Difference(int position, int y);
  int SeamCost(int position, int boundary);
  int SeamTop(int position, int first, int last, int must, int floor);
  int SeamBottom(int position, int first, int last, int must);
  void WriteRows(int position, int first, int last);
  void Ends(const Crop& view, int* top, int* bottom) const;
  bool RowsBlank(int first, int last, int left, int right) const;
  void SetBand(POINT pointer);
  void SetFollow(int first, int last);
  void SetCrop();
  void UpdateSteps();
  bool Separator(int x, int far_first, int far_last) const;
  void PaneColumns(bool panels, int* left, int* right) const;
  bool StaticFilled(int from, int to) const;
  bool BackgroundLikely() const;
  void MarkChanged(int first, int last);

  int width_ = 0;
  int height_ = 0;
  float scale_ = 1.0f;
  int limit_rows_ = 0;
  bool hinted_ = false;
  RECT box_{};
  int segments_ = 0;  // Pieces each row is hashed in.

  // The image, per row: its pixels, its pieces hashed, and how well placed in its reading it was, zero for not yet.
  RowPlane pixels_;
  RowPlane hashes_;
  std::vector<uint8_t> row_score_;
  int row_base_ = 0;

  // The last reading placed, which the next is lined up with, and the reading being taken in.
  std::vector<uint32_t> ref_;
  std::vector<uint32_t> now_;
  std::vector<uint32_t> ref_hash_;
  std::vector<uint32_t> now_hash_;
  EdgeFrame ref_edges_;
  EdgeFrame now_edges_;
  uint64_t frame_key_ = 0;  // The reading before this one, hashed, to tell when a change holds still.

  // Pixel by pixel, whether the reading being taken in differs from the last placed at the same place, and in all.
  std::vector<uint8_t> changed_;
  std::vector<uint32_t> changed_in_row_;
  uint32_t changed_count_ = 0;
  int changed_rows_ = 0;
  int changed_left_ = 0;
  int changed_right_ = 0;

  // Lining up: the last placed reading's pieces, sorted, and for which reading and columns; votes per shift; the shifts
  // to try; changed pixels to try every shift on.
  std::vector<uint64_t> ref_index_;
  int ref_serial_ = 0;
  int index_serial_ = -1;
  int index_left_ = -1;
  int index_right_ = -1;
  std::vector<uint64_t> now_index_;
  std::vector<int> votes_;
  std::vector<int> candidates_;
  std::vector<size_t> points_;

  // Per row of the view while a reading is placed: how well placed it is, whether it goes in, and how unlike the image.
  std::vector<int> score_;
  std::vector<uint8_t> want_;
  std::vector<int> difference_;

  Evidence evidence_;
  // The color steps along the rows, per column of the view, which show how the display is divided.
  std::vector<uint32_t> step_up_;
  std::vector<uint32_t> step_down_;
  uint32_t step_rows_ = 0;
  int step_tick_ = 0;
  // Per pixel of the view, how often it held still while content moved past, and how much of that there was inside
  // where content moves.
  std::vector<uint8_t> stay_n_;
  uint64_t behind_count_ = 0;

  int crop_left_ = 0;  // The columns compared.
  int crop_right_ = 0;
  int band_first_ = INT_MAX;  // The rows around the pointer in the last reading.
  int band_last_ = -1;
  int follow_first_ = INT_MAX;  // The rows of the view where content has been seen to follow the scroll.
  int follow_last_ = -1;

  int position_ = 0;
  int min_position_ = 0;
  int max_position_ = 0;
  int rest_min_ = 0;
  int rest_max_ = 0;
  int last_shift_ = 0;
  bool last_lost_replaced_ = false;
  int dirty_first_ = 0;
  int dirty_last_ = 0;
};

}  // namespace sstk
