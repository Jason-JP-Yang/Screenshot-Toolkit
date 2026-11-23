// The picture with its markup laid in, kept in tiles so that a stroke redraws and uploads only what it touches. A tile
// is made the first time a stroke reaches it, and holds the finished pixels there, picture and markup together, so
// that markup multiplied into the picture or rubbed out of it shows exactly as it will be saved. Elsewhere the picture
// shows as it is. A shared tile is never changed in place, so a snapshot can be read on another thread.
#pragma once

#include "sstk/editor/document.h"
#include "sstk/graphics/image.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace sstk {

inline constexpr int kCanvasTile = 256;

// The picture and its tiles at one moment.
struct CanvasSnapshot {
  std::shared_ptr<const Image> base;
  std::vector<std::shared_ptr<const Image>> tiles;
  int columns = 0;
  int rows = 0;
  POINT origin{};  // The plane pixel of the picture's top-left corner.

  int width() const { return base ? base->width() : 0; }
  int height() const { return base ? base->height() : 0; }

  // Opaque pixels of the picture with its markup. Pixels past its edges repeat the nearest ones.
  uint32_t At(int x, int y) const;
  void Row(int y, int x, int count, uint32_t* out) const;
  // A row of the turned picture, numbered as TurnRect numbers its pixels from the plane's.
  void TurnedRow(int turns, int y, int x, int count, uint32_t* out) const;
};

class InkCanvas {
 public:
  InkCanvas(std::shared_ptr<const Image> base, POINT origin);

  int width() const { return base_->width(); }
  int height() const { return base_->height(); }
  POINT origin() const { return origin_; }
  RECT plane() const { return RECT{origin_.x, origin_.y, origin_.x + width(), origin_.y + height()}; }
  const std::shared_ptr<const Image>& base() const { return base_; }

  // Every tile under `area`, plane pixels, made again from the strokes.
  void Redraw(const StrokeList& strokes, const RECT& area);

  // A stroke being drawn, laid over the tiles as they were when it began: stamps from `first` on, or all of them again.
  void Begin();
  void Extend(const Stroke& stroke, size_t first);
  void Restroke(const Stroke& stroke);
  void End();
  void Cancel();

  // Parts of tiles changed since the last call, in the picture's pixels, and their pixels to upload.
  std::vector<RECT> TakeChanges();
  PixelView View(const RECT& area) const;

  CanvasSnapshot Snapshot() const;

 private:
  struct Live {
    std::shared_ptr<Image> before;  // Null where the tile did not exist.
    std::vector<float> mask;
  };

  int TileOf(int x, int y) const { return (y / kCanvasTile) * columns_ + x / kCanvasTile; }
  RECT TileRect(int index) const;  // In the picture's pixels.
  Image& Fresh(int index);
  Image& Writable(int index);
  void Fill(Image& tile, int index, const RECT& part, const Image* from) const;
  void Compose(const Stroke& stroke, int index, const Live& live, const RECT& part);

  std::shared_ptr<const Image> base_;
  POINT origin_;
  int columns_ = 0;
  int rows_ = 0;
  std::vector<std::shared_ptr<Image>> tiles_;
  std::unordered_map<int, Live> live_;
  std::vector<RECT> changes_;
  std::vector<float> scratch_;
};

}  // namespace sstk
