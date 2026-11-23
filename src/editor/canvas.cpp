#include "sstk/editor/canvas.h"

#include "sstk/editor/brush.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sstk {

namespace {

uint32_t* RowOf(Image& image, int y) { return reinterpret_cast<uint32_t*>(image.data() + static_cast<ptrdiff_t>(y) * image.stride()); }

const uint32_t* RowOf(const Image& image, int y) {
  return reinterpret_cast<const uint32_t*>(image.data() + static_cast<ptrdiff_t>(y) * image.stride());
}

RECT Intersect(const RECT& a, const RECT& b) {
  RECT out{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
  if (out.right < out.left) out.right = out.left;
  if (out.bottom < out.top) out.bottom = out.top;
  return out;
}

bool Empty(const RECT& rect) { return rect.right <= rect.left || rect.bottom <= rect.top; }

}  // namespace

uint32_t CanvasSnapshot::At(int x, int y) const {
  x = std::clamp(x, 0, width() - 1);
  y = std::clamp(y, 0, height() - 1);
  const std::shared_ptr<const Image>& tile = tiles[static_cast<size_t>((y / kCanvasTile) * columns + x / kCanvasTile)];
  if (tile) return RowOf(*tile, y % kCanvasTile)[x % kCanvasTile];
  return RowOf(*base, y)[x] | 0xFF000000u;
}

void CanvasSnapshot::Row(int y, int x, int count, uint32_t* out) const {
  const int w = width();
  y = std::clamp(y, 0, height() - 1);
  int i = 0;
  for (; i < count && x + i < 0; ++i) out[i] = At(0, y);
  while (i < count && x + i < w) {
    const int at = x + i;
    const int column = at / kCanvasTile;
    const int end = std::min({(column + 1) * kCanvasTile, w, x + count});
    const int run = end - at;
    const std::shared_ptr<const Image>& tile = tiles[static_cast<size_t>((y / kCanvasTile) * columns + column)];
    if (tile) {
      std::memcpy(out + i, RowOf(*tile, y % kCanvasTile) + (at - column * kCanvasTile), static_cast<size_t>(run) * 4);
    } else {
      const uint32_t* source = RowOf(*base, y) + at;
      for (int k = 0; k < run; ++k) out[i + k] = source[k] | 0xFF000000u;
    }
    i += run;
  }
  for (; i < count; ++i) out[i] = At(w - 1, y);
}

void CanvasSnapshot::TurnedRow(int turns, int y, int x, int count, uint32_t* out) const {
  turns = ((turns % 4) + 4) % 4;
  if (turns == 0) {
    Row(y - origin.y, x - origin.x, count, out);
    return;
  }
  for (int i = 0; i < count; ++i) {
    const POINT pixel = TurnedPixel(x + i, y, turns);
    out[i] = At(pixel.x - origin.x, pixel.y - origin.y);
  }
}

InkCanvas::InkCanvas(std::shared_ptr<const Image> base, POINT origin) : base_(std::move(base)), origin_(origin) {
  columns_ = (width() + kCanvasTile - 1) / kCanvasTile;
  rows_ = (height() + kCanvasTile - 1) / kCanvasTile;
  tiles_.resize(static_cast<size_t>(columns_) * rows_);
  scratch_.resize(static_cast<size_t>(kCanvasTile) * kCanvasTile);
}

RECT InkCanvas::TileRect(int index) const {
  const int column = index % columns_;
  const int row = index / columns_;
  return RECT{column * kCanvasTile, row * kCanvasTile, std::min((column + 1) * kCanvasTile, width()), std::min((row + 1) * kCanvasTile, height())};
}

Image& InkCanvas::Fresh(int index) {
  std::shared_ptr<Image>& tile = tiles_[static_cast<size_t>(index)];
  if (!tile || tile.use_count() > 1) {
    const RECT rect = TileRect(index);
    tile = std::make_shared<Image>(Image::Allocate(rect.right - rect.left, rect.bottom - rect.top));
  }
  return *tile;
}

Image& InkCanvas::Writable(int index) {
  std::shared_ptr<Image>& tile = tiles_[static_cast<size_t>(index)];
  const RECT rect = TileRect(index);
  const RECT whole{0, 0, rect.right - rect.left, rect.bottom - rect.top};
  if (!tile) {
    tile = std::make_shared<Image>(Image::Allocate(whole.right, whole.bottom));
    if (*tile) Fill(*tile, index, whole, nullptr);
  } else if (tile.use_count() > 1) {
    auto copy = std::make_shared<Image>(Image::Allocate(whole.right, whole.bottom));
    if (*copy) Fill(*copy, index, whole, tile.get());
    tile = std::move(copy);
  }
  return *tile;
}

void InkCanvas::Fill(Image& tile, int index, const RECT& part, const Image* from) const {
  if (!tile) return;
  const RECT rect = TileRect(index);
  const int width = part.right - part.left;
  for (int y = part.top; y < part.bottom; ++y) {
    uint32_t* out = RowOf(tile, y) + part.left;
    if (from != nullptr && *from) {
      std::memcpy(out, RowOf(*from, y) + part.left, static_cast<size_t>(width) * 4);
    } else {
      const uint32_t* source = RowOf(*base_, rect.top + y) + rect.left + part.left;
      for (int x = 0; x < width; ++x) out[x] = source[x] | 0xFF000000u;
    }
  }
}

void InkCanvas::Compose(const Stroke& stroke, int index, const Live& live, const RECT& part) {
  Image& tile = Writable(index);
  if (!tile) return;
  Fill(tile, index, part, live.before.get());
  const RECT rect = TileRect(index);
  const int width = part.right - part.left;
  const int height = part.bottom - part.top;
  BlendStroke(stroke, live.mask.data() + static_cast<ptrdiff_t>(part.top) * kCanvasTile + part.left, kCanvasTile, RowOf(tile, part.top) + part.left,
              tile.width(), RowOf(*base_, rect.top + part.top) + rect.left + part.left, base_->width(), width, height);
  changes_.push_back(RECT{rect.left + part.left, rect.top + part.top, rect.left + part.right, rect.top + part.bottom});
}

void InkCanvas::Redraw(const StrokeList& strokes, const RECT& area) {
  const RECT local = Intersect(RECT{area.left - origin_.x, area.top - origin_.y, area.right - origin_.x, area.bottom - origin_.y}, RECT{0, 0, width(), height()});
  if (Empty(local)) return;
  for (int row = local.top / kCanvasTile; row <= (local.bottom - 1) / kCanvasTile; ++row) {
    for (int column = local.left / kCanvasTile; column <= (local.right - 1) / kCanvasTile; ++column) {
      const int index = row * columns_ + column;
      const RECT rect = TileRect(index);
      const RECT plane{rect.left + origin_.x, rect.top + origin_.y, rect.right + origin_.x, rect.bottom + origin_.y};
      const bool touched = std::any_of(strokes.begin(), strokes.end(), [&](const auto& stroke) { return Overlaps(stroke->bounds, plane); });
      if (!touched && !tiles_[static_cast<size_t>(index)]) continue;
      Image& tile = Fresh(index);
      if (!tile) continue;
      Fill(tile, index, RECT{0, 0, tile.width(), tile.height()}, nullptr);
      for (const auto& stroke : strokes) {
        const RECT part = Intersect(stroke->bounds, plane);
        if (Empty(part)) continue;
        const int left = part.left - plane.left;
        const int top = part.top - plane.top;
        const int width = part.right - part.left;
        const int height = part.bottom - part.top;
        float* mask = scratch_.data() + static_cast<ptrdiff_t>(top) * kCanvasTile + left;
        for (int y = 0; y < height; ++y) std::fill_n(mask + static_cast<ptrdiff_t>(y) * kCanvasTile, width, 0.0f);
        DrawStroke(*stroke, part, mask, kCanvasTile);
        BlendStroke(*stroke, mask, kCanvasTile, RowOf(tile, top) + left, tile.width(), RowOf(*base_, rect.top + top) + rect.left + left,
                    base_->width(), width, height);
      }
      changes_.push_back(rect);
    }
  }
}

void InkCanvas::Begin() { live_.clear(); }

void InkCanvas::Extend(const Stroke& stroke, size_t first) {
  std::unordered_map<int, RECT> touched;
  for (size_t i = first; i < stroke.stamps.size(); ++i) {
    const Stamp& stamp = stroke.stamps[i];
    const float reach = StampReach(stamp);
    const RECT box = Intersect(RECT{static_cast<LONG>(std::floor(stamp.x - reach)) - origin_.x, static_cast<LONG>(std::floor(stamp.y - reach)) - origin_.y,
                                    static_cast<LONG>(std::ceil(stamp.x + reach)) - origin_.x, static_cast<LONG>(std::ceil(stamp.y + reach)) - origin_.y},
                               RECT{0, 0, width(), height()});
    if (Empty(box)) continue;
    for (int row = box.top / kCanvasTile; row <= (box.bottom - 1) / kCanvasTile; ++row) {
      for (int column = box.left / kCanvasTile; column <= (box.right - 1) / kCanvasTile; ++column) {
        const int index = row * columns_ + column;
        const RECT rect = TileRect(index);
        const RECT part = Intersect(box, rect);
        const RECT inside{part.left - rect.left, part.top - rect.top, part.right - rect.left, part.bottom - rect.top};
        auto found = touched.find(index);
        if (found == touched.end()) {
          touched.emplace(index, inside);
        } else {
          found->second = Union(found->second, inside);
        }
      }
    }
  }
  for (const auto& [index, part] : touched) {
    auto found = live_.find(index);
    if (found == live_.end()) {
      Live live;
      live.before = tiles_[static_cast<size_t>(index)];
      live.mask.assign(static_cast<size_t>(kCanvasTile) * kCanvasTile, 0.0f);
      found = live_.emplace(index, std::move(live)).first;
    }
    const RECT rect = TileRect(index);
    DrawStamps(stroke, first, stroke.stamps.size(), RECT{rect.left + origin_.x, rect.top + origin_.y, rect.right + origin_.x, rect.bottom + origin_.y},
               found->second.mask.data(), kCanvasTile);
    Compose(stroke, index, found->second, part);
  }
}

void InkCanvas::Restroke(const Stroke& stroke) {
  for (auto& [index, live] : live_) std::fill(live.mask.begin(), live.mask.end(), 0.0f);
  Extend(stroke, 0);
  // Tiles the stroke no longer reaches go back to what they were.
  for (auto& [index, live] : live_) {
    const RECT rect = TileRect(index);
    Compose(stroke, index, live, RECT{0, 0, rect.right - rect.left, rect.bottom - rect.top});
  }
}

void InkCanvas::End() { live_.clear(); }

void InkCanvas::Cancel() {
  for (auto& [index, live] : live_) {
    const RECT rect = TileRect(index);
    if (live.before) {
      tiles_[static_cast<size_t>(index)] = live.before;
    } else if (Image& tile = Fresh(index); tile) {
      Fill(tile, index, RECT{0, 0, tile.width(), tile.height()}, nullptr);
    }
    changes_.push_back(rect);
  }
  live_.clear();
}

std::vector<RECT> InkCanvas::TakeChanges() {
  std::vector<RECT> changes;
  changes.swap(changes_);
  return changes;
}

CanvasSnapshot InkCanvas::Snapshot() const {
  CanvasSnapshot snapshot;
  snapshot.base = base_;
  snapshot.tiles.assign(tiles_.begin(), tiles_.end());
  snapshot.columns = columns_;
  snapshot.rows = rows_;
  snapshot.origin = origin_;
  return snapshot;
}

}  // namespace sstk
