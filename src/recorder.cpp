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

int Luma(uint32_t pixel) { return static_cast<int>((pixel & 0xFF) + ((pixel >> 8) & 0xFF) + ((pixel >> 16) & 0xFF)); }

// Whether two pixels differ by more than a see-through background does as it passes behind them.
bool Apart(uint32_t a, uint32_t b) {
  const int blue = static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF);
  const int green = static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF);
  const int red = static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
  return std::abs(blue) + std::abs(green) + std::abs(red) > config::kScrollColorSlack;
}

// A row's fingerprint over its first `width` pixels: where its color steps, and which way. Text keeps its edges when
// what lies behind it changes shade, as a window's see-through material does while the content moves over it. A row
// with no edges cannot place a match, and gets 0.
uint64_t HashRow(const uint32_t* pixels, int width) {
  uint64_t hash = kFnvBasis;
  bool any = false;
  int last = Luma(pixels[0]);
  for (int x = 1; x < width; ++x) {
    const int luma = Luma(pixels[x]);
    const int step = luma - last;
    last = luma;
    if (step <= config::kScrollEdgeStep && step >= -config::kScrollEdgeStep) continue;
    hash = (hash ^ (static_cast<uint64_t>(x) * 2 + (step > 0 ? 1 : 0))) * kFnvPrime;
    any = true;
  }
  return any ? hash | 1 : 0;
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
  bool solid = false;  // Most rows agree, and not only enough to place a scroll around something that keeps changing.
  bool clean = false;  // Nearly all of them do, so the frame is sharp rather than caught in motion.
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
    int run = 0;
    int longest = 0;
    for (int y = std::max(0, -shift); y < std::min(rows, rows - shift); ++y) {
      const uint64_t was = before[y + shift];
      const uint64_t now = after[y];
      if (was == 0 && now == 0) continue;
      ++compared;
      if (was == now) {
        ++support;
        longest = std::max(longest, ++run);
      } else {
        run = 0;
      }
    }
    const bool solid = support >= config::kScrollMatchMinRows && support >= compared * config::kScrollMatchShare;
    const bool agrees = solid || (longest >= config::kScrollMatchLooseRows && support >= compared * config::kScrollMatchLooseShare);
    const bool better = !result.found || support > result.support ||
                        (support == result.support && std::abs(shift) < std::abs(result.shift));
    if (agrees && better) result = Shift{shift, support, true, solid, support >= compared * config::kScrollMatchCleanShare};
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

Analysis Analyze(const PixelView& before, const PixelView& after, int match_width, double band_share = config::kScrollBandMinShare) {
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
  if (band < std::max(config::kScrollMatchMinRows * 2, static_cast<int>(height * band_share))) {
    result.steady = true;
    return result;
  }
  const Shift shift = FindShift(was.data() + top, now.data() + top, band);
  if (!shift.found) return result;
  if (shift.shift == 0) {
    result.steady = shift.solid;
    return result;
  }
  result.moved = true;
  result.shift = shift.shift;
  result.top = top;
  result.bottom = bottom;
  return result;
}

// What changed between two readings of an area, around the pointer, in the area's own coordinates.
struct Motion {
  bool found = false;
  RECT core{};          // The pixels that changed.
  RECT box{};           // And the plain margins beside them.
  int reach_left = 0;   // Up to whatever stays put on either side.
  int reach_right = 0;
  std::vector<uint8_t> fixed;  // Per column: drawn on, and staying where it is while the rest moves.
};

Motion FindMotion(const PixelView& before, const PixelView& after, int pointer_x, float scale) {
  Motion motion;
  const int width = before.width;
  const int height = before.height;
  std::vector<uint8_t> changed(static_cast<size_t>(width), 0);
  for (int y = 0; y < height; ++y) {
    const uint32_t* was = PixelsOf(before, y);
    const uint32_t* now = PixelsOf(after, y);
    for (int x = 0; x < width; ++x) {
      if (Apart(was[x], now[x])) changed[x] = 1;
    }
  }

  // The nearest change to the pointer.
  const int from = std::clamp(pointer_x, 0, width - 1);
  int seed = -1;
  for (int step = 0; step < width && seed < 0; ++step) {
    if (from - step >= 0 && changed[from - step]) {
      seed = from - step;
    } else if (from + step < width && changed[from + step]) {
      seed = from + step;
    }
  }
  if (seed < 0) return motion;

  // The scroll is read from a strip around it, wider if that says nothing, since whole rows may hold something fixed.
  // A column drawn on that stays where it is instead of following the scroll is fixed. A column of the same icon on
  // every row can look still, but it follows the scroll as well, so it does not count.
  motion.fixed.assign(static_cast<size_t>(width), 0);
  for (const float half : {32.0f, 96.0f, 240.0f}) {
    const int reach = static_cast<int>(std::lround(half * scale));
    const int strip_left = std::max(seed - reach, 0);
    const int strip_width = std::min(seed + reach, width) - strip_left;
    const Analysis strip = Analyze(Crop(before, strip_left, 0, strip_width, height), Crop(after, strip_left, 0, strip_width, height), strip_width, 0.0);
    if (!strip.moved) continue;
    const int rows_from = strip.top;
    const int rows_to = height - strip.bottom;
    std::vector<int> apart(static_cast<size_t>(width), 0);
    std::vector<int> apart_after_shift(static_cast<size_t>(width), 0);
    for (int y = std::max(rows_from, rows_from - strip.shift); y < std::min(rows_to, rows_to - strip.shift); ++y) {
      const uint32_t* still = PixelsOf(before, y);
      const uint32_t* shifted = PixelsOf(before, y + strip.shift);
      const uint32_t* now = PixelsOf(after, y);
      for (int x = 0; x < width; ++x) {
        if (Apart(still[x], now[x])) ++apart[x];
        if (Apart(shifted[x], now[x])) ++apart_after_shift[x];
      }
    }
    const int busy = static_cast<int>(std::lround(config::kScrollFixedColumnRows * scale));
    for (int x = 0; x < width; ++x) motion.fixed[x] = apart_after_shift[x] >= busy && apart[x] * 8 <= apart_after_shift[x];
    break;
  }

  int left = seed;
  int right = seed;
  int reach_left = seed;
  int reach_right = seed;
  while (reach_left > 0 && !motion.fixed[reach_left - 1]) {
    if (changed[--reach_left]) left = reach_left;
  }
  while (reach_right < width - 1 && !motion.fixed[reach_right + 1]) {
    if (changed[++reach_right]) right = reach_right;
  }

  const auto row_changed = [&](int y) {
    const uint32_t* was = PixelsOf(before, y);
    const uint32_t* now = PixelsOf(after, y);
    for (int x = left; x <= right; ++x) {
      if (Apart(was[x], now[x])) return true;
    }
    return false;
  };
  int top = 0;
  while (top < height && !row_changed(top)) ++top;
  int bottom = height - 1;
  while (bottom > top && !row_changed(bottom)) --bottom;
  if (top >= height) return motion;
  motion.core = RECT{left, top, right + 1, bottom + 1};

  // Margins beside what moved belong to it, up to any change of color: a border, or another panel's background.
  const int stride = std::max((bottom - top) / 32, 1);
  const auto margin = [&](int x, int color_x) {
    int same = 0;
    int all = 0;
    for (int y = top; y <= bottom; y += stride, ++all) {
      const uint32_t* now = PixelsOf(after, y);
      if (!Apart(now[x], now[color_x])) ++same;
    }
    return same * 2 >= all;
  };
  if (left > reach_left) {
    const int color_x = left - 1;
    while (left > reach_left && margin(left - 1, color_x)) --left;
  }
  if (right < reach_right) {
    const int color_x = right + 1;
    while (right < reach_right && margin(right + 1, color_x)) ++right;
  }
  const auto plain_row = [&](int y, uint32_t color) {
    const uint32_t* now = PixelsOf(after, y);
    for (int x = left; x <= right; ++x) {
      if (Apart(now[x], color)) return false;
    }
    return true;
  };
  const uint32_t background = PixelsOf(after, (top + bottom) / 2)[left];
  while (top > 0 && plain_row(top - 1, background)) --top;
  while (bottom < height - 1 && plain_row(bottom + 1, background)) ++bottom;

  motion.found = true;
  motion.box = RECT{left, top, right + 1, bottom + 1};
  motion.reach_left = reach_left;
  motion.reach_right = reach_right + 1;
  return motion;
}

// A scroll bar's track lies around its thumb, so plain columns just left of where the thumb was found go with it.
int BarStart(const PixelView& view, int top, int band, int x, int limit) {
  const int first = top + band / 8;
  const int last = top + band - band / 8;
  const auto plain = [&](int column) {
    const uint32_t color = PixelsOf(view, first)[column];
    for (int y = first; y < last; ++y) {
      if (Apart(PixelsOf(view, y)[column], color)) return false;
    }
    return true;
  };
  while (x > limit && plain(x - 1)) --x;
  return x;
}

// The columns to keep at the right edge: a scroll bar's thumb moves on its own as the content scrolls, so the first
// column near the edge that does not follow the content or, in the fixed rows, that changes at all, is where it starts.
int KeptWidth(const PixelView& before, const PixelView& after, int top, int band, int shift, int width, int margin) {
  const int limit = width - margin;
  const auto stays_apart = [&](int x, int from, int to, int offset) {
    for (int y = from; y < to; ++y) {
      if (Apart(PixelsOf(before, y + offset)[x], PixelsOf(after, y)[x])) return true;
    }
    return false;
  };
  for (int x = limit; x < width; ++x) {
    if (stays_apart(x, top + std::max(0, -shift), top + std::min(band, band - shift), shift) || stays_apart(x, 0, top, 0) ||
        stays_apart(x, top + band, before.height, 0)) {
      return BarStart(after, top, band, x, limit);
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
  int Settle(int position) const;
  std::pair<int, int> Stitch(const PixelView& frame, const std::vector<uint64_t>& hashes, int position, bool refresh);
  void WriteHeader();
  bool WriteFooter(const PixelView& frame, int position, bool moved);
  void WatchEdge(const PixelView& frame, int position);
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
  double last_wheel_ = -1e9;
  double mismatch_since_ = 0.0;
  bool done_ = false;

  Grabber probe_;
  Grabber grab_;
  std::vector<uint8_t> last_probe_;
  int probed_ = -1;

  bool locked_ = false;
  RECT grab_rect_{};   // The scrolled area with its scroll bar, in monitor pixels.
  RECT area_{};        // And without it.
  RECT wheel_rect_{};  // Where a wheel may turn without scrolling something else.
  int width_ = 0;
  int kept_width_ = 0;  // Of width_, once a scroll bar has shown itself after the area was chosen.
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
  // Lost for now, it looks harder: content blurred by its own motion lines up only in some frames, and the rows to line
  // up with are moving out of reach.
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
  last_wheel_ = now;
  if (!locked_) return PtInRect(&window_on_monitor_, point) != FALSE;
  const POINT local{point.x - request_.monitor.left, point.y - request_.monitor.top};
  if (PtInRect(&wheel_rect_, local) == FALSE) return false;
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
  // Some apps blur content in motion, so it may only line up again once the wheel has stopped.
  const double limit = now - last_wheel_ < config::kScrollWheelActiveSeconds ? config::kScrollMismatchWheelSeconds : config::kScrollMismatchSeconds;
  if (mismatch_since_ != 0.0 && now - mismatch_since_ >= limit) {
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
  if (areas.empty()) return;
  size_t outer = 0;
  while (outer < areas.size() && PtInRect(&areas[outer], local) == FALSE) ++outer;
  // With the pointer over none of them only the keyboard can be scrolling, most likely the largest.
  const bool over_one = outer < areas.size();
  if (!over_one) outer = 0;
  if (!Probe(areas[outer], static_cast<int>(outer), over_one ? local.x : -1)) {
    CheckMismatch(now);
    return;
  }
  last_change_ = now;
  const RECT outer_rect = areas[outer];
  if (!Grab(outer_rect)) {
    End(true);
    return;
  }

  // What moved says what is being scrolled. Of the areas known to scroll, the smallest that holds it and has nothing
  // fixed beside it is the one. Otherwise it scrolls without saying so, and what moved is all there is to go by.
  const PixelView screenshot = request_.frame->view();
  const PixelView reading = grab_.view();
  const int outer_width = outer_rect.right - outer_rect.left;
  const int outer_height = outer_rect.bottom - outer_rect.top;
  const Motion motion = FindMotion(Crop(screenshot, outer_rect.left, outer_rect.top, outer_width, outer_height), reading,
                                   local.x - outer_rect.left, request_.scale);
  if (!motion.found) {
    mismatch_since_ = 0.0;
    return;
  }
  RECT moved = motion.core;
  OffsetRect(&moved, outer_rect.left, outer_rect.top);
  const auto holds = [](const RECT& a, const RECT& b) { return a.left <= b.left && a.top <= b.top && a.right >= b.right && a.bottom >= b.bottom; };
  RECT chosen = motion.box;
  OffsetRect(&chosen, outer_rect.left, outer_rect.top);
  bool listed = false;
  for (size_t i = areas.size(); i-- > outer && !listed;) {
    const RECT& area = areas[i];
    if (!holds(outer_rect, area) || !holds(area, moved)) continue;
    // The last columns may be a scroll bar, whose thumb stays put until it has moved a pixel.
    const int bar = area.right - std::min<int>(margin_, (area.right - area.left) / 4);
    bool beside = false;
    for (int x = area.left; x < moved.left && !beside; ++x) beside = motion.fixed[x - outer_rect.left] != 0;
    for (int x = moved.right; x < bar && !beside; ++x) beside = motion.fixed[x - outer_rect.left] != 0;
    if (beside) continue;
    chosen = area;
    listed = true;
  }
  const int width = chosen.right - chosen.left;
  const int height = chosen.bottom - chosen.top;
  if (!listed && (width < static_cast<int>(config::kScrollAreaMinWidth * request_.scale) ||
                  height < static_cast<int>(config::kScrollAreaMinHeight * request_.scale))) {
    mismatch_since_ = 0.0;
    return;
  }
  const PixelView before = Crop(screenshot, chosen.left, chosen.top, width, height);
  const PixelView after = Crop(reading, chosen.left - outer_rect.left, chosen.top - outer_rect.top, width, height);
  const Analysis analysis = Analyze(before, after, std::max(width - std::min(margin_, width / 4), 1));
  if (analysis.moved) {
    wheel_rect_ = listed ? chosen : RECT{outer_rect.left + motion.reach_left, outer_rect.top, outer_rect.left + motion.reach_right, outer_rect.bottom};
    if (!Lock(chosen, analysis, before, after)) End(true);
    return;
  }
  // Something changing by itself, such as a video, is no reason to give up. The whole area being replaced is, and so
  // is a scroll that cannot be lined up.
  const bool replaced = motion.box.right - motion.box.left >= outer_width * config::kScrollReplacedShare &&
                        motion.box.bottom - motion.box.top >= outer_height * config::kScrollReplacedShare;
  if (analysis.steady || (!replaced && now - last_wheel_ >= config::kScrollMismatchSeconds)) {
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
  kept_width_ = width_;
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
  Stitch(after, reference_, analysis.shift, false);
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
  Shift shift = FindShift(reference_.data(), current_.data(), band_);
  const auto unplaced = [](const Shift& s) { return !s.found || (s.shift == 0 && !s.solid); };
  if (unplaced(shift)) {
    // The last frame may have been caught in motion, and this one line up only with rows kept from sharper ones.
    const Shift held = FindShift(canvas_hash_.data() + origin_ + top_band_ + position_, current_.data(), band_);
    if (!unplaced(held)) shift = held;
  }
  // Rows that stay put around something changing, such as a video, say nothing about the rows that changed. A wheel
  // turning meanwhile means the content has gone somewhere that cannot be followed.
  if (unplaced(shift)) {
    if (shift.found && now - last_wheel_ >= config::kScrollMismatchSeconds) {
      mismatch_since_ = 0.0;
    } else if (mismatch_since_ == 0.0) {
      mismatch_since_ = now;
    }
    CheckMismatch(now);
    return;
  }
  mismatch_since_ = 0.0;
  const int position = Settle(position_ + shift.shift);
  if (shift.shift != 0) WatchEdge(frame, position);
  const auto [changed_from, changed_to] = Stitch(frame, current_, position, shift.clean || shift.shift == 0);
  position_ = position;
  reference_.swap(current_);
  if (shift.shift != 0) last_scroll_ = now;
  if (changed_from < changed_to || shift.shift != 0) Send(changed_from, changed_to, false);
  if (TotalRows() >= limit_rows_) {
    Trace("scroll: reached the longest a capture may be");
    End(false);
  }
}

// Frames caught in motion can sit between two pixel rows, and rounding them must not add up over a long capture. The
// rows already held say exactly where the frame belongs.
int Session::Settle(int position) const {
  int best = position;
  int best_support = -1;
  for (const int nudge : {0, -1, 1, -2, 2, -3, 3}) {
    int support = 0;
    const int from = std::max(position + nudge, min_top_);
    const int to = std::min(position + nudge + band_, max_bottom_);
    for (int r = from; r < to; ++r) {
      const uint64_t held = canvas_hash_[origin_ + top_band_ + r];
      if (held != 0 && held == current_[r - position - nudge]) ++support;
    }
    if (support > best_support) {
      best_support = support;
      best = position + nudge;
    }
  }
  return best;
}

// Copies what the frame shows of the scrolled content into the capture. Returns the canvas rows that changed.
std::pair<int, int> Session::Stitch(const PixelView& frame, const std::vector<uint64_t>& hashes, int position, bool refresh) {
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
  // Rows already held that look different now, such as images that have finished loading, or rows first seen blurred.
  // A bar stuck to the edge of the view would be stamped onto every row passing under it, so rows near an edge are left
  // alone, unless the view is at that end of what is held and they have no better moment still to come.
  const int edge = band_ / 4;
  const bool at_top = position < position_ && position <= min_top_;
  const bool at_bottom = position + band_ >= max_bottom_;
  const int refresh_to = refresh ? std::min(position + band_, held_bottom) : held_top;
  for (int r = std::max(position, held_top); r < refresh_to; ++r) {
    const int y = r - position;
    if ((y < edge && !at_top) || (y >= band_ - edge && !at_bottom)) continue;
    const int row = origin_ + top_band_ + r;
    if (canvas_hash_[row] == hashes[y]) continue;
    take(r);
    mark(row, row + 1);
  }
  if (WriteFooter(frame, position, moved)) mark(origin_ + top_band_ + max_bottom_, ContentTo());
  return {changed_from, changed_to};
}

void Session::WriteHeader() {
  for (int y = 0; y < top_band_; ++y) CopyRow(canvas_.Row(ContentFrom() + y), header_.view(), y, width_);
}

// A scroll bar whose thumb had not moved yet when the area was chosen shows up later, as rows that match by their
// content and still differ at the edge. The finished image leaves it out.
void Session::WatchEdge(const PixelView& frame, int position) {
  const int grabbed = grab_rect_.right - grab_rect_.left;
  const int limit = std::max(grabbed - std::min(margin_, grabbed / 4), 1);
  if (kept_width_ <= limit) return;
  const auto apart = [&](const uint8_t* held, int y, int x) { return Apart(reinterpret_cast<const uint32_t*>(held)[x], PixelsOf(frame, y)[x]); };
  for (int x = limit; x < kept_width_; ++x) {
    bool found = false;
    for (int r = std::max(position, min_top_); r < std::min(position + band_, max_bottom_) && !found; ++r) {
      const int row = origin_ + top_band_ + r;
      found = canvas_hash_[row] == current_[r - position] && apart(canvas_.Row(row), top_band_ + r - position, x);
    }
    for (int y = 0; y < top_band_ && !found; ++y) {
      found = apart(header_.data() + static_cast<ptrdiff_t>(y) * header_.stride(), y, x);
    }
    if (found) {
      kept_width_ = BarStart(frame, top_band_, band_, x, limit);
      Trace("scroll: a scroll bar showed up, %d columns of %d kept", kept_width_, width_);
      return;
    }
  }
}

// The fixed footer shows as the latest frame at the end of the capture has it, and only there does it follow on from
// the rows above it, in case what looked fixed was only the same blank rows twice.
bool Session::WriteFooter(const PixelView& frame, int position, bool moved) {
  if (bottom_band_ == 0 || position + band_ < max_bottom_) return false;
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
    auto image = std::make_shared<Image>(Image::Allocate(kept_width_, rows));
    if (*image) {
      for (int y = 0; y < rows; ++y) {
        std::memcpy(image->data() + static_cast<ptrdiff_t>(y) * image->stride(), canvas_.Row(ContentFrom() + y), static_cast<size_t>(kept_width_) * 4);
      }
      update->outcome = early ? ScrollOutcome::kPartial : ScrollOutcome::kSucceeded;
      update->image = std::move(image);
      update->area = RECT{area_.left, area_.top, area_.left + kept_width_, area_.bottom};
      update->image_rect = RECT{update->area.left, area_.top + min_top_, update->area.right, area_.top + min_top_ + rows};
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
