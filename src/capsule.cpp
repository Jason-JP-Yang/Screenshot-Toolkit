#include "capsule.h"

#include "theme.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr int kShadowDownscale = 4;

uint32_t Argb(uint32_t rgb, float alpha) { return static_cast<uint32_t>(alpha * 255.0f + 0.5f) << 24 | rgb; }

uint32_t RgbToBgr(uint32_t rgb) { return (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16); }

void SetClipRadii(IDCompositionRectangleClip* clip, float radius) {
  clip->SetTopLeftRadiusX(radius);
  clip->SetTopLeftRadiusY(radius);
  clip->SetTopRightRadiusX(radius);
  clip->SetTopRightRadiusY(radius);
  clip->SetBottomLeftRadiusX(radius);
  clip->SetBottomLeftRadiusY(radius);
  clip->SetBottomRightRadiusX(radius);
  clip->SetBottomRightRadiusY(radius);
}

}  // namespace

Capsule::Capsule(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor)
    : compositor_(compositor), scale_(scale), monitor_(monitor) {
  const float h = height();
  const float wide = static_cast<float>(monitor.cx);

  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  parent->AddVisual(root_.Get(), FALSE, nullptr);

  shadow_ = compositor_.CreateVisual();
  shadow_scale_ = compositor_.CreateScale();
  shadow_->SetTransform(shadow_scale_.Get());
  root_->AddVisual(shadow_.Get(), FALSE, nullptr);

  frame_ = compositor_.CreateVisual();
  frame_clip_ = compositor_.CreateClip();
  frame_->SetClip(frame_clip_.Get());
  frame_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  frame_clip_->SetLeft(0.0f);
  frame_clip_->SetTop(0.0f);
  frame_clip_->SetBottom(h);
  SetClipRadii(frame_clip_.Get(), h * 0.5f);
  root_->AddVisual(frame_.Get(), FALSE, nullptr);

  ring_ = compositor_.CreateSolidVisual(Argb(0xFFFFFF, theme::kCapsuleHairlineAlpha), wide, h);
  frame_->AddVisual(ring_.Get(), FALSE, nullptr);

  inner_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_->SetClip(inner_clip_.Get());
  inner_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  inner_clip_->SetLeft(1.0f);
  inner_clip_->SetTop(1.0f);
  inner_clip_->SetBottom(h - 1.0f);
  SetClipRadii(inner_clip_.Get(), h * 0.5f - 1.0f);
  frame_->AddVisual(inner_.Get(), FALSE, nullptr);

  fallback_fill_ = compositor_.CreateSolidVisual(Argb(theme::kGlassTint, 1.0f), wide, h);
  inner_->AddVisual(fallback_fill_.Get(), FALSE, nullptr);
  frost_ = compositor_.CreateVisual();
  inner_->AddVisual(frost_.Get(), FALSE, nullptr);

  light_layer_ = compositor_.CreateVisual();
  inner_->AddVisual(light_layer_.Get(), FALSE, nullptr);

  veil_ = compositor_.CreateVisual();
  veil_clip_ = compositor_.CreateClip();
  veil_->SetClip(veil_clip_.Get());
  veil_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  veil_effect_ = compositor_.CreateEffectGroup();
  veil_->SetEffect(veil_effect_.Get());
  veil_fill_ = compositor_.CreateSolidVisual(Argb(0xFFFFFF, theme::kHoverVeilAlpha), wide, h);
  veil_->AddVisual(veil_fill_.Get(), FALSE, nullptr);
  inner_->AddVisual(veil_.Get(), FALSE, nullptr);

  // Dark glyphs live inside the pill's clip, so the sliding pill inverts each glyph exactly as it passes over it.
  pill_ = compositor_.CreateVisual();
  pill_clip_ = compositor_.CreateClip();
  pill_->SetClip(pill_clip_.Get());
  pill_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  const float pad = theme::kCapsulePadding * scale_;
  const float segment_h = theme::kSegmentHeight * scale_;
  pill_clip_->SetTop(pad);
  pill_clip_->SetBottom(pad + segment_h);
  SetClipRadii(pill_clip_.Get(), segment_h * 0.5f);
  pill_fill_ = compositor_.CreateSolidVisual(Argb(theme::kPorcelain, 1.0f), wide, h);
  pill_->AddVisual(pill_fill_.Get(), FALSE, nullptr);
  dark_layer_ = compositor_.CreateVisual();
  pill_->AddVisual(dark_layer_.Get(), FALSE, nullptr);
  inner_->AddVisual(pill_.Get(), FALSE, nullptr);

  x_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  y_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  opacity_.Bind(root_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  width_.Bind(frame_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  width_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight), -1.0f);
  shadow_sx_.Bind(shadow_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  pill_left_.Bind(pill_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetLeft));
  pill_right_.Bind(pill_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  veil_alpha_.Bind(veil_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));

  opacity_.Snap(0.0f);
  veil_alpha_.Snap(0.0f);
  width_.Snap(WidthFor(1));
  shadow_sx_.Snap(1.0f);
}

float Capsule::height() const { return std::round((theme::kSegmentHeight + theme::kCapsulePadding * 2.0f) * scale_); }

float Capsule::WidthFor(int count) const {
  return std::round((theme::kCapsulePadding * 2.0f + theme::kSegmentWidth * count) * scale_);
}

float Capsule::full_width() const { return WidthFor(theme::kFrostSegments); }

float Capsule::SegmentLeft(int index) const {
  return std::round((theme::kCapsulePadding + theme::kSegmentWidth * index) * scale_);
}

void Capsule::PlacePill(int index, const TimeBase& now, bool immediate) {
  const float left = SegmentLeft(index);
  const float right = SegmentLeft(index + 1);
  if (immediate) {
    pill_left_.Snap(left);
    pill_right_.Snap(right);
    return;
  }
  // The leading edge springs ahead of the trailing one, giving the pill a soft, liquid stretch.
  const bool moving_right = right > pill_right_.target();
  IDCompositionDevice* dc = compositor_.device();
  pill_left_.To(dc, now, left, moving_right ? theme::kPillTrailing : theme::kPillLeading);
  pill_right_.To(dc, now, right, moving_right ? theme::kPillLeading : theme::kPillTrailing);
}

void Capsule::Bind(const std::vector<Region>& regions, const TaskbarInsets& insets, int selected, IDCompositionSurface* frost,
                   const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const int count = static_cast<int>(regions.size());
  const bool show = count >= 2;
  const bool was_visible = visible_;
  option_count_ = count;
  selected_ = selected;
  visible_ = show;

  if (!show) {
    opacity_.To(dc, now, 0.0f, theme::kFade);
    return;
  }

  frost_->SetContent(frost);
  if (frost != nullptr) {
    UINT frost_w = 1;
    UINT frost_h = 1;
    // The frost bitmap spans the full capsule height and the widest capsule at a fixed downscale.
    frost_w = static_cast<UINT>(full_width() / theme::kFrostDownscale);
    frost_h = static_cast<UINT>(height() / theme::kFrostDownscale);
    const D2D_MATRIX_3X2_F stretch{full_width() / std::max(frost_w, 1u), 0.0f, 0.0f, height() / std::max(frost_h, 1u), 0.0f, 0.0f};
    frost_->SetTransform(stretch);
  }

  // Glyph strips are rebuilt from the shot's real geometry.
  std::vector<GlyphSpec> glyphs;
  glyphs.reserve(regions.size());
  GlyphBand band = GlyphBand::kNone;
  if (insets.bottom > 0) band = GlyphBand::kBottom;
  else if (insets.top > 0) band = GlyphBand::kTop;
  else if (insets.left > 0) band = GlyphBand::kLeft;
  else if (insets.right > 0) band = GlyphBand::kRight;

  // Boxes are relative to the area outside the taskbar, because the glyph draws the band at its own legible size.
  const float area_w = std::max(static_cast<float>(monitor_.cx - insets.left - insets.right), 1.0f);
  const float area_h = std::max(static_cast<float>(monitor_.cy - insets.top - insets.bottom), 1.0f);
  auto to_box = [&](const RECT& rect) {
    return GlyphBox{std::clamp((rect.left - insets.left) / area_w, 0.0f, 1.0f),
                    std::clamp((rect.top - insets.top) / area_h, 0.0f, 1.0f),
                    std::clamp((rect.right - insets.left) / area_w, 0.0f, 1.0f),
                    std::clamp((rect.bottom - insets.top) / area_h, 0.0f, 1.0f)};
  };
  for (const Region& region : regions) {
    GlyphSpec glyph{GlyphStyle::kWindow, band, static_cast<float>(monitor_.cx) / monitor_.cy, to_box(region.rect), {}};
    switch (region.kind) {
      case RegionKind::kFull:
        glyph.style = GlyphStyle::kWholeScreen;
        break;
      case RegionKind::kNoTaskbar:
        glyph.style = GlyphStyle::kWithoutTaskbar;
        break;
      case RegionKind::kApp:
        glyph.style = GlyphStyle::kWindow;
        break;
      case RegionKind::kZone:
        glyph.style = GlyphStyle::kZone;
        for (const Region& other : regions) {
          if (other.kind == RegionKind::kZone && &other != &region) glyph.neighbors.push_back(to_box(other.rect));
        }
        break;
    }
    glyphs.push_back(std::move(glyph));
  }
  const int pitch = static_cast<int>(std::round(theme::kSegmentWidth * scale_));
  const int strip_height = static_cast<int>(std::round(theme::kSegmentHeight * scale_));
  Image light = RenderGlyphStrip(glyphs, pitch, strip_height, scale_, RgbToBgr(theme::kGlyphOnGlass), theme::kGlyphOnGlassAlpha, theme::kGlyphFaintAlpha);
  Image dark = RenderGlyphStrip(glyphs, pitch, strip_height, scale_, RgbToBgr(theme::kGlyphOnPorcelain), 1.0f, theme::kGlyphFaintAlpha);

  auto layer = std::make_unique<GlyphLayer>();
  layer->light_surface = compositor_.CreateSurface(light, false);
  layer->dark_surface = compositor_.CreateSurface(dark, false);
  layer->light = compositor_.CreateVisual();
  layer->dark = compositor_.CreateVisual();
  layer->effect = compositor_.CreateEffectGroup();
  layer->light->SetContent(layer->light_surface.Get());
  layer->dark->SetContent(layer->dark_surface.Get());
  layer->light->SetEffect(layer->effect.Get());
  layer->dark->SetEffect(layer->effect.Get());
  const float pad = std::round(theme::kCapsulePadding * scale_);
  layer->light->SetOffsetX(pad);
  layer->light->SetOffsetY(pad);
  layer->dark->SetOffsetX(pad);
  layer->dark->SetOffsetY(pad);
  layer->alpha.Bind(layer->effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  layer->alpha.Snap(was_visible ? 0.0f : 1.0f);
  if (was_visible) layer->alpha.To(dc, now, 1.0f, theme::kFade, 0.04);
  light_layer_->AddVisual(layer->light.Get(), FALSE, nullptr);
  dark_layer_->AddVisual(layer->dark.Get(), FALSE, nullptr);
  for (auto& previous : glyph_layers_) {
    if (previous->retire_at == 0.0) {
      previous->alpha.To(dc, now, 0.0f, theme::kQuickFade);
      previous->retire_at = previous->alpha.end_time();
    }
  }
  glyph_layers_.push_back(std::move(layer));

  // Shadow is rendered at the target width and stretched along with the capsule while it resizes.
  const float width = WidthFor(count);
  const float h = height();
  const float d = static_cast<float>(kShadowDownscale);
  const ShadowLayer layers[] = {{9.0f * scale_ / d, 0.34f, 4.0f * scale_ / d}};
  const float shadow_pad = std::ceil(28.0f * scale_ / d);
  Image shadow = RenderShadow(std::max(static_cast<int>(width / d), 1), std::max(static_cast<int>(h / d), 1), h * 0.5f / d, shadow_pad, layers, 1);
  shadow_surface_ = compositor_.CreateSurface(shadow, false);
  shadow_surface_width_ = shadow.width();
  shadow_pad_ = shadow_pad * d;
  shadow_->SetContent(shadow_surface_.Get());
  shadow_->SetOffsetX(-shadow_pad_);
  shadow_->SetOffsetY(-shadow_pad_);
  shadow_scale_->SetScaleY(d);

  const float shadow_scale = (width + shadow_pad_ * 2.0f) / shadow_surface_width_;
  if (was_visible) {
    width_.To(dc, now, width, theme::kReflow);
    shadow_sx_.To(dc, now, shadow_scale, theme::kReflow);
    PlacePill(selected, now, false);
    opacity_.To(dc, now, 1.0f, theme::kFade);
  } else {
    width_.Snap(width);
    shadow_sx_.Snap(shadow_scale);
    PlacePill(selected, now, true);
    // The capsule rises into place just after the card starts flying.
    y_.Snap(target_y_ + theme::kCapsuleRise * scale_);
    opacity_.Snap(0.0f);
    opacity_.To(dc, now, 1.0f, theme::kFade, theme::kCapsuleEnterDelay);
    y_.To(dc, now, target_y_, theme::kCapsuleEnter, theme::kCapsuleEnterDelay);
  }
}

void Capsule::MoveTo(float x, float y, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const bool first = target_x_ == 0.0f && target_y_ == 0.0f;
  target_x_ = x;
  target_y_ = y;
  if (first) {
    x_.Snap(x);
    y_.Snap(y);
    return;
  }
  x_.To(dc, now, x, theme::kReflow);
  y_.To(dc, now, y, theme::kReflow);
}

void Capsule::Select(int index, const TimeBase& now) {
  selected_ = index;
  PlacePill(index, now, false);
  if (hover_ == index) SetHover(-1, now);
}

void Capsule::SetHover(int index, const TimeBase& now) {
  if (index == selected_) index = -1;
  if (index == hover_) return;
  hover_ = index;
  IDCompositionDevice* dc = compositor_.device();
  if (index >= 0) {
    const float pad = theme::kCapsulePadding * scale_;
    const float segment_h = theme::kSegmentHeight * scale_;
    veil_clip_->SetLeft(SegmentLeft(index));
    veil_clip_->SetRight(SegmentLeft(index + 1));
    veil_clip_->SetTop(pad);
    veil_clip_->SetBottom(pad + segment_h);
    SetClipRadii(veil_clip_.Get(), segment_h * 0.5f);
    veil_alpha_.To(dc, now, 1.0f, theme::kQuickFade);
  } else {
    veil_alpha_.To(dc, now, 0.0f, theme::kQuickFade);
  }
}

void Capsule::Hide(const TimeBase& now) {
  visible_ = false;
  hover_ = -1;
  IDCompositionDevice* dc = compositor_.device();
  opacity_.To(dc, now, 0.0f, theme::kFade);
  veil_alpha_.To(dc, now, 0.0f, theme::kQuickFade);
}

void Capsule::Clear() {
  for (auto& layer : glyph_layers_) {
    light_layer_->RemoveVisual(layer->light.Get());
    dark_layer_->RemoveVisual(layer->dark.Get());
  }
  glyph_layers_.clear();
  frost_->SetContent(nullptr);
  shadow_->SetContent(nullptr);
  shadow_surface_.Reset();
  opacity_.Snap(0.0f);
  veil_alpha_.Snap(0.0f);
  visible_ = false;
  hover_ = -1;
  option_count_ = 0;
}

void Capsule::Tick(double now) {
  for (auto it = glyph_layers_.begin(); it != glyph_layers_.end();) {
    if ((*it)->retire_at != 0.0 && now >= (*it)->retire_at) {
      light_layer_->RemoveVisual((*it)->light.Get());
      dark_layer_->RemoveVisual((*it)->dark.Get());
      it = glyph_layers_.erase(it);
    } else {
      ++it;
    }
  }
}

RECT Capsule::Bounds() const {
  return RECT{static_cast<LONG>(target_x_), static_cast<LONG>(target_y_),
              static_cast<LONG>(target_x_ + WidthFor(option_count_)), static_cast<LONG>(target_y_ + height())};
}

int Capsule::HitTest(int x, int y) const {
  if (!visible_) return -1;
  const RECT bounds = Bounds();
  if (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) return -1;
  for (int i = 0; i < option_count_; ++i) {
    const float left = target_x_ + SegmentLeft(i);
    const float right = target_x_ + SegmentLeft(i + 1);
    if (x >= left && x < right) return i;
  }
  // The padding at either end still belongs to the nearest segment.
  return x < target_x_ + SegmentLeft(0) ? 0 : option_count_ - 1;
}

}  // namespace sstk
