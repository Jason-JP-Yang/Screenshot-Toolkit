#include "bin.h"

#include "theme.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr float kDegrees = 3.14159265f / 180.0f;
constexpr float kLidHingeX = 19.9f;
constexpr float kLidHingeY = 7.2f;
constexpr float kFlow = 0.006f;
constexpr float kFlowMax = 0.07f;
constexpr float kGulpImpulse = 1.6f;

}  // namespace

RubbishBin::RubbishBin(Compositor& compositor, IDCompositionVisual* parent, float scale)
    : compositor_(compositor), scale_(scale), height_(std::round(theme::kBinHeight * scale)),
      icon_size_(std::round(theme::kBinIconSize * scale)) {
  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  parent->AddVisual(root_.Get(), FALSE, nullptr);

  // The selector capsule's shadow, cut into two caps and a stretching middle so that it fits any width.
  const float pad = std::ceil(28.0f * scale);
  const ShadowLayer layers[] = {{9.0f * scale, 0.34f, 4.0f * scale}};
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

  const Image label = RenderLabel(L"Rubbish Bin", static_cast<int>(std::round(theme::kBinFontSize * scale)));
  label_height_ = static_cast<float>(label.height());
  label_start_ = std::round(theme::kBinLabelStart * scale);
  full_width_ = std::round(label_start_ + label.width() + theme::kBinLabelEnd * scale);

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

  ring_ = solid(static_cast<uint32_t>(theme::kCapsuleHairlineAlpha * 255.0f + 0.5f) << 24 | 0xFFFFFF, span);
  body_->AddVisual(ring_.Get(), FALSE, nullptr);

  inner_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_->SetClip(inner_clip_.Get());
  inner_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  shape_clip(inner_clip_.Get(), 1.0f);
  body_->AddVisual(inner_.Get(), FALSE, nullptr);

  fill_ = solid(0xFF000000u | theme::kBinRed, span);
  inner_->AddVisual(fill_.Get(), FALSE, nullptr);

  veil_ = solid(0xFFFFFFFF, span);
  veil_effect_ = compositor_.CreateEffectGroup();
  veil_->SetEffect(veil_effect_.Get());
  inner_->AddVisual(veil_.Get(), FALSE, nullptr);

  const int icon_pixels = static_cast<int>(icon_size_);
  const float icon_scale = icon_size_ / theme::kBinIconSize;
  icon_ = compositor_.CreateVisual();
  can_ = compositor_.CreateVisual();
  can_->SetContent(compositor_.CreateSurface(RenderBinCan(icon_pixels, icon_scale), false).Get());
  lid_ = compositor_.CreateVisual();
  lid_->SetContent(compositor_.CreateSurface(RenderBinLid(icon_pixels, icon_scale), false).Get());
  icon_->AddVisual(can_.Get(), FALSE, nullptr);
  icon_->AddVisual(lid_.Get(), FALSE, nullptr);
  icon_->SetOffsetY(-icon_size_ * 0.5f);
  inner_->AddVisual(icon_.Get(), FALSE, nullptr);

  label_ = compositor_.CreateVisual();
  label_->SetContent(compositor_.CreateSurface(label, false).Get());
  label_->SetOffsetY(std::round(-label_height_ * 0.5f));
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
  grow_.Snap(1.0f);
  squash_.Snap(0.0f);
  lid_angle_.Snap(0.0f);
  label_alpha_.Snap(0.0f);
  engaged_light_.Snap(0.0f);
  lean_x_.Snap(0.0f);
  lean_y_.Snap(0.0f);
  slamming_ = false;
}

void RubbishBin::Place(float center_x, float bottom) {
  const float center_y = std::round(bottom - theme::kBinMargin * scale_ - height_ * 0.5f);
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
}

void RubbishBin::Hide() {
  showing_ = false;
  hide_in_ = 0.0;
  presence_.target = 0.0f;
  near_ = false;
  engaged_ = false;
  lean_x_.target = 0.0f;
  lean_y_.target = 0.0f;
}

RECT RubbishBin::HotZone() const {
  const float margin = (engaged_ ? theme::kBinHotMarginEngaged : theme::kBinHotMargin) * scale_;
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
  const float near_limit = (theme::kBinNearDistance + (near_ ? theme::kBinNearHysteresis : 0.0f)) * scale_;
  near_ = distance < near_limit;
  engaged_ = Contains(point);
  right_leads_ = point.x >= center_x_.target;

  const float reach = theme::kBinMagnetMax * scale_;
  lean_x_.target = near_ ? std::clamp((point.x - center_x_.target) * theme::kBinMagnet, -reach, reach) : 0.0f;
  lean_y_.target = near_ ? std::clamp((point.y - center_y_.target) * theme::kBinMagnet, -reach, reach) : 0.0f;
  return engaged_;
}

void RubbishBin::Nudge(float impulse) { squash_.velocity += impulse; }

void RubbishBin::Gulp(bool then_hide) {
  slamming_ = true;
  engaged_ = false;
  Nudge(kGulpImpulse);
  hide_in_ = then_hide ? theme::kBinGulpHold : 0.0;
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
  grow_.target = engaged_ ? theme::kBinGrowEngaged : (expanded ? theme::kBinGrowNear : 1.0f);
  label_alpha_.target = expanded ? 1.0f : 0.0f;
  engaged_light_.target = engaged_ ? 1.0f : 0.0f;
  lid_angle_.target = slamming_ ? 0.0f : (engaged_ ? theme::kBinLidOpenDegrees : (near_ ? theme::kBinLidNearDegrees : 0.0f));

  presence_.Step(showing_ ? theme::kCapsuleEnter : theme::kCapsuleLeave, dt);
  // The edge nearer the pointer leads, the same liquid stretch as the selector's sliding pill.
  left_.Step(right_leads_ ? theme::kPillTrailing : theme::kPillLeading, dt);
  right_.Step(right_leads_ ? theme::kPillLeading : theme::kPillTrailing, dt);
  grow_.Step(theme::kBinGrow, dt);
  squash_.Step(theme::kBinSquash, dt);
  lid_angle_.Step(slamming_ ? theme::kBinLidSlam : theme::kBinLid, dt);
  label_alpha_.Step(theme::kFade, dt);
  engaged_light_.Step(theme::kFade, dt);
  center_x_.Step(theme::kReflow, dt);
  center_y_.Step(theme::kReflow, dt);
  lean_x_.Step(theme::kPillTrailing, dt);
  lean_y_.Step(theme::kPillTrailing, dt);
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
  const float fall = (1.0f - presence) * theme::kCapsuleRise * scale_;
  const float size = (0.90f + 0.10f * presence) * grow_.value;
  const float stretch = std::clamp(squash_.value, -0.3f, 0.3f);
  // Volume is kept: while it widens quickly, it thins a little.
  const float flow = std::clamp((right_.velocity - left_.velocity) / full_width_ * kFlow, -kFlowMax, kFlowMax);
  const float sx = size * (1.0f + stretch);
  const float sy = size * (1.0f - stretch - flow);
  root_->SetOffsetX(x());
  root_->SetOffsetY(y() + fall);
  root_->SetTransform(D2D_MATRIX_3X2_F{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f});
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
  veil_effect_->SetOpacity(theme::kBinEngagedVeil * std::clamp(engaged_light_.value, 0.0f, 1.0f));

  icon_->SetOffsetX(left + (height_ - icon_size_) * 0.5f);

  const float unit = icon_size_ / theme::kBinIconSize;
  const float hinge_x = kLidHingeX * unit;
  const float hinge_y = kLidHingeY * unit;
  const float radians = lid_angle_.value * kDegrees;
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  const float lift = std::clamp(lid_angle_.value / theme::kBinLidOpenDegrees, -0.2f, 1.2f) * theme::kBinLidLift * unit;
  lid_->SetTransform(D2D_MATRIX_3X2_F{cosine, sine, -sine, cosine, hinge_x - (hinge_x * cosine - hinge_y * sine),
                                      hinge_y - (hinge_x * sine + hinge_y * cosine) - lift});

  const float label_alpha = std::clamp(label_alpha_.value, 0.0f, 1.0f);
  label_->SetOffsetX(left + label_start_ + (1.0f - label_alpha) * 8.0f * scale_);
  label_effect_->SetOpacity(label_alpha);
}

}  // namespace sstk
