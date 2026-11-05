#include "recorder.h"

#include "config/core.h"
#include "screen.h"
#include "stitcher.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace sstk {

namespace {

constexpr double kSendInterval = 1.0 / 30.0;
constexpr double kHoldReferenceSeconds = 1.5;

PixelView Crop(const PixelView& view, const RECT& rect) {
  return PixelView{view.data + static_cast<ptrdiff_t>(rect.top) * view.stride + static_cast<ptrdiff_t>(rect.left) * 4, static_cast<int>(rect.right - rect.left),
                   static_cast<int>(rect.bottom - rect.top), view.stride};
}

// Screen pixels read into memory, reused from one reading to the next.
class Grabber {
 public:
  Grabber() = default;
  ~Grabber() { Release(); }
  Grabber(const Grabber&) = delete;
  Grabber& operator=(const Grabber&) = delete;

  bool Ensure(HDC screen, int width, int height) {
    if (width == width_ && height == height_ && bits_ != nullptr) return true;
    Release();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(screen);
    if (dc_ == nullptr) return false;
    void* bits = nullptr;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap_ == nullptr) {
      Release();
      return false;
    }
    previous_ = SelectObject(dc_, bitmap_);
    bits_ = static_cast<uint8_t*>(bits);
    width_ = width;
    height_ = height;
    return true;
  }

  // Without CAPTUREBLT, so the pointer never flickers.
  bool Read(HDC screen, int x, int y) { return BitBlt(dc_, 0, 0, width_, height_, screen, x, y, SRCCOPY) != FALSE; }

  PixelView view() const { return PixelView{bits_, width_, height_, width_ * 4}; }

  void Release() {
    if (dc_ != nullptr) {
      if (previous_ != nullptr) SelectObject(dc_, previous_);
      DeleteDC(dc_);
    }
    if (bitmap_ != nullptr) DeleteObject(bitmap_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    previous_ = nullptr;
    bits_ = nullptr;
    width_ = height_ = 0;
  }

 private:
  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ previous_ = nullptr;
  uint8_t* bits_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// One scrolling screenshot, stepped by the recorder's thread.
class Session {
 public:
  explicit Session(ScrollRequest request);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool done() const { return done_; }

  // Commands are taken as they come. Sampling the screen is paced by Interval, faster while things move.
  void Handle(const std::vector<POINT>& wheels, bool finish);
  void Sample(double now);
  double Interval() const;

 private:
  void StepWaiting(double now, POINT cursor);
  void StepLocked(double now, POINT cursor);
  bool Lock(const ScrollPlan& plan, const PixelView& before, const PixelView& after, POINT local, double now);
  bool WheelFits(POINT point, double now);
  bool WindowStays() const;
  bool Grab(Grabber& grabber, const RECT& rect);
  void CheckMismatch(double now);
  void Send(bool locked);
  void End(bool early);
  void Post(std::unique_ptr<ScrollUpdate> update) const;

  int ThumbFloor(int row) const { return kLongOrigin + static_cast<int>(std::floor((row - anchor_) * k_ + 1e-6)); }
  int ThumbCeil(int row) const { return kLongOrigin + static_cast<int>(std::ceil((row - anchor_) * k_ - 1e-6)); }
  Image ThumbRows(int changed_from, int changed_to, int* at) const;

  ScrollRequest request_;
  HDC screen_ = nullptr;
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

  // Before anything has moved: the last two readings of the whole window.
  Grabber grab_[2];
  int current_ = 0;
  bool have_before_ = false;
  bool seen_motion_ = false;
  double before_since_ = 0.0;
  EdgeFrame before_;
  EdgeFrame after_;
  MotionFinder finder_;
  Motion motion_;

  bool locked_ = false;
  Stitcher stitcher_;
  RECT follow_{};     // What is read from then on, in monitor pixels.
  RECT area_{};       // The part of it the thumbnail shows, as first judged.
  int view_top_ = 0;  // Its rows within the reading.
  int view_bottom_ = 0;
  int anchor_ = 0;    // The content row at the area's top when the capture started.
  int limit_rows_ = 0;
  int thumb_width_ = 0;
  int surface_width_ = 0;
  double k_ = 1.0;  // Thumbnail pixels per screen pixel.
};

Session::Session(ScrollRequest request)
    : request_(std::move(request)), screen_(GetDC(nullptr)), own_process_(GetCurrentProcessId()), started_(NowSeconds()) {
  last_change_ = started_;
  GetWindowRect(request_.window, &window_rect_);
  if (!VisibleFrame(request_.window, request_.monitor, &watch_)) watch_ = request_.monitor;
  OffsetRect(&watch_, -request_.monitor.left, -request_.monitor.top);
}

Session::~Session() {
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
  } else {
    StepWaiting(now, cursor);
  }
}

// Over this program's own windows the wheel reached nothing. Elsewhere in the window it may scroll something else, which
// is simply not followed.
bool Session::WheelFits(POINT point, double now) {
  DWORD process = 0;
  if (HWND under = WindowFromPoint(point); under != nullptr) GetWindowThreadProcessId(under, &process);
  if (process == own_process_) return true;
  last_change_ = now;
  last_wheel_ = now;
  const POINT local{point.x - request_.monitor.left, point.y - request_.monitor.top};
  return PtInRect(&watch_, local) != FALSE;
}

bool Session::WindowStays() const {
  RECT rect;
  return IsWindow(request_.window) && !IsIconic(request_.window) && GetWindowRect(request_.window, &rect) && EqualRect(&rect, &window_rect_);
}

bool Session::Grab(Grabber& grabber, const RECT& rect) {
  if (!grabber.Ensure(screen_, rect.right - rect.left, rect.bottom - rect.top) ||
      !grabber.Read(screen_, request_.monitor.left + rect.left, request_.monitor.top + rect.top)) {
    return false;
  }
  GdiFlush();
  return true;
}

void Session::CheckMismatch(double now) {
  // Some apps blur content in motion, so it may only line up again once the wheel has stopped.
  const double limit = now - last_wheel_ < config::kScrollWheelActiveSeconds ? config::kScrollMismatchWheelSeconds : config::kScrollMismatchSeconds;
  if (mismatch_since_ != 0.0 && now - mismatch_since_ >= limit) {
    Trace("scroll: the content stopped matching");
    End(true);
  }
}

// What moved says what is being scrolled, whichever window part it is, and whatever its scroll bar looks like.
void Session::StepWaiting(double now, POINT cursor) {
  if (now - started_ >= config::kScrollStartSeconds) {
    End(false);
    return;
  }
  const int slot = 1 - current_;
  if (!Grab(grab_[slot], watch_)) {
    End(true);
    return;
  }
  const PixelView reading = grab_[slot].view();
  after_.Build(reading);
  if (!have_before_) {
    std::swap(before_, after_);
    current_ = slot;
    have_before_ = true;
    before_since_ = now;
    return;
  }
  if (after_.SameAs(before_)) return;
  last_change_ = now;
  finder_.Analyze(before_, after_, 0, 0, motion_);
  const bool wheel = now - last_wheel_ < config::kScrollWheelActiveSeconds;
  Trace("scroll: waiting: moved %d blurred %d shift %d, %d moving, wheel %d", motion_.moved, motion_.blurred, motion_.shift, motion_.moving, wheel);
  if (motion_.moved && (wheel || !motion_.blurred)) {
    const POINT local{cursor.x - request_.monitor.left - watch_.left, cursor.y - request_.monitor.top - watch_.top};
    std::vector<RECT> hints = request_.areas;
    for (RECT& hint : hints) OffsetRect(&hint, -watch_.left, -watch_.top);
    const ScrollPlan plan = PlanScrollArea(before_, after_, motion_, local, wheel, request_.scale, hints);
    if (plan.found) {
      // Without a wheel to say so, one reading may be an animation. The next will tell, measured from the same place.
      if (!wheel && !seen_motion_) {
        seen_motion_ = true;
        return;
      }
      if (!Lock(plan, grab_[current_].view(), reading, local, now)) End(true);
      return;
    }
  }
  seen_motion_ = false;
  // Caught in motion, a reading says little. The last sharp one is held on to until the motion can be measured from it.
  const bool unplaced = !motion_.moved && !motion_.blurred && motion_.changed_energy > motion_.fixed_energy;
  if ((unplaced || motion_.blurred) && now - before_since_ < kHoldReferenceSeconds) return;
  std::swap(before_, after_);
  current_ = slot;
  before_since_ = now;
}

bool Session::Lock(const ScrollPlan& plan, const PixelView& before, const PixelView& after, POINT local, double now) {
  follow_ = plan.rect;
  OffsetRect(&follow_, watch_.left, watch_.top);
  const int width = follow_.right - follow_.left;
  const int height = follow_.bottom - follow_.top;
  limit_rows_ = std::max(std::min(config::kScrollMaxHeight, config::kScrollMaxPixels / width), height);
  if (!stitcher_.Start(Crop(before, plan.rect), request_.scale, limit_rows_, plan.hinted, plan.box)) return false;
  stitcher_.Feed(Crop(after, plan.rect), POINT{local.x - plan.rect.left, local.y - plan.rect.top});
  const Stitcher::Crop crop = stitcher_.FinalCrop();
  view_top_ = crop.top - stitcher_.first_row();
  view_bottom_ = crop.bottom - stitcher_.last_row() + height;
  anchor_ = view_top_;
  area_ = RECT{follow_.left + crop.left, follow_.top + view_top_, follow_.left + crop.right, follow_.top + view_bottom_};
  const int area_width = area_.right - area_.left;
  thumb_width_ = ThumbnailSize(RECT{0, 0, area_width, view_bottom_ - view_top_}, request_.scale).cx;
  k_ = static_cast<double>(thumb_width_) / area_width;
  surface_width_ = std::max(static_cast<int>(width * k_), thumb_width_);
  locked_ = true;
  last_scroll_ = now;
  last_change_ = now;
  Trace("scroll: following %dx%d at %ld,%ld%s, showing %d columns from %d and rows %d to %d, shift %d, %d moving cells", width, height, follow_.left, follow_.top, plan.hinted ? " as reported" : "",
        area_width, crop.left, view_top_, view_bottom_, motion_.shift, motion_.moving);
  grab_[1].Release();
  before_ = EdgeFrame{};
  after_ = EdgeFrame{};
  Send(true);
  return true;
}

void Session::StepLocked(double now, POINT cursor) {
  if (now - last_scroll_ >= config::kScrollIdleSeconds) {
    End(false);
    return;
  }
  if (!Grab(grab_[0], follow_)) {
    End(true);
    return;
  }
  const POINT local{cursor.x - request_.monitor.left - follow_.left, cursor.y - request_.monitor.top - follow_.top};
  switch (stitcher_.Feed(grab_[0].view(), local)) {
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
  if (now - last_send_ >= kSendInterval) Send(false);
  if (stitcher_.grown() + stitcher_.height() >= limit_rows_) {
    Trace("scroll: reached the longest a capture may be");
    End(false);
  }
}

Image Session::ThumbRows(int changed_from, int changed_to, int* at) const {
  const int held_first = stitcher_.first_row();
  const int held_last = stitcher_.last_row();
  const int first = std::max({ThumbFloor(changed_from), ThumbCeil(held_first), 0});
  const int last = std::min({ThumbCeil(changed_to), ThumbFloor(held_last), kLongRows});
  *at = first;
  if (last <= first) return Image();
  const double top = (first - kLongOrigin) / k_ + anchor_ - held_first;
  const double bottom = (last - kLongOrigin) / k_ + anchor_ - held_first;
  return ResampleArea(stitcher_.Rows(held_first, held_last), 0.0, top, surface_width_ / k_, bottom, surface_width_, last - first);
}

void Session::Post(std::unique_ptr<ScrollUpdate> update) const {
  if (PostMessageW(request_.controller, kMsgScrollUpdate, 0, reinterpret_cast<LPARAM>(update.get()))) update.release();
}

void Session::Send(bool locked) {
  auto update = std::make_unique<ScrollUpdate>();
  update->session = request_.session;
  update->locked = locked;
  update->area = area_;
  update->thumb_width = thumb_width_;
  update->surface_left = follow_.left;
  update->surface_width = surface_width_;
  int from = 0;
  int to = 0;
  if (stitcher_.TakeChanges(&from, &to)) update->rows = ThumbRows(from, to, &update->at);
  update->content_top = ThumbCeil(stitcher_.first_row() + view_top_);
  update->content_bottom = ThumbFloor(stitcher_.last_row() - stitcher_.height() + view_bottom_);
  update->view_bottom = std::clamp(ThumbFloor(stitcher_.position() + view_bottom_), update->content_top, update->content_bottom);
  last_send_ = NowSeconds();
  Post(std::move(update));
}

void Session::End(bool early) {
  done_ = true;
  if (locked_) {
    stitcher_.Finish();
    Send(false);
  }
  auto update = std::make_unique<ScrollUpdate>();
  update->session = request_.session;
  update->finished = true;
  const int grown = locked_ ? stitcher_.grown() : 0;
  // An early end still keeps a capture that has grown far enough to be worth having.
  if (grown > 0 && (!early || grown >= stitcher_.height() * config::kScrollPartialShare)) {
    const Stitcher::Crop crop = stitcher_.FinalCrop();
    const int width = crop.right - crop.left;
    const int rows = crop.bottom - crop.top;
    auto image = std::make_shared<Image>(Image::Allocate(width, rows));
    if (*image) {
      stitcher_.Copy(crop, image->data(), image->stride());
      update->outcome = early ? ScrollOutcome::kPartial : ScrollOutcome::kSucceeded;
      update->image = std::move(image);
      update->area = RECT{follow_.left + crop.left, area_.top, follow_.left + crop.right, area_.bottom};
      const int top = area_.top + crop.top - anchor_;
      update->image_rect = RECT{update->area.left, top, update->area.right, top + rows};
      update->thumb_width = thumb_width_;
      update->surface_left = follow_.left;
      update->surface_width = surface_width_;
      update->content_top = ThumbCeil(crop.top);
      update->content_bottom = ThumbFloor(crop.bottom);
      update->view_bottom = update->content_top;
    }
  }
  Trace("scroll: finished %s with outcome %d, %d rows grown", early ? "early" : "normally", static_cast<int>(update->outcome), grown);
  Post(std::move(update));
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
