#include "overlay.h"

#include "bin.h"
#include "capsule.h"
#include "card.h"
#include "config/animation.h"
#include "config/colors.h"
#include "config/core.h"
#include "config/layout.h"
#include "dragdrop.h"
#include "sprite.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr wchar_t kStageClass[] = L"ScreenshotToolkit.Stage";
constexpr wchar_t kSinkClass[] = L"ScreenshotToolkit.Sink";
constexpr float kDegreesToRadians = 3.14159265f / 180.0f;

}  // namespace

// The overlay of one monitor. Two windows work together: a full-monitor, click-through stage that
// holds all visuals, and a small input sink that covers only the interactive shapes.
class Deck {
 public:
  Deck(Overlay& owner, Compositor& compositor, Store& store, HMONITOR monitor, const RECT& bounds);
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

  // Whether the front card offers anything to switch to, and moves to its next option if it can.
  bool HasOptions() const;
  void StepRegion(int direction);

  LRESULT HandleSinkMessage(UINT message, WPARAM wparam, LPARAM lparam);

  // While a thumbnail is held by the pointer. Screen positions are in virtual screen coordinates.
  const RECT& bounds() const { return bounds_; }
  float scale() const { return scale_; }
  IDCompositionVisual* drag_layer() const { return drag_layer_.Get(); }
  RECT FreeArea() const;
  void EnsureShown();
  void SetDragActive(bool active, const TimeBase& now);
  void ExtendDwell(double now);
  void TakeBack(std::unique_ptr<Card> card, size_t index, const TimeBase& now);
  void Land(Card* card);
  void PollInsets(const TimeBase& now);

  void ShowBin();
  void HideBin();
  void GulpBin();
  void NudgeBin(float impulse) { bin_->Nudge(impulse); }
  bool TrackBin(POINT screen);
  bool BinContains(POINT screen) const;
  void AimBin(float screen_x, float screen_y) { bin_->Aim(screen_x - static_cast<float>(bounds_.left), screen_y - static_cast<float>(bounds_.top)); }
  float BinCloseness() const { return bin_->closeness(); }
  float BinX() const { return static_cast<float>(bounds_.left) + bin_->mouth_x(); }
  float BinY() const { return static_cast<float>(bounds_.top) + bin_->mouth_y(); }
  bool FrameBin(float dt);

 private:
  enum class Press { kNone, kCard, kSwipe };

  float Dip(float value) const { return value * scale_; }
  void ApplyInsets(const TaskbarInsets& insets);
  RectF RectForRegion(const RECT& region) const;
  RectF RectBehind(const Card& card, const RectF& front, int depth) const;
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
  void LiftFront();
  void OpenFront();
  void PlaceBin();
  void ShowBinHole();

  Overlay& owner_;
  Compositor& compositor_;
  Store& store_;
  HMONITOR monitor_;
  RECT bounds_;
  SIZE size_;
  int stage_width_;
  float scale_ = 1.0f;

  HWND window_ = nullptr;
  HWND sink_ = nullptr;
  POINT sink_origin_{};
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionVisual> cards_layer_;
  ComPtr<IDCompositionVisual> capsule_layer_;
  ComPtr<IDCompositionVisual> drag_layer_;
  ComPtr<IDCompositionVisual> bin_layer_;
  ComPtr<IDCompositionVisual> flash_;
  ComPtr<IDCompositionEffectGroup> flash_effect_;
  AnimFloat flash_alpha_;
  std::unique_ptr<Capsule> capsule_;
  std::unique_ptr<RubbishBin> bin_;
  CardShadow card_shadow_;
  ComPtr<IDCompositionRectangleClip> shadow_clip_;

  std::vector<std::unique_ptr<Card>> cards_;  // Oldest first; the back is the front of the deck.
  std::vector<std::unique_ptr<Card>> exiting_;

  TaskbarInsets insets_;
  RectF front_rect_{};
  bool shown_ = false;
  int tick_count_ = 0;
  double last_tick_ = 0.0;
  bool dragging_ = false;
  bool capsule_stale_ = false;
  bool bin_running_ = false;
  bool bin_showing_ = false;
  bool bin_over_ = false;

  bool hovering_ = false;
  bool tracking_leave_ = false;
  Press press_ = Press::kNone;
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

Deck::Deck(Overlay& owner, Compositor& compositor, Store& store, HMONITOR monitor, const RECT& bounds)
    : owner_(owner), compositor_(compositor), store_(store), monitor_(monitor), bounds_(bounds),
      size_{bounds.right - bounds.left, bounds.bottom - bounds.top},
      // A window covering a whole monitor counts as a fullscreen app, and Windows answers by dropping the
      // taskbar below every other window. One pixel short of that, nothing on the desktop changes. The
      // pixel goes unmissed: the screenshot starts out identical to what is under it.
      stage_width_(size_.cx - 1) {
  RegisterClasses();
  const HINSTANCE instance = GetModuleHandleW(nullptr);

  window_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_LAYERED | WS_EX_TRANSPARENT,
                            kStageClass, L"", WS_POPUP, bounds.left, bounds.top, stage_width_, size_.cy, nullptr, nullptr,
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
  drag_layer_ = compositor_.CreateVisual();
  bin_layer_ = compositor_.CreateVisual();
  // The capsule sits under the cards, so the flying screenshot uncovers it instead of crossing over it.
  root_->AddVisual(capsule_layer_.Get(), FALSE, nullptr);
  root_->AddVisual(cards_layer_.Get(), FALSE, nullptr);
  root_->AddVisual(drag_layer_.Get(), FALSE, nullptr);
  root_->AddVisual(bin_layer_.Get(), FALSE, nullptr);

  flash_ = compositor_.CreateSolidVisual(0xFF000000u | config::kFlashColor, static_cast<float>(size_.cx), static_cast<float>(size_.cy));
  flash_effect_ = compositor_.CreateEffectGroup();
  flash_->SetEffect(flash_effect_.Get());
  flash_alpha_.Bind(flash_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  flash_alpha_.Snap(0.0f);
  root_->AddVisual(flash_.Get(), FALSE, nullptr);
  target_->SetRoot(root_.Get());

  capsule_ = std::make_unique<Capsule>(compositor_, capsule_layer_.Get(), scale_, size_);
  card_shadow_ = BuildCardShadow(compositor_, scale_);
  shadow_clip_ = compositor_.CreateClip();
}

Deck::~Deck() {
  cards_.clear();
  exiting_.clear();
  capsule_.reset();
  bin_.reset();
  if (target_) target_->SetRoot(nullptr);
  target_.Reset();
  if (sink_ != nullptr) DestroyWindow(sink_);
  if (window_ != nullptr) DestroyWindow(window_);
}

void Deck::Flash() {
  flash_alpha_.Pulse(compositor_.device(), compositor_.Now(), config::kFlashPeak, config::kFlashRise, config::kFlashFall);
  compositor_.Commit();

  // Shown after the commit so that the first composed frame already contains the flash.
  SetWindowPos(window_, HWND_TOPMOST, bounds_.left, bounds_.top, stage_width_, size_.cy,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  shown_ = true;
}

void Deck::ApplyInsets(const TaskbarInsets& insets) {
  if (insets != insets_) Trace("taskbar insets: %d %d %d %d", insets.left, insets.top, insets.right, insets.bottom);
  insets_ = insets;
  // Shadows pass under the taskbar, as those of ordinary windows do.
  shadow_clip_->SetLeft(static_cast<float>(insets.left));
  shadow_clip_->SetTop(static_cast<float>(insets.top));
  shadow_clip_->SetRight(static_cast<float>(size_.cx - insets.right));
  shadow_clip_->SetBottom(static_cast<float>(size_.cy - insets.bottom));
  if (bin_) {
    PlaceBin();
    if (bin_showing_) ShowBinHole();
  }
}

RectF Deck::RectForRegion(const RECT& region) const {
  const SIZE size = ThumbnailSize(region, scale_);
  const float width = static_cast<float>(size.cx);
  const float height = static_cast<float>(size.cy);
  const float left = std::round(static_cast<float>(insets_.left) + Dip(config::kEdgeMargin));
  const float bottom = std::round(static_cast<float>(size_.cy - insets_.bottom) - Dip(config::kEdgeMargin));
  return RectF{left, bottom - height, left + width, bottom};
}

RectF Deck::RectBehind(const Card& card, const RectF& front, int depth) const {
  // Each card keeps the shape of its own selection. Only its place in the stack comes from the front card.
  const RectF own = RectForRegion(card.selected_rect());
  const int level = std::min(depth, config::kDeckVisibleBehind + 1);
  const float keep = 1.0f - config::kDeckShrinkPerLevel * level;
  const float width = own.width() * keep;
  const float height = own.height() * keep;
  const float left = own.left + (own.width() - width) * 0.5f;
  const float peek = Dip(config::kDeckPeek) * level;
  // A card taller than the front rises above it rather than hanging below the deck.
  const float top = std::min(front.top - peek, front.bottom - height - peek);
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
      cards_[i]->MoveTo(RectBehind(*cards_[i], front_rect_, depth), depth, config::kReflow, now, 0.0, false);
    }
  }

  // The capsule is anchored to the tallest possible card, not the current one, so it does not
  // move under the pointer when the selected region changes the thumbnail's shape.
  const float box_top = static_cast<float>(size_.cy - insets_.bottom) - Dip(config::kEdgeMargin) - Dip(config::kCardMaxHeight);
  const int peeking = std::min(count - 1, config::kDeckVisibleBehind);
  const float capsule_y = std::round(box_top - Dip(config::kDeckPeek) * peeking - Dip(config::kCapsuleGap) - capsule_->height());
  capsule_->MoveTo(front_rect_.left, capsule_y, now);
  UpdateSink();
}

void Deck::UpdateSink() {
  if (dragging_) return;
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
  if (dragging_) {
    capsule_stale_ = true;
    return;
  }
  capsule_stale_ = false;
  Card* top = front();
  if (top == nullptr) {
    capsule_->Hide(now);
    return;
  }
  top->Wake(store_);
  capsule_->Bind(top->regions(), top->insets(), top->selected(), top->frost(), now);
}

void Deck::AddShot(std::unique_ptr<CaptureResult> capture) {
  const double started = NowSeconds();
  TimeBase now = compositor_.Now();

  // A card that was paused by hover resumes a full dwell once it stops being the front.
  if (Card* previous = front(); previous != nullptr && hovering_) {
    previous->expiry = std::max(previous->expiry, now.seconds + config::kDwellSeconds);
  }

  auto shot = std::make_shared<Shot>();
  shot->frame = capture->frame;
  shot->local_time = capture->local_time;
  store_.Submit(shot, capture->screen.regions.front().rect);

  ApplyInsets(capture->screen.insets);
  const float capsule_full = capsule_->full_width();
  const float left = static_cast<float>(insets_.left) + Dip(config::kEdgeMargin);
  const float box_top = static_cast<float>(size_.cy - insets_.bottom) - Dip(config::kEdgeMargin) - Dip(config::kCardMaxHeight);
  const float frost_top = box_top - Dip(config::kCapsuleGap) - capsule_->height();
  RECT frost_rect{static_cast<LONG>(left), static_cast<LONG>(frost_top), static_cast<LONG>(left + capsule_full),
                  static_cast<LONG>(frost_top + capsule_->height())};
  RECT monitor_rect{0, 0, size_.cx, size_.cy};
  IntersectRect(&frost_rect, &frost_rect, &monitor_rect);
  ComPtr<IDCompositionSurface> frost;
  if (!IsRectEmpty(&frost_rect)) {
    uint32_t tint = config::kGlassTint;
    uint32_t tint_bgr = (tint & 0xFF) << 16 | (tint & 0xFF00) | (tint >> 16);
    Image frost_pixels = RenderFrost(*capture->frame, frost_rect, config::kFrostDownscale, tint_bgr, config::kGlassTintAlpha);
    frost = compositor_.CreateSurface(frost_pixels, true);
  }

  auto card = std::make_unique<Card>(compositor_, cards_layer_.Get(), shot, capture->screen, scale_, std::move(frost),
                                     card_shadow_, shadow_clip_.Get(), now.seconds);
  Card* raw = card.get();
  raw->PlaceFullScreen();
  cards_.push_back(std::move(card));

  // The first frame only needs the full-resolution surface, so it is committed before anything else is prepared.
  now = compositor_.Now();
  Layout(config::kFlight, now, config::kFlightHold, true);
  BindCapsuleToFront(now);
  UpdateSink();
  compositor_.Commit();
  const double committed = NowSeconds();

  raw->BuildDerivedAssets(now);
  compositor_.Commit();
  Trace("shot: trigger->commit %.1f ms (ui %.1f ms), derived %.1f ms, options %zu",
        (committed - capture->trigger_time) * 1000.0, (committed - started) * 1000.0, (NowSeconds() - committed) * 1000.0,
        capture->screen.regions.size());
}

void Deck::Dismiss(size_t index, const TimeBase& now, float velocity) {
  std::unique_ptr<Card> card = std::move(cards_[index]);
  const bool was_front = index + 1 == cards_.size();
  cards_.erase(cards_.begin() + static_cast<ptrdiff_t>(index));

  card->SlideOut(now, velocity);
  exiting_.push_back(std::move(card));

  if (was_front) {
    BindCapsuleToFront(now);
    if (press_ != Press::kNone) {
      press_ = Press::kNone;
      if (GetCapture() == sink_) ReleaseCapture();
    }
  }
  Layout(config::kReflow, now);
  if (cards_.empty()) UpdateSink();
}

void Deck::SelectRegion(int index) {
  Card* top = front();
  if (top == nullptr || index == top->selected() || !top->CanSelect(index)) return;

  // A selection belongs to one screenshot, so the cards behind it stay exactly where they are.
  const TimeBase now = compositor_.Now();
  top->Select(index, now);
  top->expiry = now.seconds + config::kDwellSeconds;
  capsule_->Select(index, now);
  front_rect_ = RectForRegion(top->selected_rect());
  top->MoveTo(front_rect_, 0, config::kMorph, now, 0.0, false);
  UpdateSink();
  compositor_.Commit();
  store_.Submit(top->shot(), top->selected_rect());
}

bool Deck::HasOptions() const {
  const Card* top = front();
  return top != nullptr && top->regions().size() >= 2;
}

void Deck::StepRegion(int direction) {
  Card* top = front();
  if (!HasOptions() || top->floating || press_ != Press::kNone) return;
  const int count = static_cast<int>(top->regions().size());
  for (int step = 1; step < count; ++step) {
    const int index = (top->selected() + direction * step + count) % count;
    if (top->CanSelect(index)) {
      SelectRegion(index);
      return;
    }
  }
}

void Deck::Tick() {
  const TimeBase now = compositor_.Now();

  // Checked in place from the back so that dismissing the front cannot invalidate older indices.
  const double elapsed = std::min(now.seconds - last_tick_, 0.25);
  last_tick_ = now.seconds;
  for (size_t i = cards_.size(); i-- > 0;) {
    Card* card = cards_[i].get();
    if (owner_.drag_held()) {
      card->expiry += elapsed;
      continue;
    }
    const bool paused = card->floating || (hovering_ && card == front());
    if (!paused && now.seconds >= card->expiry) Dismiss(i, now, 0.0f);
  }

  for (size_t i = 0; i < cards_.size(); ++i) {
    Card* card = cards_[i].get();
    card->Tick(now.seconds, store_);
    // Cards still in sight keep everything, so that dismissing the front card never has to wait for a decode.
    const int depth = static_cast<int>(cards_.size() - 1 - i);
    if (depth > config::kDeckVisibleBehind) card->Hibernate(store_);
  }
  for (auto it = exiting_.begin(); it != exiting_.end();) {
    if ((*it)->FinishedExiting(now.seconds)) {
      it = exiting_.erase(it);
    } else {
      (*it)->Tick(now.seconds, store_);
      ++it;
    }
  }
  capsule_->Tick(now.seconds);

  // A release outside the sink, or a shape change under a still cursor, can end hover without a leave message.
  if (hovering_ && press_ == Press::kNone) {
    POINT cursor{};
    GetCursorPos(&cursor);
    if (WindowFromPoint(cursor) != sink_) OnMouseLeave();
  }

  if (!cards_.empty()) PollInsets(now);
  compositor_.Commit();
}

void Deck::PollInsets(const TimeBase& now) {
  // An auto-hiding taskbar can slide in at any moment, and thumbnails must stay clear of it.
  if (++tick_count_ % config::kTaskbarPollTicks != 0) return;
  const TaskbarInsets insets = QueryTaskbarInsets(monitor_);
  if (insets != insets_) {
    ApplyInsets(insets);
    Layout(config::kReflow, now);
  }
}

RECT Deck::FreeArea() const {
  return RECT{bounds_.left + insets_.left, bounds_.top + insets_.top, bounds_.right - insets_.right, bounds_.bottom - insets_.bottom};
}

void Deck::EnsureShown() {
  if (shown_) return;
  SetWindowPos(window_, HWND_TOPMOST, bounds_.left, bounds_.top, stage_width_, size_.cy,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  shown_ = true;
}

void Deck::SetDragActive(bool active, const TimeBase& now) {
  if (active == dragging_) return;
  dragging_ = active;
  if (active) {
    hovering_ = false;
    tracking_leave_ = false;
    capsule_->SetHover(-1, now);
    ShowWindow(sink_, SW_HIDE);
    return;
  }
  last_tick_ = now.seconds;
  bin_over_ = false;
  bin_showing_ = false;
  if (capsule_stale_) BindCapsuleToFront(now);
  UpdateSink();
}

void Deck::ExtendDwell(double now) {
  for (auto& card : cards_) {
    const double remaining = card->expiry - now;
    if (remaining < config::kDwellSeconds) card->expiry = now + std::min(remaining + config::kDropDwellBonus, config::kDwellSeconds);
  }
}

void Deck::TakeBack(std::unique_ptr<Card> card, size_t index, const TimeBase& now) {
  card->expiry = now.seconds + config::kDwellSeconds;
  cards_.insert(cards_.begin() + static_cast<ptrdiff_t>(std::min(index, cards_.size())), std::move(card));
  Layout(config::kReflow, now);
}

void Deck::Land(Card* card) {
  card->floating = false;
  card->Attach();
  auto found = std::find_if(cards_.begin(), cards_.end(), [&](const auto& entry) { return entry.get() == card; });
  if (found == cards_.end()) return;
  for (++found; found != cards_.end(); ++found) (*found)->Raise();
}

void Deck::PlaceBin() {
  bin_->Place(static_cast<float>(insets_.left + size_.cx - insets_.right) * 0.5f, static_cast<float>(size_.cy - insets_.bottom));
}

// During a drag the sink covers the bin's hot zone, so that the application behind it can't receive the drop.
void Deck::ShowBinHole() {
  const RECT zone = bin_->HotZone();
  SetWindowRgn(sink_, nullptr, FALSE);
  SetWindowPos(sink_, HWND_TOPMOST, bounds_.left + zone.left, bounds_.top + zone.top, zone.right - zone.left,
               zone.bottom - zone.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

void Deck::ShowBin() {
  EnsureShown();
  if (!bin_) bin_ = std::make_unique<RubbishBin>(compositor_, bin_layer_.Get(), scale_);
  const TaskbarInsets insets = QueryTaskbarInsets(monitor_);
  if (insets != insets_) {
    ApplyInsets(insets);
    Layout(config::kReflow, compositor_.Now());
  }
  PlaceBin();
  bin_->Show();
  bin_running_ = true;
  bin_showing_ = true;
  bin_over_ = false;
  ShowBinHole();
}

void Deck::HideBin() {
  if (!bin_) return;
  bin_->Hide();
  bin_showing_ = false;
  bin_over_ = false;
  ShowWindow(sink_, SW_HIDE);
}

void Deck::GulpBin() {
  if (bin_) bin_->Gulp(!dragging_);
}

bool Deck::TrackBin(POINT screen) {
  const bool over = bin_->Track(POINT{screen.x - bounds_.left, screen.y - bounds_.top});
  if (over != bin_over_) {
    bin_over_ = over;
    ShowBinHole();
  }
  return over;
}

bool Deck::BinContains(POINT screen) const {
  return bin_ && bin_->Contains(POINT{screen.x - bounds_.left, screen.y - bounds_.top});
}

bool Deck::FrameBin(float dt) {
  if (!bin_running_) return false;
  bin_running_ = bin_->Frame(dt);
  return bin_running_;
}

bool Deck::IsIdle() const {
  return cards_.empty() && exiting_.empty() && NowSeconds() >= flash_alpha_.end_time();
}

void Deck::Rest() {
  if (!shown_) return;
  shown_ = false;
  capsule_->Clear();
  if (bin_) bin_->Reset();
  bin_running_ = false;
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
    const int slop = static_cast<int>(Dip(config::kPressSlop));
    if (std::abs(dx) > slop || std::abs(dy) > slop) {
      // Left is dismissal; every other direction is a file drag toward another application.
      if (dx < 0 && std::abs(dx) > std::abs(dy)) {
        press_ = Press::kSwipe;
        swipe_velocity_ = 0.0f;
        swipe_offset_ = 0.0f;
        swipe_time_ = now.seconds;
      } else {
        LiftFront();
        return;
      }
    }
  }

  if (press_ == Press::kSwipe) {
    Card* top = front();
    if (top == nullptr) return;
    float offset = static_cast<float>(point.x - press_point_.x);
    if (offset > 0.0f) offset *= config::kRubberBand;
    const float elapsed = static_cast<float>(now.seconds - swipe_time_);
    if (elapsed > 0.001f) {
      // Smoothed so that one irregular sample does not decide whether a flick dismisses.
      swipe_velocity_ = swipe_velocity_ * 0.6f + (offset - swipe_offset_) / elapsed * 0.4f;
      swipe_offset_ = offset;
      swipe_time_ = now.seconds;
    }
    const float travel = std::max(front_rect_.right + Dip(config::kExitOvershoot), 1.0f);
    top->SetSwipe(offset, std::clamp(-offset / travel, 0.0f, 1.0f));
    compositor_.Commit();
    return;
  }

  if (press_ == Press::kNone) {
    capsule_->SetHover(capsule_->HitTest(point.x, point.y), now);
    compositor_.Commit();
  }
}

void Deck::OnButtonDown(POINT point) {
  if (front() == nullptr || front()->floating) return;
  const int segment = capsule_->HitTest(point.x, point.y);
  Trace("sink: down at %ld,%ld segment %d", point.x, point.y, segment);
  if (segment >= 0) {
    // Switching is reversible and costs nothing, so it happens on press rather than making the user wait for release.
    SelectRegion(segment);
    return;
  }
  const bool on_card = point.x >= front_rect_.left && point.x < front_rect_.right && point.y >= front_rect_.top &&
                       point.y < front_rect_.bottom;
  if (!on_card) return;
  SetCapture(sink_);
  press_point_ = point;
  press_ = Press::kCard;
}

void Deck::OnButtonUp(POINT point) {
  const Press press = press_;
  Trace("sink: up at %ld,%ld press %d", point.x, point.y, static_cast<int>(press));
  press_ = Press::kNone;
  if (GetCapture() == sink_) ReleaseCapture();

  switch (press) {
    case Press::kCard:
      OpenFront();
      break;
    case Press::kSwipe: {
      Card* top = front();
      if (top == nullptr) break;
      const TimeBase now = compositor_.Now();
      const bool far_enough = -swipe_offset_ > front_rect_.width() * config::kSwipeDismissFraction;
      const bool flicked = swipe_velocity_ < -Dip(config::kSwipeDismissVelocity);
      if (far_enough || flicked) {
        Dismiss(cards_.size() - 1, now, swipe_velocity_);
      } else {
        top->SnapBack(now, swipe_velocity_);
        top->expiry = now.seconds + config::kDwellSeconds;
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
  if (press_ != Press::kNone || dragging_) return;
  hovering_ = false;
  const TimeBase now = compositor_.Now();
  if (Card* top = front(); top != nullptr) top->expiry = now.seconds + config::kDwellSeconds;
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
  if (path.empty()) return;
  EnsureOleInitialized();
  // The click makes this process eligible to pass the foreground on, which the viewer needs to come to the front.
  AllowSetForegroundWindow(ASFW_ANY);
  // Handed to the shell, so that the viewer does not inherit this process's UI Access right.
  const std::wstring quoted = L"\"" + path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
}

void Deck::LiftFront() {
  press_ = Press::kNone;
  if (front() == nullptr || front()->thumbnail() == nullptr) {
    if (GetCapture() == sink_) ReleaseCapture();
    return;
  }

  // The capture stays until the drag thread's loop takes it over.
  const TimeBase now = compositor_.Now();
  const size_t index = cards_.size() - 1;
  std::unique_ptr<Card> card = std::move(cards_.back());
  cards_.pop_back();
  capsule_->Hide(now);
  capsule_stale_ = true;

  const POINT press{bounds_.left + press_point_.x, bounds_.top + press_point_.y};
  owner_.BeginDrag(*this, std::move(card), index, press, now);
  Layout(config::kReflow, now);
  compositor_.Commit();
}

// One thumbnail from lift until it lands, vanishes, or enters the bin. Stepped per frame, as the pointer
// decides the next value. The card is off screen meanwhile, and its sprite is drawn where the card was.
class DragSession {
 public:
  DragSession(Overlay& overlay, Compositor& compositor, Store& store, Deck& deck, std::unique_ptr<Card> card, size_t index,
              POINT press, uint64_t id, const TimeBase& now);

  uint64_t id() const { return id_; }
  const std::shared_ptr<Shot>& shot() const { return shot_; }
  void SetAccepts(bool accepts) { accepts_ = accepts; }

  // Returns whether the screenshot left the deck for good.
  bool Finish(const FileDragOutcome& outcome, const TimeBase& now);

  // Returns false once there is nothing more to show.
  bool Frame(const TimeBase& now);

 private:
  enum class Phase { kHeld, kVanishing, kSwallowed, kReturning, kDone };

  void StepHeld(float dt);
  void StepVanishing(float dt);
  void StepSwallowed(float dt);
  void StepReturning(float dt);
  void EnsureViews();
  bool TrackBin(POINT cursor);
  void Render();

  struct Monitor {
    HMONITOR handle;
    RECT bounds;
    bool viewed;
  };

  Overlay& overlay_;
  Store& store_;
  Deck& deck_;
  std::unique_ptr<Card> card_;
  Card* landing_card_ = nullptr;
  std::shared_ptr<Shot> shot_;
  size_t index_;
  uint64_t id_;
  std::unique_ptr<DragSprite> sprite_;
  std::vector<Monitor> monitors_;
  Deck* bin_deck_ = nullptr;

  Phase phase_ = Phase::kHeld;
  double last_time_;
  float dip_;
  float thumb_width_;
  float thumb_height_;
  float grab_x_;
  float grab_y_;
  bool accepts_ = false;
  bool over_bin_ = false;
  bool gulped_ = false;
  bool inside_ = false;
  POINT last_cursor_{};
  float pointer_vx_ = 0.0f;
  float pointer_vy_ = 0.0f;
  float pull_x_ = 0.0f;
  float pull_y_ = 0.0f;

  SpringValue x_, y_, catch_x_, catch_y_, scale_, tilt_, lift_, pull_, badge_, vanish_, opacity_;
};

DragSession::DragSession(Overlay& overlay, Compositor& compositor, Store& store, Deck& deck, std::unique_ptr<Card> card, size_t index,
                         POINT press, uint64_t id, const TimeBase& now)
    : overlay_(overlay), store_(store), deck_(deck), card_(std::move(card)), shot_(card_->shot()), index_(index), id_(id),
      last_time_(now.seconds), dip_(deck.scale()) {
  const RECT& bounds = deck.bounds();
  const Image& thumbnail = *card_->thumbnail();
  const RectF from = card_->RectAt(now.seconds);
  const float start_scale = from.width() / static_cast<float>(thumbnail.width());
  thumb_width_ = static_cast<float>(thumbnail.width());
  thumb_height_ = static_cast<float>(thumbnail.height());
  grab_x_ = (static_cast<float>(press.x - bounds.left) - from.left) / start_scale;
  grab_y_ = (static_cast<float>(press.y - bounds.top) - from.top) / start_scale;
  sprite_ = std::make_unique<DragSprite>(compositor, thumbnail, grab_x_, grab_y_, dip_);
  card_->Detach();

  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT rect, LPARAM context) -> BOOL {
    reinterpret_cast<std::vector<Monitor>*>(context)->push_back(Monitor{handle, *rect, false});
    return TRUE;
  }, reinterpret_cast<LPARAM>(&monitors_));

  GetCursorPos(&last_cursor_);
  // The pointer is already past the press threshold, so the image closes that gap over a few frames.
  x_.Snap(static_cast<float>(press.x));
  y_.Snap(static_cast<float>(press.y));
  catch_x_.Snap(static_cast<float>(press.x - last_cursor_.x));
  catch_y_.Snap(static_cast<float>(press.y - last_cursor_.y));
  catch_x_.target = 0.0f;
  catch_y_.target = 0.0f;
  scale_.Snap(start_scale);
  lift_.Snap(0.0f);
  lift_.target = 1.0f;
  opacity_.Snap(1.0f);
}

void DragSession::EnsureViews() {
  const float reach = (sprite_->reach() + 24.0f * dip_) * std::max(scale_.value, 1.0f);
  const RECT area{static_cast<LONG>(x_.value - reach), static_cast<LONG>(y_.value - reach), static_cast<LONG>(x_.value + reach),
                  static_cast<LONG>(y_.value + reach)};
  for (Monitor& monitor : monitors_) {
    RECT overlap;
    if (monitor.viewed || !IntersectRect(&overlap, &area, &monitor.bounds)) continue;
    Deck* deck = overlay_.DeckFor(monitor.handle);
    if (deck == nullptr) continue;
    deck->EnsureShown();
    sprite_->AddView(deck->drag_layer(), deck->bounds(), deck->FreeArea());
    monitor.viewed = true;
  }
}

bool DragSession::TrackBin(POINT cursor) {
  Deck* deck = overlay_.DeckFor(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
  if (deck != nullptr && deck != bin_deck_) {
    if (bin_deck_ != nullptr) bin_deck_->HideBin();
    deck->ShowBin();
    bin_deck_ = deck;
  }
  if (bin_deck_ == nullptr) return false;
  const bool over = bin_deck_->TrackBin(cursor);
  pull_x_ = bin_deck_->BinX();
  pull_y_ = bin_deck_->BinY();
  return over;
}

void DragSession::StepHeld(float dt) {
  POINT cursor;
  GetCursorPos(&cursor);
  if (dt > 0.0f) {
    const float blend = 1.0f - std::exp(-dt / config::kDragSpeedSmoothing);
    pointer_vx_ += (static_cast<float>(cursor.x - last_cursor_.x) / dt - pointer_vx_) * blend;
    pointer_vy_ += (static_cast<float>(cursor.y - last_cursor_.y) / dt - pointer_vy_) * blend;
    last_cursor_ = cursor;
  }

  const bool was_over = over_bin_;
  over_bin_ = TrackBin(cursor);

  catch_x_.Step(config::kDragCatchUp, dt);
  catch_y_.Step(config::kDragCatchUp, dt);
  x_.value = static_cast<float>(cursor.x) + catch_x_.value;
  y_.value = static_cast<float>(cursor.y) + catch_y_.value;

  scale_.target = config::kDragScale;
  scale_.Step(config::kDragLift, dt);
  lift_.Step(config::kDragLift, dt);

  const float tilt_limit = config::kDragTiltMaxDegrees;
  tilt_.target = std::clamp(pointer_vx_ / dip_ * config::kDragTiltPerSpeed, -tilt_limit, tilt_limit);
  tilt_.Step(config::kDragTilt, dt);

  // The card stays on the pointer throughout, and only this one value draws it in and lets it out. Coming close, the
  // pull is already felt. Crossing the edge starts it moving at once, faster the faster the pointer came.
  if (bin_deck_ != nullptr) {
    bin_deck_->AimBin(x_.value + (thumb_width_ * 0.5f - grab_x_) * scale_.value, y_.value + (thumb_height_ * 0.5f - grab_y_) * scale_.value);
    const float felt = std::max(bin_deck_->BinCloseness() - config::kBinMagnetFrom, 0.0f) / (1.0f - config::kBinMagnetFrom);
    pull_.target = over_bin_ ? 1.0f : config::kBinMagnetPull * felt * std::sqrt(felt);
  }
  if (over_bin_ != was_over) {
    const float speed = std::sqrt(pointer_vx_ * pointer_vx_ + pointer_vy_ * pointer_vy_) / dip_;
    const float kick = std::min(config::kBinPullKick + speed * config::kBinPullKickPerSpeed, config::kBinPullKickMax);
    pull_.velocity += over_bin_ ? kick : -kick;
  }
  pull_.Step(config::kBinPull, dt);

  const bool inside = pull_.value > config::kBinInsideAt;
  if (inside != inside_) {
    inside_ = inside;
    if (bin_deck_ != nullptr) bin_deck_->NudgeBin(inside ? config::kBinInsideImpulse : config::kBinOutsideImpulse);
  }

  badge_.target = accepts_ && !over_bin_ && pull_.value < config::kBinMagnetPull * 1.5f ? 1.0f : 0.0f;
  badge_.Step(badge_.target > 0.0f ? config::kDropBadgeIn : config::kDropBadgeOut, dt);
}

void DragSession::StepVanishing(float dt) {
  const float decay = std::exp(-dt * config::kDropDriftDecay);
  pointer_vx_ *= decay;
  pointer_vy_ *= decay;
  x_.value += pointer_vx_ * dt;
  y_.value += pointer_vy_ * dt;

  tilt_.target = 0.0f;
  tilt_.Step(config::kDragTilt, dt);
  vanish_.Step(config::kDropVanish, dt);
  badge_.Step(config::kDropBadgeOut, dt);
  opacity_.value = std::clamp(1.0f - vanish_.value, 0.0f, 1.0f);
  if (vanish_.value > 0.985f) phase_ = Phase::kDone;
}

void DragSession::StepSwallowed(float dt) {
  if (bin_deck_ != nullptr) {
    pull_x_ = bin_deck_->BinX();
    pull_y_ = bin_deck_->BinY();
  }
  pull_.Step(config::kBinSwallow, dt);
  tilt_.Step(config::kBinSwallow, dt);
  if (!gulped_ && pull_.value >= config::kBinGulpAt) {
    gulped_ = true;
    if (bin_deck_ != nullptr) bin_deck_->GulpBin();
  }
  if (pull_.value > 0.99f) phase_ = Phase::kDone;
}

void DragSession::StepReturning(float dt) {
  // Read every frame: a shot taken during the flight can change where this card belongs.
  const RECT& bounds = deck_.bounds();
  const RectF home = landing_card_->target_rect();
  const float home_scale = home.width() / thumb_width_;
  x_.target = static_cast<float>(bounds.left) + home.left + grab_x_ * home_scale;
  y_.target = static_cast<float>(bounds.top) + home.top + grab_y_ * home_scale;
  scale_.target = home_scale;
  opacity_.target = landing_card_->resting_opacity();

  x_.Step(config::kFlyHome, dt);
  y_.Step(config::kFlyHome, dt);
  scale_.Step(config::kFlyHome, dt);
  opacity_.Step(config::kFade, dt);
  tilt_.target = 0.0f;
  tilt_.Step(config::kDragTilt, dt);
  pull_.target = 0.0f;
  pull_.Step(config::kBinPull, dt);
  badge_.target = 0.0f;
  badge_.Step(config::kDropBadgeOut, dt);

  const float dx = x_.value - x_.target;
  const float dy = y_.value - y_.target;
  const float distance = std::sqrt(dx * dx + dy * dy);
  lift_.target = std::min(distance / (config::kDragLandingZone * dip_), 1.0f);
  lift_.Step(config::kFade, dt);

  const bool arrived = distance < 0.3f && std::fabs(x_.velocity) + std::fabs(y_.velocity) < 8.0f &&
                       std::fabs(scale_.value - scale_.target) < 0.0012f && std::fabs(tilt_.value) < 0.02f &&
                       pull_.value < 0.002f;
  if (arrived) {
    deck_.Land(landing_card_);
    phase_ = Phase::kDone;
  }
}

void DragSession::Render() {
  const float shadow_base = config::kDragShadowRest + (1.0f - config::kDragShadowRest) * std::clamp(lift_.value, 0.0f, 1.0f);
  const float vanish = std::clamp(vanish_.value, 0.0f, 1.0f);

  SpritePose pose{};
  pose.x = x_.value;
  pose.y = y_.value;
  pose.scale = scale_.value * (1.0f - config::kDropVanishShrink * vanish);
  pose.angle = tilt_.value * kDegreesToRadians;
  pose.pull = std::max(pull_.value, 0.0f);
  pose.pull_x = pull_x_;
  pose.pull_y = pull_y_;
  // Fully inside the bin it stays out of sight until drawn back out.
  pose.opacity = std::clamp(opacity_.value, 0.0f, 1.0f) * std::clamp((1.0f - pull_.value) / config::kBinHideMargin, 0.0f, 1.0f);
  // The shadow keeps the card's flat shape, so it is gone before the bending shows against it.
  pose.shadow = shadow_base * std::clamp(1.0f - (pose.pull - config::kBinMagnetPull) * config::kBinShadowFade, 0.0f, 1.0f) *
                std::clamp(1.0f - vanish * config::kDropShadowFade, 0.0f, 1.0f);
  pose.shadow_free = lift_.value;
  pose.shadow_drop = std::clamp(lift_.value, 0.0f, 1.0f) * config::kDragShadowDrop * dip_;
  pose.badge = badge_.value * (1.0f + config::kDropBadgeGrow * vanish);
  sprite_->Render(pose);
}

bool DragSession::Frame(const TimeBase& now) {
  const float dt = std::clamp(static_cast<float>(now.seconds - last_time_), 0.0f, config::kMaxFrameTime);
  last_time_ = now.seconds;
  switch (phase_) {
    case Phase::kHeld: StepHeld(dt); break;
    case Phase::kVanishing: StepVanishing(dt); break;
    case Phase::kSwallowed: StepSwallowed(dt); break;
    case Phase::kReturning: StepReturning(dt); break;
    case Phase::kDone: break;
  }
  if (phase_ == Phase::kDone) return false;
  EnsureViews();
  Render();
  return true;
}

bool DragSession::Finish(const FileDragOutcome& outcome, const TimeBase& now) {
  const bool dropped = outcome.effect != DROPEFFECT_NONE;
  const bool into_bin = !dropped && !outcome.cancelled && bin_deck_ != nullptr && bin_deck_->BinContains(outcome.release);
  Trace("drag: released dropped %d bin %d cancelled %d", dropped, into_bin, outcome.cancelled);

  if (into_bin) {
    phase_ = Phase::kSwallowed;
    pull_.target = 1.0f;
    tilt_.target = 0.0f;
    badge_.Snap(0.0f);
    if (store_.Discard(shot_)) PostMessageW(overlay_.controller_, kMsgClearClipboard, 0, 0);
    return true;
  }

  if (bin_deck_ != nullptr) bin_deck_->HideBin();
  if (dropped) {
    phase_ = Phase::kVanishing;
    vanish_.target = 1.0f;
    return true;
  }

  phase_ = Phase::kReturning;
  x_.velocity = pointer_vx_;
  y_.velocity = pointer_vy_;
  landing_card_ = card_.get();
  landing_card_->floating = true;
  deck_.TakeBack(std::move(card_), index_, now);
  return false;
}

LRESULT Deck::HandleSinkMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  if (dragging_ && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) return 0;
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

Overlay::~Overlay() {
  CancelDrags();
  file_drag_.reset();
  decks_.clear();
}

Deck* Overlay::DeckFor(HMONITOR monitor) {
  for (auto& deck : decks_) {
    if (deck->monitor() == monitor) return deck.get();
  }
  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info)) return nullptr;
  auto deck = std::make_unique<Deck>(*this, compositor_, store_, monitor, info.rcMonitor);
  if (!deck->ready()) return nullptr;
  if (session_) deck->SetDragActive(true, compositor_.Now());
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
  SetTimer(controller_, kTickTimer, config::kTickIntervalMs, nullptr);
}

void Overlay::OnFlash(HMONITOR monitor) {
  if (!compositor_.IsHealthy()) {
    // DWM restarted underneath us: every composition object is gone, so start over.
    CancelDrags();
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
    SyncHookMode();
  }
}

void Overlay::SyncHookMode() {
  HookMode mode = HookMode::kIdle;
  if (session_) {
    mode = HookMode::kDrag;
  } else if (std::any_of(decks_.begin(), decks_.end(), [](const auto& deck) { return deck->HasOptions(); })) {
    mode = HookMode::kSelector;
  }
  if (mode == hook_mode_) return;
  hook_mode_ = mode;
  SetHookMode(mode);
}

void Overlay::OnSelectorStep(int direction) {
  Trace("selector: step %d", direction);
  if (session_) return;
  POINT cursor{};
  GetCursorPos(&cursor);
  const HMONITOR under = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  Deck* chosen = nullptr;
  for (auto& deck : decks_) {
    if (deck->HasOptions() && (chosen == nullptr || deck->monitor() == under)) chosen = deck.get();
  }
  if (chosen != nullptr) chosen->StepRegion(direction);
}

void Overlay::BeginDrag(Deck& deck, std::unique_ptr<Card> card, size_t index, POINT press, const TimeBase& now) {
  const double started = NowSeconds();
  for (auto& each : decks_) each->SetDragActive(true, now);
  const uint64_t id = next_drag_id_++;
  session_ = std::make_unique<DragSession>(*this, compositor_, store_, deck, std::move(card), index, press, id, now);
  if (!file_drag_) file_drag_ = std::make_unique<FileDragThread>(controller_, store_);
  file_drag_->Begin(session_->shot(), id);
  last_frame_ = now.seconds;
  session_->Frame(now);
  pacer_.Start(controller_, kMsgDragFrame);
  EnsureTicking();
  SyncHookMode();
  Trace("drag: lifted in %.1f ms", (NowSeconds() - started) * 1000.0);
}

void Overlay::CancelDrags() {
  if (file_drag_) file_drag_->Cancel();
  session_.reset();
  finishing_.clear();
  pacer_.Stop();
}

void Overlay::OnDragFrame() {
  pacer_.Acknowledge();
  const TimeBase now = compositor_.Now();
  const float dt = std::clamp(static_cast<float>(now.seconds - last_frame_), 0.0f, config::kMaxFrameTime);
  last_frame_ = now.seconds;

  if (session_) session_->Frame(now);
  finishing_.erase(std::remove_if(finishing_.begin(), finishing_.end(), [&](const auto& each) { return !each->Frame(now); }),
                   finishing_.end());
  bool bins_moving = false;
  for (auto& deck : decks_) bins_moving = deck->FrameBin(dt) || bins_moving;
  compositor_.Commit();

  if (!session_ && finishing_.empty() && !bins_moving) pacer_.Stop();
}

void Overlay::OnDragFeedback(uint64_t id, bool accepts) {
  if (session_ && session_->id() == id) session_->SetAccepts(accepts);
}

void Overlay::OnDragFinished(std::unique_ptr<FileDragOutcome> outcome) {
  // A drag called off from here has already ended, and the loop is only now reporting back.
  if (session_ && session_->id() == outcome->id) EndDrag(*outcome);
}

void Overlay::OnDragCancel() {
  if (!session_) return;
  Trace("drag: called off");
  file_drag_->Cancel();
  FileDragOutcome outcome;
  outcome.id = session_->id();
  outcome.cancelled = true;
  GetCursorPos(&outcome.release);
  EndDrag(outcome);
}

void Overlay::EndDrag(const FileDragOutcome& outcome) {
  const TimeBase now = compositor_.Now();
  std::unique_ptr<DragSession> session = std::move(session_);
  const bool removed = session->Finish(outcome, now);
  finishing_.push_back(std::move(session));
  for (auto& deck : decks_) {
    if (removed) deck->ExtendDwell(now.seconds);
    deck->SetDragActive(false, now);
  }
  SyncHookMode();
  compositor_.Commit();
}

void Overlay::OnTick() {
  bool any_active = session_ != nullptr || !finishing_.empty() || pacer_.running();
  const TimeBase now = compositor_.Now();
  for (auto& deck : decks_) {
    if (!deck->IsIdle()) {
      deck->Tick();
      any_active = any_active || !deck->IsIdle();
    } else if (session_) {
      deck->PollInsets(now);
    }
  }
  SyncHookMode();
  if (any_active) return;

  for (auto& deck : decks_) deck->Rest();
  KillTimer(controller_, kTickTimer);
  ticking_ = false;
  active_ = false;
}

void Overlay::OnDisplayChange() {
  CancelDrags();
  // Geometry is stale for every monitor, so everything on screen is dropped; files and clipboard are already safe.
  decks_.clear();
  SyncHookMode();
  compositor_.Commit();
  if (ticking_) {
    KillTimer(controller_, kTickTimer);
    ticking_ = false;
  }
  active_ = false;
}

bool Overlay::IsIdle() const { return !active_; }

}  // namespace sstk
