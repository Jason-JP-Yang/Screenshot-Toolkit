#include "overlay.h"

#include "capsule.h"
#include "card.h"
#include "dragdrop.h"
#include "theme.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr wchar_t kStageClass[] = L"ScreenshotToolkit.Stage";
constexpr wchar_t kSinkClass[] = L"ScreenshotToolkit.Sink";
constexpr UINT kTickIntervalMs = 40;
constexpr int kTaskbarPollTicks = 3;

}  // namespace

// The overlay of one monitor. Two windows work together: a full-monitor, click-through stage that
// holds all visuals, and a small input sink that covers only the interactive shapes.
class Deck {
 public:
  Deck(Compositor& compositor, Store& store, HMONITOR monitor, const RECT& bounds);
  ~Deck();
  Deck(const Deck&) = delete;
  Deck& operator=(const Deck&) = delete;

  HMONITOR monitor() const { return monitor_; }
  bool ready() const { return window_ != nullptr; }

  void Flash();
  void AddShot(std::unique_ptr<CaptureResult> capture);
  void Tick();
  bool IsIdle() const;
  void Rest();

  LRESULT HandleSinkMessage(UINT message, WPARAM wparam, LPARAM lparam);

 private:
  enum class Press { kNone, kSegment, kCard, kSwipe };

  float Dip(float value) const { return value * scale_; }
  RectF RectForRegion(const RECT& region) const;
  RectF RectBehind(const RectF& front, int depth) const;
  void Layout(const SpringSpec& front_spec, const TimeBase& now, double delay = 0.0, bool in_flight = false);
  void UpdateSink();
  void Dismiss(size_t index, const TimeBase& now, float velocity);
  void SelectRegion(int index);
  void BindCapsuleToFront(const TimeBase& now);
  POINT StagePoint(LPARAM lparam) const;
  Card* front() const { return cards_.empty() ? nullptr : cards_.back().get(); }

  void OnMouseMove(POINT point);
  void OnButtonDown(POINT point);
  void OnButtonUp(POINT point);
  void OnMouseLeave();
  void BeginFileDrag();
  void OpenFront();

  Compositor& compositor_;
  Store& store_;
  HMONITOR monitor_;
  RECT bounds_;
  SIZE size_;
  float scale_ = 1.0f;

  HWND window_ = nullptr;
  HWND sink_ = nullptr;
  POINT sink_origin_{};
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionVisual> cards_layer_;
  ComPtr<IDCompositionVisual> capsule_layer_;
  ComPtr<IDCompositionVisual> flash_;
  ComPtr<IDCompositionEffectGroup> flash_effect_;
  AnimFloat flash_alpha_;
  std::unique_ptr<Capsule> capsule_;

  std::vector<std::unique_ptr<Card>> cards_;  // Oldest first; the back is the front of the deck.
  std::vector<std::unique_ptr<Card>> exiting_;

  TaskbarInsets insets_;
  RectF front_rect_{};
  bool shown_ = false;
  int tick_count_ = 0;

  bool hovering_ = false;
  bool tracking_leave_ = false;
  Press press_ = Press::kNone;
  int press_segment_ = -1;
  POINT press_point_{};
  float swipe_velocity_ = 0.0f;
  float swipe_offset_ = 0.0f;
  double swipe_time_ = 0.0;
};

namespace {

LRESULT CALLBACK SinkProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* deck = reinterpret_cast<Deck*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (deck != nullptr) return deck->HandleSinkMessage(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

void RegisterClasses() {
  static bool registered = false;
  if (registered) return;
  registered = true;

  WNDCLASSW stage{};
  stage.lpfnWndProc = DefWindowProcW;
  stage.hInstance = GetModuleHandleW(nullptr);
  stage.lpszClassName = kStageClass;
  RegisterClassW(&stage);

  WNDCLASSW sink{};
  sink.lpfnWndProc = SinkProc;
  sink.hInstance = GetModuleHandleW(nullptr);
  sink.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  sink.lpszClassName = kSinkClass;
  RegisterClassW(&sink);
}

void ExcludeFromCapture(HWND window) {
  if (!OverlayVisibleToCapture()) SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
}

}  // namespace

Deck::Deck(Compositor& compositor, Store& store, HMONITOR monitor, const RECT& bounds)
    : compositor_(compositor), store_(store), monitor_(monitor), bounds_(bounds),
      size_{bounds.right - bounds.left, bounds.bottom - bounds.top} {
  RegisterClasses();
  const HINSTANCE instance = GetModuleHandleW(nullptr);

  window_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED | WS_EX_TRANSPARENT,
                            kStageClass, L"", WS_POPUP, bounds.left, bounds.top, size_.cx, size_.cy, nullptr, nullptr,
                            instance, nullptr);
  if (window_ == nullptr) return;
  SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
  ExcludeFromCapture(window_);
  scale_ = static_cast<float>(GetDpiForWindow(window_)) / USER_DEFAULT_SCREEN_DPI;

  sink_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kSinkClass,
                          L"", WS_POPUP, bounds.left, bounds.top, 1, 1, nullptr, nullptr, instance, nullptr);
  SetWindowLongPtrW(sink_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ExcludeFromCapture(sink_);

  compositor_.device()->CreateTargetForHwnd(window_, TRUE, &target_);
  root_ = compositor_.CreateVisual();
  cards_layer_ = compositor_.CreateVisual();
  capsule_layer_ = compositor_.CreateVisual();
  // The capsule sits under the cards, so the flying screenshot uncovers it instead of crossing over it.
  root_->AddVisual(capsule_layer_.Get(), FALSE, nullptr);
  root_->AddVisual(cards_layer_.Get(), FALSE, nullptr);

  flash_ = compositor_.CreateSolidVisual(0xFFFFFFFF, static_cast<float>(size_.cx), static_cast<float>(size_.cy));
  flash_effect_ = compositor_.CreateEffectGroup();
  flash_->SetEffect(flash_effect_.Get());
  flash_alpha_.Bind(flash_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  flash_alpha_.Snap(0.0f);
  root_->AddVisual(flash_.Get(), FALSE, nullptr);
  target_->SetRoot(root_.Get());

  capsule_ = std::make_unique<Capsule>(compositor_, capsule_layer_.Get(), scale_, size_);
}

Deck::~Deck() {
  cards_.clear();
  exiting_.clear();
  capsule_.reset();
  if (target_) target_->SetRoot(nullptr);
  target_.Reset();
  if (sink_ != nullptr) DestroyWindow(sink_);
  if (window_ != nullptr) DestroyWindow(window_);
}

void Deck::Flash() {
  const TimeBase now = compositor_.Now();
  flash_alpha_.Snap(theme::kFlashPeak);
  flash_alpha_.To(compositor_.device(), now, 0.0f, theme::kFlash);
  compositor_.Commit();

  // Shown after the commit so that the first composed frame already contains the flash.
  SetWindowPos(window_, HWND_TOPMOST, bounds_.left, bounds_.top, size_.cx, size_.cy,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  shown_ = true;
}

RectF Deck::RectForRegion(const RECT& region) const {
  const float region_w = static_cast<float>(region.right - region.left);
  const float region_h = static_cast<float>(region.bottom - region.top);
  const float fit = std::min({1.0f, Dip(theme::kCardMaxWidth) / region_w, Dip(theme::kCardMaxHeight) / region_h});
  const float width = std::max(std::round(region_w * fit), 1.0f);
  const float height = std::max(std::round(region_h * fit), 1.0f);
  const float left = std::round(static_cast<float>(insets_.left) + Dip(theme::kEdgeMargin));
  const float bottom = std::round(static_cast<float>(size_.cy - insets_.bottom) - Dip(theme::kEdgeMargin));
  return RectF{left, bottom - height, left + width, bottom};
}

RectF Deck::RectBehind(const RectF& front, int depth) const {
  const float shrink = theme::kDeckShrinkPerLevel * std::min(depth, theme::kDeckVisibleBehind + 1);
  const float width = front.width() * (1.0f - shrink);
  const float height = front.height() * (1.0f - shrink);
  const float left = front.left + (front.width() - width) * 0.5f;
  const float top = front.top - Dip(theme::kDeckPeek) * std::min(depth, theme::kDeckVisibleBehind + 1);
  return RectF{left, top, left + width, top + height};
}

void Deck::Layout(const SpringSpec& front_spec, const TimeBase& now, double delay, bool in_flight) {
  Card* top = front();
  if (top == nullptr) return;

  front_rect_ = RectForRegion(top->selected_rect());
  const int count = static_cast<int>(cards_.size());
  for (int i = count - 1; i >= 0; --i) {
    const int depth = count - 1 - i;
    if (depth == 0) {
      cards_[i]->MoveTo(front_rect_, 0, front_spec, now, delay, in_flight);
    } else {
      cards_[i]->MoveTo(RectBehind(front_rect_, depth), depth, theme::kReflow, now, 0.0, false);
    }
  }

  // The capsule is anchored to the tallest possible card, not the current one, so it does not
  // move under the pointer when the selected region changes the thumbnail's shape.
  const float box_top = static_cast<float>(size_.cy - insets_.bottom) - Dip(theme::kEdgeMargin) - Dip(theme::kCardMaxHeight);
  const int peeking = std::min(count - 1, theme::kDeckVisibleBehind);
  const float capsule_y = std::round(box_top - Dip(theme::kDeckPeek) * peeking - Dip(theme::kCapsuleGap) - capsule_->height());
  capsule_->MoveTo(front_rect_.left, capsule_y, now);
  UpdateSink();
}

void Deck::UpdateSink() {
  Card* top = front();
  if (top == nullptr) {
    ShowWindow(sink_, SW_HIDE);
    return;
  }

  RECT card{static_cast<LONG>(front_rect_.left), static_cast<LONG>(front_rect_.top),
            static_cast<LONG>(front_rect_.right), static_cast<LONG>(front_rect_.bottom)};
  RECT bounding = card;
  HRGN shape = CreateRectRgnIndirect(&card);

  if (capsule_->visible()) {
    const RECT capsule = capsule_->Bounds();
    // The gap between the capsule and the card stays interactive, so crossing it keeps hover alive.
    RECT bridge{std::max(card.left, capsule.left), capsule.bottom, std::min(card.right, capsule.right), card.top};
    HRGN extra = CreateRectRgnIndirect(&capsule);
    CombineRgn(shape, shape, extra, RGN_OR);
    SetRectRgn(extra, bridge.left, bridge.top, bridge.right, bridge.bottom);
    CombineRgn(shape, shape, extra, RGN_OR);
    DeleteObject(extra);
    UnionRect(&bounding, &card, &capsule);
  }

  sink_origin_ = POINT{bounding.left, bounding.top};
  OffsetRgn(shape, -bounding.left, -bounding.top);
  SetWindowPos(sink_, HWND_TOPMOST, bounds_.left + bounding.left, bounds_.top + bounding.top,
               bounding.right - bounding.left, bounding.bottom - bounding.top,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  SetWindowRgn(sink_, shape, FALSE);  // The window takes ownership of the region.
}

void Deck::BindCapsuleToFront(const TimeBase& now) {
  Card* top = front();
  if (top == nullptr) {
    capsule_->Hide(now);
    return;
  }
  capsule_->Bind(top->regions(), top->selected(), top->frost(), now);
}

void Deck::AddShot(std::unique_ptr<CaptureResult> capture) {
  const double started = NowSeconds();
  TimeBase now = compositor_.Now();

  // A card that was paused by hover resumes a full dwell once it stops being the front.
  if (Card* previous = front(); previous != nullptr && hovering_) {
    previous->expiry = std::max(previous->expiry, now.seconds + theme::kDwellSeconds);
  }

  auto shot = std::make_shared<Shot>();
  shot->frame = capture->frame;
  shot->local_time = capture->local_time;
  store_.Submit(shot, capture->screen.regions.front().rect);

  insets_ = QueryTaskbarInsets(monitor_);
  const float capsule_full = capsule_->full_width();
  const float left = static_cast<float>(insets_.left) + Dip(theme::kEdgeMargin);
  const float box_top = static_cast<float>(size_.cy - insets_.bottom) - Dip(theme::kEdgeMargin) - Dip(theme::kCardMaxHeight);
  const float frost_top = box_top - Dip(theme::kCapsuleGap) - capsule_->height();
  RECT frost_rect{static_cast<LONG>(left), static_cast<LONG>(frost_top), static_cast<LONG>(left + capsule_full),
                  static_cast<LONG>(frost_top + capsule_->height())};
  RECT monitor_rect{0, 0, size_.cx, size_.cy};
  IntersectRect(&frost_rect, &frost_rect, &monitor_rect);
  ComPtr<IDCompositionSurface> frost;
  if (!IsRectEmpty(&frost_rect)) {
    uint32_t tint = theme::kGlassTint;
    uint32_t tint_bgr = (tint & 0xFF) << 16 | (tint & 0xFF00) | (tint >> 16);
    Image frost_pixels = RenderFrost(*capture->frame, frost_rect, theme::kFrostDownscale, tint_bgr, theme::kGlassTintAlpha);
    frost = compositor_.CreateSurface(frost_pixels, true);
  }

  auto card = std::make_unique<Card>(compositor_, cards_layer_.Get(), shot, capture->screen.regions, scale_,
                                     std::move(frost), now.seconds);
  Card* raw = card.get();
  raw->PlaceFullScreen();
  cards_.push_back(std::move(card));

  // The first frame only needs the full-resolution surface, so it is committed before anything else is prepared.
  now = compositor_.Now();
  Layout(theme::kFlight, now, theme::kFlightHold, true);
  BindCapsuleToFront(now);
  UpdateSink();
  compositor_.Commit();
  const double committed = NowSeconds();

  raw->BuildDerivedAssets(front_rect_, now);
  compositor_.Commit();
  Trace("shot: trigger->commit %.1f ms (ui %.1f ms), derived %.1f ms, options %zu",
        (committed - capture->trigger_time) * 1000.0, (committed - started) * 1000.0, (NowSeconds() - committed) * 1000.0,
        capture->screen.regions.size());
}

void Deck::Dismiss(size_t index, const TimeBase& now, float velocity) {
  std::unique_ptr<Card> card = std::move(cards_[index]);
  const bool was_front = index + 1 == cards_.size();
  cards_.erase(cards_.begin() + static_cast<ptrdiff_t>(index));

  const float distance = -(front_rect_.right + Dip(theme::kExitOvershoot));
  card->SlideOut(now, distance, velocity);
  exiting_.push_back(std::move(card));

  if (was_front) {
    BindCapsuleToFront(now);
    if (press_ != Press::kNone) {
      press_ = Press::kNone;
      if (GetCapture() == sink_) ReleaseCapture();
    }
  }
  Layout(theme::kReflow, now);
  if (cards_.empty()) UpdateSink();
}

void Deck::SelectRegion(int index) {
  Card* top = front();
  if (top == nullptr || index == top->selected()) return;

  const TimeBase now = compositor_.Now();
  const RECT region = top->regions()[index].rect;
  RectF target = RectForRegion(region);
  top->Select(index, target, now);
  top->expiry = now.seconds + theme::kDwellSeconds;
  capsule_->Select(index, now);
  Layout(theme::kMorph, now);
  compositor_.Commit();
  store_.Submit(top->shot(), region);
}

void Deck::Tick() {
  const TimeBase now = compositor_.Now();

  // Checked in place from the back so that dismissing the front cannot invalidate older indices.
  for (size_t i = cards_.size(); i-- > 0;) {
    Card* card = cards_[i].get();
    const bool paused = card->held_by_drag || (hovering_ && card == front());
    if (!paused && now.seconds >= card->expiry) Dismiss(i, now, 0.0f);
  }

  for (auto& card : cards_) card->Tick(now.seconds);
  for (auto it = exiting_.begin(); it != exiting_.end();) {
    if ((*it)->FinishedExiting(now.seconds)) {
      it = exiting_.erase(it);
    } else {
      (*it)->Tick(now.seconds);
      ++it;
    }
  }
  capsule_->Tick(now.seconds);

  // An auto-hiding taskbar can slide in at any moment, and thumbnails must stay clear of it.
  if (!cards_.empty() && ++tick_count_ % kTaskbarPollTicks == 0) {
    const TaskbarInsets insets = QueryTaskbarInsets(monitor_);
    if (insets != insets_) {
      insets_ = insets;
      Layout(theme::kReflow, now);
    }
  }
  compositor_.Commit();
}

bool Deck::IsIdle() const {
  return cards_.empty() && exiting_.empty() && NowSeconds() >= flash_alpha_.end_time();
}

void Deck::Rest() {
  if (!shown_) return;
  shown_ = false;
  capsule_->Clear();
  compositor_.Commit();
  ShowWindow(sink_, SW_HIDE);
  ShowWindow(window_, SW_HIDE);
  hovering_ = false;
  tracking_leave_ = false;
}

POINT Deck::StagePoint(LPARAM lparam) const {
  return POINT{GET_X_LPARAM(lparam) + sink_origin_.x, GET_Y_LPARAM(lparam) + sink_origin_.y};
}

void Deck::OnMouseMove(POINT point) {
  if (!tracking_leave_) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, sink_, 0};
    TrackMouseEvent(&track);
    tracking_leave_ = true;
  }
  hovering_ = true;
  const TimeBase now = compositor_.Now();

  if (press_ == Press::kCard) {
    const int dx = point.x - press_point_.x;
    const int dy = point.y - press_point_.y;
    const int slop = static_cast<int>(Dip(theme::kPressSlop));
    if (std::abs(dx) > slop || std::abs(dy) > slop) {
      // Left is dismissal; every other direction is a file drag toward another application.
      if (dx < 0 && std::abs(dx) > std::abs(dy)) {
        press_ = Press::kSwipe;
        swipe_velocity_ = 0.0f;
        swipe_offset_ = 0.0f;
        swipe_time_ = now.seconds;
      } else {
        BeginFileDrag();
        return;
      }
    }
  }

  if (press_ == Press::kSwipe) {
    Card* top = front();
    if (top == nullptr) return;
    float offset = static_cast<float>(point.x - press_point_.x);
    if (offset > 0.0f) offset *= theme::kRubberBand;
    const float elapsed = static_cast<float>(now.seconds - swipe_time_);
    if (elapsed > 0.001f) {
      // Smoothed so that one irregular sample does not decide whether a flick dismisses.
      swipe_velocity_ = swipe_velocity_ * 0.6f + (offset - swipe_offset_) / elapsed * 0.4f;
      swipe_offset_ = offset;
      swipe_time_ = now.seconds;
    }
    const float travel = std::max(front_rect_.right + Dip(theme::kExitOvershoot), 1.0f);
    top->SetSwipe(offset, std::clamp(1.0f + std::min(offset, 0.0f) / travel, 0.0f, 1.0f));
    compositor_.Commit();
    return;
  }

  if (press_ == Press::kNone) {
    capsule_->SetHover(capsule_->HitTest(point.x, point.y), now);
    compositor_.Commit();
  }
}

void Deck::OnButtonDown(POINT point) {
  if (front() == nullptr) return;
  SetCapture(sink_);
  press_point_ = point;
  const int segment = capsule_->HitTest(point.x, point.y);
  Trace("sink: down at %ld,%ld segment %d", point.x, point.y, segment);
  if (segment >= 0) {
    press_ = Press::kSegment;
    press_segment_ = segment;
    return;
  }
  const bool on_card = point.x >= front_rect_.left && point.x < front_rect_.right && point.y >= front_rect_.top &&
                       point.y < front_rect_.bottom;
  press_ = on_card ? Press::kCard : Press::kNone;
  if (press_ == Press::kNone) ReleaseCapture();
}

void Deck::OnButtonUp(POINT point) {
  const Press press = press_;
  Trace("sink: up at %ld,%ld press %d", point.x, point.y, static_cast<int>(press));
  press_ = Press::kNone;
  if (GetCapture() == sink_) ReleaseCapture();

  switch (press) {
    case Press::kSegment:
      if (capsule_->HitTest(point.x, point.y) == press_segment_) SelectRegion(press_segment_);
      break;
    case Press::kCard:
      OpenFront();
      break;
    case Press::kSwipe: {
      Card* top = front();
      if (top == nullptr) break;
      const TimeBase now = compositor_.Now();
      const bool far_enough = -swipe_offset_ > front_rect_.width() * theme::kSwipeDismissFraction;
      const bool flicked = swipe_velocity_ < -Dip(theme::kSwipeDismissVelocity);
      if (far_enough || flicked) {
        Dismiss(cards_.size() - 1, now, swipe_velocity_);
      } else {
        top->SnapBack(now, swipe_velocity_);
        top->expiry = now.seconds + theme::kDwellSeconds;
      }
      compositor_.Commit();
      break;
    }
    case Press::kNone:
      break;
  }
}

void Deck::OnMouseLeave() {
  tracking_leave_ = false;
  if (press_ != Press::kNone) return;
  hovering_ = false;
  const TimeBase now = compositor_.Now();
  if (Card* top = front(); top != nullptr) top->expiry = now.seconds + theme::kDwellSeconds;
  capsule_->SetHover(-1, now);
  compositor_.Commit();
}

void Deck::OpenFront() {
  Card* top = front();
  if (top == nullptr) return;
  const std::wstring path = store_.WaitUntilSaved(top->shot());
  const TimeBase now = compositor_.Now();
  Dismiss(cards_.size() - 1, now, 0.0f);
  compositor_.Commit();
  if (!path.empty()) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Deck::BeginFileDrag() {
  Card* card = front();
  press_ = Press::kNone;
  if (GetCapture() == sink_) ReleaseCapture();
  if (card == nullptr) return;

  const std::wstring path = store_.WaitUntilSaved(card->shot());
  if (path.empty()) return;

  card->held_by_drag = true;
  card->SetDimmed(true, compositor_.Now());
  compositor_.Commit();

  Image preview = card->RenderDragPreview(static_cast<int>(Dip(theme::kDragPreviewWidth)));
  // DoDragDrop runs its own message loop, so ticks and new shots can still arrive during the drag.
  const DWORD effect = RunFileDrag(path, preview);

  auto found = std::find_if(cards_.begin(), cards_.end(), [&](const auto& entry) { return entry.get() == card; });
  if (found == cards_.end()) return;
  const TimeBase now = compositor_.Now();
  card->held_by_drag = false;
  card->SetDimmed(false, now);
  card->expiry = now.seconds + theme::kDwellSeconds;
  if (effect != DROPEFFECT_NONE) Dismiss(static_cast<size_t>(found - cards_.begin()), now, 0.0f);
  compositor_.Commit();
}

LRESULT Deck::HandleSinkMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
      OnMouseMove(StagePoint(lparam));
      return 0;
    case WM_LBUTTONDOWN:
      OnButtonDown(StagePoint(lparam));
      return 0;
    case WM_LBUTTONUP:
      OnButtonUp(StagePoint(lparam));
      return 0;
    case WM_CAPTURECHANGED:
      Trace("sink: capture changed, press %d", static_cast<int>(press_));
      if (press_ == Press::kSwipe) {
        press_ = Press::kNone;
        if (Card* top = front(); top != nullptr) {
          top->SnapBack(compositor_.Now(), 0.0f);
          compositor_.Commit();
        }
      } else if (press_ != Press::kNone && reinterpret_cast<HWND>(lparam) != sink_) {
        press_ = Press::kNone;
      }
      return 0;
    case WM_MOUSELEAVE:
      OnMouseLeave();
      return 0;
    default:
      return DefWindowProcW(sink_, message, wparam, lparam);
  }
}

Overlay::Overlay(HWND controller, Compositor& compositor, Store& store)
    : controller_(controller), compositor_(compositor), store_(store) {}

Overlay::~Overlay() { decks_.clear(); }

Deck* Overlay::DeckFor(HMONITOR monitor) {
  for (auto& deck : decks_) {
    if (deck->monitor() == monitor) return deck.get();
  }
  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info)) return nullptr;
  auto deck = std::make_unique<Deck>(compositor_, store_, monitor, info.rcMonitor);
  if (!deck->ready()) return nullptr;
  decks_.push_back(std::move(deck));
  return decks_.back().get();
}

void Overlay::Warmup() {
  POINT cursor{};
  GetCursorPos(&cursor);
  DeckFor(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
  compositor_.Commit();
}

void Overlay::EnsureTicking() {
  active_ = true;
  if (ticking_) return;
  ticking_ = true;
  SetTimer(controller_, kTickTimer, kTickIntervalMs, nullptr);
}

void Overlay::OnFlash(HMONITOR monitor) {
  if (!compositor_.IsHealthy()) {
    // DWM restarted underneath us: every composition object is gone, so start over.
    decks_.clear();
    compositor_.Shutdown();
    compositor_.Initialize();
  }
  if (Deck* deck = DeckFor(monitor); deck != nullptr) {
    deck->Flash();
    EnsureTicking();
  }
}

void Overlay::OnCaptured(std::unique_ptr<CaptureResult> capture) {
  if (!capture->frame || capture->screen.regions.empty()) return;
  if (Deck* deck = DeckFor(capture->screen.monitor); deck != nullptr) {
    deck->AddShot(std::move(capture));
    EnsureTicking();
  }
}

void Overlay::OnTick() {
  bool any_active = false;
  for (auto& deck : decks_) {
    if (!deck->IsIdle()) {
      deck->Tick();
      any_active = any_active || !deck->IsIdle();
    }
  }
  if (any_active) return;

  for (auto& deck : decks_) deck->Rest();
  KillTimer(controller_, kTickTimer);
  ticking_ = false;
  active_ = false;
}

void Overlay::OnDisplayChange() {
  // Geometry is stale for every monitor, so everything on screen is dropped; files and clipboard are already safe.
  decks_.clear();
  compositor_.Commit();
  if (ticking_) {
    KillTimer(controller_, kTickTimer);
    ticking_ = false;
  }
  active_ = false;
}

bool Overlay::IsIdle() const { return !active_; }

}  // namespace sstk
