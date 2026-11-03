#include "sprite.h"

#include "theme.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr int kMargin = 2;
constexpr int kShadowDownscale = 4;
constexpr float kStretchEnd = 0.55f;
constexpr float kRunStart = 0.30f;
constexpr float kStretchLength = 0.24f;
constexpr float kBandOverlap = 1.0f;
constexpr float kBentFrom = 0.001f;
constexpr float kBadgeVisibleFrom = 0.02f;

struct Vec {
  float x, y;
};

Vec Transform(const D2D_MATRIX_3X2_F& m, float x, float y) {
  return Vec{x * m._11 + y * m._21 + m._31, x * m._12 + y * m._22 + m._32};
}

// Applies a, then b.
D2D_MATRIX_3X2_F Multiply(const D2D_MATRIX_3X2_F& a, const D2D_MATRIX_3X2_F& b) {
  D2D_MATRIX_3X2_F r;
  r._11 = a._11 * b._11 + a._12 * b._21;
  r._12 = a._11 * b._12 + a._12 * b._22;
  r._21 = a._21 * b._11 + a._22 * b._21;
  r._22 = a._21 * b._12 + a._22 * b._22;
  r._31 = a._31 * b._11 + a._32 * b._21 + b._31;
  r._32 = a._31 * b._12 + a._32 * b._22 + b._32;
  return r;
}

float Mix(float a, float b, float t) { return a + (b - a) * t; }

float Smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// The perspective transform taking the rectangle (0, top)-(width, top + height) onto a quad, so that a band's
// sides meet its neighbors' exactly however fast the funnel narrows. A collapsed quad becomes a speck.
D3DMATRIX QuadTransform(float width, float top, float height, Vec p0, Vec p1, Vec p2, Vec p3) {
  const float dx1 = p1.x - p2.x;
  const float dy1 = p1.y - p2.y;
  const float dx2 = p3.x - p2.x;
  const float dy2 = p3.y - p2.y;
  const float sx = p0.x - p1.x + p2.x - p3.x;
  const float sy = p0.y - p1.y + p2.y - p3.y;
  const float det = dx1 * dy2 - dx2 * dy1;

  D3DMATRIX m{};
  m._33 = 1.0f;
  if (std::fabs(det) < 0.01f) {
    m._11 = m._22 = 1e-3f;
    m._41 = p0.x;
    m._42 = p0.y;
    m._44 = 1.0f;
    return m;
  }
  const float g = (sx * dy2 - dx2 * sy) / det;
  const float h = (dx1 * sy - sx * dy1) / det;
  const float a = p1.x - p0.x + g * p1.x;
  const float b = p3.x - p0.x + h * p3.x;
  const float d = p1.y - p0.y + g * p1.y;
  const float e = p3.y - p0.y + h * p3.y;
  m._11 = a / width;
  m._12 = d / width;
  m._14 = g / width;
  m._21 = b / height;
  m._22 = e / height;
  m._24 = h / height;
  m._41 = p0.x - top * m._21;
  m._42 = p0.y - top * m._22;
  m._44 = 1.0f - top * m._24;
  return m;
}

}  // namespace

DragSprite::DragSprite(Compositor& compositor, const Image& thumbnail, float grab_x, float grab_y, float scale)
    : compositor_(compositor), grab_x_(grab_x + kMargin), grab_y_(grab_y + kMargin) {
  const float radius = theme::kCardRadius * scale;
  const float border = std::max(std::round(theme::kCardBorder * scale), 1.0f);
  const Image sprite = RenderSprite(thumbnail, radius, border, kMargin);
  surface_ = compositor_.CreateSurface(sprite, false);
  width_ = static_cast<float>(sprite.width());
  height_ = static_cast<float>(sprite.height());

  const float far_x = std::max(grab_x_, width_ - grab_x_);
  const float far_y = std::max(grab_y_, height_ - grab_y_);
  reach_ = std::sqrt(far_x * far_x + far_y * far_y);

  for (int i = 0; i <= kBandCount; ++i) {
    band_edges_[i] = static_cast<int>(std::round(height_ * i / kBandCount));
  }

  const float d = static_cast<float>(kShadowDownscale);
  const ShadowLayer layers[] = {
      {15.0f * scale / d, 0.34f, 11.0f * scale / d},
      {3.5f * scale / d, 0.16f, 2.5f * scale / d},
  };
  const int box_w = std::max(static_cast<int>(std::round(thumbnail.width() / d)), 1);
  const int box_h = std::max(static_cast<int>(std::round(thumbnail.height() / d)), 1);
  const float pad = std::ceil(50.0f * scale / d);
  const Image shadow = RenderShadow(box_w, box_h, radius / d, pad, layers, ARRAYSIZE(layers));
  shadow_surface_ = compositor_.CreateSurface(shadow, false);
  const float kx = static_cast<float>(thumbnail.width()) / box_w;
  const float ky = static_cast<float>(thumbnail.height()) / box_h;
  shadow_local_ = D2D_MATRIX_3X2_F{kx, 0.0f, 0.0f, ky, kMargin - pad * kx, kMargin - pad * ky};

  const int badge_size = static_cast<int>(std::round(theme::kDropBadgeSize * scale));
  const int badge_pad = static_cast<int>(std::ceil(7.0f * scale));
  const Image badge = RenderPlusBadge(badge_size, badge_pad, scale, theme::kDropBlue);
  badge_surface_ = compositor_.CreateSurface(badge, false);
  badge_half_ = badge.width() * 0.5f;
  corner_inset_ = kMargin + radius * 0.22f;
  neck_ = theme::kBinNeck * scale;
}

DragSprite::~DragSprite() {
  for (View& view : views_) view.layer->RemoveVisual(view.root.Get());
}

void DragSprite::AddView(IDCompositionVisual* layer, const RECT& bounds, const RECT& free) {
  View view;
  view.layer = layer;
  view.bounds = bounds;
  view.free = free;

  view.root = compositor_.CreateVisual();
  view.root_effect = compositor_.CreateEffectGroup();
  view.root->SetEffect(view.root_effect.Get());
  view.root->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  view.root->SetOffsetX(static_cast<float>(-bounds.left));
  view.root->SetOffsetY(static_cast<float>(-bounds.top));

  view.shadow_holder = compositor_.CreateVisual();
  view.shadow_clip = compositor_.CreateClip();
  view.shadow_holder->SetClip(view.shadow_clip.Get());
  view.root->AddVisual(view.shadow_holder.Get(), FALSE, nullptr);
  view.shadow = compositor_.CreateVisual();
  view.shadow->SetContent(shadow_surface_.Get());
  view.shadow_effect = compositor_.CreateEffectGroup();
  view.shadow->SetEffect(view.shadow_effect.Get());
  view.shadow_holder->AddVisual(view.shadow.Get(), FALSE, nullptr);

  view.whole = compositor_.CreateVisual();
  view.whole->SetContent(surface_.Get());
  view.root->AddVisual(view.whole.Get(), FALSE, nullptr);

  view.bands_root = compositor_.CreateVisual();
  for (int i = 0; i < kBandCount; ++i) {
    Band& band = view.bands[i];
    band.holder = compositor_.CreateVisual();
    band.effect = compositor_.CreateEffectGroup();
    compositor_.device()->CreateMatrixTransform3D(&band.transform);
    band.effect->SetTransform3D(band.transform.Get());
    band.holder->SetEffect(band.effect.Get());
    band.image = compositor_.CreateVisual();
    band.image->SetContent(surface_.Get());
    // One row past its own, so that neighbors overlap instead of leaving a seam.
    const float bottom = std::min(band_edges_[i + 1] + kBandOverlap, height_);
    band.image->SetClip(D2D_RECT_F{0.0f, static_cast<float>(band_edges_[i]), width_, bottom});
    band.image->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    band.holder->AddVisual(band.image.Get(), FALSE, nullptr);
    view.bands_root->AddVisual(band.holder.Get(), FALSE, nullptr);
  }

  view.badge = compositor_.CreateVisual();
  view.badge->SetContent(badge_surface_.Get());
  view.badge_effect = compositor_.CreateEffectGroup();
  view.badge->SetEffect(view.badge_effect.Get());
  view.root->AddVisual(view.badge.Get(), FALSE, nullptr);

  layer->AddVisual(view.root.Get(), FALSE, nullptr);
  views_.push_back(std::move(view));
}

void DragSprite::ComputeBands(const D2D_MATRIX_3X2_F& rigid, const SpritePose& pose) {
  const float pull = std::clamp(pose.pull, 0.0f, 1.0f);
  const float stretch = Smooth(pull / kStretchEnd);
  const float run = Smooth((pull - kRunStart) / (1.0f - kRunStart));

  const float top_y = Transform(rigid, width_ * 0.5f, 0.0f).y;
  const float bottom_y = Transform(rigid, width_ * 0.5f, height_).y;
  const bool bottom_is_near = std::fabs(bottom_y - pose.pull_y) <= std::fabs(top_y - pose.pull_y);
  const float far_y = bottom_is_near ? top_y : bottom_y;
  const float near_y = bottom_is_near ? bottom_y : top_y;
  const float span = pose.pull_y - far_y;
  // Drawn out before it goes: the far edge first pulls away as the near edge reaches for the bin.
  const float length = std::sqrt(rigid._21 * rigid._21 + rigid._22 * rigid._22) * height_;
  const float far_out = far_y - std::copysign(length * kStretchLength * stretch, span);
  const float far_now = Mix(far_out, pose.pull_y, run);
  const float near_now = Mix(near_y, pose.pull_y, stretch);
  const Vec width_axis{rigid._11 * width_, rigid._12 * width_};
  const float neck = std::min(neck_ / std::max(std::sqrt(width_axis.x * width_axis.x + width_axis.y * width_axis.y), 1.0f), 1.0f);

  // A band's place along the funnel decides its width and how far it has swung toward the bin.
  auto bend = [&](float row, Vec* left, Vec* right) {
    const float t = bottom_is_near ? row / height_ : 1.0f - row / height_;
    const float y = Mix(far_now, near_now, t);
    const float along = std::fabs(span) > 1.0f ? std::clamp((y - far_y) / span, 0.0f, 1.0f) : 1.0f;
    const float inward = Smooth(along) * stretch;
    const Vec mid = Transform(rigid, width_ * 0.5f, row);
    const float half = Mix(1.0f, neck, inward) * 0.5f;
    const float x = Mix(mid.x, pose.pull_x, inward);
    *left = Vec{x - width_axis.x * half, y - width_axis.y * half};
    *right = Vec{x + width_axis.x * half, y + width_axis.y * half};
  };

  Vec top_left;
  Vec top_right;
  bend(0.0f, &top_left, &top_right);
  for (int i = 0; i < kBandCount; ++i) {
    const float top = static_cast<float>(band_edges_[i]);
    const float bottom = static_cast<float>(band_edges_[i + 1]);
    Vec bottom_left;
    Vec bottom_right;
    bend(bottom, &bottom_left, &bottom_right);
    band_matrices_[i] = QuadTransform(width_, top, std::max(bottom - top, 1.0f), top_left, top_right, bottom_right, bottom_left);

    top_left = bottom_left;
    top_right = bottom_right;
  }
}

void DragSprite::Render(const SpritePose& pose) {
  const float cosine = std::cos(pose.angle) * pose.scale;
  const float sine = std::sin(pose.angle) * pose.scale;
  D2D_MATRIX_3X2_F rigid{cosine, sine, -sine, cosine, 0.0f, 0.0f};
  rigid._31 = pose.x - (grab_x_ * rigid._11 + grab_y_ * rigid._21);
  rigid._32 = pose.y - (grab_x_ * rigid._12 + grab_y_ * rigid._22);

  const bool bent = pose.pull > kBentFrom;
  if (bent) ComputeBands(rigid, pose);

  D2D_MATRIX_3X2_F shadow = Multiply(shadow_local_, rigid);
  shadow._32 += pose.shadow_drop;

  const Vec corner = Transform(rigid, corner_inset_, corner_inset_);
  const float badge_scale = std::max(pose.badge, 0.0f);
  const D2D_MATRIX_3X2_F badge{badge_scale, 0.0f, 0.0f, badge_scale, corner.x - badge_half_ * badge_scale, corner.y - badge_half_ * badge_scale};
  const float badge_alpha = std::clamp((pose.badge - kBadgeVisibleFrom) * 2.2f, 0.0f, 1.0f);

  for (View& view : views_) {
    view.root_effect->SetOpacity(pose.opacity);

    view.shadow_effect->SetOpacity(pose.shadow);
    view.shadow->SetTransform(shadow);
    const float t = std::clamp(pose.shadow_free, 0.0f, 1.0f);
    view.shadow_clip->SetLeft(Mix(static_cast<float>(view.free.left), static_cast<float>(view.bounds.left), t));
    view.shadow_clip->SetTop(Mix(static_cast<float>(view.free.top), static_cast<float>(view.bounds.top), t));
    view.shadow_clip->SetRight(Mix(static_cast<float>(view.free.right), static_cast<float>(view.bounds.right), t));
    view.shadow_clip->SetBottom(Mix(static_cast<float>(view.free.bottom), static_cast<float>(view.bounds.bottom), t));

    if (bent != view.bent) {
      view.bent = bent;
      if (bent) {
        view.root->RemoveVisual(view.whole.Get());
        view.root->AddVisual(view.bands_root.Get(), TRUE, view.shadow_holder.Get());
      } else {
        view.root->RemoveVisual(view.bands_root.Get());
        view.root->AddVisual(view.whole.Get(), TRUE, view.shadow_holder.Get());
      }
    }
    if (bent) {
      for (int i = 0; i < kBandCount; ++i) view.bands[i].transform->SetMatrix(band_matrices_[i]);
    } else {
      view.whole->SetTransform(rigid);
    }

    view.badge->SetTransform(badge);
    view.badge_effect->SetOpacity(badge_alpha);
  }
}

}  // namespace sstk
