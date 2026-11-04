#include "bin.h"

#include "config/animation.h"
#include "config/colors.h"
#include "config/core.h"
#include "config/layout.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr float kDegrees = 3.14159265f / 180.0f;

// In the icon's 24-unit grid. See RenderBinCan and RenderBinLid.
constexpr float kIconGrid = 24.0f;
constexpr float kLidCenterX = 12.0f;
constexpr float kLidY = 7.3f;
constexpr float kLidHalf = 7.9f;
constexpr float kCanTop = 9.6f;
constexpr float kCanBottom = 21.4f;

constexpr float kRestDistance = 0.02f;

float EvenPixels(float value) { return static_cast<float>(static_cast<int>(std::round(value)) & ~1); }

// Ends a spring exactly on its target, so that text comes to rest on whole pixels instead of nearly on them.
void StepToRest(SpringValue& value, const SpringSpec& spec, float dt) {
  value.Step(spec, dt);
  if (value.Settled(kRestDistance)) value.Snap(value.target);
}

}  // namespace

RubbishBin::RubbishBin(Compositor& compositor, IDCompositionVisual* parent, float scale)
    : compositor_(compositor), scale_(scale), height_(EvenPixels(config::kBinHeight * scale)),
      icon_size_(EvenPixels(config::kBinIconSize * scale)) {
  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  parent->AddVisual(root_.Get(), FALSE, nullptr);

  // The selector capsule's shadow, cut into two caps and a stretching middle so that it fits any width.
  const float pad = std::ceil(28.0f * scale);
  const config::ShadowSpec& cast = config::kCapsuleShadow;
  const ShadowLayer layers[] = {{cast.sigma * scale, cast.alpha, cast.offset * scale}};
  const Image shadow = RenderShadow(static_cast<int>(height_), static_cast<int>(height_), height_ * 0.5f, pad, layers, ARRAYSIZE(layers));
  shadow_pad_ = pad;
  const int shadow_left_columns = shadow.width() / 2;
  const int shadow_right_columns = shadow.width() - shadow_left_columns - 1;
  shadow_left_width_ = static_cast<float>(shadow_left_columns);
  shadow_right_width_ = static_cast<float>(shadow_right_columns);
  auto slice = [&](int left, int width) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateVisual();
    visual->SetContent(compositor_.CreateSurface(CopyRect(shadow, left, 0, width, shadow.height()), false).Get());
    visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    return visual;
  };
  shadow_holder_ = compositor_.CreateVisual();
  shadow_left_ = slice(0, shadow_left_columns);
  shadow_middle_ = slice(shadow_left_columns, 1);
  shadow_right_ = slice(shadow_left_columns + 1, shadow_right_columns);
  shadow_holder_->AddVisual(shadow_left_.Get(), FALSE, nullptr);
  shadow_holder_->AddVisual(shadow_middle_.Get(), FALSE, nullptr);
  shadow_holder_->AddVisual(shadow_right_.Get(), FALSE, nullptr);
  root_->AddVisual(shadow_holder_.Get(), FALSE, nullptr);

  const int icon_pixels = static_cast<int>(icon_size_);
  const float icon_scale = icon_size_ / kIconGrid;
  const Image can = RenderBinCan(icon_pixels, icon_scale, config::kBinInk);
  const Image lid = RenderBinLid(icon_pixels, icon_scale, config::kBinInk);
  const Image label = RenderLabel(config::kBinLabel, static_cast<int>(std::round(config::kBinFontSize * scale)), config::kBinInk);
  const RECT can_ink = InkBounds(can);
  const RECT lid_ink = InkBounds(lid);
  RECT icon_ink;
  UnionRect(&icon_ink, &can_ink, &lid_ink);
  const RECT label_ink = InkBounds(label);

  // Laid out by what is visibly drawn. The icon is concentric with the left cap, which also centers it in the disc,
  // and the text ends as far from the right edge as the icon's body starts from the left.
  icon_x_ = std::round((height_ - static_cast<float>(icon_ink.right - icon_ink.left)) * 0.5f) - static_cast<float>(icon_ink.left);
  label_x_ = icon_x_ + static_cast<float>(icon_ink.right) + std::round(config::kBinTextGap * scale) - static_cast<float>(label_ink.left);
  full_width_ = EvenPixels(label_x_ + static_cast<float>(label_ink.right) + std::round(config::kBinTextInset * scale));

  auto shape_clip = [&](IDCompositionRectangleClip* clip, float inset) {
    const float radius = height_ * 0.5f - inset;
    clip->SetTop(-radius);
    clip->SetBottom(radius);
    clip->SetTopLeftRadiusX(radius);
    clip->SetTopLeftRadiusY(radius);
    clip->SetTopRightRadiusX(radius);
    clip->SetTopRightRadiusY(radius);
    clip->SetBottomLeftRadiusX(radius);
    clip->SetBottomLeftRadiusY(radius);
    clip->SetBottomRightRadiusX(radius);
    clip->SetBottomRightRadiusY(radius);
  };
  const auto solid = [&](uint32_t argb, float width) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateSolidVisual(argb, width, height_);
    visual->SetOffsetX(-width * 0.5f);
    visual->SetOffsetY(-height_ * 0.5f);
    return visual;
  };

  // The same build as the selector: a hairline showing between two nested clips.
  const float span = full_width_ * 2.0f;
  body_ = compositor_.CreateVisual();
  body_clip_ = compositor_.CreateClip();
  body_->SetClip(body_clip_.Get());
  body_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  shape_clip(body_clip_.Get(), 0.0f);
  root_->AddVisual(body_.Get(), FALSE, nullptr);

  ring_ = solid(static_cast<uint32_t>(config::kCapsuleHairlineAlpha * 255.0f + 0.5f) << 24 | config::kCapsuleHairlineColor, span);
  body_->AddVisual(ring_.Get(), FALSE, nullptr);

  inner_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_->SetClip(inner_clip_.Get());
  inner_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  shape_clip(inner_clip_.Get(), 1.0f);
  body_->AddVisual(inner_.Get(), FALSE, nullptr);

  fill_ = solid(0xFF000000u | config::kBinRed, span);
  inner_->AddVisual(fill_.Get(), FALSE, nullptr);

  fill_over_ = solid(0xFF000000u | config::kBinRedOver, span);
  fill_over_effect_ = compositor_.CreateEffectGroup();
  fill_over_->SetEffect(fill_over_effect_.Get());
  inner_->AddVisual(fill_over_.Get(), FALSE, nullptr);

  icon_ = compositor_.CreateVisual();
  can_ = compositor_.CreateVisual();
  can_->SetContent(compositor_.CreateSurface(can, false).Get());
  lid_ = compositor_.CreateVisual();
  lid_->SetContent(compositor_.CreateSurface(lid, false).Get());
  icon_->AddVisual(can_.Get(), FALSE, nullptr);
  icon_->AddVisual(lid_.Get(), FALSE, nullptr);
  icon_->SetOffsetY(std::round(-static_cast<float>(icon_ink.top + icon_ink.bottom) * 0.5f));
  inner_->AddVisual(icon_.Get(), FALSE, nullptr);

  label_ = compositor_.CreateVisual();
  label_->SetContent(compositor_.CreateSurface(label, false).Get());
  label_->SetOffsetY(std::round(-static_cast<float>(label_ink.top + label_ink.bottom) * 0.5f));
  label_effect_ = compositor_.CreateEffectGroup();
  label_->SetEffect(label_effect_.Get());
  inner_->AddVisual(label_.Get(), FALSE, nullptr);

  SnapToDisc();
  Apply();
}

void RubbishBin::SnapToDisc() {
  presence_.Snap(0.0f);
  left_.Snap(-height_ * 0.5f);
  right_.Snap(height_ * 0.5f);
  label_alpha_.Snap(0.0f);
  over_light_.Snap(0.0f);
  lid_open_.Snap(0.0f);
  lid_aim_.Snap(0.0f);
  can_squash_.Snap(0.0f);
  closeness_ = 0.0f;
  slamming_ = false;
}

void RubbishBin::Place(float center_x, float bottom) {
  const float center_y = std::round(bottom - config::kBinMargin * scale_ - height_ * 0.5f);
  center_x = std::round(center_x);
  if (!placed_ || !showing_) {
    center_x_.Snap(center_x);
    center_y_.Snap(center_y);
    placed_ = true;
  } else {
    center_x_.target = center_x;
    center_y_.target = center_y;
  }
}

void RubbishBin::Show() {
  showing_ = true;
  hide_in_ = 0.0;
  slamming_ = false;
  presence_.target = 1.0f;
  near_ = false;
  engaged_ = false;
  closeness_ = 0.0f;
}

void RubbishBin::Hide() {
  showing_ = false;
  hide_in_ = 0.0;
  presence_.target = 0.0f;
  near_ = false;
  engaged_ = false;
  closeness_ = 0.0f;
}

RECT RubbishBin::HotZone() const {
  const float margin = (engaged_ ? config::kBinHotMarginEngaged : config::kBinHotMargin) * scale_;
  const float half_w = full_width_ * 0.5f + margin;
  const float half_h = height_ * 0.5f + margin;
  return RECT{static_cast<LONG>(center_x_.target - half_w), static_cast<LONG>(center_y_.target - half_h),
              static_cast<LONG>(center_x_.target + half_w), static_cast<LONG>(center_y_.target + half_h)};
}

bool RubbishBin::Contains(POINT point) const {
  const RECT zone = HotZone();
  return PtInRect(&zone, point) != FALSE;
}

bool RubbishBin::Track(POINT point) {
  const float half_w = full_width_ * 0.5f;
  const float half_h = height_ * 0.5f;
  const float dx = std::max(std::fabs(point.x - center_x_.target) - half_w, 0.0f);
  const float dy = std::max(std::fabs(point.y - center_y_.target) - half_h, 0.0f);
  const float distance = std::sqrt(dx * dx + dy * dy);
  const float noticed = config::kBinNearDistance * scale_;
  near_ = distance < noticed + (near_ ? config::kBinNearHysteresis * scale_ : 0.0f);
  closeness_ = std::clamp(1.0f - distance / noticed, 0.0f, 1.0f);
  engaged_ = Contains(point);
  return engaged_;
}

void RubbishBin::Aim(float x, float y) {
  const float angle = std::atan2(x - center_x_.value, std::max(center_y_.value - y, 1.0f));
  lid_aim_.target = std::clamp(angle / (config::kBinLidFullAimDegrees * kDegrees), -1.0f, 1.0f);
}

void RubbishBin::Nudge(float impulse) { can_squash_.velocity += impulse; }

void RubbishBin::Gulp(bool then_hide) {
  slamming_ = true;
  engaged_ = false;
  Nudge(config::kBinGulpImpulse);
  hide_in_ = then_hide ? config::kBinGulpHold : 0.0;
}

bool RubbishBin::Frame(float dt) {
  if (hide_in_ > 0.0) {
    hide_in_ -= dt;
    if (hide_in_ <= 0.0) Hide();
  }

  const bool expanded = near_ || engaged_ || slamming_;
  const float half = (expanded ? full_width_ : height_) * 0.5f;
  left_.target = -half;
  right_.target = half;
  label_alpha_.target = expanded ? 1.0f : 0.0f;
  over_light_.target = engaged_ ? 1.0f : 0.0f;
  lid_open_.target = slamming_ ? 0.0f : (engaged_ ? 1.0f : (near_ ? closeness_ * config::kBinLidNearOpen : 0.0f));

  presence_.Step(showing_ ? config::kCapsuleEnter : config::kCapsuleLeave, dt);
  // One spring for both edges keeps its center where it is.
  StepToRest(left_, config::kBinMorph, dt);
  StepToRest(right_, config::kBinMorph, dt);
  label_alpha_.Step(config::kFade, dt);
  over_light_.Step(config::kFade, dt);
  StepToRest(center_x_, config::kReflow, dt);
  StepToRest(center_y_, config::kReflow, dt);
  lid_open_.Step(slamming_ ? config::kBinLidSlam : config::kBinLid, dt);
  lid_aim_.Step(config::kBinLidAim, dt);
  can_squash_.Step(config::kBinCan, dt);
  Apply();

  const bool hidden = !showing_ && hide_in_ <= 0.0 && presence_.Settled(0.004f);
  if (hidden) {
    SnapToDisc();
    Apply();
    return false;
  }
  return true;
}

void RubbishBin::Reset() {
  showing_ = false;
  hide_in_ = 0.0;
  SnapToDisc();
  Apply();
}

void RubbishBin::Apply() {
  const float presence = presence_.value;
  root_->SetOffsetX(center_x_.value);
  root_->SetOffsetY(center_y_.value + (1.0f - presence) * config::kCapsuleRise * scale_);
  root_effect_->SetOpacity(std::clamp(presence * 1.5f, 0.0f, 1.0f));

  const float left = left_.value;
  const float right = std::max(right_.value, left + height_ * 0.6f);
  body_clip_->SetLeft(left);
  body_clip_->SetRight(right);
  inner_clip_->SetLeft(left + 1.0f);
  inner_clip_->SetRight(right - 1.0f);

  const float outer_left = left - shadow_pad_;
  const float outer_right = right + shadow_pad_;
  const float top = -height_ * 0.5f - shadow_pad_;
  const float middle_left = outer_left + shadow_left_width_;
  const float middle_right = outer_right - shadow_right_width_;
  shadow_left_->SetOffsetX(outer_left);
  shadow_left_->SetOffsetY(top);
  shadow_right_->SetOffsetX(middle_right);
  shadow_right_->SetOffsetY(top);
  shadow_middle_->SetTransform(D2D_MATRIX_3X2_F{std::max(middle_right - middle_left, 0.01f), 0.0f, 0.0f, 1.0f, middle_left, top});
  fill_over_effect_->SetOpacity(std::clamp(over_light_.value, 0.0f, 1.0f));

  icon_->SetOffsetX(left + icon_x_);
  label_->SetOffsetX(left + label_x_);
  label_effect_->SetOpacity(std::clamp(label_alpha_.value, 0.0f, 1.0f));

  const float unit = icon_size_ / kIconGrid;

  // The can squats from its base, and what rests on it goes down with it.
  const float squash = std::clamp(can_squash_.value, -config::kBinCanSquashMax, config::kBinCanSquashMax);
  const float can_x = 1.0f + squash * config::kBinCanWidening;
  const float can_y = 1.0f - squash;
  can_->SetTransform(D2D_MATRIX_3X2_F{can_x, 0.0f, 0.0f, can_y, kLidCenterX * unit * (1.0f - can_x), kCanBottom * unit * (1.0f - can_y)});
  const float can_drop = (kCanBottom - kCanTop) * unit * squash;

  // Opening toward the thumbnail, the lid pivots on its far end. Directly below it, it lifts off level instead.
  const float open = std::max(lid_open_.value, config::kBinLidClosedPastMin);
  const float aim = std::clamp(lid_aim_.value, -1.0f, 1.0f);
  const float radians = -aim * config::kBinLidTiltDegrees * kDegrees * std::max(open, 0.0f);
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  const float hinge_x = (kLidCenterX + (aim < 0.0f ? kLidHalf : -kLidHalf)) * unit;
  const float hinge_y = kLidY * unit;
  const float rise = open * (config::kBinLidRiseAbove * (1.0f - std::fabs(aim)) + config::kBinLidRiseAside) * unit;
  const float slide = -aim * open * config::kBinLidSlide * unit;
  lid_->SetTransform(D2D_MATRIX_3X2_F{cosine, sine, -sine, cosine, hinge_x - (hinge_x * cosine - hinge_y * sine) + slide,
                                      hinge_y - (hinge_x * sine + hinge_y * cosine) - rise + can_drop});
}

}  // namespace sstk
