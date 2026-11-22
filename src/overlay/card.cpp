#include "sstk/overlay/card.h"

#include "sstk/base/settings.h"
#include "sstk/config/animation.h"
#include "sstk/config/colors.h"
#include "sstk/config/core.h"
#include "sstk/config/layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sstk {

namespace {

bool NearlyEqual(const RECT& a, const RECT& b) {
  return std::abs(a.left - b.left) <= config::kSameRectTolerance && std::abs(a.top - b.top) <= config::kSameRectTolerance &&
         std::abs(a.right - b.right) <= config::kSameRectTolerance && std::abs(a.bottom - b.bottom) <= config::kSameRectTolerance;
}

}  // namespace

CardShadow BuildCardShadow(Compositor& compositor, float scale) {
  CardShadow result;
  const config::ShadowSpec& ambient = config::kCardShadowAmbient;
  const config::ShadowSpec& contact = config::kCardShadowContact;
  const float largest_offset = std::max(ambient.offset, contact.offset) * scale;
  // Past this distance from a corner the shadow no longer varies along the edge.
  const int reach = static_cast<int>(std::ceil(config::kCardRadius * scale + largest_offset));
  const int outer = static_cast<int>(std::ceil(ambient.sigma * 3.0f * scale + largest_offset));
  const ShadowLayer layers[] = {
      {ambient.sigma * scale, ambient.alpha, ambient.offset * scale},
      {contact.sigma * scale, contact.alpha, contact.offset * scale},
  };
  const int box = reach * 2 + 1;
  Image tile = RenderShadow(box, box, config::kCardRadius * scale, static_cast<float>(outer), layers, ARRAYSIZE(layers));
  if (!tile) return result;

  const int corner = outer + reach;
  const int far_side = corner + 1;
  auto slice = [&](int left, int top, int width, int height) {
    return compositor.CreateSurface(CopyRect(tile, left, top, width, height), false);
  };
  result.corners[0] = slice(0, 0, corner, corner);
  result.corners[1] = slice(far_side, 0, corner, corner);
  result.corners[2] = slice(0, far_side, corner, corner);
  result.corners[3] = slice(far_side, far_side, corner, corner);
  result.edges[0] = slice(corner, 0, 1, corner);
  result.edges[1] = slice(corner, far_side, 1, corner);
  result.edges[2] = slice(0, corner, corner, 1);
  result.edges[3] = slice(far_side, corner, corner, 1);
  result.outer = static_cast<float>(outer);
  result.reach = static_cast<float>(reach);
  return result;
}

Card::Card(Compositor& compositor, IDCompositionVisual* parent, std::shared_ptr<Shot> shot, const CaptureResult& capture,
           float scale, ComPtr<IDCompositionSurface> frost, const CardShadow& shadow, IDCompositionClip* shadow_clip,
           double created_at)
    : compositor_(compositor), parent_(parent), shot_(std::move(shot)), regions_(capture.screen.regions), insets_(capture.screen.insets),
      scale_(scale), border_(std::max(std::round(config::kCardBorder * scale), 1.0f)), frost_(std::move(frost)),
      sources_(regions_.size()), capture_(capture.id), taken_(capture.trigger_time), app_(capture.screen.app),
      app_rect_(capture.screen.app_rect), app_bounds_(capture.screen.app_bounds) {
  const Image& frame = *shot_->frame;

  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);

  frame_ = compositor_.CreateVisual();
  frame_clip_ = compositor_.CreateClip();
  frame_->SetClip(frame_clip_.Get());
  frame_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);

  // The border is a solid layer showing through the gap between two nested rounded clips,
  // which keeps it the same width at every size the card passes through.
  const uint32_t ring_color = static_cast<uint32_t>(config::kCardBorderAlpha * 255.0f) << 24 | config::kCardBorderColor;
  ring_ = compositor_.CreateSolidVisual(ring_color, static_cast<float>(frame.width()), static_cast<float>(frame.height()));
  ring_effect_ = compositor_.CreateEffectGroup();
  ring_->SetEffect(ring_effect_.Get());
  frame_->AddVisual(ring_.Get(), FALSE, nullptr);

  inner_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_->SetClip(inner_clip_.Get());
  inner_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  frame_->AddVisual(inner_.Get(), FALSE, nullptr);

  plane_ = compositor_.CreateVisual();
  plane_scaled_ = compositor_.CreateVisual();
  plane_scale_ = compositor_.CreateScale();
  plane_scaled_->SetTransform(plane_scale_.Get());
  plane_->AddVisual(plane_scaled_.Get(), FALSE, nullptr);
  inner_->AddVisual(plane_.Get(), FALSE, nullptr);

  full_surface_ = compositor_.CreateSurface(frame, true);
  full_visual_ = compositor_.CreateVisual();
  full_visual_->SetContent(full_surface_.Get());
  plane_scaled_->AddVisual(full_visual_.Get(), FALSE, nullptr);
  rims_ = compositor_.CreateVisual();
  plane_scaled_->AddVisual(rims_.Get(), TRUE, nullptr);

  auto* clip = frame_clip_.Get();
  auto* inner = inner_clip_.Get();
  left_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetLeft));
  left_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetLeft), border_);
  top_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetTop));
  top_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetTop), border_);
  right_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  right_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetRight), -border_);
  bottom_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  bottom_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetBottom), -border_);
  radius_.Bind(clip, SetClipRadii);
  radius_.Bind(inner, SetClipRadii, -border_);
  plane_x_.Bind(plane_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  plane_y_.Bind(plane_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  plane_sx_.Bind(plane_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  plane_sy_.Bind(plane_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));
  opacity_.Bind(root_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  slide_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  ring_alpha_.Bind(ring_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));

  BuildShadowVisuals(shadow, shadow_clip);
  root_->AddVisual(frame_.Get(), FALSE, nullptr);

  parent_->AddVisual(root_.Get(), FALSE, nullptr);
  expiry = created_at + config::kFlightLanding + CurrentSettings().dwell_seconds;
  exit_end_time_ = 1e300;
}

Card::~Card() { Detach(); }

void Card::BuildShadowVisuals(const CardShadow& shadow, IDCompositionClip* clip) {
  shadow_holder_ = compositor_.CreateVisual();
  shadow_effect_ = compositor_.CreateEffectGroup();
  shadow_holder_->SetEffect(shadow_effect_.Get());
  shadow_holder_->SetClip(clip);
  shadow_alpha_.Bind(shadow_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  shadow_reach_ = shadow.reach;

  shadow_edge_x_ = compositor_.CreateScale();
  shadow_edge_y_ = compositor_.CreateScale();
  shadow_edge_w_.Bind(shadow_edge_x_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  shadow_edge_h_.Bind(shadow_edge_y_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));

  auto add_slice = [&](size_t index, IDCompositionSurface* surface) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateVisual();
    visual->SetContent(surface);
    // Neighboring slices share an edge, and a soft edge on each would leave a faint seam mid-animation.
    visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    shadow_holder_->AddVisual(visual.Get(), FALSE, nullptr);
    shadow_slices_[index] = visual;
    return visual.Get();
  };
  auto bind_x = [](AnimFloat& edge, IDCompositionVisual* visual, float bias) {
    edge.Bind(visual, SSTK_SETTER(IDCompositionVisual, SetOffsetX), bias);
  };
  auto bind_y = [](AnimFloat& edge, IDCompositionVisual* visual, float bias) {
    edge.Bind(visual, SSTK_SETTER(IDCompositionVisual, SetOffsetY), bias);
  };

  const float outer = shadow.outer;
  const float reach = shadow.reach;

  IDCompositionVisual* top_left = add_slice(0, shadow.corners[0].Get());
  bind_x(left_, top_left, -outer);
  bind_y(top_, top_left, -outer);
  IDCompositionVisual* top_right = add_slice(1, shadow.corners[1].Get());
  bind_x(right_, top_right, -reach);
  bind_y(top_, top_right, -outer);
  IDCompositionVisual* bottom_left = add_slice(2, shadow.corners[2].Get());
  bind_x(left_, bottom_left, -outer);
  bind_y(bottom_, bottom_left, -reach);
  IDCompositionVisual* bottom_right = add_slice(3, shadow.corners[3].Get());
  bind_x(right_, bottom_right, -reach);
  bind_y(bottom_, bottom_right, -reach);

  IDCompositionVisual* top_edge = add_slice(4, shadow.edges[0].Get());
  bind_x(left_, top_edge, reach);
  bind_y(top_, top_edge, -outer);
  top_edge->SetTransform(shadow_edge_x_.Get());
  IDCompositionVisual* bottom_edge = add_slice(5, shadow.edges[1].Get());
  bind_x(left_, bottom_edge, reach);
  bind_y(bottom_, bottom_edge, -reach);
  bottom_edge->SetTransform(shadow_edge_x_.Get());
  IDCompositionVisual* left_edge = add_slice(6, shadow.edges[2].Get());
  bind_x(left_, left_edge, -outer);
  bind_y(top_, left_edge, reach);
  left_edge->SetTransform(shadow_edge_y_.Get());
  IDCompositionVisual* right_edge = add_slice(7, shadow.edges[3].Get());
  bind_x(right_, right_edge, -reach);
  bind_y(top_, right_edge, reach);
  right_edge->SetTransform(shadow_edge_y_.Get());

  root_->AddVisual(shadow_holder_.Get(), FALSE, nullptr);
}

void Card::PlaceFullScreen() {
  const float width = static_cast<float>(shot_->frame->width());
  const float height = static_cast<float>(shot_->frame->height());
  left_.Snap(0.0f);
  top_.Snap(0.0f);
  right_.Snap(width);
  bottom_.Snap(height);
  // Starting at the border's width keeps the inner radius from ever going negative.
  radius_.Snap(border_);
  plane_x_.Snap(0.0f);
  plane_y_.Snap(0.0f);
  plane_sx_.Snap(1.0f);
  plane_sy_.Snap(1.0f);
  opacity_.Snap(1.0f);
  slide_.Snap(0.0f);
  ring_alpha_.Snap(0.0f);
  shadow_alpha_.Snap(0.0f);
  shadow_edge_w_.Snap(std::max(width - shadow_reach_ * 2.0f, 0.0f));
  shadow_edge_h_.Snap(std::max(height - shadow_reach_ * 2.0f, 0.0f));
  shadow_edge_x_->SetScaleY(1.0f);
  shadow_edge_y_->SetScaleX(1.0f);
}

void Card::BuildCaches(const Image& frame, bool with_base) {
  Image half = Halve(frame);
  if (!half) return;

  if (with_base && !base_surface_) {
    // Twice the density of the whole-screen thumbnail: linear filtering stays clean across the whole
    // zoom range of a region switch, without the shimmer of a larger copy or the blur of a smaller one.
    const SIZE thumbnail = ThumbnailSize(regions_.front().rect, scale_);
    const int width = std::min<int>(thumbnail.cx * 2, half.width());
    const int height = std::min<int>(thumbnail.cy * 2, half.height());
    if (width == half.width() && height == half.height()) {
      base_surface_ = compositor_.CreateSurface(half, true);
    } else {
      Image base = ResampleArea(half, 0.0, 0.0, half.width(), half.height(), width, height);
      base_surface_ = compositor_.CreateSurface(base, true);
    }
    if (base_surface_) {
      if (!base_visual_) {
        base_visual_ = compositor_.CreateVisual();
        base_effect_ = compositor_.CreateEffectGroup();
        base_visual_->SetEffect(base_effect_.Get());
        base_alpha_.Bind(base_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
        const D2D_MATRIX_3X2_F enlarge{static_cast<float>(frame.width()) / width, 0.0f, 0.0f,
                                       static_cast<float>(frame.height()) / height, 0.0f, 0.0f};
        base_visual_->SetTransform(enlarge);
        plane_scaled_->AddVisual(base_visual_.Get(), FALSE, nullptr);
      }
      base_visual_->SetContent(base_surface_.Get());
    }
  }

  crisp_surfaces_.resize(regions_.size());
  crisp_sizes_.resize(regions_.size());
  crisp_pixels_.resize(regions_.size());
  for (size_t i = 0; i < regions_.size(); ++i) {
    const RegionKind kind = regions_[i].kind;
    if (crisp_surfaces_[i] || kind == RegionKind::kScroll || kind == RegionKind::kLong) continue;
    const RECT& region = regions_[i].rect;
    const SIZE size = ThumbnailSize(region, scale_);
    if (const std::shared_ptr<const LongImage>& source = sources_[i]; source != nullptr) {
      SetCrisp(i, source->Resample(0.0, source->height(), size.cx, size.cy));
      continue;
    }
    const double region_w = region.right - region.left;
    const double region_h = region.bottom - region.top;
    // The border takes the outermost pixels, so the bitmap is mapped 1:1 onto the whole card and simply clipped by it.
    Image pixels;
    if (region_w / size.cx >= 2.0 && region_h / size.cy >= 2.0) {
      const double kx = static_cast<double>(half.width()) / frame.width();
      const double ky = static_cast<double>(half.height()) / frame.height();
      pixels = ResampleArea(half, region.left * kx, region.top * ky, region.right * kx, region.bottom * ky, size.cx, size.cy);
    } else {
      pixels = ResampleArea(frame, region.left, region.top, region.right, region.bottom, size.cx, size.cy);
    }
    SetCrisp(i, std::move(pixels));
  }
}

void Card::SetCrisp(size_t index, Image pixels) {
  if (!pixels) return;
  crisp_surfaces_[index] = compositor_.CreateSurface(pixels, true);
  crisp_sizes_[index] = SIZE{pixels.width(), pixels.height()};
  crisp_pixels_[index] = std::move(pixels);
}

bool Card::AttachWindows(std::vector<WindowShot>& shots, const TimeBase& now) {
  const int shown = CrispIndex(selected_);
  bool selection = false;
  for (WindowShot& shot : shots) {
    for (size_t i = 0; i < regions_.size(); ++i) {
      Region& region = regions_[i];
      const bool own = region.kind == RegionKind::kApp || region.kind == RegionKind::kZone || region.kind == RegionKind::kScroll;
      if (!own || region.window != shot.window || shot.image == nullptr) continue;
      region.rect = shot.rect;
      sources_[i] = shot.image;
      // The scroll option borrows its picture, and a card deep in the deck makes its own when it comes back.
      const bool kept = static_cast<int>(i) == shown;
      if (region.kind == RegionKind::kScroll || (hibernating_ && !kept)) continue;
      const SIZE size = ThumbnailSize(region.rect, scale_);
      if (shot.thumbnail.width() == size.cx && shot.thumbnail.height() == size.cy) {
        SetCrisp(i, std::move(shot.thumbnail));
      } else {
        SetCrisp(i, shot.image->Resample(0.0, shot.image->height(), size.cx, size.cy));
      }
      selection = selection || kept;
    }
  }
  if (selection && !ShowsLong(selected_)) ShowCrisp(selected_, now, 0.0, config::kMorphFade);
  return selection;
}

std::shared_ptr<const LongImage> Card::whole() const {
  if (regions_[selected_].kind == RegionKind::kLong) return long_image_;
  const int source = CrispIndex(selected_);
  return source >= 0 && source < static_cast<int>(sources_.size()) ? sources_[source] : nullptr;
}

void Card::BuildDerivedAssets(const TimeBase& now) {
  BuildCaches(*shot_->frame, true);
  if (base_surface_) {
    base_alpha_.Snap(0.0f);
    base_alpha_.To(device(), now, 1.0f, config::kQuickFade, config::kBaseFadeDelay);
  }
  full_release_at_ = now.seconds + config::kFullReleaseDelay;
  ShowCrisp(selected_, now, config::kCrispFlightDelay, config::kFade);
}

void Card::MoveTo(const RectF& rect, int depth, const SpringSpec& spec, const TimeBase& now, double delay, bool in_flight) {
  const RectF source = SourceRect();
  const float width = rect.width();
  const float height = rect.height();
  const float sx = width / source.width();
  const float sy = height / source.height();
  const float px = rect.left - source.left * sx;
  const float py = rect.top - source.top * sy;

  IDCompositionDevice* dc = device();
  radius_.To(dc, now, config::kCardRadius * scale_, spec, delay);
  if (depth != 0 || delay > 0.0 || in_flight || !MoveThrough(rect, source, spec, now)) {
    left_.To(dc, now, rect.left, spec, delay);
    top_.To(dc, now, rect.top, spec, delay);
    right_.To(dc, now, rect.right, spec, delay);
    bottom_.To(dc, now, rect.bottom, spec, delay);
    plane_x_.To(dc, now, px, spec, delay);
    plane_y_.To(dc, now, py, spec, delay);
    plane_sx_.To(dc, now, sx, spec, delay);
    plane_sy_.To(dc, now, sy, spec, delay);
    shadow_edge_w_.To(dc, now, std::max(width - shadow_reach_ * 2.0f, 0.0f), spec, delay);
    shadow_edge_h_.To(dc, now, std::max(height - shadow_reach_ * 2.0f, 0.0f), spec, delay);
  }

  const bool shown = depth <= config::kDeckVisibleBehind;
  const double decoration_delay = in_flight ? config::kFlightDecorationDelay : 0.0;
  ring_alpha_.To(dc, now, 1.0f, config::kFade, decoration_delay);
  // A card narrower than two shadow corners casts a proportionally weaker shadow, and the corners overlap.
  const float slim = std::clamp(std::min(width, height) / (shadow_reach_ * 2.0f), 0.5f, 1.0f);
  const float shadow_target = !shown ? 0.0f : (depth == 0 ? 1.0f : config::kBehindShadowAlpha) * slim;
  shadow_alpha_.To(dc, now, shadow_target, config::kFade, decoration_delay);
  resting_opacity_ = shown ? 1.0f - depth * config::kDeckOpacityPerLevel : 0.0f;
  opacity_.To(dc, now, resting_opacity_, config::kFade);
}

RectF Card::Screen() const {
  const RECT& full = regions_.front().rect;
  return RectF{static_cast<float>(full.left), static_cast<float>(full.top), static_cast<float>(full.right), static_cast<float>(full.bottom)};
}

bool Card::OwnPicture(int index) const {
  if (ShowsLong(index)) return true;
  const int crisp = CrispIndex(index);
  return crisp >= 0 && crisp < static_cast<int>(sources_.size()) && sources_[crisp] != nullptr;
}

RectF Card::Valid(int index) const {
  if (ShowsLong(index)) {
    const double stretch = 1.0 / long_.scale;
    return RectF{static_cast<float>(long_.area.left), static_cast<float>(long_.area.top + (long_.content_top - kLongOrigin) * stretch),
                 static_cast<float>(long_.area.right), static_cast<float>(long_.area.top + (long_.content_bottom - kLongOrigin) * stretch)};
  }
  if (!OwnPicture(index)) return Screen();
  const RECT& rect = regions_[CrispIndex(index)].rect;
  return RectF{static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right), static_cast<float>(rect.bottom)};
}

namespace {

bool Inside(const RectF& inner, const RectF& outer) {
  constexpr float kSlack = 1.0f;
  return inner.left >= outer.left - kSlack && inner.top >= outer.top - kSlack && inner.right <= outer.right + kSlack && inner.bottom <= outer.bottom + kSlack;
}

RectF Meet(const RectF& a, const RectF& b) {
  return RectF{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

float Travel(const RectF& a, const RectF& b) {
  return std::fabs(a.left - b.left) + std::fabs(a.top - b.top) + std::fabs(a.right - b.right) + std::fabs(a.bottom - b.bottom);
}

}  // namespace

// A view reaching past the screen, now or once there, goes there only within a picture that has something to show
// there: it first comes back within the screen, then crosses it, and only then goes out into where it is heading. Each
// stretch keeps every edge between its ends, so the view never leaves the picture it is in. Returns false where it can
// go straight there.
bool Card::MoveThrough(const RectF& rect, const RectF& source, const SpringSpec& spec, const TimeBase& now) {
  const double t = now.seconds;
  const float scale_x = plane_sx_.ValueAt(t);
  const float scale_y = plane_sy_.ValueAt(t);
  if (scale_x <= 1e-6f || scale_y <= 1e-6f) return false;
  const RectF card{left_.ValueAt(t), top_.ValueAt(t), right_.ValueAt(t), bottom_.ValueAt(t)};
  const float offset_x = plane_x_.ValueAt(t);
  const float offset_y = plane_y_.ValueAt(t);
  const RectF from{(card.left - offset_x) / scale_x, (card.top - offset_y) / scale_y, (card.right - offset_x) / scale_x, (card.bottom - offset_y) / scale_y};
  const RectF screen = Screen();
  if (Inside(from, Valid(selected_)) || (Inside(from, screen) && Inside(source, screen))) return false;

  std::array<RectF, 3> views{};
  std::array<RectF, 3> cards{};
  std::array<float, 3> at{};
  int count = 0;
  float total = 0.0f;
  RectF last = from;
  const auto add = [&](const RectF& view, bool final) {
    const float travel = Travel(last, view);
    if (!final && travel < 1.0f) return;
    views[static_cast<size_t>(count)] = view;
    at[static_cast<size_t>(count)] = total + travel;
    total += travel;
    last = view;
    ++count;
  };
  if (!Inside(from, screen)) add(Meet(from, screen), false);
  if (!Inside(source, screen)) add(Meet(source, screen), false);
  add(source, true);
  if (count < 2 || total < 1.0f) return false;
  for (int i = 0; i < count; ++i) {
    const RectF& view = views[static_cast<size_t>(i)];
    if (view.width() < 1.0f || view.height() < 1.0f) return false;
    at[static_cast<size_t>(i)] /= total;
    if (i + 1 == count) {
      at[static_cast<size_t>(i)] = 1.0f;
      cards[static_cast<size_t>(i)] = rect;
      continue;
    }
    // On the way the card takes each view's own shape, kept where the deck keeps its front card.
    const SIZE size = ThumbnailSize(RECT{static_cast<LONG>(std::lround(view.left)), static_cast<LONG>(std::lround(view.top)),
                                         static_cast<LONG>(std::lround(view.right)), static_cast<LONG>(std::lround(view.bottom))}, scale_);
    cards[static_cast<size_t>(i)] = RectF{rect.left, rect.bottom - static_cast<float>(size.cy), rect.left + static_cast<float>(size.cx), rect.bottom};
    if (i > 0 && at[static_cast<size_t>(i)] <= at[static_cast<size_t>(i) - 1] + 1e-3f) return false;
  }

  // The progress the card already has along the first stretch, so that a change of mind mid-flight carries on smoothly.
  const float first = std::max(at[0], 1e-3f);
  const RectF& first_card = cards[0];
  const float first_px = first_card.left - views[0].left * (first_card.width() / views[0].width());
  const float first_py = first_card.top - views[0].top * (first_card.height() / views[0].height());
  const float pace[6] = {(first_card.left - card.left) / first, (first_card.top - card.top) / first, (first_card.right - card.right) / first,
                         (first_card.bottom - card.bottom) / first, (first_px - offset_x) / first, (first_py - offset_y) / first};
  const float moving[6] = {left_.VelocityAt(t), top_.VelocityAt(t), right_.VelocityAt(t), bottom_.VelocityAt(t), plane_x_.VelocityAt(t), plane_y_.VelocityAt(t)};
  float along = 0.0f;
  float length = 0.0f;
  for (int i = 0; i < 6; ++i) {
    along += pace[i] * moving[i];
    length += pace[i] * pace[i];
  }
  // A path keeps to its stretches only on a spring that never overshoots.
  const SpringSpec path{spec.response, std::max(spec.damping, 1.0f)};
  const float omega = 6.2831853f / path.response;
  const float progress_velocity = length > 1e-6f ? std::clamp(along / length, 0.0f, omega) : 0.0f;

  IDCompositionDevice* dc = device();
  float stops[3];
  const auto go = [&](AnimFloat& value, auto&& stop) {
    for (int i = 0; i < count; ++i) stops[i] = stop(cards[static_cast<size_t>(i)], views[static_cast<size_t>(i)]);
    value.Through(dc, now, stops, at.data(), count, path, progress_velocity);
  };
  go(left_, [](const RectF& c, const RectF&) { return c.left; });
  go(top_, [](const RectF& c, const RectF&) { return c.top; });
  go(right_, [](const RectF& c, const RectF&) { return c.right; });
  go(bottom_, [](const RectF& c, const RectF&) { return c.bottom; });
  go(plane_sx_, [](const RectF& c, const RectF& v) { return c.width() / v.width(); });
  go(plane_sy_, [](const RectF& c, const RectF& v) { return c.height() / v.height(); });
  go(plane_x_, [](const RectF& c, const RectF& v) { return c.left - v.left * (c.width() / v.width()); });
  go(plane_y_, [](const RectF& c, const RectF& v) { return c.top - v.top * (c.height() / v.height()); });
  const float reach = shadow_reach_ * 2.0f;
  go(shadow_edge_w_, [reach](const RectF& c, const RectF&) { return std::max(c.width() - reach, 0.0f); });
  go(shadow_edge_h_, [reach](const RectF& c, const RectF&) { return std::max(c.height() - reach, 0.0f); });

  // Pictures being left keep what they show past the screen until the view is back within it.
  const double end = left_.end_time();
  for (auto& layer : crisp_layers_) {
    if (layer->rim && layer->retire_at != 0.0) layer->retire_at = std::max(layer->retire_at, end);
  }
  return true;
}

void Card::ShowCrisp(int index, const TimeBase& now, double delay, const SpringSpec& spec) {
  IDCompositionSurface* surface = nullptr;
  D2D_MATRIX_3X2_F place{};
  if (ShowsLong(index)) {
    // Thumbnail row kLongOrigin is the scrolled area's first row, and the rest follow below and above it.
    const float stretch = static_cast<float>(1.0 / long_.scale);
    surface = long_.surface.Get();
    place = D2D_MATRIX_3X2_F{stretch, 0.0f, 0.0f, stretch, static_cast<float>(long_.surface_left),
                             static_cast<float>(long_.area.top - kLongOrigin / long_.scale)};
  } else {
    const int crisp = CrispIndex(index);
    if (crisp < 0 || crisp >= static_cast<int>(crisp_surfaces_.size()) || !crisp_surfaces_[crisp]) return;
    const RECT& region = regions_[crisp].rect;
    const SIZE size = crisp_sizes_[crisp];
    surface = crisp_surfaces_[crisp].Get();
    place = D2D_MATRIX_3X2_F{static_cast<float>(region.right - region.left) / size.cx, 0.0f, 0.0f,
                             static_cast<float>(region.bottom - region.top) / size.cy, static_cast<float>(region.left),
                             static_cast<float>(region.top)};
  }

  auto layer = std::make_unique<CrispLayer>();
  layer->visual = compositor_.CreateVisual();
  layer->effect = compositor_.CreateEffectGroup();
  layer->visual->SetContent(surface);
  layer->visual->SetEffect(layer->effect.Get());
  layer->visual->SetTransform(place);
  layer->alpha.Bind(layer->effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  layer->alpha.Snap(0.0f);
  layer->alpha.To(device(), now, 1.0f, spec, delay);
  plane_scaled_->AddVisual(layer->visual.Get(), FALSE, nullptr);
  if (OwnPicture(index)) {
    layer->rim = compositor_.CreateVisual();
    layer->rim->SetContent(surface);
    layer->rim->SetTransform(place);
    rims_->AddVisual(layer->rim.Get(), FALSE, nullptr);
  }

  for (auto& previous : crisp_layers_) {
    if (previous->retire_at == 0.0) {
      previous->alpha.To(device(), now, 0.0f, spec, delay);
      previous->retire_at = previous->alpha.end_time();
    }
  }
  crisp_layers_.push_back(std::move(layer));
}

bool Card::CanSelect(int index) const {
  if (hibernating_ || index < 0 || index >= static_cast<int>(regions_.size())) return false;
  if (regions_[index].kind == RegionKind::kLong) return long_.surface != nullptr;
  const int crisp = CrispIndex(index);
  return crisp >= 0 && crisp < static_cast<int>(crisp_surfaces_.size()) && crisp_surfaces_[crisp];
}

void Card::Select(int index, const TimeBase& now) {
  if (regions_[index].kind == RegionKind::kScroll && regions_[selected_].kind != RegionKind::kScroll) before_scroll_ = selected_;
  selected_ = index;
  ShowCrisp(index, now, config::kCrispMorphDelay, config::kMorphFade);
}

int Card::CrispIndex(int index) const { return regions_[index].kind == RegionKind::kScroll ? scroll_source_ : index; }

bool Card::ShowsLong(int index) const {
  const RegionKind kind = regions_[index].kind;
  return long_.surface && (kind == RegionKind::kScroll || kind == RegionKind::kLong);
}

SIZE Card::DisplaySize() const {
  if (ShowsLong(selected_)) return SIZE{long_.view_width, std::max(long_.view_bottom - long_.view_top, 1)};
  return ThumbnailSize(regions_[selected_].rect, scale_);
}

RectF Card::SourceRect() const {
  if (ShowsLong(selected_)) {
    const double stretch = 1.0 / long_.scale;
    const double top = long_.area.top + (long_.view_top - kLongOrigin) * stretch;
    const double bottom = long_.area.top + (long_.view_bottom - kLongOrigin) * stretch;
    return RectF{static_cast<float>(long_.area.left), static_cast<float>(top), static_cast<float>(long_.area.right),
                 static_cast<float>(bottom)};
  }
  const RECT& rect = regions_[selected_].rect;
  return RectF{static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right),
               static_cast<float>(rect.bottom)};
}

bool Card::OfferScroll(ScrollHints hints) {
  if (!scroll_open()) return false;
  if (scroll_source_ >= 0) {
    // A later look at the app saw more.
    if (regions_.back().kind == RegionKind::kScroll) scroll_hints_ = std::move(hints);
    return false;
  }
  // The option showing the app window, or one showing the same as it does, as the whole screen may.
  int source = -1;
  for (size_t i = 0; i < regions_.size() && source < 0; ++i) {
    if (regions_[i].window == app_) source = static_cast<int>(i);
  }
  for (size_t i = 0; i < regions_.size() && source < 0; ++i) {
    if (regions_[i].window == nullptr && NearlyEqual(regions_[i].rect, app_rect_)) source = static_cast<int>(i);
  }
  if (source < 0) return false;
  scroll_source_ = source;
  scroll_hints_ = std::move(hints);
  regions_.push_back(Region{RegionKind::kScroll, regions_[source].rect, regions_[source].window});
  crisp_surfaces_.resize(regions_.size());
  crisp_sizes_.resize(regions_.size());
  crisp_pixels_.resize(regions_.size());
  sources_.push_back(sources_[source]);
  return true;
}

bool Card::AppChanged() const {
  RECT rect{};
  return !IsWindow(app_) || IsIconic(app_) || !GetWindowRect(app_, &rect) || !EqualRect(&rect, &app_bounds_);
}

bool Card::WithdrawScroll() {
  if (!scroll_open()) return false;
  scroll_closed_ = true;
  if (busy || long_.surface || scroll_source_ < 0 || regions_.back().kind != RegionKind::kScroll) return false;
  // The scroll option shows the app window, which stays.
  if (selected_ == static_cast<int>(regions_.size()) - 1) selected_ = scroll_source_;
  PopOption();
  return true;
}

void Card::PopOption() {
  regions_.pop_back();
  crisp_surfaces_.pop_back();
  crisp_sizes_.pop_back();
  crisp_pixels_.pop_back();
  sources_.pop_back();
}

void Card::SetLongView(int content_top, int content_bottom, int view_bottom) {
  long_.content_top = content_top;
  long_.content_bottom = content_bottom;
  const int rows = std::max(std::min(content_bottom - content_top, long_.max_rows), 1);
  long_.view_bottom = std::clamp(view_bottom, content_top + rows, std::max(content_bottom, content_top + rows));
  long_.view_top = long_.view_bottom - rows;
}

bool Card::BeginLong(const ScrollUpdate& update, int max_rows, const TimeBase& now) {
  long_.surface = compositor_.CreateVirtualSurface(update.surface_width, kLongRows, true);
  if (!long_.surface) return false;
  long_.area = update.area;
  long_.surface_left = update.surface_left;
  long_.view_width = update.thumb_width;
  long_.scale = static_cast<double>(update.thumb_width) / (update.area.right - update.area.left);
  long_.max_rows = max_rows;
  DrawLong(update);
  ShowCrisp(selected_, now, config::kCrispMorphDelay, config::kMorphFade);
  return true;
}

void Card::DrawLong(const ScrollUpdate& update) {
  if (!long_.surface) return;
  for (const ScrollPiece& piece : update.pieces) {
    if (!piece.rows) continue;
    compositor_.Upload(long_.surface.Get(), POINT{0, piece.at}, piece.rows);
    if (!long_.done) KeepDrawn(piece);
  }
  SetLongView(update.content_top, update.content_bottom, update.view_bottom);
}

void Card::KeepDrawn(const ScrollPiece& piece) {
  const Image& rows = piece.rows;
  if (rows.width() != long_.view_width) return;
  if (long_.drawn.empty()) long_.drawn_top = piece.at;
  for (; piece.at < long_.drawn_top; --long_.drawn_top) long_.drawn.emplace_front();
  while (long_.drawn_top + static_cast<int>(long_.drawn.size()) < piece.at + rows.height()) long_.drawn.emplace_back();
  for (int y = 0; y < rows.height(); ++y) {
    const auto* source = reinterpret_cast<const uint32_t*>(rows.data() + static_cast<ptrdiff_t>(y) * rows.stride());
    long_.drawn[static_cast<size_t>(piece.at + y - long_.drawn_top)].assign(source, source + rows.width());
  }
}

void Card::FreezeView() {
  if (!ShowsLong(selected_) || long_.done) return;
  Image view = Image::Allocate(long_.view_width, std::max(long_.view_bottom - long_.view_top, 1));
  if (!view) return;
  for (int y = 0; y < view.height(); ++y) {
    auto* out = reinterpret_cast<uint32_t*>(view.data() + static_cast<ptrdiff_t>(y) * view.stride());
    const int at = long_.view_top + y - long_.drawn_top;
    const bool known = at >= 0 && at < static_cast<int>(long_.drawn.size()) && long_.drawn[static_cast<size_t>(at)].size() == static_cast<size_t>(view.width());
    if (known) {
      std::copy(long_.drawn[static_cast<size_t>(at)].begin(), long_.drawn[static_cast<size_t>(at)].end(), out);
    } else {
      std::fill(out, out + view.width(), 0xFF000000u);
    }
  }
  long_.pixels = std::move(view);
}

void Card::KeepLong(const ScrollUpdate& update) {
  const int index = static_cast<int>(regions_.size()) - 1;
  regions_[index].kind = RegionKind::kLong;
  long_size_ = SIZE{update.image->width(), update.image->height()};
  long_.area = update.area;
  long_.view_width = std::max(static_cast<int>(std::lround((update.area.right - update.area.left) * long_.scale)), 1);
  // The last rows come with the image. Finished, it shows from the top, as the image does when opened.
  for (const ScrollPiece& piece : update.pieces) {
    if (piece.rows && long_.surface) compositor_.Upload(long_.surface.Get(), POINT{0, piece.at}, piece.rows);
  }
  SetLongView(update.content_top, update.content_bottom, update.content_top);
  const double stretch = 1.0 / long_.scale;
  const double skipped = update.image_rect.top - long_.area.top;
  const double top = (long_.view_top - kLongOrigin) * stretch - skipped;
  const double bottom = (long_.view_bottom - kLongOrigin) * stretch - skipped;
  long_.pixels = update.image->Resample(top, bottom, long_.view_width, long_.view_bottom - long_.view_top);
  long_.done = true;
  long_.drawn = std::deque<std::vector<uint32_t>>{};
  long_image_ = update.image;
}

void Card::DropLong(const TimeBase& now) {
  long_ = Long{};
  // One image takes one scrolling screenshot, so the option goes with the attempt.
  if (scroll_source_ >= 0 && regions_.back().kind == RegionKind::kScroll) PopOption();
  selected_ = std::min(before_scroll_, static_cast<int>(regions_.size()) - 1);
  ShowCrisp(selected_, now, config::kCrispMorphDelay, config::kMorphFade);
}

float Card::long_height() const { return long_.surface ? static_cast<float>(long_.view_bottom - long_.view_top) : 0.0f; }

void Card::SetSwipe(float offset, float progress) {
  slide_.Snap(offset);
  opacity_.Snap(resting_opacity_ + (config::kSwipeEndOpacity - 1.0f) * progress);
}

void Card::SnapBack(const TimeBase& now, float velocity) {
  slide_.To(device(), now, 0.0f, config::kSnapBack, 0.0, velocity);
  opacity_.To(device(), now, resting_opacity_, config::kFade);
}

void Card::SlideOut(const TimeBase& now, float velocity) {
  // Leaving through the screen edge needs no fade of its own, only a slight loss of presence.
  const float distance = -(right_.target() + config::kExitOvershoot * scale_);
  slide_.To(device(), now, distance, config::kExit, 0.0, velocity);
  opacity_.To(device(), now, std::min(resting_opacity_, config::kExitOpacity), config::kExit);
  exit_end_time_ = std::max(slide_.end_time(), opacity_.end_time());
}

void Card::Detach() {
  if (!attached_) return;
  attached_ = false;
  parent_->RemoveVisual(root_.Get());
}

void Card::Attach() {
  if (attached_) return;
  attached_ = true;
  parent_->AddVisual(root_.Get(), FALSE, nullptr);
}

void Card::Raise() {
  if (!attached_) return;
  parent_->RemoveVisual(root_.Get());
  parent_->AddVisual(root_.Get(), FALSE, nullptr);
}

RectF Card::RectAt(double time) const {
  return RectF{left_.ValueAt(time), top_.ValueAt(time), right_.ValueAt(time), bottom_.ValueAt(time)};
}

RectF Card::target_rect() const { return RectF{left_.target(), top_.target(), right_.target(), bottom_.target()}; }

const Image* Card::thumbnail() const {
  if (ShowsLong(selected_)) return long_.pixels ? &long_.pixels : nullptr;
  const int crisp = CrispIndex(selected_);
  if (crisp < 0 || crisp >= static_cast<int>(crisp_pixels_.size()) || !crisp_pixels_[crisp]) return nullptr;
  return &crisp_pixels_[crisp];
}

void Card::Tick(double now, Store& store) {
  if (full_release_at_ != 0.0 && now >= full_release_at_) {
    full_release_at_ = 0.0;
    full_visual_->SetContent(nullptr);
    full_surface_.Reset();
    // With a single region there is nothing to switch to, so the base copy has done its job.
    if (regions_.size() < 2 && base_visual_) {
      base_visual_->SetContent(nullptr);
      base_surface_.Reset();
    }
  }
  // The thumbnails exist from here on, and later crops can come from the archive.
  if (!crisp_surfaces_.empty() && shot_->frame) store.ReleaseFrame(shot_);
  for (auto it = crisp_layers_.begin(); it != crisp_layers_.end();) {
    if ((*it)->retire_at != 0.0 && now >= (*it)->retire_at) {
      plane_scaled_->RemoveVisual((*it)->visual.Get());
      if ((*it)->rim) rims_->RemoveVisual((*it)->rim.Get());
      it = crisp_layers_.erase(it);
    } else {
      ++it;
    }
  }
}

void Card::Hibernate(Store& store) {
  // The crossfades of the flight need these layers, so a card still landing stays as it is.
  if (hibernating_ || full_release_at_ != 0.0 || crisp_surfaces_.empty()) return;
  if (!store.ReleaseFrame(shot_)) return;
  hibernating_ = true;
  const int kept = CrispIndex(selected_);
  for (size_t i = 0; i < crisp_surfaces_.size(); ++i) {
    if (static_cast<int>(i) != kept) {
      crisp_surfaces_[i].Reset();
      crisp_pixels_[i] = Image();
    }
  }
  if (base_visual_) base_visual_->SetContent(nullptr);
  base_surface_.Reset();
}

void Card::Wake(Store& store) {
  if (!hibernating_) return;
  std::shared_ptr<const Image> frame = store.AcquireFrame(shot_);
  if (!frame) return;
  hibernating_ = false;
  BuildCaches(*frame, regions_.size() >= 2);
}

}  // namespace sstk
