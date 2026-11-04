#include "sprite.h"

#include "theme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace sstk {

namespace {

constexpr int kMargin = 2;
constexpr int kShadowDownscale = 4;
constexpr float kMeshCells = 170.0f;
constexpr int kMeshMinCuts = 6;
constexpr float kCellOverlap = 1.0f;
constexpr float kBentFrom = 0.001f;
constexpr float kBadgeVisibleFrom = 0.02f;
constexpr float kFarthestLag = 0.45f;  // How much of the pull passes before the farthest point starts moving, relative to the nearest.
constexpr float kSmallestQuadArea = 0.25f;
constexpr float kShallowestDepth = 0.12f;

struct Vec {
  float x, y;
};

Vec operator-(Vec a, Vec b) { return Vec{a.x - b.x, a.y - b.y}; }

float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }

float Length(Vec a) { return std::sqrt(Dot(a, a)); }

float Cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }

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

// The perspective transform taking one rectangle onto a quad, so that a cell's sides meet its neighbors' exactly
// however hard the mesh is pulled. A quad too thin to see, or folded over itself, becomes a speck.
D3DMATRIX QuadTransform(float left, float top, float width, float height, Vec p0, Vec p1, Vec p2, Vec p3) {
  D3DMATRIX m{};
  m._33 = 1.0f;

  const float z0 = Cross(p1 - p0, p2 - p1);
  const float z1 = Cross(p2 - p1, p3 - p2);
  const float z2 = Cross(p3 - p2, p0 - p3);
  const float z3 = Cross(p0 - p3, p1 - p0);
  const bool convex = (z0 >= 0.0f && z1 >= 0.0f && z2 >= 0.0f && z3 >= 0.0f) || (z0 <= 0.0f && z1 <= 0.0f && z2 <= 0.0f && z3 <= 0.0f);
  const float area = std::fabs(Cross(p2 - p0, p3 - p1)) * 0.5f;

  const float dx1 = p1.x - p2.x;
  const float dy1 = p1.y - p2.y;
  const float dx2 = p3.x - p2.x;
  const float dy2 = p3.y - p2.y;
  const float det = dx1 * dy2 - dx2 * dy1;
  if (!convex || area < kSmallestQuadArea || std::fabs(det) < 0.01f) {
    m._11 = m._22 = 1e-3f;
    m._41 = p0.x;
    m._42 = p0.y;
    m._44 = 1.0f;
    return m;
  }

  const float sx = p0.x - p1.x + p2.x - p3.x;
  const float sy = p0.y - p1.y + p2.y - p3.y;
  const float g = (sx * dy2 - dx2 * sy) / det;
  const float h = (dx1 * sy - sx * dy1) / det;

  // The clip reaches one pixel past the cell. Perspective steep enough to pass zero depth out there
  // would throw the image across the screen, so such a cell goes flat.
  const float depth = 1.0f + std::min(g * (1.0f + kCellOverlap / width), 0.0f) + std::min(h * (1.0f + kCellOverlap / height), 0.0f);
  if (depth < kShallowestDepth) {
    const Vec along{(p1.x - p0.x + p2.x - p3.x) * 0.5f, (p1.y - p0.y + p2.y - p3.y) * 0.5f};
    const Vec down{(p3.x - p0.x + p2.x - p1.x) * 0.5f, (p3.y - p0.y + p2.y - p1.y) * 0.5f};
    const Vec origin{(p0.x + p1.x + p2.x + p3.x - 2.0f * (along.x + down.x)) * 0.25f, (p0.y + p1.y + p2.y + p3.y - 2.0f * (along.y + down.y)) * 0.25f};
    m._11 = along.x / width;
    m._12 = along.y / width;
    m._21 = down.x / height;
    m._22 = down.y / height;
    m._41 = origin.x - left * m._11 - top * m._21;
    m._42 = origin.y - left * m._12 - top * m._22;
    m._44 = 1.0f;
    return m;
  }

  m._11 = (p1.x - p0.x + g * p1.x) / width;
  m._12 = (p1.y - p0.y + g * p1.y) / width;
  m._14 = g / width;
  m._21 = (p3.x - p0.x + h * p3.x) / height;
  m._22 = (p3.y - p0.y + h * p3.y) / height;
  m._24 = h / height;
  m._41 = p0.x - left * m._11 - top * m._21;
  m._42 = p0.y - left * m._12 - top * m._22;
  m._44 = 1.0f - left * m._14 - top * m._24;
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

  // Square cells, of about the same number whatever the card's shape.
  const float pitch = std::sqrt(width_ * height_ / kMeshCells);
  columns_ = std::max(static_cast<int>(std::round(width_ / pitch)), kMeshMinCuts);
  rows_ = std::max(static_cast<int>(std::round(height_ / pitch)), kMeshMinCuts);
  column_edges_.resize(columns_ + 1);
  row_edges_.resize(rows_ + 1);
  for (int i = 0; i <= columns_; ++i) column_edges_[i] = std::round(width_ * i / columns_);
  for (int i = 0; i <= rows_; ++i) row_edges_[i] = std::round(height_ * i / rows_);
  cell_matrices_.resize(static_cast<size_t>(columns_) * rows_);

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

  view.badge = compositor_.CreateVisual();
  view.badge->SetContent(badge_surface_.Get());
  view.badge_effect = compositor_.CreateEffectGroup();
  view.badge->SetEffect(view.badge_effect.Get());
  view.root->AddVisual(view.badge.Get(), FALSE, nullptr);

  layer->AddVisual(view.root.Get(), FALSE, nullptr);
  views_.push_back(std::move(view));
}

void DragSprite::BuildMesh(View& view) {
  view.mesh_root = compositor_.CreateVisual();
  view.cells.resize(cell_matrices_.size());
  size_t index = 0;
  for (int row = 0; row < rows_; ++row) {
    for (int column = 0; column < columns_; ++column, ++index) {
      Cell& cell = view.cells[index];
      cell.holder = compositor_.CreateVisual();
      cell.effect = compositor_.CreateEffectGroup();
      compositor_.device()->CreateMatrixTransform3D(&cell.transform);
      cell.effect->SetTransform3D(cell.transform.Get());
      cell.holder->SetEffect(cell.effect.Get());
      cell.image = compositor_.CreateVisual();
      cell.image->SetContent(surface_.Get());
      // One pixel past its own on two sides, so that neighbors overlap instead of leaving a seam.
      cell.image->SetClip(D2D_RECT_F{column_edges_[column], row_edges_[row], std::min(column_edges_[column + 1] + kCellOverlap, width_),
                                     std::min(row_edges_[row + 1] + kCellOverlap, height_)});
      cell.image->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
      cell.holder->AddVisual(cell.image.Get(), FALSE, nullptr);
      view.mesh_root->AddVisual(cell.holder.Get(), FALSE, nullptr);
    }
  }
}

void DragSprite::ComputeMesh(const D2D_MATRIX_3X2_F& rigid, const SpritePose& pose) {
  const float pull = std::clamp(pose.pull, 0.0f, 1.0f);
  const Vec mouth{pose.pull_x, pose.pull_y};

  // Points leave in order along the line from the card to the mouth, which keeps the far edge from
  // breaking into horns as it runs in. Everything arrives on a short line across the mouth.
  const Vec toward = mouth - Transform(rigid, width_ * 0.5f, height_ * 0.5f);
  const float toward_length = std::max(Length(toward), 1.0f);
  const Vec axis{toward.x / toward_length, toward.y / toward_length};
  const Vec across{-axis.y, axis.x};
  const Vec corners[4] = {Transform(rigid, 0.0f, 0.0f), Transform(rigid, width_, 0.0f), Transform(rigid, width_, height_), Transform(rigid, 0.0f, height_)};
  float nearest = FLT_MAX;
  float farthest = -FLT_MAX;
  float half_span = 1.0f;
  for (const Vec& corner : corners) {
    const float away = -Dot(corner - mouth, axis);
    nearest = std::min(nearest, away);
    farthest = std::max(farthest, away);
    half_span = std::max(half_span, std::fabs(Dot(corner - mouth, across)));
  }
  const float depth = std::max(farthest - nearest, 1.0f);

  auto place = [&](float x, float y) {
    const Vec point = Transform(rigid, x, y);
    const Vec from_mouth = point - mouth;
    const float along = (-Dot(from_mouth, axis) - nearest) / depth;
    const float progress = std::clamp((1.0f + kFarthestLag) * pull - kFarthestLag * along, 0.0f, 1.0f);
    // Slow to come away from the card, quickening into the mouth.
    const float eased = progress * std::sqrt(progress);
    const float offset = Dot(from_mouth, across) / half_span * neck_ * 0.5f;
    return Vec{Mix(point.x, mouth.x + across.x * offset, eased), Mix(point.y, mouth.y + across.y * offset, eased)};
  };

  std::vector<Vec> above(columns_ + 1);
  std::vector<Vec> below(columns_ + 1);
  for (int column = 0; column <= columns_; ++column) above[column] = place(column_edges_[column], row_edges_[0]);
  size_t index = 0;
  for (int row = 0; row < rows_; ++row) {
    for (int column = 0; column <= columns_; ++column) below[column] = place(column_edges_[column], row_edges_[row + 1]);
    for (int column = 0; column < columns_; ++column, ++index) {
      const float left = column_edges_[column];
      const float top = row_edges_[row];
      cell_matrices_[index] = QuadTransform(left, top, std::max(column_edges_[column + 1] - left, 1.0f), std::max(row_edges_[row + 1] - top, 1.0f),
                                            above[column], above[column + 1], below[column + 1], below[column]);
    }
    above.swap(below);
  }
}

void DragSprite::Render(const SpritePose& pose) {
  const float cosine = std::cos(pose.angle) * pose.scale;
  const float sine = std::sin(pose.angle) * pose.scale;
  D2D_MATRIX_3X2_F rigid{cosine, sine, -sine, cosine, 0.0f, 0.0f};
  rigid._31 = pose.x - (grab_x_ * rigid._11 + grab_y_ * rigid._21);
  rigid._32 = pose.y - (grab_x_ * rigid._12 + grab_y_ * rigid._22);

  const bool bent = pose.pull > kBentFrom;
  if (bent) ComputeMesh(rigid, pose);

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
        if (view.cells.empty()) BuildMesh(view);
        view.root->RemoveVisual(view.whole.Get());
        view.root->AddVisual(view.mesh_root.Get(), TRUE, view.shadow_holder.Get());
      } else {
        view.root->RemoveVisual(view.mesh_root.Get());
        view.root->AddVisual(view.whole.Get(), TRUE, view.shadow_holder.Get());
      }
    }
    if (bent) {
      for (size_t i = 0; i < view.cells.size(); ++i) view.cells[i].transform->SetMatrix(cell_matrices_[i]);
    } else {
      view.whole->SetTransform(rigid);
    }

    view.badge->SetTransform(badge);
    view.badge_effect->SetOpacity(badge_alpha);
  }
}

}  // namespace sstk
