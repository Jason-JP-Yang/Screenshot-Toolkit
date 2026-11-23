#include "sstk/editor/toolbar.h"

#include "sstk/config/animation.h"
#include "sstk/config/layout.h"
#include "sstk/editor/art.h"
#include "sstk/editor/brush.h"

#include <algorithm>
#include <cmath>

namespace sstk {

namespace {

constexpr float kTwoPi = 6.2831853f;

LONG Px(float dips, float scale) { return std::lround(dips * scale); }

// Lays one premultiplied image over another.
void Over(Image& canvas, const Image& part, int at_x, int at_y) {
  if (!canvas || !part) return;
  for (int y = 0; y < part.height(); ++y) {
    const int to_y = at_y + y;
    if (to_y < 0 || to_y >= canvas.height()) continue;
    const uint8_t* from = part.data() + static_cast<ptrdiff_t>(y) * part.stride();
    uint8_t* to = canvas.data() + static_cast<ptrdiff_t>(to_y) * canvas.stride();
    for (int x = 0; x < part.width(); ++x) {
      const int to_x = at_x + x;
      if (to_x < 0 || to_x >= canvas.width()) continue;
      const uint8_t* source = from + static_cast<ptrdiff_t>(x) * 4;
      uint8_t* target = to + static_cast<ptrdiff_t>(to_x) * 4;
      const int keep = 255 - source[3];
      for (int c = 0; c < 4; ++c) target[c] = static_cast<uint8_t>(source[c] + (target[c] * keep + 127) / 255);
    }
  }
}

ComPtr<IDCompositionVisual> ImageVisual(Compositor& compositor, const Image& image) {
  ComPtr<IDCompositionVisual> visual = compositor.CreateVisual();
  visual->SetContent(compositor.CreateSurface(image, false).Get());
  return visual;
}

// A sample of a tool's mark at one width, drawn by the tool itself, in a square cell.
Image RenderSample(Tool tool, float width, float scale, uint32_t color, int cell) {
  Stroke sample;
  sample.tool = tool;
  sample.color = color;
  sample.opacity = 1.0f;
  sample.width = width;
  sample.scale = scale;
  sample.seed = 7u;
  const float unit = static_cast<float>(cell) / 46.0f;
  for (int i = 0; i <= 64; ++i) {
    const float t = static_cast<float>(i) / 64.0f;
    InkPoint point{};
    point.x = (11.0f + 24.0f * t) * unit;
    point.y = (23.0f + 8.0f * std::sin(kTwoPi * t * 0.85f + 0.4f)) * unit;
    point.pressure = 0.35f + 0.65f * std::sin(3.14159265f * t);
    point.time = t * 0.3;
    sample.points.push_back(point);
  }
  StampTrail trail;
  LayStamps(sample, trail, true);
  std::vector<float> mask(static_cast<size_t>(cell) * cell, 0.0f);
  DrawStroke(sample, RECT{0, 0, cell, cell}, mask.data(), cell);
  Image layer = Image::Allocate(cell, cell);
  if (!layer) return layer;
  std::fill_n(reinterpret_cast<uint32_t*>(layer.data()), static_cast<size_t>(cell) * cell, 0u);
  PaintStroke(sample, mask.data(), cell, reinterpret_cast<uint32_t*>(layer.data()), cell, cell, cell);
  return layer;
}

}  // namespace

MarkupToolbar::MarkupToolbar(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor, float ceiling,
                             const config::EditorPalette& palette)
    : compositor_(compositor), scale_(scale), monitor_(monitor), ceiling_(ceiling), palette_(palette) {
  layer_ = compositor_.CreateVisual();
  layer_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  parent->AddVisual(layer_.Get(), FALSE, nullptr);

  const LONG pad = Px(config::kToolbarPadding, scale);
  const LONG cell = Px(config::kToolCell, scale);
  const LONG swatch = Px(config::kSwatch, scale);
  const LONG swatch_gap = Px(config::kSwatchGap, scale);
  const LONG section = Px(config::kToolbarSection, scale);
  const LONG fold = Px(config::kToolbarFold, scale);
  height_ = static_cast<float>(Px(config::kToolbarHeight, scale));
  width_ = static_cast<float>(pad + cell * kToolCount + section + swatch * 3 + swatch_gap * 2 + section + fold + pad);
  const float longest = std::max(width_, height_);
  const float radius = config::kToolbarRadius * scale;
  const LONG button = Px(config::kMarkupButton, scale);
  dock_ = static_cast<ToolbarDock>(std::clamp(Tools().dock, 0, kToolbarDocks - 1));
  if (!Fits(dock_)) dock_ = ToolbarDock::kBottom;
  button_right_ = side() == ToolbarSide::kRight;
  button_rect_ = ButtonRect(button_right_);

  // Where a dragged toolbar will land.
  landing_ = compositor_.CreateVisual();
  landing_effect_ = compositor_.CreateEffectGroup();
  landing_->SetEffect(landing_effect_.Get());
  landing_clip_ = compositor_.CreateClip();
  landing_clip_->SetLeft(0.0f);
  landing_clip_->SetTop(0.0f);
  landing_->SetClip(landing_clip_.Get());
  landing_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  const float landing_alpha = std::min(palette.hover_alpha * config::kToolbarLandingShare, 1.0f);
  landing_->AddVisual(compositor_.CreateSolidVisual(static_cast<uint32_t>(landing_alpha * 255.0f + 0.5f) << 24 | palette.hover, longest, longest).Get(), FALSE,
                      nullptr);
  layer_->AddVisual(landing_.Get(), FALSE, nullptr);

  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  layer_->AddVisual(root_.Get(), FALSE, nullptr);
  shape_ = compositor_.CreateVisual();
  root_->AddVisual(shape_.Get(), FALSE, nullptr);

  // Two shadows, the toolbar's and the button's, as the shape between them moves too fast to need one of its own. The
  // toolbar's is drawn for its dock.
  const config::ShadowSpec& cast = config::kToolbarShadow;
  const ShadowLayer layers[] = {{cast.sigma * scale, cast.alpha, cast.offset * scale}};
  const float shadow_pad = std::ceil(cast.sigma * 3.0f * scale + cast.offset * scale);
  const auto shadow = [&](ComPtr<IDCompositionVisual>& visual, ComPtr<IDCompositionEffectGroup>& effect) {
    visual = compositor_.CreateVisual();
    visual->SetOffsetX(-shadow_pad);
    visual->SetOffsetY(-shadow_pad);
    effect = compositor_.CreateEffectGroup();
    visual->SetEffect(effect.Get());
    shape_->AddVisual(visual.Get(), FALSE, nullptr);
  };
  shadow(wide_shadow_, wide_shadow_effect_);
  shadow(round_shadow_, round_shadow_effect_);
  round_shadow_->SetContent(compositor_.CreateSurface(RenderShadow(button, button, static_cast<float>(button) * 0.5f, shadow_pad, layers, 1), false).Get());

  // A hairline between two clips that follow the shape, and the panel within.
  outer_ = compositor_.CreateVisual();
  outer_clip_ = compositor_.CreateClip();
  outer_clip_->SetLeft(0.0f);
  outer_clip_->SetTop(0.0f);
  outer_->SetClip(outer_clip_.Get());
  outer_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  outer_->AddVisual(compositor_.CreateSolidVisual(static_cast<uint32_t>(palette.hairline_alpha * 255.0f + 0.5f) << 24 | palette.hairline, longest, longest).Get(),
                    FALSE, nullptr);
  shape_->AddVisual(outer_.Get(), FALSE, nullptr);
  inner_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_clip_->SetLeft(1.0f);
  inner_clip_->SetTop(1.0f);
  inner_->SetClip(inner_clip_.Get());
  inner_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  inner_->AddVisual(compositor_.CreateSolidVisual(0xFF000000u | palette.panel, longest, longest).Get(), FALSE, nullptr);
  outer_->AddVisual(inner_.Get(), FALSE, nullptr);
  contents_ = compositor_.CreateVisual();
  contents_effect_ = compositor_.CreateEffectGroup();
  contents_->SetEffect(contents_effect_.Get());
  inner_->AddVisual(contents_.Get(), FALSE, nullptr);

  tools_ = compositor_.CreateVisual();
  tools_clip_ = compositor_.CreateClip();
  tools_clip_->SetLeft(1.0f);
  tools_clip_->SetTop(1.0f);
  SetClipRadii(tools_clip_.Get(), radius - 1.0f, nullptr);
  tools_->SetClip(tools_clip_.Get());
  contents_->AddVisual(tools_.Get(), FALSE, nullptr);

  for (int i = 0; i < kToolCount; ++i) {
    Slot& slot = slots_[static_cast<size_t>(i)];
    slot.tool = static_cast<Tool>(i);
    slot.holder = compositor_.CreateVisual();
    slot.visual = compositor_.CreateVisual();
    slot.holder->AddVisual(slot.visual.Get(), FALSE, nullptr);
    tools_->AddVisual(slot.holder.Get(), FALSE, nullptr);
    slot.lift_x.Bind(slot.visual.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
    slot.lift_y.Bind(slot.visual.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  }

  for (int i = 0; i < 6; ++i) {
    Swatch entry;
    entry.hue = i == 5;
    entry.color = i < 5 ? config::kInkColors[i] : 0;
    entry.visual = compositor_.CreateVisual();
    contents_->AddVisual(entry.visual.Get(), FALSE, nullptr);
    swatches_.push_back(std::move(entry));
  }
  RenderSwatches();

  fold_face_ = ImageVisual(compositor_, RenderBox(fold, fold, static_cast<float>(fold) * 0.5f, palette.control, 1.0f));
  contents_->AddVisual(fold_face_.Get(), FALSE, nullptr);
  fold_veil_ = ImageVisual(compositor_, RenderBox(fold, fold, static_cast<float>(fold) * 0.5f, palette.hover, 1.0f));
  fold_veil_effect_ = compositor_.CreateEffectGroup();
  fold_veil_->SetEffect(fold_veil_effect_.Get());
  contents_->AddVisual(fold_veil_.Get(), FALSE, nullptr);
  const int icon = static_cast<int>(Px(config::kEditorIcon, scale));
  fold_icon_ = ImageVisual(compositor_, RenderEditorIcon(EditorIcon::kFold, icon, scale, palette.control_ink));
  contents_->AddVisual(fold_icon_.Get(), FALSE, nullptr);

  // Folded, the shape shows the tool in hand at a slant, the stroke it has just drawn trailing from its point.
  badge_ = compositor_.CreateVisual();
  badge_effect_ = compositor_.CreateEffectGroup();
  badge_->SetEffect(badge_effect_.Get());
  badge_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  badge_line_ = compositor_.CreateVisual();
  badge_art_ = compositor_.CreateVisual();
  badge_art_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  badge_veil_ = ImageVisual(compositor_, RenderBox(button, button, static_cast<float>(button) * 0.5f, palette.hover, 1.0f));
  badge_veil_effect_ = compositor_.CreateEffectGroup();
  badge_veil_->SetEffect(badge_veil_effect_.Get());
  badge_->AddVisual(badge_line_.Get(), FALSE, nullptr);
  badge_->AddVisual(badge_art_.Get(), FALSE, nullptr);
  badge_->AddVisual(badge_veil_.Get(), FALSE, nullptr);
  inner_->AddVisual(badge_.Get(), FALSE, nullptr);

  popover_root_ = compositor_.CreateVisual();
  popover_effect_ = compositor_.CreateEffectGroup();
  popover_root_->SetEffect(popover_effect_.Get());
  popover_scale_ = compositor_.CreateScale();
  popover_root_->SetTransform(popover_scale_.Get());
  popover_body_ = compositor_.CreateVisual();
  popover_root_->AddVisual(popover_body_.Get(), FALSE, nullptr);
  knob_ = compositor_.CreateVisual();
  popover_root_->AddVisual(knob_.Get(), FALSE, nullptr);
  layer_->AddVisual(popover_root_.Get(), FALSE, nullptr);

  const auto opacity = SSTK_SETTER(IDCompositionEffectGroup, SetOpacity);
  alpha_.Bind(root_effect_.Get(), opacity);
  slide_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  fold_hover_.Bind(fold_veil_effect_.Get(), opacity);
  badge_hover_.Bind(badge_veil_effect_.Get(), opacity);
  shape_x_.Bind(shape_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  shape_y_.Bind(shape_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  shape_w_.Bind(outer_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  shape_w_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight), -1.0f);
  shape_h_.Bind(outer_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  shape_h_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom), -1.0f);
  shape_r_.Bind(outer_clip_.Get(), SetClipRadii);
  shape_r_.Bind(inner_clip_.Get(), SetClipRadii, -1.0f);
  contents_alpha_.Bind(contents_effect_.Get(), opacity);
  badge_alpha_.Bind(badge_effect_.Get(), opacity);
  wide_shadow_alpha_.Bind(wide_shadow_effect_.Get(), opacity);
  round_shadow_alpha_.Bind(round_shadow_effect_.Get(), opacity);
  popover_alpha_.Bind(popover_effect_.Get(), opacity);
  popover_pop_.Bind(popover_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  popover_pop_.Bind(popover_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));
  landing_x_.Bind(landing_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  landing_y_.Bind(landing_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  landing_w_.Bind(landing_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  landing_h_.Bind(landing_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  landing_alpha_.Bind(landing_effect_.Get(), opacity);
  alpha_.Snap(0.0f);
  slide_.Snap(0.0f);
  fold_hover_.Snap(0.0f);
  badge_hover_.Snap(0.0f);
  popover_alpha_.Snap(0.0f);
  popover_pop_.Snap(1.0f);
  landing_alpha_.Snap(0.0f);
  Layout();
  Morph(false, TimeBase{}, true);
  RenderBadge();
}

Image MarkupToolbar::ToolImage(Tool tool, bool percent, int turns) const {
  ToolLook look{palette_.barrel, palette_.barrel_shade, palette_.barrel_edge, palette_.barrel_edge, -1, palette_.control_ink};
  look.label_turns = turns;
  if (IsInk(tool)) {
    const ToolBox::Ink& ink = Tools().inks[static_cast<size_t>(tool)];
    look.ink = ink.color;
    const int shown = static_cast<int>(std::lround(ink.opacity * 100.0f));
    if (percent && shown < 100) look.percent = shown;
  }
  Image art = RenderToolArt(tool, static_cast<int>(Px(config::kToolArtWidth, scale_)), static_cast<int>(Px(config::kToolArtHeight, scale_)), scale_, look);
  return turns == 0 ? std::move(art) : TurnImage(art, turns);
}

void MarkupToolbar::RenderTool(Slot& slot) {
  slot.art = compositor_.CreateSurface(ToolImage(slot.tool, true, Turns()), false);
  slot.visual->SetContent(slot.art.Get());
}

ToolbarSide MarkupToolbar::side() const {
  switch (dock_) {
    case ToolbarDock::kTopLeft:
    case ToolbarDock::kTop:
    case ToolbarDock::kTopRight:
      return ToolbarSide::kTop;
    case ToolbarDock::kLeft:
      return ToolbarSide::kLeft;
    case ToolbarDock::kRight:
      return ToolbarSide::kRight;
    default:
      return ToolbarSide::kBottom;
  }
}

int MarkupToolbar::Turns() const {
  switch (side()) {
    case ToolbarSide::kTop:
      return 2;
    case ToolbarSide::kLeft:
      return 1;
    case ToolbarSide::kRight:
      return 3;
    default:
      return 0;
  }
}

RECT MarkupToolbar::DockRect(ToolbarDock dock) const {
  const LONG margin = Px(config::kEditorMargin, scale_);
  const LONG w = std::lround(width_);
  const LONG h = std::lround(height_);
  const LONG top = std::lround(ceiling_);
  const LONG bottom = monitor_.cy - margin;
  const LONG middle = std::lround((static_cast<float>(monitor_.cx) - width_) * 0.5f);
  const LONG upright = std::clamp(std::lround((static_cast<float>(top + bottom) - width_) * 0.5f), top, std::max(bottom - w, top));
  switch (dock) {
    case ToolbarDock::kBottomLeft:
      return RECT{margin, bottom - h, margin + w, bottom};
    case ToolbarDock::kBottomRight:
      return RECT{monitor_.cx - margin - w, bottom - h, monitor_.cx - margin, bottom};
    case ToolbarDock::kTopLeft:
      return RECT{margin, top, margin + w, top + h};
    case ToolbarDock::kTop:
      return RECT{middle, top, middle + w, top + h};
    case ToolbarDock::kTopRight:
      return RECT{monitor_.cx - margin - w, top, monitor_.cx - margin, top + h};
    case ToolbarDock::kLeft:
      return RECT{margin, upright, margin + h, upright + w};
    case ToolbarDock::kRight:
      return RECT{monitor_.cx - margin - h, upright, monitor_.cx - margin, upright + w};
    default:
      return RECT{middle, bottom - h, middle + w, bottom};
  }
}

// Standing up along a side needs the height of the toolbar's length below the controls; lying along an edge, the
// places at either end need room apart from the middle.
bool MarkupToolbar::Fits(ToolbarDock dock) const {
  const LONG margin = Px(config::kEditorMargin, scale_);
  if (Upright(dock)) return static_cast<float>(monitor_.cy - margin) - ceiling_ >= width_;
  if (dock == ToolbarDock::kBottom || dock == ToolbarDock::kTop) return true;
  return static_cast<float>(monitor_.cx - margin * 2) >= width_ * 1.4f;
}

ToolbarDock MarkupToolbar::NearestDock(POINT center) const {
  ToolbarDock nearest = ToolbarDock::kBottom;
  double best = 1e300;
  for (int i = 0; i < kToolbarDocks; ++i) {
    const auto dock = static_cast<ToolbarDock>(i);
    if (!Fits(dock)) continue;
    const RECT rect = DockRect(dock);
    const double dx = (rect.left + rect.right) * 0.5 - center.x;
    const double dy = (rect.top + rect.bottom) * 0.5 - center.y;
    if (dx * dx + dy * dy < best) {
      best = dx * dx + dy * dy;
      nearest = dock;
    }
  }
  return nearest;
}

RECT MarkupToolbar::ButtonRect(bool right) const {
  const LONG margin = Px(config::kEditorMargin, scale_);
  const LONG button = Px(config::kMarkupButton, scale_);
  const LONG left = right ? monitor_.cx - margin - button : margin;
  return RECT{left, monitor_.cy - margin - button, left + button, monitor_.cy - margin};
}

// Laid along an edge the tools stand side by side across the toolbar's depth, then the colors three by two, then the
// button that folds it; standing along a side, the same from the top down, the colors two by three. The tools lie
// turned to point away from the edge, their bodies running on past it.
void MarkupToolbar::Layout() {
  const int turns = Turns();
  const bool upright = Upright(dock_);
  panel_ = DockRect(dock_);
  const LONG pw = panel_.right - panel_.left;
  const LONG ph = panel_.bottom - panel_.top;
  const bool turned = turns != laid_turns_;
  const bool reshaped = (upright ? 1 : 0) != laid_upright_;
  laid_turns_ = turns;
  laid_upright_ = upright ? 1 : 0;

  if (reshaped) {
    tools_clip_->SetRight(static_cast<float>(pw - 1));
    tools_clip_->SetBottom(static_cast<float>(ph - 1));
    const config::ShadowSpec& cast = config::kToolbarShadow;
    const ShadowLayer layers[] = {{cast.sigma * scale_, cast.alpha, cast.offset * scale_}};
    const float shadow_pad = std::ceil(cast.sigma * 3.0f * scale_ + cast.offset * scale_);
    wide_shadow_->SetContent(
        compositor_.CreateSurface(RenderShadow(pw, ph, config::kToolbarRadius * scale_, shadow_pad, layers, 1), false).Get());
  }

  const LONG pad = Px(config::kToolbarPadding, scale_);
  const LONG cell = Px(config::kToolCell, scale_);
  const LONG swatch = Px(config::kSwatch, scale_);
  const LONG swatch_gap = Px(config::kSwatchGap, scale_);
  const LONG section = Px(config::kToolbarSection, scale_);
  const LONG fold = Px(config::kToolbarFold, scale_);
  const LONG art_w = Px(config::kToolArtWidth, scale_);
  const LONG art_h = Px(config::kToolArtHeight, scale_);
  const LONG rest = Px(config::kToolRest, scale_);
  // Local (along, across) to the panel's own pixels, and to the screen's.
  const auto local = [&](LONG along, LONG across) { return upright ? POINT{across, along} : POINT{along, across}; };
  const auto on_screen = [&](POINT at, LONG w, LONG h) { return RECT{panel_.left + at.x, panel_.top + at.y, panel_.left + at.x + w, panel_.top + at.y + h}; };
  const LONG depth = upright ? pw : ph;

  for (int i = 0; i < kToolCount; ++i) {
    Slot& slot = slots_[static_cast<size_t>(i)];
    const LONG along = pad + cell * i;
    const POINT corner = local(along, 0);
    slot.rect = on_screen(corner, upright ? pw : cell, upright ? cell : ph);
    const LONG centered = along + (cell - art_w) / 2;
    POINT at{};
    switch (turns) {
      case 2:
        at = POINT{centered, ph - rest - art_h};
        break;
      case 1:
        at = POINT{pw - rest - art_h, centered};
        break;
      case 3:
        at = POINT{rest, centered};
        break;
      default:
        at = POINT{centered, rest};
        break;
    }
    slot.holder->SetOffsetX(static_cast<float>(at.x));
    slot.holder->SetOffsetY(static_cast<float>(at.y));
    if (turned || !slot.art) RenderTool(slot);
  }

  const LONG colors = pad + cell * kToolCount + section;
  const LONG first_across = (depth - (swatch * 2 + swatch_gap)) / 2;
  for (int i = 0; i < 6; ++i) {
    Swatch& entry = swatches_[static_cast<size_t>(i)];
    const POINT at = local(colors + (i % 3) * (swatch + swatch_gap), first_across + (i / 3) * (swatch + swatch_gap));
    entry.rect = on_screen(at, swatch, swatch);
    entry.visual->SetOffsetX(static_cast<float>(at.x));
    entry.visual->SetOffsetY(static_cast<float>(at.y));
  }

  const POINT button = local((upright ? ph : pw) - pad - fold, (depth - fold) / 2);
  fold_ = on_screen(button, fold, fold);
  for (IDCompositionVisual* visual : {fold_face_.Get(), fold_veil_.Get()}) {
    visual->SetOffsetX(static_cast<float>(button.x));
    visual->SetOffsetY(static_cast<float>(button.y));
  }
  const LONG icon = Px(config::kEditorIcon, scale_);
  fold_icon_->SetOffsetX(static_cast<float>(button.x + (fold - icon) / 2));
  fold_icon_->SetOffsetY(static_cast<float>(button.y + (fold - icon) / 2));
  Lift(TimeBase{});
}

void MarkupToolbar::Land(ToolbarDock dock, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const int turns = laid_turns_;
  dock_ = dock;
  Tools().dock = static_cast<int>(dock);
  Layout();
  // Turned another way, the contents and the shadow are laid out again unseen and come back once the shape has moved.
  if (laid_turns_ != turns && dc != nullptr) {
    contents_alpha_.Snap(0.0f);
    wide_shadow_alpha_.Snap(0.0f);
  }
  Morph(false, now, false);
}

// The tool that lays down color, drawn at a slant with its point toward the lower left, and a short stroke of its
// color trailing from the point.
void MarkupToolbar::RenderBadge() {
  const ToolBox& tools = Tools();
  const Tool tool = InkTool();
  const ToolBox::Ink& ink = tools.inks[static_cast<size_t>(tool)];
  const uint32_t state = static_cast<uint32_t>(tool) | ink.color << 4 | static_cast<uint32_t>(std::lround(ink.opacity * 100.0f)) << 28;
  if (state == badge_state_) return;
  badge_state_ = state;
  const float size = static_cast<float>(button_rect_.right - button_rect_.left);
  const float unit = static_cast<float>(Px(config::kToolArtWidth, scale_)) / 26.0f;
  const float tip_x = 13.0f * unit;
  const float tip_y = 3.4f * unit;
  const float k = config::kMarkupButtonTool;
  const float cosine = -0.7071068f;
  const float sine = -0.7071068f;
  const float to_x = size * 0.36f;
  const float to_y = size * 0.66f;
  D2D_MATRIX_3X2_F place{k * cosine, k * sine, -k * sine, k * cosine, 0.0f, 0.0f};
  place._31 = to_x - (tip_x * place._11 + tip_y * place._21);
  place._32 = to_y - (tip_x * place._12 + tip_y * place._22);
  // Drawn without its opacity, which the button has no room to show.
  badge_art_->SetContent(compositor_.CreateSurface(ToolImage(tool, false, 0), false).Get());
  badge_art_->SetTransform(place);

  // The stroke, drawn by the tool itself, waving in from the left to the point.
  const int pixels = static_cast<int>(size);
  Stroke stroke;
  stroke.tool = tool;
  stroke.color = ink.color;
  stroke.opacity = std::max(ink.opacity, 0.6f);
  stroke.width = std::min(tools.WidthOf(tool, ink.width), tool == Tool::kHighlighter ? 9.0f : 3.2f) * scale_;
  stroke.scale = scale_;
  stroke.seed = 3u;
  for (int i = 0; i <= 40; ++i) {
    const float t = static_cast<float>(i) / 40.0f;
    InkPoint point{};
    point.x = size * 0.10f + (to_x - size * 0.10f) * t;
    point.y = to_y + size * 0.07f * std::sin(t * kTwoPi * 0.9f + 0.6f) * (1.0f - t * 0.6f);
    point.pressure = 0.45f + 0.4f * t;
    point.time = t * 0.25;
    stroke.points.push_back(point);
  }
  StampTrail trail;
  LayStamps(stroke, trail, true);
  std::vector<float> mask(static_cast<size_t>(pixels) * pixels, 0.0f);
  DrawStroke(stroke, RECT{0, 0, pixels, pixels}, mask.data(), pixels);
  Image line = Image::Allocate(pixels, pixels);
  if (!line) return;
  std::fill_n(reinterpret_cast<uint32_t*>(line.data()), static_cast<size_t>(pixels) * pixels, 0u);
  PaintStroke(stroke, mask.data(), pixels, reinterpret_cast<uint32_t*>(line.data()), pixels, pixels, pixels);
  badge_line_->SetContent(compositor_.CreateSurface(line, false).Get());
}

// Between the toolbar at its dock and the round button in a bottom corner, or between two docks: the shape slides and
// changes size, its contents fading out first, and grows back with them fading in after.
void MarkupToolbar::Morph(bool fold, const TimeBase& now, bool immediate) {
  IDCompositionDevice* dc = compositor_.device();
  const RECT& to = fold ? button_rect_ : panel_;
  const float button = static_cast<float>(button_rect_.right - button_rect_.left);
  const float panel_w = static_cast<float>(panel_.right - panel_.left);
  const float panel_h = static_cast<float>(panel_.bottom - panel_.top);
  const auto set = [&](AnimFloat& value, float target, const SpringSpec& spec, double delay) {
    if (immediate || dc == nullptr) {
      value.Snap(target);
    } else {
      value.To(dc, now, target, spec, delay);
    }
  };
  set(shape_x_, static_cast<float>(to.left), config::kEditorToolbar, 0.0);
  set(shape_y_, static_cast<float>(to.top), config::kEditorToolbar, 0.0);
  set(shape_w_, fold ? button : panel_w, config::kEditorToolbar, 0.0);
  set(shape_h_, fold ? button : panel_h, config::kEditorToolbar, 0.0);
  set(shape_r_, fold ? button * 0.5f : config::kToolbarRadius * scale_, config::kEditorToolbar, 0.0);
  if (fold) {
    set(contents_alpha_, 0.0f, config::kQuickFade, 0.0);
    set(wide_shadow_alpha_, 0.0f, config::kQuickFade, 0.0);
    set(badge_alpha_, 1.0f, config::kEditorFade, 0.16);
    set(round_shadow_alpha_, 1.0f, config::kEditorFade, 0.22);
  } else {
    set(badge_alpha_, 0.0f, config::kQuickFade, 0.0);
    set(round_shadow_alpha_, 0.0f, config::kQuickFade, 0.0);
    set(contents_alpha_, 1.0f, config::kEditorFade, 0.12);
    set(wide_shadow_alpha_, 1.0f, config::kEditorFade, 0.22);
  }
}

Tool MarkupToolbar::InkTool() const {
  const ToolBox& tools = Tools();
  return IsInk(tools.tool) ? tools.tool : tools.last_ink;
}

int MarkupToolbar::ChosenSwatch() const {
  const ToolBox& tools = Tools();
  const uint32_t color = tools.inks[static_cast<size_t>(InkTool())].color;
  for (int i = 0; i < 5; ++i) {
    if (config::kInkColors[i] == color) return i;
  }
  return tools.custom_used && tools.custom_color == color ? 5 : -1;
}

void MarkupToolbar::RenderSwatches() {
  const ToolBox& tools = Tools();
  const int chosen = ChosenSwatch();
  const int size = static_cast<int>(Px(config::kSwatch, scale_));
  for (size_t i = 0; i < swatches_.size(); ++i) {
    Swatch& swatch = swatches_[i];
    const bool on = static_cast<int>(i) == chosen;
    const int state = (on ? 1 : 0) | (swatch.hue && tools.custom_used ? 2 : 0) | (swatch.hue ? static_cast<int>(tools.custom_color << 2) : 0);
    if (state == swatch.shown_state) continue;
    swatch.shown_state = state;
    const Image image = swatch.hue ? RenderHueSwatch(size, scale_, tools.custom_color, tools.custom_used, on, palette_.panel)
                                   : RenderSwatch(size, scale_, swatch.color, on, palette_.panel);
    swatch.visual->SetContent(compositor_.CreateSurface(image, false).Get());
  }
}

// The chosen tool, and the one under the pointer, stand out from the edge the toolbar rests along.
void MarkupToolbar::Lift(const TimeBase& now) {
  const Tool current = Tools().tool;
  const int turns = Turns();
  const float inward = turns == 1 || turns == 2 ? 1.0f : -1.0f;
  const bool snap = now.seconds <= 0.0 || compositor_.device() == nullptr;
  for (size_t i = 0; i < slots_.size(); ++i) {
    Slot& slot = slots_[i];
    const bool chosen = slot.tool == Tool::kRuler ? ruler_ : slot.tool == current;
    const float raised = chosen ? config::kToolRaise * scale_ : (static_cast<int>(i) == hover_slot_ ? config::kToolHover * scale_ : 0.0f);
    if (raised == slot.raised && !snap) continue;
    slot.raised = raised;
    AnimFloat& moving = turns % 2 == 1 ? slot.lift_x : slot.lift_y;
    AnimFloat& still = turns % 2 == 1 ? slot.lift_y : slot.lift_x;
    if (snap) {
      moving.Snap(inward * raised);
      still.Snap(0.0f);
    } else {
      moving.To(compositor_.device(), now, inward * raised, config::kToolLift);
    }
  }
}

void MarkupToolbar::Show(const TimeBase& now, double delay) {
  if (shown_) return;
  shown_ = true;
  IDCompositionDevice* dc = compositor_.device();
  const float away = side() == ToolbarSide::kTop && !folded_ ? -1.0f : 1.0f;
  if (alpha_.ValueAt(now.seconds) < 0.02f) slide_.Snap(24.0f * scale_ * away);
  alpha_.To(dc, now, 1.0f, config::kEditorFade, delay);
  slide_.To(dc, now, 0.0f, config::kEditorToolbar, delay);
}

void MarkupToolbar::Hide(const TimeBase& now) {
  shown_ = false;
  IDCompositionDevice* dc = compositor_.device();
  ClosePopover(now);
  const float away = side() == ToolbarSide::kTop && !folded_ ? -1.0f : 1.0f;
  alpha_.To(dc, now, 0.0f, config::kEditorFade);
  slide_.To(dc, now, 16.0f * scale_ * away, config::kEditorFade);
  fold_hover_.To(dc, now, 0.0f, config::kQuickFade);
  badge_hover_.To(dc, now, 0.0f, config::kQuickFade);
  landing_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  hover_slot_ = -1;
  pressing_ = false;
  carrying_ = false;
}

void MarkupToolbar::Fold(bool fold, const TimeBase& now) {
  if (fold == folded_) return;
  folded_ = fold;
  IDCompositionDevice* dc = compositor_.device();
  ClosePopover(now);
  // Folded, it goes to whichever bottom corner is nearer.
  if (fold) {
    button_right_ = panel_.left + panel_.right > monitor_.cx;
    button_rect_ = ButtonRect(button_right_);
    RenderBadge();
  }
  Morph(fold, now, !shown_);
  fold_hover_.To(dc, now, 0.0f, config::kQuickFade);
  badge_hover_.To(dc, now, 0.0f, config::kQuickFade);
  hover_slot_ = -1;
  Lift(now);
}

void MarkupToolbar::SetRuler(bool on, const TimeBase& now) {
  ruler_ = on;
  Lift(now);
}

bool MarkupToolbar::Contains(POINT point) const {
  if (!shown_) return false;
  if (popover_ != Popover::kNone && PtInRect(&popover_rect_, point)) return true;
  return folded_ ? PtInRect(&button_rect_, point) != FALSE : PtInRect(&panel_, point) != FALSE;
}

RECT MarkupToolbar::Inner(float left, float top, float width, float height) const {
  const LONG x = popover_rect_.left + Px(left, scale_);
  const LONG y = popover_rect_.top + Px(top, scale_);
  return RECT{x, y, x + Px(width, scale_), y + Px(height, scale_)};
}

// A popover opens on the side of the toolbar facing into the screen, beside what opened it.
void MarkupToolbar::OpenPopover(Popover kind, POINT anchor, const TimeBase& now) {
  if (popover_ == kind) {
    ClosePopover(now);
    return;
  }
  popover_ = kind;
  const float pad = config::kPopoverPadding;
  float width = pad * 2.0f + config::kWidthCell * 5.0f;
  float height = pad * 2.0f + config::kWidthCell + config::kPopoverGap + (kind == Popover::kEraser ? config::kModeHeight : config::kOpacityTrack);
  if (kind == Popover::kColors) {
    width = pad * 2.0f + config::kGridCell * config::kGridColumns;
    height = pad * 2.0f + config::kGridCell * config::kGridRows;
  }
  const LONG w = Px(width, scale_);
  const LONG h = Px(height, scale_);
  const LONG margin = Px(config::kEditorMargin, scale_);
  const LONG gap = Px(config::kPopoverGap, scale_);
  const LONG across_x = std::clamp(anchor.x - w / 2, margin, std::max(monitor_.cx - margin - w, margin));
  const LONG across_y = std::clamp(anchor.y - h / 2, margin, std::max(monitor_.cy - margin - h, margin));
  LONG x = across_x;
  LONG y = panel_.top - gap - h;
  float center_x = static_cast<float>(std::clamp(anchor.x - x, 0L, w));
  float center_y = static_cast<float>(h);
  switch (side()) {
    case ToolbarSide::kTop:
      y = panel_.bottom + gap;
      center_y = 0.0f;
      break;
    case ToolbarSide::kLeft:
      x = panel_.right + gap;
      y = across_y;
      center_x = 0.0f;
      center_y = static_cast<float>(std::clamp(anchor.y - y, 0L, h));
      break;
    case ToolbarSide::kRight:
      x = panel_.left - gap - w;
      y = across_y;
      center_x = static_cast<float>(w);
      center_y = static_cast<float>(std::clamp(anchor.y - y, 0L, h));
      break;
    default:
      break;
  }
  popover_rect_ = RECT{x, y, x + w, y + h};
  for (int i = 0; i < 5; ++i) width_cells_[static_cast<size_t>(i)] = Inner(pad + config::kWidthCell * i, pad, config::kWidthCell, config::kWidthCell);
  const float row = pad + config::kWidthCell + config::kPopoverGap;
  track_ = Inner(pad, row, config::kWidthCell * 5.0f, config::kOpacityTrack);
  modes_[0] = Inner(pad, row, config::kWidthCell * 2.5f, config::kModeHeight);
  modes_[1] = Inner(pad + config::kWidthCell * 2.5f, row, config::kWidthCell * 2.5f, config::kModeHeight);
  grid_ = Inner(pad, pad, config::kGridCell * config::kGridColumns, config::kGridCell * config::kGridRows);

  popover_root_->SetOffsetX(static_cast<float>(x));
  popover_root_->SetOffsetY(static_cast<float>(y));
  popover_scale_->SetCenterX(center_x);
  popover_scale_->SetCenterY(center_y);
  RenderPopover();
  IDCompositionDevice* dc = compositor_.device();
  popover_alpha_.Snap(0.0f);
  popover_pop_.Snap(config::kPopoverFrom);
  popover_alpha_.To(dc, now, 1.0f, config::kEditorFade);
  popover_pop_.To(dc, now, 1.0f, config::kPopover);
}

void MarkupToolbar::ClosePopover(const TimeBase& now) {
  if (popover_ == Popover::kNone) return;
  popover_ = Popover::kNone;
  dragging_opacity_ = false;
  IDCompositionDevice* dc = compositor_.device();
  popover_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  popover_pop_.To(dc, now, config::kPopoverFrom, config::kEditorFade);
}

void MarkupToolbar::RenderPopover() {
  if (popover_ == Popover::kNone) return;
  const ToolBox& tools = Tools();
  const int w = popover_rect_.right - popover_rect_.left;
  const int h = popover_rect_.bottom - popover_rect_.top;
  Image body = RenderPanel(w, h, config::kPopoverRadius * scale_, palette_.panel, palette_.hairline, palette_.hairline_alpha);
  if (!body) return;
  const auto local = [&](const RECT& rect) { return POINT{rect.left - popover_rect_.left, rect.top - popover_rect_.top}; };

  if (popover_ == Popover::kColors) {
    int chosen_column = -1;
    int chosen_row = -1;
    const uint32_t color = tools.inks[static_cast<size_t>(InkTool())].color;
    for (int row = 0; row < config::kGridRows && chosen_column < 0; ++row) {
      for (int column = 0; column < config::kGridColumns; ++column) {
        if (GridColor(column, row) == color) {
          chosen_column = column;
          chosen_row = row;
          break;
        }
      }
    }
    const POINT at = local(grid_);
    Over(body, RenderColorGrid(config::kGridColumns, config::kGridRows, static_cast<int>(Px(config::kGridCell, scale_)), scale_, chosen_column, chosen_row), at.x, at.y);
    knob_->SetContent(nullptr);
  } else {
    const bool eraser = popover_ == Popover::kEraser;
    const Tool tool = eraser ? Tool::kEraser : InkTool();
    const int chosen = eraser ? tools.eraser_width : tools.inks[static_cast<size_t>(tool)].width;
    const int cell = width_cells_[0].right - width_cells_[0].left;
    for (int i = 0; i < 5; ++i) {
      const POINT at = local(width_cells_[static_cast<size_t>(i)]);
      const bool on = i == chosen;
      if (on) Over(body, RenderBox(cell, cell, 11.0f * scale_, palette_.chosen, 1.0f), at.x, at.y);
      const uint32_t ink = on ? palette_.chosen_ink : palette_.control_ink;
      if (eraser) {
        const int diameter = static_cast<int>(std::lround(std::min(tools.WidthOf(Tool::kEraser, i) * 0.55f + 5.0f, 34.0f) * scale_));
        const Image ring = RenderRoundedOutline(diameter, diameter, static_cast<float>(diameter) * 0.5f, std::max(1.4f * scale_, 1.0f), ink);
        Over(body, ring, at.x + (cell - diameter) / 2, at.y + (cell - diameter) / 2);
      } else {
        const float shown = std::min(tools.WidthOf(tool, i), 16.0f) * scale_;
        Over(body, RenderSample(tool, shown, scale_, ink, cell), at.x, at.y);
      }
    }
    if (eraser) {
      const int mode_w = modes_[1].right - modes_[0].left;
      const int mode_h = modes_[0].bottom - modes_[0].top;
      const POINT at = local(modes_[0]);
      Over(body, RenderBox(mode_w, mode_h, static_cast<float>(mode_h) * 0.5f, palette_.control, 1.0f), at.x, at.y);
      const int half = mode_w / 2;
      const int inset = static_cast<int>(Px(3.0f, scale_));
      const int pick = tools.object_eraser ? 1 : 0;
      Over(body, RenderBox(half - inset * 2, mode_h - inset * 2, static_cast<float>(mode_h - inset * 2) * 0.5f, palette_.chosen, 1.0f), at.x + half * pick + inset,
           at.y + inset);
      const int font = static_cast<int>(Px(config::kEditorFontSize, scale_));
      for (int i = 0; i < 2; ++i) {
        const TextLine text = RenderText(i == 0 ? config::kLabelPixelEraser : config::kLabelObjectEraser, font, i == pick ? palette_.chosen_ink : palette_.control_ink);
        const RECT ink = InkBounds(text.image);
        Over(body, text.image, at.x + half * i + (half - (ink.right + ink.left)) / 2,
             at.y + static_cast<int>(std::lround(static_cast<float>(mode_h) * 0.5f - (text.baseline - text.cap_height * 0.5f))));
      }
      knob_->SetContent(nullptr);
    } else {
      const POINT at = local(track_);
      Over(body, RenderOpacityTrack(track_.right - track_.left, track_.bottom - track_.top, scale_, tools.inks[static_cast<size_t>(tool)].color), at.x, at.y);
      PlaceKnob();
    }
  }
  popover_body_->SetContent(compositor_.CreateSurface(body, false).Get());
}

void MarkupToolbar::PlaceKnob() {
  const ToolBox::Ink& ink = Tools().inks[static_cast<size_t>(InkTool())];
  const int size = static_cast<int>(Px(config::kOpacityThumb, scale_));
  const int track_w = track_.right - track_.left;
  const int track_h = track_.bottom - track_.top;
  const float travel = static_cast<float>(track_w - track_h);
  const float x = static_cast<float>(track_.left - popover_rect_.left) + static_cast<float>(track_h) * 0.5f + travel * ink.opacity - static_cast<float>(size) * 0.5f;
  const float y = static_cast<float>(track_.top - popover_rect_.top) + static_cast<float>(track_h - size) * 0.5f;
  knob_->SetContent(compositor_.CreateSurface(RenderOpacityKnob(size, scale_, ink.color, ink.opacity), false).Get());
  knob_->SetOffsetX(std::round(x));
  knob_->SetOffsetY(std::round(y));
}

void MarkupToolbar::SetColor(uint32_t color, bool custom, const TimeBase& now) {
  ToolBox& tools = Tools();
  if (!IsInk(tools.tool)) tools.tool = tools.last_ink;
  tools.inks[static_cast<size_t>(tools.tool)].color = color;
  if (custom) {
    tools.custom_color = color;
    tools.custom_used = true;
  }
  Refresh(now);
}

ToolbarEvent MarkupToolbar::Press(POINT point, const TimeBase& now) {
  if (!shown_) return ToolbarEvent::kNone;
  ToolBox& tools = Tools();
  if (popover_ != Popover::kNone && PtInRect(&popover_rect_, point)) {
    if (popover_ == Popover::kColors) {
      if (!PtInRect(&grid_, point)) return ToolbarEvent::kNone;
      const int cell = static_cast<int>(Px(config::kGridCell, scale_));
      const int column = std::clamp((point.x - grid_.left) / cell, 0L, static_cast<LONG>(config::kGridColumns - 1));
      const int row = std::clamp((point.y - grid_.top) / cell, 0L, static_cast<LONG>(config::kGridRows - 1));
      SetColor(GridColor(column, row), true, now);
      return ToolbarEvent::kColor;
    }
    for (int i = 0; i < 5; ++i) {
      if (!PtInRect(&width_cells_[static_cast<size_t>(i)], point)) continue;
      if (popover_ == Popover::kEraser) {
        tools.eraser_width = i;
      } else {
        tools.inks[static_cast<size_t>(InkTool())].width = i;
      }
      RenderPopover();
      return ToolbarEvent::kInk;
    }
    if (popover_ == Popover::kEraser) {
      for (int i = 0; i < 2; ++i) {
        if (!PtInRect(&modes_[static_cast<size_t>(i)], point)) continue;
        tools.object_eraser = i == 1;
        RenderPopover();
        return ToolbarEvent::kInk;
      }
      return ToolbarEvent::kNone;
    }
    if (PtInRect(&track_, point)) {
      dragging_opacity_ = true;
      return Drag(point, now);
    }
    return ToolbarEvent::kNone;
  }
  // A press may go on to carry the toolbar, or the button, somewhere else. The button unfolds when let go unmoved.
  const RECT& under = folded_ ? button_rect_ : panel_;
  if (!PtInRect(&under, point)) return ToolbarEvent::kNone;
  pressing_ = true;
  carrying_ = false;
  pressed_ = point;
  last_point_ = point;
  grab_ = POINT{point.x - under.left, point.y - under.top};
  if (folded_) return ToolbarEvent::kNone;
  if (PtInRect(&fold_, point)) {
    pressing_ = false;
    Fold(true, now);
    return ToolbarEvent::kFolded;
  }
  for (Slot& slot : slots_) {
    if (!PtInRect(&slot.rect, point)) continue;
    const POINT center{(slot.rect.left + slot.rect.right) / 2, (slot.rect.top + slot.rect.bottom) / 2};
    if (slot.tool == Tool::kRuler) {
      ClosePopover(now);
      SetRuler(!ruler_, now);
      return ToolbarEvent::kRuler;
    }
    if (slot.tool == tools.tool) {
      if (IsInk(slot.tool)) OpenPopover(Popover::kInk, center, now);
      if (slot.tool == Tool::kEraser) OpenPopover(Popover::kEraser, center, now);
      return ToolbarEvent::kNone;
    }
    ClosePopover(now);
    tools.tool = slot.tool;
    if (IsInk(slot.tool)) tools.last_ink = slot.tool;
    Lift(now);
    RenderSwatches();
    return ToolbarEvent::kTool;
  }
  for (const Swatch& swatch : swatches_) {
    if (!PtInRect(&swatch.rect, point)) continue;
    if (swatch.hue) {
      OpenPopover(Popover::kColors, POINT{(swatch.rect.left + swatch.rect.right) / 2, (swatch.rect.top + swatch.rect.bottom) / 2}, now);
      return ToolbarEvent::kNone;
    }
    ClosePopover(now);
    SetColor(swatch.color, false, now);
    return ToolbarEvent::kColor;
  }
  return ToolbarEvent::kNone;
}

ToolbarEvent MarkupToolbar::Drag(POINT point, const TimeBase& now) {
  if (pressing_ && !dragging_opacity_) {
    last_point_ = point;
    IDCompositionDevice* dc = compositor_.device();
    const bool starting = !carrying_;
    if (starting) {
      const LONG slop = Px(config::kToolbarDragSlop, scale_);
      if (std::abs(point.x - pressed_.x) <= slop && std::abs(point.y - pressed_.y) <= slop) return ToolbarEvent::kNone;
      carrying_ = true;
      ClosePopover(now);
      hover_slot_ = -1;
      Lift(now);
      fold_hover_.To(dc, now, 0.0f, config::kQuickFade);
      badge_hover_.To(dc, now, 0.0f, config::kQuickFade);
      const float button = static_cast<float>(button_rect_.right - button_rect_.left);
      SetClipRadii(landing_clip_.Get(), folded_ ? button * 0.5f : config::kToolbarRadius * scale_, nullptr);
      landing_alpha_.To(dc, now, 1.0f, config::kQuickFade);
    }
    // It follows the pointer, and where it would land shows under it.
    shape_x_.Snap(static_cast<float>(point.x - grab_.x));
    shape_y_.Snap(static_cast<float>(point.y - grab_.y));
    RECT target{};
    if (folded_) {
      target = ButtonRect(point.x * 2 > monitor_.cx);
    } else {
      const POINT center{point.x - grab_.x + (panel_.right - panel_.left) / 2, point.y - grab_.y + (panel_.bottom - panel_.top) / 2};
      landing_dock_ = NearestDock(center);
      target = DockRect(landing_dock_);
    }
    const auto place = [&](AnimFloat& value, LONG to) {
      if (starting || dc == nullptr) {
        value.Snap(static_cast<float>(to));
      } else {
        value.To(dc, now, static_cast<float>(to), config::kEditorToolbar);
      }
    };
    place(landing_x_, target.left);
    place(landing_y_, target.top);
    place(landing_w_, target.right - target.left);
    place(landing_h_, target.bottom - target.top);
    return ToolbarEvent::kNone;
  }
  if (!dragging_opacity_ || popover_ != Popover::kInk) return ToolbarEvent::kNone;
  const float track_w = static_cast<float>(track_.right - track_.left);
  const float track_h = static_cast<float>(track_.bottom - track_.top);
  const float share = std::clamp((static_cast<float>(point.x - track_.left) - track_h * 0.5f) / std::max(track_w - track_h, 1.0f), 0.0f, 1.0f);
  ToolBox::Ink& ink = Tools().inks[static_cast<size_t>(InkTool())];
  const float opacity = std::max(std::round(share * 100.0f) / 100.0f, 0.05f);
  if (opacity == ink.opacity) return ToolbarEvent::kNone;
  ink.opacity = opacity;
  PlaceKnob();
  RenderTool(slots_[static_cast<size_t>(InkTool())]);
  (void)now;
  return ToolbarEvent::kInk;
}

ToolbarEvent MarkupToolbar::Release(const TimeBase& now) {
  dragging_opacity_ = false;
  const bool pressing = pressing_;
  const bool carrying = carrying_;
  pressing_ = false;
  carrying_ = false;
  if (!pressing) return ToolbarEvent::kNone;
  if (carrying) landing_alpha_.To(compositor_.device(), now, 0.0f, config::kQuickFade);
  if (folded_) {
    if (!carrying) {
      Fold(false, now);
      return ToolbarEvent::kUnfolded;
    }
    button_right_ = last_point_.x * 2 > monitor_.cx;
    button_rect_ = ButtonRect(button_right_);
    Morph(true, now, false);
    return ToolbarEvent::kNone;
  }
  if (!carrying) return ToolbarEvent::kNone;
  const bool moved = landing_dock_ != dock_;
  Land(landing_dock_, now);
  return moved ? ToolbarEvent::kDocked : ToolbarEvent::kNone;
}

void MarkupToolbar::Hover(POINT point, const TimeBase& now) {
  if (carrying_) return;
  int over = -1;
  if (shown_ && !folded_ && PtInRect(&panel_, point)) {
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (PtInRect(&slots_[i].rect, point)) over = static_cast<int>(i);
    }
  }
  if (over != hover_slot_) {
    hover_slot_ = over;
    Lift(now);
  }
  const bool on_fold = shown_ && !folded_ && PtInRect(&fold_, point);
  fold_hover_.To(compositor_.device(), now, on_fold ? palette_.hover_alpha : 0.0f, config::kQuickFade);
  const bool on_button = shown_ && folded_ && PtInRect(&button_rect_, point);
  badge_hover_.To(compositor_.device(), now, on_button ? palette_.hover_alpha : 0.0f, config::kQuickFade);
}

void MarkupToolbar::Refresh(const TimeBase& now) {
  for (Slot& slot : slots_) {
    if (IsInk(slot.tool)) RenderTool(slot);
  }
  badge_state_ = 0xFFFFFFFFu;
  RenderBadge();
  RenderSwatches();
  RenderPopover();
  Lift(now);
}

}  // namespace sstk
