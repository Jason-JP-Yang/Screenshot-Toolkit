#include "recorder.h"

#include "config/core.h"
#include "screen.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace sstk {

namespace {

constexpr int kProbeColumns = 3;
constexpr int kProbeWidth = 4;
constexpr uint32_t kColorMask = 0x00FCFCFC;  // Ignores the lowest bits, where rendering noise lives.
constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr size_t kMaxCandidates = 4;

const uint32_t* PixelsOf(const PixelView& view, int y) {
  return reinterpret_cast<const uint32_t*>(view.data + static_cast<ptrdiff_t>(y) * view.stride);
}

uint64_t Fnv(const uint32_t* pixels, int width, uint64_t hash) {
  for (int x = 0; x < width; ++x) hash = (hash ^ (pixels[x] & kColorMask)) * kFnvPrime;
  return hash;
}

// A row's fingerprint over its first `width` pixels, or 0 when they are all one color and so cannot place a match.
uint64_t HashRow(const uint32_t* pixels, int width) {
  const uint32_t first = pixels[0] & kColorMask;
  bool varied = false;
  for (int x = 1; x < width && !varied; ++x) varied = (pixels[x] & kColorMask) != first;
  return varied ? Fnv(pixels, width, kFnvBasis) | 1 : 0;
}

void HashRows(const PixelView& view, int first, int count, int width, std::vector<uint64_t>& out) {
  out.resize(static_cast<size_t>(count));
  for (int y = 0; y < count; ++y) out[y] = HashRow(PixelsOf(view, first + y), width);
}

PixelView Crop(const PixelView& view, int left, int top, int width, int height) {
  return PixelView{view.data + static_cast<ptrdiff_t>(top) * view.stride + static_cast<ptrdiff_t>(left) * 4, width, height, view.stride};
}

void CopyRow(uint8_t* to, const PixelView& from, int y, int width) {
  std::memcpy(to, from.data + static_cast<ptrdiff_t>(y) * from.stride, static_cast<size_t>(width) * 4);
}

struct Shift {
  int shift = 0;  // Row y of the later frame shows what row y + shift of the earlier one did.
  int support = 0;
  bool found = false;
};

// Rows vote for every shift that would line them up with an equal row, and the leading shifts are then checked row
// by row, so that repeated rows cannot outvote the true one. Rows of one color carry no vote.
Shift FindShift(const uint64_t* before, const uint64_t* after, int rows) {
  Shift result;
  if (rows <= 0) return result;
  std::vector<std::pair<uint64_t, int>> sorted;
  sorted.reserve(static_cast<size_t>(rows));
  for (int y = 0; y < rows; ++y) {
    if (before[y] != 0) sorted.emplace_back(before[y], y);
  }
  if (sorted.empty()) return result;
  std::sort(sorted.begin(), sorted.end());

  std::vector<int> votes(static_cast<size_t>(rows) * 2, 0);
  for (int y = 0; y < rows; ++y) {
    if (after[y] == 0) continue;
    const auto first = std::lower_bound(sorted.begin(), sorted.end(), std::pair<uint64_t, int>(after[y], INT_MIN));
    auto last = first;
    while (last != sorted.end() && last->first == after[y]) ++last;
    if (last == first || last - first > config::kScrollRepeatLimit) continue;
    for (auto it = first; it != last; ++it) ++votes[static_cast<size_t>(it->second - y + rows)];
  }
  const int best_votes = *std::max_element(votes.begin(), votes.end());
  if (best_votes == 0) return result;

  std::vector<int> candidates;
  for (int i = 0; i < rows * 2; ++i) {
    if (votes[i] * 2 >= best_votes) candidates.push_back(i - rows);
  }
  std::sort(candidates.begin(), candidates.end(), [&](int a, int b) { return votes[a + rows] > votes[b + rows]; });
  if (candidates.size() > kMaxCandidates) candidates.resize(kMaxCandidates);

  for (const int shift : candidates) {
    int support = 0;
    int compared = 0;
    for (int y = std::max(0, -shift); y < std::min(rows, rows - shift); ++y) {
      const uint64_t was = before[y + shift];
      const uint64_t now = after[y];
      if (was == 0 && now == 0) continue;
      ++compared;
      if (was == now) ++support;
    }
    const bool agrees = support >= config::kScrollMatchMinRows && support >= compared * config::kScrollMatchShare;
    const bool better = !result.found || support > result.support ||
                        (support == result.support && std::abs(shift) < std::abs(result.shift));
    if (agrees && better) result = Shift{shift, support, true};
  }
  return result;
}

// How two readings of an area differ: by a scroll, by nothing much, or by something that cannot be stitched.
struct Analysis {
  bool moved = false;
  bool steady = false;
  int shift = 0;
  int top = 0;     // Rows at the top that stayed put, such as a fixed header.
  int bottom = 0;  // And at the bottom.
};

Analysis Analyze(const PixelView& before, const PixelView& after, int match_width) {
  Analysis result;
  const int height = before.height;
  std::vector<uint64_t> was;
  std::vector<uint64_t> now;
  HashRows(before, 0, height, match_width, was);
  HashRows(after, 0, height, match_width, now);
  int top = 0;
  while (top < height && was[top] == now[top]) ++top;
  int bottom = 0;
  while (bottom < height - top && was[height - 1 - bottom] == now[height - 1 - bottom]) ++bottom;
  const int band = height - top - bottom;
  if (band < std::max(config::kScrollMatchMinRows * 2, static_cast<int>(height * config::kScrollBandMinShare))) {
    result.steady = true;
    return result;
  }
  const Shift shift = FindShift(was.data() + top, now.data() + top, band);
  if (!shift.found) return result;
  if (shift.shift == 0) {
    result.steady = true;
    return result;
  }
  result.moved = true;
  result.shift = shift.shift;
  result.top = top;
  result.bottom = bottom;
  return result;
}

// The columns to keep at the right edge: a scroll bar's thumb moves on its own as the content scrolls, so the first
// column near the edge that does not follow the content, and everything right of it, is left out.
int KeptWidth(const PixelView& before, const PixelView& after, int top, int band, int shift, int width, int margin) {
  for (int x = width - margin; x < width; ++x) {
    for (int y = std::max(0, -shift); y < std::min(band, band - shift); ++y) {
      if (((PixelsOf(before, top + y + shift)[x] ^ PixelsOf(after, top + y)[x]) & kColorMask) != 0) return x;
    }
  }
  return width;
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

  // Copies a block of the screen to a column of the bitmap. Without CAPTUREBLT, so the pointer never flickers.
  bool Read(HDC screen, int to_x, int x, int y, int width, int height) {
    return BitBlt(dc_, to_x, 0, width, height, screen, x, y, SRCCOPY) != FALSE;
  }

  PixelView view() const { return PixelView{bits_, width_, height_, width_ * 4}; }

 private:
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

  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ previous_ = nullptr;
  uint8_t* bits_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// The rows of the capture, growing either way from an origin, in address space reserved up front.
class Canvas {
 public:
  Canvas() = default;
  ~Canvas() {
    if (base_ != nullptr) VirtualFree(base_, 0, MEM_RELEASE);
  }
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  bool Reserve(int width, int rows) {
    width_ = width;
    stride_ = static_cast<size_t>(width) * 4;
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, stride_ * rows, MEM_RESERVE, PAGE_READWRITE));
    return base_ != nullptr;
  }

  // Makes rows [first, last) writable.
  bool Commit(int first, int last) {
    if (first >= last) return true;
    if (committed_first_ == committed_last_) {
      if (!CommitSpan(first, last)) return false;
      committed_first_ = first;
      committed_last_ = last;
      return true;
    }
    if (first < committed_first_) {
      if (!CommitSpan(first, committed_first_)) return false;
      committed_first_ = first;
    }
    if (last > committed_last_) {
      if (!CommitSpan(committed_last_, last)) return false;
      committed_last_ = last;
    }
    return true;
  }

  uint8_t* Row(int row) const { return base_ + stride_ * row; }
  PixelView View(int first, int last) const { return PixelView{Row(first), width_, last - first, static_cast<int>(stride_)}; }

 private:
  bool CommitSpan(int first, int last) { return VirtualAlloc(Row(first), stride_ * (last - first), MEM_COMMIT, PAGE_READWRITE) != nullptr; }

  uint8_t* base_ = nullptr;
  size_t stride_ = 0;
  int width_ = 0;
  int committed_first_ = 0;
  int committed_last_ = 0;
};

constexpr int kLockedProbe = -2;

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
  void StepWaiting(double now);
  void StepLocked(double now);
  bool WheelFits(POINT point, double now);
  bool WindowStays() const;
  bool Probe(const RECT& rect, int id, int focus_x);
  bool Grab(const RECT& rect);
  bool Lock(const RECT& rect, const Analysis& analysis, const PixelView& before, const PixelView& after);
  std::pair<int, int> Stitch(const PixelView& frame, const std::vector<uint64_t>& hashes, int position);
  void WriteHeader();
  bool WriteFooter(const PixelView& frame, bool moved);
  void CheckMismatch(double now);
  void Send(int changed_from, int changed_to, bool locked);
  void End(bool early);
  void Post(std::unique_ptr<ScrollUpdate> update) const;

  int ContentFrom() const { return origin_ + min_top_; }
  int ContentTo() const { return origin_ + top_band_ + max_bottom_ + bottom_band_; }
  int TotalRows() const { return ContentTo() - ContentFrom(); }
  int ThumbFrom() const;
  int ThumbTo() const;
  int ViewBottom() const;
  Image ThumbRows(int changed_from, int changed_to, int* at) const;

  ScrollRequest request_;
  HDC screen_ = nullptr;
  DWORD own_process_ = 0;
  double started_ = 0.0;
  RECT window_rect_{};
  RECT window_on_monitor_{};
  int margin_ = 0;  // The widest a scroll bar may be.
  double last_change_ = 0.0;
  double last_scroll_ = 0.0;
  double mismatch_since_ = 0.0;
  bool done_ = false;

  Grabber probe_;
  Grabber grab_;
  std::vector<uint8_t> last_probe_;
  int probed_ = -1;

  bool locked_ = false;
  RECT grab_rect_{};  // The scrolled area with its scroll bar, in monitor pixels.
  RECT area_{};       // And without it.
  int width_ = 0;
  int height_ = 0;
  int top_band_ = 0;  // The fixed rows above and below the moving band.
  int bottom_band_ = 0;
  int band_ = 0;
  int match_width_ = 0;
  int thumb_width_ = 0;
  double k_ = 1.0;  // Thumbnail pixels per screen pixel.
  int limit_rows_ = 0;
  int origin_ = 0;  // The canvas row of the area's first row as the screenshot showed it.
  int min_top_ = 0;  // The content held, in rows counted from where the screenshot's moving band began.
  int max_bottom_ = 0;
  int position_ = 0;  // The content row at the top of the latest matched frame's band.
  std::vector<uint64_t> reference_;
  std::vector<uint64_t> current_;
  std::vector<uint64_t> canvas_hash_;
  Image header_;
  uint64_t footer_hash_ = 0;
  Canvas canvas_;
};

Session::Session(ScrollRequest request)
    : request_(std::move(request)), screen_(GetDC(nullptr)), own_process_(GetCurrentProcessId()), started_(NowSeconds()) {
  last_change_ = started_;
  GetWindowRect(request_.window, &window_rect_);
  IntersectRect(&window_on_monitor_, &window_rect_, &request_.monitor);
  const UINT dpi = static_cast<UINT>(std::lround(USER_DEFAULT_SCREEN_DPI * request_.scale));
  margin_ = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) + static_cast<int>(std::lround(2.0f * request_.scale));
}

Session::~Session() {
  if (screen_ != nullptr) ReleaseDC(nullptr, screen_);
}

double Session::Interval() const {
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
      Trace("scroll: a wheel turned outside the scrolled area");
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
  if (locked_) {
    StepLocked(now);
  } else {
    StepWaiting(now);
  }
}

bool Session::WheelFits(POINT point, double now) {
  // Over this program's own windows the wheel reached nothing.
  DWORD process = 0;
  if (HWND under = WindowFromPoint(point); under != nullptr) GetWindowThreadProcessId(under, &process);
  if (process == own_process_) return true;
  last_change_ = now;
  if (!locked_) return PtInRect(&window_on_monitor_, point) != FALSE;
  const POINT local{point.x - request_.monitor.left, point.y - request_.monitor.top};
  if (PtInRect(&grab_rect_, local) == FALSE) return false;
  last_scroll_ = now;
  return true;
}

bool Session::WindowStays() const {
  RECT rect;
  return IsWindow(request_.window) && !IsIconic(request_.window) && GetWindowRect(request_.window, &rect) &&
         EqualRect(&rect, &window_rect_);
}

// Reads a few thin columns of an area, and says whether they differ from the last reading of the same area.
bool Session::Probe(const RECT& rect, int id, int focus_x) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (!probe_.Ensure(screen_, kProbeColumns * kProbeWidth, height)) return true;
  const int usable = std::max(width - std::min(margin_, width / 4) - kProbeWidth, 0);
  for (int i = 0; i < kProbeColumns; ++i) {
    int x = usable * (i + 1) / (kProbeColumns + 1);
    // Before an area is chosen, one column follows the pointer, which is where a scroll lands.
    if (i == 1 && focus_x >= 0) x = std::clamp(focus_x - static_cast<int>(rect.left) - kProbeWidth / 2, 0, width - kProbeWidth);
    probe_.Read(screen_, i * kProbeWidth, request_.monitor.left + rect.left + x, request_.monitor.top + rect.top, kProbeWidth, height);
  }
  GdiFlush();
  const PixelView view = probe_.view();
  const size_t bytes = static_cast<size_t>(view.stride) * view.height;
  const bool same = id == probed_ && last_probe_.size() == bytes && std::memcmp(last_probe_.data(), view.data, bytes) == 0;
  last_probe_.assign(view.data, view.data + bytes);
  probed_ = id;
  return !same;
}

bool Session::Grab(const RECT& rect) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (!grab_.Ensure(screen_, width, height) ||
      !grab_.Read(screen_, 0, request_.monitor.left + rect.left, request_.monitor.top + rect.top, width, height)) {
    return false;
  }
  GdiFlush();
  return true;
}

void Session::CheckMismatch(double now) {
  if (mismatch_since_ != 0.0 && now - mismatch_since_ >= config::kScrollMismatchSeconds) {
    Trace("scroll: the content stopped matching");
    End(true);
  }
}

void Session::StepWaiting(double now) {
  if (now - started_ >= config::kScrollStartSeconds || !request_.frame) {
    End(false);
    return;
  }
  POINT cursor{};
  GetCursorPos(&cursor);
  const POINT local{cursor.x - request_.monitor.left, cursor.y - request_.monitor.top};
  const std::vector<RECT>& areas = request_.areas;
  size_t outer = 0;
  while (outer < areas.size() && PtInRect(&areas[outer], local) == FALSE) ++outer;
  if (outer == areas.size() || !Probe(areas[outer], static_cast<int>(outer), local.x)) {
    CheckMismatch(now);
    return;
  }
  last_change_ = now;
  const RECT outer_rect = areas[outer];
  if (!Grab(outer_rect)) {
    End(true);
    return;
  }

  // From the outside in: a scroll carries everything inside the area that scrolled, so the largest area whose
  // content moved as a whole is the one being scrolled.
  const PixelView screenshot = request_.frame->view();
  const PixelView reading = grab_.view();
  bool steady = false;
  for (size_t i = outer; i < areas.size(); ++i) {
    const RECT area = areas[i];
    RECT inside;
    if (PtInRect(&area, local) == FALSE || !IntersectRect(&inside, &area, &outer_rect) || !EqualRect(&inside, &area)) continue;
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    const PixelView before = Crop(screenshot, area.left, area.top, width, height);
    const PixelView after = Crop(reading, area.left - outer_rect.left, area.top - outer_rect.top, width, height);
    const Analysis analysis = Analyze(before, after, std::max(width - std::min(margin_, width / 4), 1));
    if (analysis.moved) {
      if (!Lock(area, analysis, before, after)) End(true);
      return;
    }
    steady = steady || analysis.steady;
  }
  if (steady) {
    mismatch_since_ = 0.0;
  } else if (mismatch_since_ == 0.0) {
    mismatch_since_ = now;
  }
  CheckMismatch(now);
}

bool Session::Lock(const RECT& rect, const Analysis& analysis, const PixelView& before, const PixelView& after) {
  const int width = rect.right - rect.left;
  const int margin = std::min(margin_, width / 4);
  grab_rect_ = rect;
  height_ = rect.bottom - rect.top;
  top_band_ = analysis.top;
  bottom_band_ = analysis.bottom;
  band_ = height_ - top_band_ - bottom_band_;
  width_ = KeptWidth(before, after, top_band_, band_, analysis.shift, width, margin);
  area_ = RECT{rect.left, rect.top, rect.left + width_, rect.bottom};
  match_width_ = std::min(width_, width - margin);
  thumb_width_ = ThumbnailSize(RECT{0, 0, width_, height_}, request_.scale).cx;
  k_ = static_cast<double>(thumb_width_) / width_;
  limit_rows_ = std::max(std::min(config::kScrollMaxHeight, config::kScrollMaxPixels / width_), height_);
  origin_ = limit_rows_;
  const int capacity = origin_ + limit_rows_ + height_;
  if (!canvas_.Reserve(width_, capacity) || !canvas_.Commit(origin_, origin_ + height_)) return false;
  canvas_hash_.assign(static_cast<size_t>(capacity), 0);

  // The capture starts as the screenshot showed the area.
  for (int y = 0; y < height_; ++y) CopyRow(canvas_.Row(origin_ + y), before, y, width_);
  for (int r = 0; r < band_; ++r) {
    const int row = origin_ + top_band_ + r;
    canvas_hash_[row] = HashRow(reinterpret_cast<const uint32_t*>(canvas_.Row(row)), match_width_);
  }
  if (top_band_ > 0) {
    header_ = Image::Allocate(width_, top_band_);
    if (!header_) return false;
    for (int y = 0; y < top_band_; ++y) CopyRow(header_.data() + static_cast<ptrdiff_t>(y) * header_.stride(), before, y, width_);
  }
  request_.frame.reset();
  min_top_ = 0;
  max_bottom_ = band_;
  locked_ = true;
  last_scroll_ = NowSeconds();
  Trace("scroll: following %ldx%ld at %ld,%ld, fixed rows %d above and %d below, %d columns of %d kept", rect.right - rect.left,
        rect.bottom - rect.top, rect.left, rect.top, top_band_, bottom_band_, width_, width);

  HashRows(after, top_band_, band_, match_width_, reference_);
  Stitch(after, reference_, analysis.shift);
  position_ = analysis.shift;
  Send(ContentFrom(), ContentTo(), true);
  return true;
}

void Session::StepLocked(double now) {
  if (now - last_scroll_ >= config::kScrollIdleSeconds) {
    End(false);
    return;
  }
  if (!Probe(area_, kLockedProbe, -1)) {
    CheckMismatch(now);
    return;
  }
  last_change_ = now;
  if (!Grab(grab_rect_)) {
    End(true);
    return;
  }
  const PixelView frame = grab_.view();
  HashRows(frame, top_band_, band_, match_width_, current_);
  const Shift shift = FindShift(reference_.data(), current_.data(), band_);
  if (!shift.found) {
    if (mismatch_since_ == 0.0) mismatch_since_ = now;
    CheckMismatch(now);
    return;
  }
  mismatch_since_ = 0.0;
  const int position = position_ + shift.shift;
  const auto [changed_from, changed_to] = Stitch(frame, current_, position);
  position_ = position;
  reference_.swap(current_);
  if (shift.shift != 0) last_scroll_ = now;
  if (changed_from < changed_to || shift.shift != 0) Send(changed_from, changed_to, false);
  if (TotalRows() >= limit_rows_) {
    Trace("scroll: reached the longest a capture may be");
    End(false);
  }
}

// Copies what the frame shows of the scrolled content into the capture. Returns the canvas rows that changed.
std::pair<int, int> Session::Stitch(const PixelView& frame, const std::vector<uint64_t>& hashes, int position) {
  int changed_from = INT_MAX;
  int changed_to = INT_MIN;
  const auto mark = [&](int from, int to) {
    changed_from = std::min(changed_from, from);
    changed_to = std::max(changed_to, to);
  };
  const auto take = [&](int content_row) {
    const int row = origin_ + top_band_ + content_row;
    CopyRow(canvas_.Row(row), frame, top_band_ + content_row - position, width_);
    canvas_hash_[row] = hashes[content_row - position];
  };

  const int held_top = min_top_;
  const int held_bottom = max_bottom_;
  int room = limit_rows_ - TotalRows();
  // Above what is held, with the fixed header moving up to make way.
  const int top = std::max(position, held_top - room);
  if (top < held_top && canvas_.Commit(origin_ + top, origin_ + top_band_ + held_top)) {
    for (int r = top; r < held_top; ++r) take(r);
    room -= held_top - top;
    min_top_ = top;
    WriteHeader();
    mark(ContentFrom(), origin_ + top_band_ + held_top);
  }
  // Below it, with the fixed footer moving down.
  const int bottom = std::min(position + band_, held_bottom + room);
  bool moved = false;
  if (bottom > held_bottom && canvas_.Commit(origin_ + top_band_ + held_bottom, origin_ + top_band_ + bottom + bottom_band_)) {
    for (int r = held_bottom; r < bottom; ++r) take(r);
    max_bottom_ = bottom;
    moved = true;
    mark(origin_ + top_band_ + held_bottom, ContentTo());
  }
  // Rows already held that look different now, such as images that have finished loading.
  for (int r = std::max(position, held_top); r < std::min(position + band_, held_bottom); ++r) {
    const int row = origin_ + top_band_ + r;
    if (canvas_hash_[row] == hashes[r - position]) continue;
    take(r);
    mark(row, row + 1);
  }
  if (WriteFooter(frame, moved)) mark(origin_ + top_band_ + max_bottom_, ContentTo());
  return {changed_from, changed_to};
}

void Session::WriteHeader() {
  for (int y = 0; y < top_band_; ++y) CopyRow(canvas_.Row(ContentFrom() + y), header_.view(), y, width_);
}

// The fixed footer always shows as the latest frame has it.
bool Session::WriteFooter(const PixelView& frame, bool moved) {
  if (bottom_band_ == 0) return false;
  uint64_t hash = kFnvBasis;
  for (int y = height_ - bottom_band_; y < height_; ++y) hash = Fnv(PixelsOf(frame, y), width_, hash);
  if (!moved && hash == footer_hash_) return false;
  footer_hash_ = hash;
  const int at = origin_ + top_band_ + max_bottom_;
  for (int y = 0; y < bottom_band_; ++y) CopyRow(canvas_.Row(at + y), frame, height_ - bottom_band_ + y, width_);
  return true;
}

// Thumbnail rows count only where they lie wholly inside the capture.
int Session::ThumbFrom() const { return kLongOrigin + static_cast<int>(std::ceil((ContentFrom() - origin_) * k_ - 1e-6)); }

int Session::ThumbTo() const { return kLongOrigin + static_cast<int>(std::floor((ContentTo() - origin_) * k_ + 1e-6)); }

int Session::ViewBottom() const {
  const int row = std::min(origin_ + top_band_ + position_ + band_ + bottom_band_, ContentTo());
  return std::clamp(kLongOrigin + static_cast<int>(std::floor((row - origin_) * k_ + 1e-6)), ThumbFrom(), ThumbTo());
}

Image Session::ThumbRows(int changed_from, int changed_to, int* at) const {
  const int first = std::max(ThumbFrom(), kLongOrigin + static_cast<int>(std::floor((changed_from - origin_) * k_)));
  const int last = std::min(ThumbTo(), kLongOrigin + static_cast<int>(std::ceil((changed_to - origin_) * k_)));
  *at = first;
  if (last <= first) return Image();
  const int from = ContentFrom();
  const double top = (first - kLongOrigin) / k_ + origin_ - from;
  const double bottom = (last - kLongOrigin) / k_ + origin_ - from;
  return ResampleArea(canvas_.View(from, ContentTo()), 0.0, top, width_, bottom, thumb_width_, last - first);
}

void Session::Post(std::unique_ptr<ScrollUpdate> update) const {
  if (PostMessageW(request_.controller, kMsgScrollUpdate, 0, reinterpret_cast<LPARAM>(update.get()))) update.release();
}

void Session::Send(int changed_from, int changed_to, bool locked) {
  auto update = std::make_unique<ScrollUpdate>();
  update->session = request_.session;
  update->locked = locked;
  update->area = area_;
  update->thumb_width = thumb_width_;
  if (changed_from < changed_to) update->rows = ThumbRows(changed_from, changed_to, &update->at);
  update->content_top = ThumbFrom();
  update->content_bottom = ThumbTo();
  update->view_bottom = ViewBottom();
  Post(std::move(update));
}

void Session::End(bool early) {
  done_ = true;
  auto update = std::make_unique<ScrollUpdate>();
  update->session = request_.session;
  update->finished = true;
  const int grown = locked_ ? TotalRows() - height_ : 0;
  // An early end still keeps a capture that has grown far enough to be worth having.
  if (grown > 0 && (!early || grown >= height_ * config::kScrollPartialShare)) {
    const int rows = TotalRows();
    auto image = std::make_shared<Image>(Image::Allocate(width_, rows));
    if (*image) {
      for (int y = 0; y < rows; ++y) {
        std::memcpy(image->data() + static_cast<ptrdiff_t>(y) * image->stride(), canvas_.Row(ContentFrom() + y), static_cast<size_t>(width_) * 4);
      }
      update->outcome = early ? ScrollOutcome::kPartial : ScrollOutcome::kSucceeded;
      update->image = std::move(image);
      update->image_rect = RECT{area_.left, area_.top + min_top_, area_.right, area_.top + min_top_ + rows};
      update->area = area_;
      update->thumb_width = thumb_width_;
      update->content_top = ThumbFrom();
      update->content_bottom = ThumbTo();
      update->view_bottom = ViewBottom();
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
