#include "recorder.h"

#include "config/core.h"
#include "screen.h"
#include "stitcher.h"
#include "workers.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

namespace sstk {

namespace {

constexpr double kSendInterval = 1.0 / 30.0;
constexpr double kHoldReferenceSeconds = 1.5;
constexpr uint32_t kQuietPixels = 24;
constexpr int kHintSlack = 16;   // How far what moved may reach past an area the app reported and still be in it.
constexpr int kPartRows = 64;    // Rows of the image made, drawn and packed together on one worker.

// One scrolling screenshot, stepped by the recorder's thread.
class Session : private Stitcher::Sink {
 public:
  explicit Session(ScrollRequest request);
  ~Session() override;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool done() const { return done_; }

  // Commands are taken as they come. Sampling the screen is paced by Interval, faster while things move.
  void Handle(const std::vector<POINT>& wheels, bool finish);
  void Sample(double now);
  double Interval() const;

 private:
  void Rows(int first, int count, const Stitcher::RowMaker& rows, bool final) override;
  void Record(int slot) override;
  void Stop() override;
  void Replay(int slot) override;

  void StepWaiting(double now, POINT cursor);
  void StepLocked(double now, POINT cursor);
  bool FindArea(bool wheel, RECT* area, RECT* frame);
  bool Lock(const RECT& area, const RECT& frame, int after, POINT local, double now);
  bool WheelFits(POINT point, double now);
  bool WindowStays() const;
  bool Read(Frame& frame, const RECT& rect);
  std::shared_ptr<Frame> Acquire(int width, int height);
  void Trim();
  void ReadBars();
  void CheckMismatch(double now);
  void Draw(const PixelView& rows, int first);
  void DrawBars();
  void Send(bool locked);
  void End(bool early);
  void Post(std::unique_ptr<ScrollUpdate> update) const;

  int ThumbFloor(int row) const { return kLongOrigin + static_cast<int>(std::floor(row * k_ + 1e-6)); }
  int ThumbCeil(int row) const { return kLongOrigin + static_cast<int>(std::ceil(row * k_ - 1e-6)); }

  ScrollRequest request_;
  Workers workers_;
  HDC screen_ = nullptr;
  HDC memory_ = nullptr;  // What the screen is read into a reading through.
  DWORD own_process_ = 0;
  double started_ = 0.0;
  RECT window_rect_{};
  RECT watch_{};  // The window on the monitor, in monitor pixels.
  double last_change_ = 0.0;
  double last_scroll_ = 0.0;
  double last_wheel_ = -1e9;
  double last_send_ = -1e9;
  double mismatch_since_ = 0.0;
  bool done_ = false;

  // Before anything has moved: the last two readings of the whole window, and what tells a scroll between them.
  std::shared_ptr<Frame> grab_[2];
  int current_ = 0;
  bool have_before_ = false;
  bool seen_motion_ = false;
  double before_since_ = 0.0;
  Tracker finder_;

  // Once the area is known, only it is read. A reading is read into again once nothing needs it any longer.
  bool locked_ = false;
  Stitcher stitcher_;
  std::vector<std::shared_ptr<Frame>> frames_;
  RECT follow_{};  // The area, in monitor pixels. Content row 0 is its first row when the capture started.
  int limit_rows_ = 0;
  int thumb_width_ = 0;
  double k_ = 1.0;  // Thumbnail pixels per screen pixel.

  // What the window shows right above and below the area: read as it is, and set on the two ends of the image.
  RECT head_rect_{};  // Monitor pixels.
  RECT tail_rect_{};
  std::vector<uint32_t> head_;
  std::vector<uint32_t> tail_;
  int head_rows_ = 0;
  int tail_rows_ = 0;

  std::shared_ptr<LongImage> image_;
  std::vector<ScrollPiece> pieces_;  // Thumbnail rows of finished rows, not sent yet.
  std::vector<ScrollPiece> recorded_[2];
  int recording_ = -1;
  size_t recording_from_ = 0;
  bool view_changed_ = false;
};

Session::Session(ScrollRequest request)
    : request_(std::move(request)), screen_(GetDC(nullptr)), own_process_(GetCurrentProcessId()), started_(NowSeconds()) {
  memory_ = CreateCompatibleDC(screen_);
  last_change_ = started_;
  GetWindowRect(request_.window, &window_rect_);
  if (!VisibleFrame(request_.window, request_.monitor, &watch_)) watch_ = request_.monitor;
  OffsetRect(&watch_, -request_.monitor.left, -request_.monitor.top);
}

Session::~Session() {
  if (memory_ != nullptr) DeleteDC(memory_);
  if (screen_ != nullptr) ReleaseDC(nullptr, screen_);
}

double Session::Interval() const {
  // Lost for now, it looks harder: the rows to line up with are moving out of reach.
  if (locked_ && mismatch_since_ != 0.0) return config::kScrollCatchUpMs / 1000.0;
  const unsigned ms = NowSeconds() - last_change_ < config::kScrollActiveHold ? config::kScrollActiveMs : config::kScrollRestingMs;
  return ms / 1000.0;
}

void Session::Handle(const std::vector<POINT>& wheels, bool finish) {
  if (finish) {
    End(false);
    return;
  }
  const double now = NowSeconds();
  for (const POINT& point : wheels) {
    if (!WheelFits(point, now)) {
      Trace("scroll: a wheel turned outside the window");
      End(true);
      return;
    }
  }
}

void Session::Sample(double now) {
  if (!WindowStays()) {
    Trace("scroll: the window moved or closed");
    End(true);
    return;
  }
  POINT cursor{};
  GetCursorPos(&cursor);
  if (locked_) {
    StepLocked(now, cursor);
    Trim();
  } else {
    StepWaiting(now, cursor);
  }
}

// Over this program's own windows the wheel goes on to whatever lies beneath, so outside the app it is no reason to stop.
// Elsewhere in the app it may scroll something else, which is simply not followed.
bool Session::WheelFits(POINT point, double now) {
  DWORD process = 0;
  if (HWND under = WindowFromPoint(point); under != nullptr) GetWindowThreadProcessId(under, &process);
  const POINT local{point.x - request_.monitor.left, point.y - request_.monitor.top};
  const bool inside = PtInRect(&watch_, local) != FALSE;
  if (process == own_process_ && !inside) return true;
  last_change_ = now;
  last_wheel_ = now;
  return inside;
}

bool Session::WindowStays() const {
  RECT rect;
  return IsWindow(request_.window) && !IsIconic(request_.window) && GetWindowRect(request_.window, &rect) && EqualRect(&rect, &window_rect_);
}

// Reads monitor pixels into a reading of the same size, without CAPTUREBLT, so the pointer never flickers.
bool Session::Read(Frame& frame, const RECT& rect) {
  const HGDIOBJ previous = SelectObject(memory_, frame.bitmap());
  if (previous == nullptr) return false;
  const bool read = BitBlt(memory_, 0, 0, frame.width(), frame.height(), screen_, request_.monitor.left + rect.left, request_.monitor.top + rect.top, SRCCOPY) != FALSE;
  SelectObject(memory_, previous);
  GdiFlush();
  return read;
}

// A reading of the area to read into: one nothing needs any longer, or else a new one.
std::shared_ptr<Frame> Session::Acquire(int width, int height) {
  for (const std::shared_ptr<Frame>& frame : frames_) {
    if (frame.use_count() == 1 && frame->width() == width && frame->height() == height) return frame;
  }
  auto frame = std::make_shared<Frame>(width, height);
  if (!*frame) return nullptr;
  frames_.push_back(frame);
  return frame;
}

// Readings nothing needs are let go, but for one to read the next into.
void Session::Trim() {
  bool spare = false;
  for (auto it = frames_.begin(); it != frames_.end();) {
    if (it->use_count() == 1) {
      if (spare) {
        it = frames_.erase(it);
        continue;
      }
      spare = true;
    }
    ++it;
  }
}

void Session::CheckMismatch(double now) {
  // Some apps blur content in motion, so it may only line up again once the wheel has stopped.
  const double limit = now - last_wheel_ < config::kScrollWheelActiveSeconds ? config::kScrollMismatchWheelSeconds : config::kScrollMismatchSeconds;
  if (mismatch_since_ != 0.0 && now - mismatch_since_ >= limit) {
    Trace("scroll: the content stopped matching");
    End(true);
  }
}

// What moved says what is being scrolled. Of the areas the app reported as scrolling, it is the smallest one around
// everything that moved: scrolling a page moves what is in a list inside it too, but reaches beyond the list.
// `frame` is the area together with whatever the window shows above and below it across the same columns, as bars
// that belong with it. Those are no part of what scrolls, and go on the ends of the image as they are.
bool Session::FindArea(bool wheel, RECT* area, RECT* frame) {
  const PixelView& reading = finder_.now();
  const RECT whole{0, 0, reading.width, reading.height};
  const auto within = [&](const std::vector<RECT>& reported) {
    std::vector<RECT> rects;
    for (RECT rect : reported) {
      OffsetRect(&rect, -watch_.left, -watch_.top);
      RECT inside{};
      if (IntersectRect(&inside, &rect, &whole)) rects.push_back(inside);
    }
    return rects;
  };
  const std::vector<RECT> hints = within(request_.hints.areas);
  const std::vector<RECT> frames = within(request_.hints.frames);
  // Other things may change in the window at the same time, so where the whole of it shows no one shift, each area
  // is looked at by itself, largest first.
  Tracker::Match match;
  bool sure = false;
  bool found = finder_.Register(0, &match, &sure);
  for (size_t i = 0; i < hints.size() && !found; ++i) {
    if (finder_.Focus(hints[i]) > kQuietPixels) found = finder_.Register(0, &match, &sure);
  }
  if (!found || (!wheel && !sure)) {
    finder_.Focus(whole);
    return false;
  }
  finder_.Focus(whole);
  const RECT box = finder_.MovedBox(match.shift);
  Trace("scroll: waiting: shift %d explains %u of %u, moved %ld,%ld-%ld,%ld, wheel %d", match.shift, match.explained, match.compared, box.left, box.top, box.right, box.bottom, wheel);
  if (box.right <= box.left) return false;
  if (!wheel && box.bottom - box.top < static_cast<int>(config::kScrollAreaMinHeight * request_.scale)) return false;
  const auto size = [](const RECT& rect) { return (rect.right - rect.left) * (rect.bottom - rect.top); };
  const auto holds = [](const RECT& outer, const RECT& inner) {
    return outer.left <= inner.left + kHintSlack && outer.top <= inner.top + kHintSlack && outer.right >= inner.right - kHintSlack && outer.bottom >= inner.bottom - kHintSlack;
  };
  const auto smallest = [&](const std::vector<RECT>& among, const RECT& first, const RECT& second) {
    const RECT* least = nullptr;
    for (const RECT& rect : among) {
      if (holds(rect, first) && holds(rect, second) && (least == nullptr || size(rect) < size(*least))) least = &rect;
    }
    return least;
  };
  const RECT* chosen = smallest(hints, box, box);
  if (chosen == nullptr) {
    // What moved reaches beyond every area reported, as a ruler beside a document does. It is then the smallest part
    // of the window around both what moved and the area most of it lies in.
    const RECT* most = nullptr;
    long shared = 0;
    for (const RECT& hint : hints) {
      RECT both{};
      if (IntersectRect(&both, &hint, &box) && size(both) > shared) {
        most = &hint;
        shared = size(both);
      }
    }
    if (most != nullptr && shared * 2 >= size(box)) chosen = smallest(frames, box, *most);
  }
  // What the app reports is the area, whole. Otherwise it is what moved, if that is worth following.
  *area = chosen != nullptr ? *chosen : box;
  *frame = *area;
  if (chosen == nullptr && box.right - box.left < static_cast<int>(config::kScrollAreaMinWidth * request_.scale)) return false;
  if (chosen != nullptr) {
    // As wide as the area, give or take a scroll bar, a part of the window around it holds nothing beside it.
    const int slack = static_cast<int>(std::lround(config::kScrollBarWidth * request_.scale));
    for (const RECT& outer : frames) {
      if (std::abs(outer.left - area->left) > slack || std::abs(outer.right - area->right) > slack || outer.top > area->top || outer.bottom < area->bottom) continue;
      frame->left = std::min(frame->left, outer.left);
      frame->right = std::max(frame->right, outer.right);
      frame->top = std::min(frame->top, outer.top);
      frame->bottom = std::max(frame->bottom, outer.bottom);
    }
    // The area takes the columns of the bars above and below it, so that the image is as wide all the way down.
    area->left = frame->left;
    area->right = frame->right;
  }
  return area->right - area->left >= Tracker::kSegment * 2 && area->bottom - area->top > Tracker::kMinOverlap * 2;
}

void Session::StepWaiting(double now, POINT cursor) {
  if (now - started_ >= config::kScrollStartSeconds) {
    End(false);
    return;
  }
  const int slot = 1 - current_;
  if (!grab_[slot]) grab_[slot] = std::make_shared<Frame>(watch_.right - watch_.left, watch_.bottom - watch_.top);
  if (!*grab_[slot] || !Read(*grab_[slot], watch_)) {
    End(true);
    return;
  }
  const PixelView reading = grab_[slot]->view();
  if (!have_before_) {
    finder_.Reset(reading.width, reading.height, &workers_);
    finder_.SetRef(reading);
    current_ = slot;
    have_before_ = true;
    before_since_ = now;
    return;
  }
  if (finder_.Load(reading, RECT{0, 0, reading.width, reading.height}, INT_MAX, -1, kQuietPixels) <= kQuietPixels) return;
  last_change_ = now;
  const bool wheel = now - last_wheel_ < config::kScrollWheelActiveSeconds;
  // Caught in motion, a reading says little, and the last settled one is held on to until the motion can be measured
  // from it.
  const bool widely = finder_.changed_rows() * 8 >= reading.height;
  RECT area{};
  RECT frame{};
  if (FindArea(wheel, &area, &frame)) {
    // Without a wheel to say so, one reading may be an animation. The next will tell, measured from the same place.
    if (!wheel && !seen_motion_) {
      seen_motion_ = true;
      return;
    }
    const POINT local{cursor.x - request_.monitor.left - watch_.left - area.left, cursor.y - request_.monitor.top - watch_.top - area.top};
    if (!Lock(area, frame, slot, local, now)) End(true);
    return;
  }
  seen_motion_ = false;
  if (widely && now - before_since_ < kHoldReferenceSeconds) return;
  finder_.Promote();
  current_ = slot;
  before_since_ = now;
}

// Starts following the area, from the two readings of the whole window that showed it scroll: the one before, and
// the one in `after`. They are let go one by one, as soon as what they hold of the area is copied.
bool Session::Lock(const RECT& area, const RECT& frame, int after, POINT local, double now) {
  finder_ = Tracker{};
  follow_ = area;
  OffsetRect(&follow_, watch_.left, watch_.top);
  const int width = area.right - area.left;
  const int height = area.bottom - area.top;
  limit_rows_ = std::max(std::min(config::kScrollMaxHeight, config::kScrollMaxPixels / width), height);
  head_rect_ = RECT{follow_.left, frame.top + watch_.top, follow_.right, follow_.top};
  tail_rect_ = RECT{follow_.left, follow_.bottom, follow_.right, frame.bottom + watch_.top};
  head_rows_ = head_rect_.bottom - head_rect_.top;
  tail_rows_ = tail_rect_.bottom - tail_rect_.top;
  const auto copy = [&](const PixelView& reading, int top, int rows, std::vector<uint32_t>& out) {
    out.resize(static_cast<size_t>(width) * rows);
    for (int y = 0; y < rows; ++y) std::memcpy(out.data() + static_cast<size_t>(y) * width, PixelRow(reading, top + y) + area.left, static_cast<size_t>(width) * 4);
  };
  const auto crop = [&](const PixelView& reading) {
    std::shared_ptr<Frame> out = Acquire(width, height);
    if (out) {
      for (int y = 0; y < height; ++y) std::memcpy(out->pixels() + static_cast<size_t>(y) * width, PixelRow(reading, area.top + y) + area.left, static_cast<size_t>(width) * 4);
    }
    return out;
  };
  const PixelView before = grab_[1 - after]->view();
  copy(before, frame.top, head_rows_, head_);
  copy(before, area.bottom, tail_rows_, tail_);
  const std::shared_ptr<Frame> first = crop(before);
  grab_[1 - after].reset();
  if (!first) return false;
  const std::shared_ptr<Frame> second = crop(grab_[after]->view());
  grab_[after].reset();
  if (!second) return false;

  thumb_width_ = ThumbnailSize(RECT{0, 0, width, head_rows_ + height + tail_rows_}, request_.scale).cx;
  k_ = static_cast<double>(thumb_width_) / width;
  image_ = std::make_shared<LongImage>(width);
  if (!stitcher_.Start(first, request_.scale, limit_rows_, this, &workers_)) return false;
  stitcher_.Feed(second, local);
  locked_ = true;
  view_changed_ = true;
  last_scroll_ = now;
  last_change_ = now;
  Trace("scroll: following %dx%d at %ld,%ld, with %d rows above and %d below", width, height, follow_.left, follow_.top, head_rows_, tail_rows_);
  Send(true);
  return true;
}

void Session::StepLocked(double now, POINT cursor) {
  if (now - last_scroll_ >= config::kScrollIdleSeconds) {
    End(false);
    return;
  }
  const std::shared_ptr<Frame> reading = Acquire(follow_.right - follow_.left, follow_.bottom - follow_.top);
  if (!reading || !Read(*reading, follow_)) {
    End(true);
    return;
  }
  const POINT local{cursor.x - request_.monitor.left - follow_.left, cursor.y - request_.monitor.top - follow_.top};
  const Stitcher::Step step = stitcher_.Feed(reading, local);
  if (stitcher_.kept()) view_changed_ = true;
  switch (step) {
    case Stitcher::Step::kSame:
      mismatch_since_ = 0.0;
      break;
    case Stitcher::Step::kRest:
      last_change_ = now;
      mismatch_since_ = 0.0;
      break;
    case Stitcher::Step::kMoved:
      last_change_ = now;
      last_scroll_ = now;
      mismatch_since_ = 0.0;
      break;
    case Stitcher::Step::kLost:
      // Something changing by itself, such as a video, is no reason to give up. The whole area being replaced is, and so
      // is a wheel turning while the content cannot be followed.
      last_change_ = now;
      if (stitcher_.replaced() || now - last_wheel_ < config::kScrollMismatchSeconds) {
        if (mismatch_since_ == 0.0) mismatch_since_ = now;
      } else {
        mismatch_since_ = 0.0;
      }
      CheckMismatch(now);
      if (done_) return;
      break;
  }
  // Nothing new to show is nothing to send, and nothing for the screen to draw again.
  if ((view_changed_ || !pieces_.empty()) && now - last_send_ >= kSendInterval) Send(false);
  if (stitcher_.grown() + stitcher_.height() >= limit_rows_) {
    Trace("scroll: reached the longest a capture may be");
    End(false);
  }
}

// Rows from the stitcher: drawn small for the thumbnail, and packed once final. Workers take a part of the rows each,
// and make each row once, both for the thumbnail rows they draw and for the rows they pack.
void Session::Rows(int first, int count, const Stitcher::RowMaker& rows, bool final) {
  if (count <= 0) return;
  const int width = stitcher_.width();
  const int parts = (count + kPartRows - 1) / kPartRows;
  // Thumbnail rows these rows only reach into are drawn from what is here of them, and again from the rest when that
  // comes.
  const int from = std::max(ThumbFloor(first), 0);
  const int to = std::min(ThumbCeil(first + count), kLongRows);
  Image image;
  std::optional<Resampler> sampler;
  std::vector<int> drawn(static_cast<size_t>(parts) + 1, 0);  // Where the thumbnail rows each part draws begin.
  if (to > from) {
    const double top = std::max((from - kLongOrigin) / k_ - first, 0.0);
    const double bottom = std::min((to - kLongOrigin) / k_ - first, static_cast<double>(count));
    image = Image::Allocate(thumb_width_, to - from);
    if (image) {
      sampler.emplace(width, count, 0.0, top, width, bottom, thumb_width_, to - from);
      // A thumbnail row is drawn by the part its first row lies in.
      int row = 0;
      for (int part = 0; part < parts; ++part) {
        drawn[static_cast<size_t>(part)] = row;
        const int end = std::min((part + 1) * kPartRows, count);
        while (row < to - from && sampler->Row(row).first < end) ++row;
      }
      drawn[static_cast<size_t>(parts)] = row;
    }
  }
  std::vector<LongImage::Strip> strips(final ? static_cast<size_t>(parts) : 0);
  workers_.For(parts, [&](int part) {
    const int own = part * kPartRows;
    const int own_end = std::min(own + kPartRows, count);
    const int draw_first = drawn[static_cast<size_t>(part)];
    const int draw_last = drawn[static_cast<size_t>(part) + 1];
    const bool draws = sampler.has_value() && draw_first < draw_last;
    if (!draws && !final) return;
    const int need = draws ? std::max(own_end, sampler->Row(draw_last - 1).last + 1) : own_end;
    std::vector<uint32_t> pixels(static_cast<size_t>(need - own) * width);
    rows.Make(first + own, need - own, pixels.data());
    if (draws) sampler->Make(draw_first, draw_last, reinterpret_cast<const uint8_t*>(pixels.data()), own, static_cast<ptrdiff_t>(width) * 4, image.data(), image.stride());
    if (final) strips[static_cast<size_t>(part)] = image_->Pack(first + own, own_end - own, pixels.data());
  });
  if (image) pieces_.push_back(ScrollPiece{from, std::move(image)});
  for (LongImage::Strip& strip : strips) image_->Append(std::move(strip));
}

void Session::Record(int slot) {
  recording_ = slot;
  recording_from_ = pieces_.size();
}

void Session::Stop() {
  if (recording_ < 0) return;
  std::vector<ScrollPiece>& kept = recorded_[recording_];
  kept.clear();
  for (size_t i = recording_from_; i < pieces_.size(); ++i) {
    const Image& rows = pieces_[i].rows;
    kept.push_back(ScrollPiece{pieces_[i].at, CopyRect(rows, 0, 0, rows.width(), rows.height())});
  }
  recording_ = -1;
}

void Session::Replay(int slot) {
  for (const ScrollPiece& piece : recorded_[slot]) {
    pieces_.push_back(ScrollPiece{piece.at, CopyRect(piece.rows, 0, 0, piece.rows.width(), piece.rows.height())});
  }
}

// Thumbnail rows for rows of the image, the first of them content row `first`. Thumbnail rows these rows only reach
// into are drawn from what is here of them, and again from the rest when that comes.
void Session::Draw(const PixelView& rows, int first) {
  const int from = std::max(ThumbFloor(first), 0);
  const int to = std::min(ThumbCeil(first + rows.height), kLongRows);
  if (to <= from || rows.height <= 0) return;
  const double top = std::max((from - kLongOrigin) / k_ - first, 0.0);
  const double bottom = std::min((to - kLongOrigin) / k_ - first, static_cast<double>(rows.height));
  Image image = ResampleArea(rows, 0.0, top, rows.width, bottom, thumb_width_, to - from);
  if (image) pieces_.push_back(ScrollPiece{from, std::move(image)});
}

void Session::DrawBars() {
  const int width = stitcher_.width();
  if (head_rows_ > 0) Draw(PixelView{reinterpret_cast<const uint8_t*>(head_.data()), width, head_rows_, width * 4}, stitcher_.first_row() - head_rows_);
  if (tail_rows_ > 0) Draw(PixelView{reinterpret_cast<const uint8_t*>(tail_.data()), width, tail_rows_, width * 4}, stitcher_.last_row());
}

// Reads the bar beside the end of the image the view rests at again, as it is now. The other stays as it was when the
// capture began, beside the end the view was at then: what shows there may be content that has scrolled on since.
void Session::ReadBars() {
  const auto read = [&](const RECT& rect, int rows, std::vector<uint32_t>& out) {
    if (rows <= 0) return;
    Frame bar(rect.right - rect.left, rows);
    if (!bar || !Read(bar, rect)) return;
    std::memcpy(out.data(), bar.pixels(), static_cast<size_t>(bar.width()) * rows * 4);
  };
  if (stitcher_.position() == stitcher_.first_row()) read(head_rect_, head_rows_, head_);
  if (stitcher_.position() + stitcher_.height() == stitcher_.last_row()) read(tail_rect_, tail_rows_, tail_);
}

void Session::Post(std::unique_ptr<ScrollUpdate> update) const {
  if (PostMessageW(request_.controller, kMsgScrollUpdate, 0, reinterpret_cast<LPARAM>(update.get()))) update.release();
}

void Session::Send(bool locked) {
  auto update = std::make_unique<ScrollUpdate>();
  update->session = request_.session;
  update->locked = locked;
  update->area = follow_;
  update->thumb_width = thumb_width_;
  update->surface_left = follow_.left;
  update->surface_width = thumb_width_;
  // The rows not made yet show as they would be made now, with whatever is fixed where the image will have it.
  if (view_changed_) stitcher_.Preview();
  DrawBars();
  update->pieces = std::move(pieces_);
  pieces_.clear();
  view_changed_ = false;
  update->content_top = ThumbCeil(stitcher_.first_row() - head_rows_);
  update->content_bottom = ThumbFloor(stitcher_.last_row() + tail_rows_);
  update->view_bottom = std::clamp(ThumbFloor(stitcher_.position() + stitcher_.height() + tail_rows_), update->content_top, update->content_bottom);
  last_send_ = NowSeconds();
  Post(std::move(update));
}

void Session::End(bool early) {
  done_ = true;
  auto update = std::make_unique<ScrollUpdate>();
  update->session = request_.session;
  update->finished = true;
  int grown = 0;
  if (locked_) {
    stitcher_.Finish();
    // The ends are settled only now.
    const int top = stitcher_.first_row();
    const int bottom = stitcher_.last_row();
    grown = bottom - top - stitcher_.height();
    // An early end still keeps a capture that has grown far enough to be worth having.
    const bool keep = grown > 0 && (!early || grown >= stitcher_.height() * config::kScrollPartialShare);
    if (keep) {
      ReadBars();
      DrawBars();
      if (head_rows_ > 0) image_->Add(top - head_rows_, head_rows_, head_.data());
      if (tail_rows_ > 0) image_->Add(bottom, tail_rows_, tail_.data());
      image_->Seal(top - head_rows_, bottom + tail_rows_, 0, stitcher_.width());
      Trace("scroll: %dx%d packed into %.1f MB", image_->width(), image_->height(), image_->packed_bytes() / 1048576.0);
      update->outcome = early ? ScrollOutcome::kPartial : ScrollOutcome::kSucceeded;
      update->image = image_;
      update->area = follow_;
      update->image_rect = RECT{follow_.left, follow_.top + top - head_rows_, follow_.right, follow_.top + bottom + tail_rows_};
      update->thumb_width = thumb_width_;
      update->surface_left = follow_.left;
      update->surface_width = thumb_width_;
      update->pieces = std::move(pieces_);
      update->content_top = ThumbCeil(top - head_rows_);
      update->content_bottom = ThumbFloor(bottom + tail_rows_);
      update->view_bottom = update->content_top;
    }
    pieces_.clear();
  }
  Trace("scroll: finished %s with outcome %d, %d rows grown", early ? "early" : "normally", static_cast<int>(update->outcome), grown);
  Post(std::move(update));
  // Pictures for tracing are written once nobody waits for the outcome.
  if (locked_ && Tracing()) stitcher_.Dump();
}

}  // namespace

ScrollRecorder::ScrollRecorder(ScrollRequest request) : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
  thread_ = std::thread([this, request = std::move(request)]() mutable { Main(std::move(request)); });
}

ScrollRecorder::~ScrollRecorder() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  if (wake_ != nullptr) SetEvent(wake_);
  if (thread_.joinable()) thread_.join();
  if (wake_ != nullptr) CloseHandle(wake_);
}

void ScrollRecorder::Wheel(POINT point) {
  {
    std::lock_guard lock(mutex_);
    wheels_.push_back(point);
  }
  if (wake_ != nullptr) SetEvent(wake_);
}

void ScrollRecorder::Finish() {
  {
    std::lock_guard lock(mutex_);
    finish_ = true;
  }
  if (wake_ != nullptr) SetEvent(wake_);
}

void ScrollRecorder::Main(ScrollRequest request) {
  Run(std::move(request));
  done_.store(true, std::memory_order_release);
}

void ScrollRecorder::Run(ScrollRequest request) {
  Session session(std::move(request));
  std::vector<POINT> wheels;
  double last_sample = -1.0;
  while (!session.done()) {
    bool finish = false;
    {
      std::lock_guard lock(mutex_);
      if (stop_) return;
      wheels.swap(wheels_);
      finish = finish_;
    }
    session.Handle(wheels, finish);
    wheels.clear();
    if (session.done()) break;
    // However fast the wheel turns, the screen is read no more often than the pace allows.
    const double now = NowSeconds();
    if (now >= last_sample + session.Interval()) {
      session.Sample(now);
      last_sample = now;
      if (session.done()) break;
    }
    const double wait = std::max(last_sample + session.Interval() - NowSeconds(), 0.0);
    const DWORD ms = static_cast<DWORD>(std::ceil(wait * 1000.0));
    if (wake_ != nullptr) {
      WaitForSingleObject(wake_, ms);
    } else {
      Sleep(ms);
    }
  }
}

}  // namespace sstk
