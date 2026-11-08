// The pixel work of a scrolling screenshot, apart from the screen and the clock. Readings are lined up as edge maps,
// cell by cell, so that what moves can be told from what stays no matter how the two share a row. The long image is
// then built in layers, pixel by pixel:
// - Content enters it from readings where it was seen following the scroll, away from the pointer and from anything
//   over it. Until then it holds what was seen, and anything seen better replaces that.
// - What stays where it is while content moves past is either a background behind the content or something fixed in
//   front of it. Something in front hides content that is seen elsewhere, or tints it, or differs from a plain
//   background. Each such thing belongs to the end of the capture nearer to it, and appears only there, just as the
//   screen showed it.
// - A background that scrolls is content. A plain one is its color. A fixed one is rebuilt from wherever it showed
//   between and beside the content, and repeats down the image one view at a time, each view from the background as it
//   was when that part was on screen.
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

  // Puts the capture in its final form once the last reading is in: content, background, and its two ends.
  void Finish();

  Crop ViewCrop() const;   // The columns, and the rows of the view, that the image keeps.
  Crop FinalCrop() const;  // The same, from the reading at the top of the capture to the one at the bottom.
  void Copy(const Crop& crop, uint8_t* to, ptrdiff_t stride) const;

  int width() const { return width_; }
  int height() const { return height_; }
  int position() const { return position_; }
  int first_row() const { return min_position_; }
  int last_row() const { return max_position_ + height_; }
  int grown() const { return max_position_ - min_position_; }
  int span() const { return bottom_position_ - top_position_; }  // How far the image reaches beyond one view.
  bool replaced() const;  // Whether a lost reading changed nearly everywhere.
  bool fixed_background() const { return fixed_background_; }
  int follow_first() const { return follow_first_; }
  int follow_last() const { return follow_last_; }
  PixelView Rows(int first, int last) const;

  // The content rows changed since the last call.
  bool TakeChanges(int* first, int* last);

 private:
  // What stays where it is in the view, as last seen holding still, and how many times in a row.
  struct Plate {
    std::vector<uint32_t> color;
    std::vector<uint8_t> count;
  };

  bool Commit(int position);
  void SetBand(POINT pointer);
  void KeepEnds(const PixelView& frame, int position, bool at_rest);
  void Note(const PixelView& frame, int position, bool sharp);
  void SetFollow(int first, int last);
  void UpdateSteps(const PixelView& frame);
  bool Separator(int x, int far_first, int far_last) const;
  void PaneColumns(bool panels, int* left, int* right) const;
  bool StaticFilled(int from, int to) const;
  bool BackgroundLikely() const;
  bool Observe(const PixelView& frame, int position, bool at_rest);
  bool Classify(const PixelView& frame, int position);
  void RestUpdate(const PixelView& frame, int position);
  void Fill(const PixelView& frame, int position);
  void ObservePlate(int x, int y, uint32_t color, uint64_t* compared, uint64_t* differ);
  bool Overlay(size_t index) const;
  bool Suspect(size_t index, bool band) const;
  void MarkChanged(int row);
  int Settle(const EdgeFrame& edges, const Motion& motion, int position, bool* clean, bool at_rest) const;
  bool RowsBlank(int first, int last, int left, int right) const;
  int VersionAt(int first, int last) const;

  int width_ = 0;
  int height_ = 0;
  int slices_ = 0;
  int cell_rows_ = 0;
  float scale_ = 1.0f;
  int limit_rows_ = 0;
  bool hinted_ = false;
  RECT box_{};

  // The capture, per pixel: its color, its edge map for lining readings up, how far it can be believed, and how many
  // more readings agreed with it than not. For each slice of a row, the row of the view it was last seen holding still at.
  RowPlane pixels_;
  RowPlane edges_;
  RowPlane states_;
  RowPlane votes_;
  RowPlane sources_;
  bool committed_ = false;
  int committed_first_ = 0;
  int committed_last_ = 0;

  EdgeFrame before_;
  EdgeFrame after_;
  EdgeFrame anchor_;  // The last sharp reading, when the last reading was not.
  bool anchor_is_before_ = true;
  int anchor_position_ = 0;
  EdgeFrame reference_;
  MotionFinder finder_;
  Motion motion_;
  Evidence evidence_;

  // The color steps along the rows, per column of the view, which show how the display is divided.
  std::vector<uint32_t> step_up_;
  std::vector<uint32_t> step_down_;
  uint32_t step_rows_ = 0;
  int step_tick_ = 0;

  // Per pixel of the view, over the sharp readings compared so far: how often it showed content following the
  // scroll, held still while content moved, changed in neither way, hid content known to be there, and showed content
  // otherwise than most readings of it do.
  std::vector<uint8_t> follow_n_;
  std::vector<uint8_t> stay_n_;
  std::vector<uint8_t> neither_n_;
  std::vector<uint8_t> occluded_n_;
  std::vector<uint8_t> dissent_n_;
  uint64_t behind_count_ = 0;  // Pixels where content moved and a still background showed as well.
  std::vector<uint32_t> clean_;  // The last sharp reading,
  int clean_position_ = 0;       // where it was,
  int unsettled_ = 0;            // and how many readings since have changed too much to compare with it.

  // What holds still, a new version each time a background changes as a whole, and which version each sharp reading
  // saw, in order.
  std::vector<Plate> plates_;
  std::vector<std::pair<int, int>> history_;
  int version_votes_ = 0;
  int tile_columns_ = 0;
  std::vector<uint32_t> tile_compared_;
  std::vector<uint32_t> tile_differ_;

  std::vector<uint32_t> top_frame_;  // The readings at the top and the bottom of the capture.
  std::vector<uint32_t> bottom_frame_;
  int top_position_ = 0;
  int bottom_position_ = 0;
  int band_first_ = INT_MAX;  // The rows around the pointer in the last reading,
  int band_last_ = -1;
  int top_band_first_ = INT_MAX;  // and in the readings at either end.
  int top_band_last_ = -1;
  int bottom_band_first_ = INT_MAX;
  int bottom_band_last_ = -1;

  int position_ = 0;
  int min_position_ = 0;
  int max_position_ = 0;
  int rest_min_ = 0;
  int rest_max_ = 0;
  int last_shift_ = 0;
  bool settled_ = true;  // The last reading was sharp, so its place is exact.
  int follow_first_ = INT_MAX;  // The rows of the view where anything has been seen to follow the scroll.
  int follow_last_ = -1;
  bool fixed_background_ = false;
  bool last_lost_replaced_ = false;
  int changed_first_ = 0;
  int changed_last_ = 0;
};

}  // namespace sstk
