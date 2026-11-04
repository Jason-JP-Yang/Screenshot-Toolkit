#include "card.h"

#include "config/animation.h"
#include "config/colors.h"
#include "config/core.h"
#include "config/layout.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

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

SIZE ThumbnailSize(const RECT& region, float scale) {
  const float region_w = static_cast<float>(region.right - region.left);
  const float region_h = static_cast<float>(region.bottom - region.top);
  const float fit = std::min({1.0f, config::kCardMaxWidth * scale / region_w, config::kCardMaxHeight * scale / region_h});
  return SIZE{std::max(static_cast<LONG>(std::round(region_w * fit)), 1L),
              std::max(static_cast<LONG>(std::round(region_h * fit)), 1L)};
}

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

Card::Card(Compositor& compositor, IDCompositionVisual* parent, std::shared_ptr<Shot> shot, const ScreenInfo& screen,
           float scale, ComPtr<IDCompositionSurface> frost, const CardShadow& shadow, IDCompositionClip* shadow_clip,
           double created_at)
    : compositor_(compositor), parent_(parent), shot_(std::move(shot)), regions_(screen.regions), insets_(screen.insets),
      scale_(scale), border_(std::max(std::round(config::kCardBorder * scale), 1.0f)), frost_(std::move(frost)) {
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
  radius_.Bind(clip, SetAllRadii);
  radius_.Bind(inner, SetAllRadii, -border_);
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
  expiry = created_at + config::kFlightLanding + config::kDwellSeconds;
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
    if (crisp_surfaces_[i]) continue;
    const RECT& region = regions_[i].rect;
    const SIZE size = ThumbnailSize(region, scale_);
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
    crisp_surfaces_[i] = compositor_.CreateSurface(pixels, true);
    crisp_sizes_[i] = size;
    crisp_pixels_[i] = std::move(pixels);
  }
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
  const RECT& region = selected_rect();
  const float width = rect.width();
  const float height = rect.height();
  const float sx = width / static_cast<float>(region.right - region.left);
  const float sy = height / static_cast<float>(region.bottom - region.top);
  const float px = rect.left - region.left * sx;
  const float py = rect.top - region.top * sy;

  IDCompositionDevice* dc = device();
  left_.To(dc, now, rect.left, spec, delay);
  top_.To(dc, now, rect.top, spec, delay);
  right_.To(dc, now, rect.right, spec, delay);
  bottom_.To(dc, now, rect.bottom, spec, delay);
  radius_.To(dc, now, config::kCardRadius * scale_, spec, delay);
  plane_x_.To(dc, now, px, spec, delay);
  plane_y_.To(dc, now, py, spec, delay);
  plane_sx_.To(dc, now, sx, spec, delay);
  plane_sy_.To(dc, now, sy, spec, delay);
  shadow_edge_w_.To(dc, now, std::max(width - shadow_reach_ * 2.0f, 0.0f), spec, delay);
  shadow_edge_h_.To(dc, now, std::max(height - shadow_reach_ * 2.0f, 0.0f), spec, delay);

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

void Card::ShowCrisp(int index, const TimeBase& now, double delay, const SpringSpec& spec) {
  if (index >= static_cast<int>(crisp_surfaces_.size()) || !crisp_surfaces_[index]) return;
  const RECT& region = regions_[index].rect;
  const SIZE size = crisp_sizes_[index];

  auto layer = std::make_unique<CrispLayer>();
  layer->visual = compositor_.CreateVisual();
  layer->effect = compositor_.CreateEffectGroup();
  layer->visual->SetContent(crisp_surfaces_[index].Get());
  layer->visual->SetEffect(layer->effect.Get());
  const D2D_MATRIX_3X2_F place{static_cast<float>(region.right - region.left) / size.cx, 0.0f, 0.0f,
                               static_cast<float>(region.bottom - region.top) / size.cy, static_cast<float>(region.left),
                               static_cast<float>(region.top)};
  layer->visual->SetTransform(place);
  layer->alpha.Bind(layer->effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  layer->alpha.Snap(0.0f);
  layer->alpha.To(device(), now, 1.0f, spec, delay);
  plane_scaled_->AddVisual(layer->visual.Get(), FALSE, nullptr);

  for (auto& previous : crisp_layers_) {
    if (previous->retire_at == 0.0) {
      previous->alpha.To(device(), now, 0.0f, spec, delay);
      previous->retire_at = previous->alpha.end_time();
    }
  }
  crisp_layers_.push_back(std::move(layer));
}

bool Card::CanSelect(int index) const {
  return !hibernating_ && index < static_cast<int>(crisp_surfaces_.size()) && crisp_surfaces_[index];
}

void Card::Select(int index, const TimeBase& now) {
  selected_ = index;
  ShowCrisp(index, now, config::kCrispMorphDelay, config::kMorphFade);
}

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
  if (selected_ >= static_cast<int>(crisp_pixels_.size()) || !crisp_pixels_[selected_]) return nullptr;
  return &crisp_pixels_[selected_];
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
  for (size_t i = 0; i < crisp_surfaces_.size(); ++i) {
    if (static_cast<int>(i) != selected_) {
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
