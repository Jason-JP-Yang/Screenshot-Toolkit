// The pixel work of a scrolling screenshot, apart from the screen and the clock. Readings are compared as edge maps,
// cell by cell, so that what moves can be told from what stays no matter how the two share a row. The long image is
// then built per cell: a pixel enters it only from a reading where it was really following the scroll, so anything
// fixed over the content, a pointer's hover, or a moving scroll bar does not.
#pragma once

#include "image.h"

#include <climits>
#include <cstdint>
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
  // `rest` is the shift at which nothing has moved, and `expected` the shift to prefer among equals.
  void Analyze(const EdgeFrame& before, const EdgeFrame& after, int rest, int expected, Motion& motion);

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

  std::vector<uint32_t> energy_;
  std::vector<uint8_t> stays_;
  std::vector<float> votes_;
  std::vector<float> costs_;
  std::vector<int> candidates_;
};

// Pixel by pixel, what each column and row did across the readings compared so far.
struct Evidence {
  std::vector<uint32_t> col_follow, col_stay, col_other, col_edge, col_faint;
  std::vector<int> col_first, col_last;  // The rows between which a column has held still.
  std::vector<uint32_t> row_follow, row_edge;
  uint64_t samples = 0;
  int frames = 0;
  int first_row = 0;  // The rows that columns were counted over.
  int last_row = 0;

  void Reset(int width, int height);
  void Add(const EdgeFrame& before, const EdgeFrame& after, int shift, int first, int last);
  bool Barrier(int x) const;   // Tall, drawn on, and not following the scroll.
  bool Line(int x) const;      // Or a line from top to bottom that was there through it all.
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

  bool Start(const PixelView& first, float scale, int limit_rows, bool hinted, const RECT& box);

  // `pointer` is in the reading's own pixels.
  Step Feed(const PixelView& frame, POINT pointer);

  // Puts the two ends of the capture in their final form, once the last reading is in.
  void Finish();

  Crop FinalCrop() const;
  void Copy(const Crop& crop, uint8_t* to, ptrdiff_t stride) const;

  int width() const { return width_; }
  int height() const { return height_; }
  int position() const { return position_; }
  int first_row() const { return min_position_; }
  int last_row() const { return max_position_ + height_; }
  int grown() const { return max_position_ - min_position_; }
  bool replaced() const;  // Whether a lost reading changed nearly everywhere.
  PixelView Rows(int first, int last) const;

  // The content rows changed since the last call.
  bool TakeChanges(int* first, int* last);

 private:
  bool Commit(int position);
  void Merge(const PixelView& frame, const EdgeFrame& edges, const Motion& motion, int position, bool clean, POINT pointer);
  void KeepEnds(const PixelView& frame, int position, bool at_rest);
  void Note(int position, bool sharp);
  void UpdateFixedRows(const EdgeFrame& earlier, int shift);
  void UpdateColors(const PixelView& frame, const Motion& motion);
  void BuildMask(const EdgeFrame& edges);
  bool RowTrusted(int y, int slice) const;
  bool RowClear(int y, int slice) const;
  bool RowChanges(int y, int slice) const;
  bool RowLooksLikePage(int y, int slice) const;
  bool Differs(const PixelView& frame, const EdgeFrame& edges, int slice, int y0, int position) const;
  void WriteCell(const PixelView& frame, const EdgeFrame& edges, int slice, int y, int position);
  int Settle(const EdgeFrame& edges, const Motion& motion, int position, bool* clean) const;
  bool RowsBlank(int first, int last, int left, int right) const;

  int width_ = 0;
  int height_ = 0;
  int slices_ = 0;
  int cell_rows_ = 0;
  float scale_ = 1.0f;
  int limit_rows_ = 0;
  bool hinted_ = false;
  RECT box_{};

  RowPlane pixels_;
  RowPlane edges_;
  RowPlane quality_;
  std::vector<uint8_t> differences_;
  int difference_origin_ = 0;

  EdgeFrame before_;
  EdgeFrame after_;
  EdgeFrame anchor_;  // The last sharp reading, when the last reading was not.
  bool anchor_is_before_ = true;
  int anchor_position_ = 0;
  EdgeFrame reference_;
  MotionFinder finder_;
  Motion motion_;
  Motion still_;
  Evidence evidence_;
  std::vector<int8_t> row_fixed_;     // Per row of the view and slice: how sure it is the edge of something fixed,
  std::vector<uint8_t> row_static_;   // for how many scrolled readings it has not changed,
  std::vector<uint16_t> row_color_;   // and what it last looked like.
  std::vector<uint32_t> page_colors_;  // What the plain parts of moving content look like, by votes.
  int page_color_ = 0;
  std::vector<uint8_t> row_mask_;
  std::vector<uint8_t> row_blank_;
  std::vector<uint8_t> differ_;
  std::vector<uint8_t> hover_;
  std::vector<int> flood_;

  std::vector<uint8_t> top_end_;     // The upper half of the reading at the top of the capture,
  std::vector<uint8_t> bottom_end_;  // and the lower half of the one at the bottom.
  int top_position_ = 0;
  int bottom_position_ = 0;
  int band_first_ = INT_MAX;  // The cell rows around the pointer in the last reading.
  int band_last_ = -1;

  int position_ = 0;
  int min_position_ = 0;
  int max_position_ = 0;
  int rest_min_ = 0;
  int rest_max_ = 0;
  int last_shift_ = 0;
  int follow_first_ = INT_MAX;  // The rows of the view where anything has been seen to follow the scroll.
  int follow_last_ = -1;
  bool pending_ = false;
  bool last_lost_replaced_ = false;
  int changed_first_ = 0;
  int changed_last_ = 0;
};

}  // namespace sstk
