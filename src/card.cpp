#include "card.h"

#include "theme.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr int kShadowDownscale = 4;
constexpr float kDraggedOpacity = 0.4f;
constexpr float kBehindShadowAlpha = 0.5f;
constexpr double kFlightDecorationDelay = 0.22;
constexpr double kHalfFadeDelay = 0.07;
constexpr double kCrispFlightDelay = 0.26;
constexpr double kCrispMorphDelayFraction = 0.55;
constexpr double kFullReleaseDelay = 0.7;

void SetAllRadii(void* object, float value, IDCompositionAnimation* animation) {
  auto* clip = static_cast<IDCompositionRectangleClip*>(object);
  if (animation != nullptr) {
    clip->SetTopLeftRadiusX(animation);
    clip->SetTopLeftRadiusY(animation);
    clip->SetTopRightRadiusX(animation);
    clip->SetTopRightRadiusY(animation);
    clip->SetBottomLeftRadiusX(animation);
    clip->SetBottomLeftRadiusY(animation);
    clip->SetBottomRightRadiusX(animation);
    clip->SetBottomRightRadiusY(animation);
  } else {
    clip->SetTopLeftRadiusX(value);
    clip->SetTopLeftRadiusY(value);
    clip->SetTopRightRadiusX(value);
    clip->SetTopRightRadiusY(value);
    clip->SetBottomLeftRadiusX(value);
    clip->SetBottomLeftRadiusY(value);
    clip->SetBottomRightRadiusX(value);
    clip->SetBottomRightRadiusY(value);
  }
}

}  // namespace

Card::Card(Compositor& compositor, IDCompositionVisual* parent, std::shared_ptr<Shot> shot, std::vector<Region> regions,
           float scale, ComPtr<IDCompositionSurface> frost, double created_at)
    : compositor_(compositor), parent_(parent), shot_(std::move(shot)), regions_(std::move(regions)), scale_(scale),
      frost_(std::move(frost)) {
  const Image& frame = *shot_->frame;

  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);

  shadow_holder_ = compositor_.CreateVisual();
  shadow_visual_ = compositor_.CreateVisual();
  shadow_effect_ = compositor_.CreateEffectGroup();
  shadow_scale_ = compositor_.CreateScale();
  shadow_holder_->SetEffect(shadow_effect_.Get());
  shadow_visual_->SetTransform(shadow_scale_.Get());
  shadow_holder_->AddVisual(shadow_visual_.Get(), FALSE, nullptr);
  root_->AddVisual(shadow_holder_.Get(), FALSE, nullptr);

  frame_ = compositor_.CreateVisual();
  frame_clip_ = compositor_.CreateClip();
  frame_->SetClip(frame_clip_.Get());
  frame_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  root_->AddVisual(frame_.Get(), FALSE, nullptr);

  // The hairline is a solid layer showing through the gap between two nested rounded clips,
  // which keeps it exactly one pixel wide at every size the card passes through.
  const uint32_t ring_color = static_cast<uint32_t>(theme::kHairlineAlpha * 255.0f) << 24 | 0xFFFFFF;
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

  auto* clip = frame_clip_.Get();
  auto* inner = inner_clip_.Get();
  left_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetLeft));
  left_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetLeft), 1.0f);
  top_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetTop));
  top_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetTop), 1.0f);
  right_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  right_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetRight), -1.0f);
  bottom_.Bind(clip, SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  bottom_.Bind(inner, SSTK_SETTER(IDCompositionRectangleClip, SetBottom), -1.0f);
  radius_.Bind(clip, SetAllRadii);
  radius_.Bind(inner, SetAllRadii);
  plane_x_.Bind(plane_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  plane_y_.Bind(plane_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  plane_sx_.Bind(plane_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  plane_sy_.Bind(plane_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));
  opacity_.Bind(root_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  slide_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  ring_alpha_.Bind(ring_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  shadow_x_.Bind(shadow_holder_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  shadow_y_.Bind(shadow_holder_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  shadow_sx_.Bind(shadow_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  shadow_sy_.Bind(shadow_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));
  shadow_alpha_.Bind(shadow_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));

  parent_->AddVisual(root_.Get(), FALSE, nullptr);
  expiry = created_at + theme::kFlightLanding + theme::kDwellSeconds;
  exit_end_time_ = 1e300;
}

Card::~Card() { parent_->RemoveVisual(root_.Get()); }

void Card::PlaceFullScreen() {
  const float width = static_cast<float>(shot_->frame->width());
  const float height = static_cast<float>(shot_->frame->height());
  left_.Snap(0.0f);
  top_.Snap(0.0f);
  right_.Snap(width);
  bottom_.Snap(height);
  radius_.Snap(0.0f);
  plane_x_.Snap(0.0f);
  plane_y_.Snap(0.0f);
  plane_sx_.Snap(1.0f);
  plane_sy_.Snap(1.0f);
  opacity_.Snap(1.0f);
  slide_.Snap(0.0f);
  ring_alpha_.Snap(0.0f);
  shadow_alpha_.Snap(0.0f);
  shadow_x_.Snap(0.0f);
  shadow_y_.Snap(0.0f);
  shadow_sx_.Snap(1.0f);
  shadow_sy_.Snap(1.0f);
}

void Card::BuildDerivedAssets(const RectF& landing, const TimeBase& now) {
  const Image& frame = *shot_->frame;
  half_ = Halve(frame);
  half_surface_ = compositor_.CreateSurface(half_, true);
  if (half_surface_) {
    half_visual_ = compositor_.CreateVisual();
    half_effect_ = compositor_.CreateEffectGroup();
    half_visual_->SetContent(half_surface_.Get());
    half_visual_->SetEffect(half_effect_.Get());
    const D2D_MATRIX_3X2_F enlarge{static_cast<float>(frame.width()) / half_.width(), 0.0f, 0.0f,
                                   static_cast<float>(frame.height()) / half_.height(), 0.0f, 0.0f};
    half_visual_->SetTransform(enlarge);
    half_alpha_.Bind(half_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
    half_alpha_.Snap(0.0f);
    half_alpha_.To(device(), now, 1.0f, theme::kQuickFade, kHalfFadeDelay);
    plane_scaled_->AddVisual(half_visual_.Get(), FALSE, nullptr);
    full_release_at_ = now.seconds + kFullReleaseDelay;
  }
  AddCrisp(selected_rect(), static_cast<int>(landing.width()), static_cast<int>(landing.height()), now, kCrispFlightDelay);
}

void Card::UpdateShadow(int width, int height) {
  if (width == shadow_width_ && height == shadow_height_) return;
  shadow_width_ = width;
  shadow_height_ = height;

  const float d = static_cast<float>(kShadowDownscale);
  const ShadowLayer layers[] = {
      {16.0f * scale_ / d, 0.40f, 8.0f * scale_ / d},
      {3.0f * scale_ / d, 0.22f, 1.0f * scale_ / d},
  };
  const float pad = std::ceil(48.0f * scale_ / d);
  Image shadow = RenderShadow(std::max(static_cast<int>(width / d), 1), std::max(static_cast<int>(height / d), 1),
                              theme::kCardRadius * scale_ / d, pad, layers, ARRAYSIZE(layers));
  shadow_surface_width_ = shadow.width();
  shadow_surface_height_ = shadow.height();
  shadow_pad_ = pad * d;
  shadow_surface_ = compositor_.CreateSurface(shadow, false);
  shadow_visual_->SetContent(shadow_surface_.Get());
}

void Card::MoveTo(const RectF& rect, int depth, const SpringSpec& spec, const TimeBase& now, double delay, bool in_flight) {
  const RECT& region = selected_rect();
  const float region_w = static_cast<float>(region.right - region.left);
  const float region_h = static_cast<float>(region.bottom - region.top);
  const float width = rect.width();
  const float height = rect.height();

  float sx;
  float sy;
  float px;
  float py;
  if (depth == 0) {
    sx = width / region_w;
    sy = height / region_h;
    px = rect.left - region.left * sx;
    py = rect.top - region.top * sy;
  } else {
    // Cards behind the front share its outline, so they crop to fill rather than letterbox.
    sx = sy = std::max(width / region_w, height / region_h);
    px = rect.left + (width - region_w * sx) * 0.5f - region.left * sx;
    py = rect.top + (height - region_h * sy) * 0.5f - region.top * sy;
  }

  IDCompositionDevice* dc = device();
  left_.To(dc, now, rect.left, spec, delay);
  top_.To(dc, now, rect.top, spec, delay);
  right_.To(dc, now, rect.right, spec, delay);
  bottom_.To(dc, now, rect.bottom, spec, delay);
  radius_.To(dc, now, theme::kCardRadius * scale_, spec, delay);
  plane_x_.To(dc, now, px, spec, delay);
  plane_y_.To(dc, now, py, spec, delay);
  plane_sx_.To(dc, now, sx, spec, delay);
  plane_sy_.To(dc, now, sy, spec, delay);

  if (depth == 0) UpdateShadow(static_cast<int>(width), static_cast<int>(height));
  const float scale_x = (width + shadow_pad_ * 2.0f) / shadow_surface_width_;
  const float scale_y = (height + shadow_pad_ * 2.0f) / shadow_surface_height_;
  shadow_x_.To(dc, now, rect.left - shadow_pad_, spec, delay);
  shadow_y_.To(dc, now, rect.top - shadow_pad_, spec, delay);
  shadow_sx_.To(dc, now, scale_x, spec, delay);
  shadow_sy_.To(dc, now, scale_y, spec, delay);

  const bool shown = depth <= theme::kDeckVisibleBehind;
  const double decoration_delay = in_flight ? kFlightDecorationDelay : 0.0;
  ring_alpha_.To(dc, now, 1.0f, theme::kFade, decoration_delay);
  const float shadow_target = !shown ? 0.0f : (depth == 0 ? 1.0f : kBehindShadowAlpha);
  shadow_alpha_.To(dc, now, shadow_target, theme::kFade, decoration_delay);
  resting_opacity_ = shown ? 1.0f - depth * theme::kDeckOpacityPerLevel : 0.0f;
  opacity_.To(dc, now, dimmed_ ? kDraggedOpacity : resting_opacity_, theme::kFade);
}

void Card::AddCrisp(const RECT& region, int width, int height, const TimeBase& now, double delay) {
  width = std::max(width, 1);
  height = std::max(height, 1);
  const float region_w = static_cast<float>(region.right - region.left);
  const float region_h = static_cast<float>(region.bottom - region.top);

  // The hairline takes the outermost pixel, so the bitmap is mapped 1:1 onto the whole card and simply clipped by it.
  Image pixels;
  if (half_ && region_w / width >= 2.0f && region_h / height >= 2.0f) {
    const double kx = static_cast<double>(half_.width()) / shot_->frame->width();
    const double ky = static_cast<double>(half_.height()) / shot_->frame->height();
    pixels = ResampleArea(half_, region.left * kx, region.top * ky, region.right * kx, region.bottom * ky, width, height);
  } else {
    pixels = ResampleArea(*shot_->frame, region.left, region.top, region.right, region.bottom, width, height);
  }

  auto layer = std::make_unique<CrispLayer>();
  layer->surface = compositor_.CreateSurface(pixels, true);
  if (!layer->surface) return;
  layer->visual = compositor_.CreateVisual();
  layer->effect = compositor_.CreateEffectGroup();
  layer->visual->SetContent(layer->surface.Get());
  layer->visual->SetEffect(layer->effect.Get());
  const D2D_MATRIX_3X2_F place{region_w / width, 0.0f, 0.0f, region_h / height, static_cast<float>(region.left),
                               static_cast<float>(region.top)};
  layer->visual->SetTransform(place);
  layer->alpha.Bind(layer->effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  layer->alpha.Snap(0.0f);
  layer->alpha.To(device(), now, 1.0f, theme::kFade, delay);
  plane_scaled_->AddVisual(layer->visual.Get(), FALSE, nullptr);

  for (auto& previous : crisp_layers_) {
    if (previous->retire_at == 0.0) {
      previous->alpha.To(device(), now, 0.0f, theme::kQuickFade);
      previous->retire_at = previous->alpha.end_time();
    }
  }
  crisp_layers_.push_back(std::move(layer));
}

void Card::Select(int index, const RectF& rect, const TimeBase& now) {
  selected_ = index;
  const double crisp_delay = SpringSettleTime(theme::kMorph) * kCrispMorphDelayFraction;
  AddCrisp(selected_rect(), static_cast<int>(rect.width()), static_cast<int>(rect.height()), now, crisp_delay);
}

void Card::SetSwipe(float offset, float opacity) {
  slide_.Snap(offset);
  opacity_.Snap(opacity);
}

void Card::SnapBack(const TimeBase& now, float velocity) {
  slide_.To(device(), now, 0.0f, theme::kSnapBack, 0.0, velocity);
  opacity_.To(device(), now, resting_opacity_, theme::kFade);
}

void Card::SlideOut(const TimeBase& now, float distance, float velocity) {
  slide_.To(device(), now, distance, theme::kExit, 0.0, velocity);
  opacity_.To(device(), now, 0.0f, theme::kExit);
  exit_end_time_ = std::max(slide_.end_time(), opacity_.end_time());
}

void Card::SetDimmed(bool dimmed, const TimeBase& now) {
  dimmed_ = dimmed;
  opacity_.To(device(), now, dimmed ? kDraggedOpacity : resting_opacity_, theme::kFade);
}

void Card::Tick(double now) {
  if (full_release_at_ != 0.0 && now >= full_release_at_) {
    full_release_at_ = 0.0;
    full_visual_->SetContent(nullptr);
    full_surface_.Reset();
  }
  for (auto it = crisp_layers_.begin(); it != crisp_layers_.end();) {
    if ((*it)->retire_at != 0.0 && now >= (*it)->retire_at) {
      plane_scaled_->RemoveVisual((*it)->visual.Get());
      it = crisp_layers_.erase(it);
    } else {
      ++it;
    }
  }
}

void Card::Hibernate(Store& store) {
  // The crossfades of the flight need these layers, so a card still landing stays as it is.
  if (hibernating_ || full_release_at_ != 0.0 || !half_) return;
  if (!store.ReleaseFrame(shot_)) return;
  hibernating_ = true;
  half_ = Image();
  if (half_visual_) half_visual_->SetContent(nullptr);
  half_surface_.Reset();
}

void Card::Wake(Store& store) {
  if (!hibernating_) return;
  hibernating_ = false;
  store.RestoreFrame(shot_);
  if (!shot_->frame) return;
  half_ = Halve(*shot_->frame);
  half_surface_ = compositor_.CreateSurface(half_, true);
  if (half_visual_) half_visual_->SetContent(half_surface_.Get());
}

Image Card::RenderDragPreview(int width) const {
  const RECT& region = selected_rect();
  const float region_w = static_cast<float>(region.right - region.left);
  const float region_h = static_cast<float>(region.bottom - region.top);
  const int target_w = std::min(width, static_cast<int>(region_w));
  const int target_h = std::max(static_cast<int>(target_w * region_h / region_w), 1);
  return ResampleArea(*shot_->frame, region.left, region.top, region.right, region.bottom, target_w, target_h);
}

}  // namespace sstk
