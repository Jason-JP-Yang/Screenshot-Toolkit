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
// view of either end are held until the capture finishes, and all others are handed on as they leave the view; over
// a picture, every row is held, so that all of them show the one picture as finally known.
// A scroll bar at the side runs down the whole image as its plain track, with the arrows at its two ends as the screen
// showed them there; its thumb, which only says where the view was, is left out.
//
// Readings are shared, not copied: the screens at the ends are the readings they were until something changes them in
// place. Held rows are kept packed. The loops over the view are shared with the workers, each part taking rows of its
// own, or columns where a pixel's past readings tie it to others in its column only.
#pragma once

#include "tracker.h"
#include "workers.h"

#include <climits>
#include <memory>
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

// Rows of 32-bit pixels held until the capture finishes, packed a block at a time once rows stop coming to the block,
// so that holding them takes a small part of their pixels. Rows are numbered from -origin on.
class RowStore {
 public:
  void Reset(int width, int rows, int origin, Workers* workers);
  void Put(int row, const uint32_t* pixels);  // Replaces what was there.
  void Erase(int first, int last);
  void Pack();  // Packs the blocks no row came to since it last ran, and the full ones.
  void Clear() { blocks_ = std::vector<Block>{}; }

  // Reads held rows back, on one thread each, while none are put or erased. Rows read stay valid until the next read.
  class Reader {
   public:
    explicit Reader(const RowStore& store) : store_(store) {}
    const uint32_t* Row(int row);

   private:
    const RowStore& store_;
    int block_ = -1;
    std::vector<uint32_t> plain_;
  };

 private:
  static constexpr int kRows = 32;

  struct Block {
    uint32_t held = 0;   // A bit for each of its rows held.
    bool fresh = false;  // Whether a row came to it since Pack last ran.
    std::vector<uint32_t> plain;
    std::vector<uint8_t> packed;
  };

  int width_ = 0;
  int origin_ = 0;
  Workers* workers_ = nullptr;
  std::vector<Block> blocks_;
};

class Stitcher {
 public:
  enum class Step {
    kSame,   // Nothing changed.
    kRest,   // It did not scroll, though something may have changed in place.
    kMoved,  // It scrolled.
    kLost,   // The reading cannot be placed.
  };

  // Makes rows of the image when asked, on any thread, for as long as the call that hands it on lasts.
  class RowMaker {
   public:
    virtual ~RowMaker() = default;
    virtual void Make(int first, int count, uint32_t* out) const = 0;  // Rows [first, first + count), as opaque pixels.
  };

  // Takes rows, numbered as content rows: row 0 is the first row of the view when the capture started. They come in
  // any order, and rows that come again replace the ones before. Rows not final are only for showing meanwhile.
  class Sink {
   public:
    virtual ~Sink() = default;
    virtual void Rows(int first, int count, const RowMaker& rows, bool final) = 0;
    // What is shown between Record and Stop can be shown again, just as it was, with Replay.
    virtual void Record(int slot) = 0;
    virtual void Stop() = 0;
    virtual void Replay(int slot) = 0;
  };

  // Readings are kept for as long as they are needed. `pointer` is in the reading's own pixels.
  bool Start(const std::shared_ptr<const Frame>& first, float scale, int limit_rows, Sink* sink, Workers* workers);
  Step Feed(const std::shared_ptr<const Frame>& reading, POINT pointer);
  bool kept() const { return kept_; }  // Whether the reading just fed is the one the next is lined up with.

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

  // Writes what was found of the view as pictures, for tracing, once finished.
  void Dump() const;

 private:
  // The screen with the view at one end of the capture: a reading as it was, until something changes it in place.
  struct End {
    std::shared_ptr<const Frame> frame;
    std::vector<uint32_t> own;
    const uint32_t* pixels = nullptr;
    int place = 0;
    int serial = -1;       // Which reading it is.
    uint64_t version = 0;  // Changes whenever its pixels do,
    uint64_t held = 0;     // and whenever the rows held in its view do.
  };

  // What an end's view was last shown from. While all of it stays as it was, the view is shown again as it was.
  struct Shown {
    bool valid = false;
    int place = 0;
    int other_place = INT_MIN;  // Where the other end's view reaches into this one,
    uint64_t other_version = 0;
    uint64_t version = 0;
    uint64_t held = 0;
    uint64_t fixed = 0;
    uint32_t page = 0;
    size_t strips = 0;
    int edge = 0;
    int bar = 0;
    bool bar_ready = false;
    int cap_top = 0;
    int cap_bottom = 0;
    std::vector<uint32_t> track;
    bool operator==(const Shown&) const = default;
  };

  // Rows handed on together.
  struct Run {
    int first = 0;
    int count = 0;
    bool final = false;
  };

  int Parts(int rows) const;
  void Band(POINT pointer, int* first, int* last) const;
  bool Accept(const std::shared_ptr<const Frame>& reading, int shift, bool sharp);
  void Stay(const std::shared_ptr<const Frame>& reading);
  void Settle();
  bool Changes() const;
  void Rested();
  bool Calm() const;
  bool Scrolled(int shift) const;
  bool Anchored() const;
  bool Lively(size_t index) const;
  void Sort(const PixelView& ref, const PixelView& now, int shift, int place);
  void SortColumns(const PixelView& ref, const PixelView& now, int shift, int place, int left, int right);
  void Blur(const PixelView& ref, const PixelView& now);
  void Capture(End& end, const std::shared_ptr<const Frame>& frame, int place);
  uint32_t* Writable(End& end);
  void Freeze(const PixelView& ref, int place, int first, int last);
  void Make(int first, int last);
  bool Deferring() const;
  void Segment(bool final, const PixelView& now);
  void Background(bool ends);
  std::vector<uint32_t> StartPixels() const;
  void Strips();
  const End* Showing(int row) const;
  int TileRow(int row) const;
  void Compose(int row, uint32_t* out);
  bool Through(size_t index, uint32_t color) const;
  void Tally(const uint32_t* line);
  void Page();
  void Blend(int row, uint32_t* out) const;
  void Render(int row, uint32_t* line) const;
  void PaintHeld(int first, int count, uint32_t* out) const;
  void Show(int first, int last, bool held);
  void ShowEnd(bool top);
  Shown Showable(bool top) const;
  void Paint(int row, uint32_t* line) const;
  void Flush(int old_min, int old_max);
  void Drop(int first, int last);
  void Touched(int first, int last);
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
  Workers* workers_ = nullptr;

  Tracker tracker_;
  std::shared_ptr<const Frame> ref_;  // The reading the tracker lines the next up with.
  int band_first_ = INT_MAX;
  int band_last_ = -1;

  // Per pixel of the view, as of the last reading placed.
  std::vector<uint8_t> mask_;   // What it is.
  // Per pixel of the view, over the capture.
  std::vector<uint8_t> marks_;   // The end what is fixed in front of the content there belongs at, if anything is, and
                                 // whether it changed in place just now, what held still there came, went or changed,
                                 // and content came in front of what held still there once or again.
  std::vector<uint8_t> still_;   // Readings it held still in though something else scrolled to it.
  std::vector<uint8_t> moved_;   // Readings it showed content scrolling through it.
  std::vector<uint16_t> odd_;    // Readings it changed by itself in.
  std::vector<uint32_t> wall_;   // What it mostly showed while holding still: the layer that holds still.
  std::vector<uint32_t> back_;   // The background behind the content, gaps filled, where there is one,
  bool back_final_ = false;      // as finally known.
  std::vector<std::vector<uint8_t>> start_;  // The reading the capture began with, taken at rest, packed a few rows at a time.

  std::vector<uint32_t> column_true_;  // Per column: pixels seen to scroll, and pixels that changed by themselves.
  std::vector<uint32_t> column_odd_;
  // Of the content made, per coarse color: how much of it showed that, and the exact color most of that did. The
  // fullest is the page's own color, which content never seen shows.
  struct Shade {
    uint64_t count = 0;
    uint32_t color = 0;
    uint32_t lead = 0;
  };
  std::vector<Shade> shades_;
  uint32_t page_ = 0;
  std::vector<uint32_t> strip_;  // Per column: the one color it shows nearly all the way down at both ends, if any.
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
  uint64_t fixed_version_ = 0;  // Changes whenever the view is told apart.
  int revision_ = 0;         // Changes whenever what the rows near the ends are painted from does,
  int shown_ = -1;           // and as it was when they were last shown.
  Shown shown_top_;
  Shown shown_bottom_;
  int top_band_ = 0;         // Rows at the top and bottom of the view that never show content.
  int bottom_band_ = 0;

  RowPlane truth_;   // Per pixel of a content row in view: what it last showed when seen to scroll, or taken whole.
  RowPlane votes_;   // Per pixel of a content row in view: what most of the clear places it passed showed.
  RowPlane backs_;   // Per pixel of a content row in view: how often it showed the background through it.
  RowPlane sights_;  // Per pixel of a content row in view: the row of the view it was first seen to scroll in, plus one.
  RowStore made_;    // Rows made and held: near the ends, and over a picture all of them, until the picture is known.
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
