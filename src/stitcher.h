// Builds the long image of a scrolling screenshot a row at a time, without ever holding the image. Every pixel of a
// reading is compared with the reading before it, which the tracker (tracker.h) has placed a known number of rows
// away: one that shows what scrolled to it is content, one that holds still though something else scrolled to it
// shows what holds still in the view, and one that does neither changed by itself. Over the capture, each pixel of
// the view is then known as one of three layers:
// - Content. The image shows it as it last looked when seen to scroll, or else as most clear sights of it showed it.
// - A background that holds still behind the content, as a chat's wallpaper: content was seen to pass in front of it.
//   Where the content shows it, the image shows it too, repeated one band of the view at a time and in step with the
//   screen at each end, so that it looks whole between the content.
// - Something fixed in front of the content, as a header, a bar to type in, a side bar, or a floating button or
//   panel. It shows once, whole, just as the screen at the end it belongs at showed it: what reaches the bottom of the
//   view without reaching its top belongs at the bottom end, and everything else at the top end. Elsewhere, the
//   content behind it shows.
// Within a view of each end, the image is that end's screen as it was, less what belongs at the other end. Content
// that changes by itself, as a video does, is taken whole from the last reading that shows all of it. Rows within a
// view of either end are held until the capture finishes, and all others are handed on as they leave the view.
// A scroll bar at the side runs down the whole image as its plain track, with the arrows at its two ends as the screen
// showed them there; its thumb, which only says where the view was, is left out.
#pragma once

#include "tracker.h"

#include <climits>
#include <utility>
#include <vector>

namespace sstk {

// Rows reserved up front in address space, and made usable only where they are written.
class RowPlane {
 public:
  RowPlane() = default;
  ~RowPlane();
  RowPlane(const RowPlane&) = delete;
  RowPlane& operator=(const RowPlane&) = delete;

  bool Reserve(size_t stride, int rows, int origin);
  bool Ensure(int first, int last);    // Makes rows usable. Those not usable before read as zero.
  void Release(int first, int last);   // Gives whole pages inside the rows back, until they are made usable again.
  uint8_t* Row(int row) const { return base_ + stride_ * static_cast<ptrdiff_t>(row + origin_); }

 private:
  uint8_t* base_ = nullptr;
  size_t stride_ = 0;
  int origin_ = 0;
};

class Stitcher {
 public:
  enum class Step {
    kSame,   // Nothing changed.
    kRest,   // It did not scroll, though something may have changed in place.
    kMoved,  // It scrolled.
    kLost,   // The reading cannot be placed.
  };

  // Takes rows, numbered as content rows: row 0 is the first row of the view when the capture started. They come in
  // any order, and rows that come again replace the ones before. Rows not final are only for showing meanwhile.
  class Sink {
   public:
    virtual ~Sink() = default;
    virtual void Rows(int first, int count, std::vector<uint32_t> pixels, bool final) = 0;
  };

  // The first reading must stay as it is until another is kept. `pointer` is in the reading's own pixels.
  bool Start(const PixelView& first, float scale, int limit_rows, Sink* sink);
  Step Feed(const PixelView& frame, POINT pointer);
  bool kept() const { return kept_; }  // Whether the reading just fed must stay as it is until another is kept.

  // Makes every row still to be made, and hands all of them on as final.
  void Finish();

  int width() const { return width_; }
  int height() const { return height_; }
  int position() const { return position_; }
  int first_row() const { return min_position_; }
  int last_row() const { return max_position_ + height_; }
  int grown() const { return max_position_ - min_position_; }
  bool replaced() const { return lost_replaced_; }  // Whether a lost reading changed nearly everywhere.

  // Hands on, as not final, the rows near the ends and in view as they would be made now.
  void Preview();

 private:
  // The screen with the view at one end of the capture.
  struct End {
    std::vector<uint32_t> pixels;
    int place = 0;
    int serial = -1;  // Which reading it is.
  };

  void Band(POINT pointer, int* first, int* last) const;
  bool Accept(const PixelView& frame, int shift, bool sharp);
  void Stay();
  void Settle();
  void Rested();
  bool Calm() const;
  bool Scrolled(int shift) const;
  bool Anchored() const;
  bool Lively(size_t index) const;
  void Sort(const PixelView& ref, const PixelView& now, int shift, int place);
  void Blur(const PixelView& ref, const PixelView& now, int place);
  void Capture(End& end, const PixelView& frame, int place);
  void Freeze(const PixelView& ref, int place, int first, int last);
  void Make(int first, int last);
  void Segment();
  void Background();
  const End* Showing(int row) const;
  int TileRow(int row) const;
  void Compose(int row, uint32_t* out);
  void Blend(int row, uint32_t* out) const;
  void Render(int row, uint32_t* line) const;
  bool Consensus(int row, uint32_t* out) const;
  void Send(int first, std::vector<uint32_t>& block, int rows, bool final);
  void Show(int first, int last, bool held);
  void Paint(int row, uint32_t* line) const;
  void Dump() const;
  void Flush(int old_min, int old_max);
  void Drop(int first, int last);
  void FindBar();
  void Tracks();
  int Cap(bool top) const;
  void Fill(uint32_t* line) const;
  bool Held(int row) const { return held_[static_cast<size_t>(row + row_base_)] != 0; }
  bool InEnds(int row) const;
  bool Done(int row) const;
  void Mark(int first, int last);
  void Undo(int first, int last);
  bool Blank(int first, int last) const;

  int width_ = 0;
  int height_ = 0;
  float scale_ = 1.0f;
  int limit_rows_ = 0;
  Sink* sink_ = nullptr;

  Tracker tracker_;
  int band_first_ = INT_MAX;
  int band_last_ = -1;

  // Per pixel of the view, as of the last reading placed.
  std::vector<uint8_t> mask_;   // What it is.
  std::vector<uint8_t> how_;    // Whether it stayed, and whether it followed.
  // Per pixel of the view, over the capture.
  std::vector<uint8_t> still_;   // Readings it held still in though something else scrolled to it.
  std::vector<uint8_t> moved_;   // Readings it showed content scrolling through it.
  std::vector<uint8_t> covers_;  // Times content came in front of what held still there.
  std::vector<uint8_t> flips_;   // Times what held still there came, went or changed.
  std::vector<uint8_t> fresh_;   // Whether it just changed in place, to count as that once what came holds still.
  std::vector<uint16_t> odd_;    // Readings it changed by itself in.
  std::vector<uint32_t> wall_;   // What it mostly showed while holding still: the layer that holds still.
  std::vector<uint8_t> fixed_;   // The end what is fixed in front of the content there belongs at, if anything is.
  std::vector<uint32_t> back_;   // The background behind the content, gaps filled, where there is one.

  std::vector<uint32_t> column_true_;  // Per column: pixels seen to scroll, and pixels that changed by themselves.
  std::vector<uint32_t> column_odd_;
  std::vector<uint32_t> backdrop_;     // Per column: its most common color, for content never seen clear.
  std::vector<uint32_t> backdrop_count_;
  bool textured_ = false;    // Whether content scrolls over a picture that holds still behind it, as a chat's wallpaper.
  uint64_t texture_seen_ = 0;
  uint64_t texture_apart_ = 0;
  uint32_t wall_base_ = 0;   // The background's most common color.
  int tile_top_ = 0;         // Rows of the view the background repeats in down the image.
  int tile_period_ = 1;
  int elements_ = 0;         // Things found fixed in front of the content, for tracing.
  int moves_ = 0;
  int max_step_ = 0;         // The farthest a sharp reading moved.
  int next_segment_ = 2;     // After how many scrolls the view is told apart next.
  int segments_ = 0;
  int revision_ = 0;         // Changes whenever what the rows near the ends are painted from does,
  int shown_ = -1;           // and as it was when they were last shown.
  int top_band_ = 0;         // Rows at the top and bottom of the view that never show content.
  int bottom_band_ = 0;

  RowPlane truth_;   // Per pixel of a content row in view: what it last showed when seen to scroll, or taken whole.
  RowPlane votes_;   // Per pixel of a content row in view: what most of the clear places it passed showed.
  RowPlane backs_;   // Per pixel of a content row in view: how often it showed the background through it.
  RowPlane made_;    // Rows made and held, near the ends.
  std::vector<uint8_t> held_;    // Per content row: whether it is made and held.
  std::vector<uint32_t> tones_;  // Per content row made: its one color, if it has just one.
  int row_base_ = 0;
  std::vector<std::pair<int, int>> done_;  // The content rows made, as runs.
  std::vector<uint8_t> grid_;
  std::vector<int> stack_;
  std::vector<uint32_t> line_;

  End top_;
  End bottom_;
  int edge_ = 0;                   // Columns from here on show a bar, and take nothing fixed from an end.
  int bar_ = 0;                    // Columns from here on are a scroll bar at the side, which shows no content.
  bool bar_ready_ = false;         // Whether the bar's track and arrows are known.
  std::vector<uint32_t> track_;    // Per column of the bar: the color of its track.
  int cap_top_ = 0;                // Rows of the arrows at the bar's two ends.
  int cap_bottom_ = 0;

  int serial_ = 0;   // Which reading was placed last.
  int position_ = 0;
  int min_position_ = 0;
  int max_position_ = 0;
  int rest_min_ = 0;
  int rest_max_ = 0;
  int pending_ = 0;  // How far a reading not yet taken in had moved from the last one that was.
  int step_ = 0;     // How far the last reading moved from the one before it.
  uint64_t last_key_ = 0;
  bool kept_ = false;
  bool lost_replaced_ = false;
  bool finished_ = false;
};

}  // namespace sstk
