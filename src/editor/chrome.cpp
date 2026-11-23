#include "sstk/editor/chrome.h"

#include "sstk/config/animation.h"
#include "sstk/config/layout.h"
#include "sstk/editor/art.h"

#include <algorithm>
#include <cmath>

namespace sstk {

EditorChrome::EditorChrome(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor, const config::EditorPalette& palette)
    : compositor_(compositor), scale_(scale), palette_(palette) {
  layer_ = compositor_.CreateVisual();
  layer_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  parent->AddVisual(layer_.Get(), FALSE, nullptr);

  const LONG margin = std::lround(config::kEditorMargin * scale);
  const LONG button = std::lround(config::kEditorButton * scale);
  const LONG gap = std::lround(config::kEditorControlGap * scale);
  bottom_ = static_cast<float>(margin + button);
  middle_ = static_cast<float>(margin) + static_cast<float>(button) * 0.5f;

  const auto cell = [&](LONG left) { return RECT{left, margin, left + button, margin + button}; };
  const uint32_t ink = palette.control_ink;
  const uint32_t veil = palette.hover;

  AddGroup(cell(margin), palette.control);
  AddCell(0, ChromeAction::kClose, cell(margin), static_cast<int>(EditorIcon::kClose), ink, veil);

  const LONG history = margin + button + gap;
  AddGroup(RECT{history, margin, history + button * 3, margin + button}, palette.control);
  AddCell(1, ChromeAction::kReset, cell(history), static_cast<int>(EditorIcon::kReset), ink, veil);
  AddCell(1, ChromeAction::kUndo, cell(history + button), static_cast<int>(EditorIcon::kUndo), ink, veil);
  AddCell(1, ChromeAction::kRedo, cell(history + button * 2), static_cast<int>(EditorIcon::kRedo), ink, veil);

  const LONG done = monitor.cx - margin - button;
  const LONG turns = done - gap - button * 2;
  AddGroup(RECT{turns, margin, turns + button * 2, margin + button}, palette.control);
  AddCell(2, ChromeAction::kRotateRight, cell(turns), static_cast<int>(EditorIcon::kRotateRight), ink, veil);
  AddCell(2, ChromeAction::kRotateLeft, cell(turns + button), static_cast<int>(EditorIcon::kRotateLeft), ink, veil);

  AddGroup(cell(done), palette.done);
  AddCell(3, ChromeAction::kDone, cell(done), static_cast<int>(EditorIcon::kDone), palette.done_ink, palette.done_ink);
}

void EditorChrome::AddGroup(const RECT& rect, uint32_t face) {
  Group group;
  group.rect = rect;
  group.root = compositor_.CreateVisual();
  group.effect = compositor_.CreateEffectGroup();
  group.root->SetEffect(group.effect.Get());
  group.root->SetOffsetX(static_cast<float>(rect.left));
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  ComPtr<IDCompositionVisual> face_visual = compositor_.CreateVisual();
  face_visual->SetContent(compositor_.CreateSurface(RenderBox(width, height, static_cast<float>(height) * 0.5f, face, 1.0f), false).Get());
  group.root->AddVisual(face_visual.Get(), FALSE, nullptr);
  layer_->AddVisual(group.root.Get(), FALSE, nullptr);
  groups_.push_back(std::move(group));
  Group& added = groups_.back();
  added.alpha.Bind(added.effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  added.rise.Bind(added.root.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY), static_cast<float>(rect.top));
  added.alpha.Snap(0.0f);
  added.rise.Snap(0.0f);
}

void EditorChrome::AddCell(int group, ChromeAction action, const RECT& rect, int icon, uint32_t ink, uint32_t veil) {
  Group& owner = groups_[static_cast<size_t>(group)];
  Cell cell;
  cell.action = action;
  cell.group = group;
  cell.rect = rect;
  const float size = static_cast<float>(rect.right - rect.left);
  cell.holder = compositor_.CreateVisual();
  cell.holder->SetOffsetX(static_cast<float>(rect.left - owner.rect.left));
  cell.press = compositor_.CreateScale();
  cell.press->SetCenterX(size * 0.5f);
  cell.press->SetCenterY(size * 0.5f);
  cell.holder->SetTransform(cell.press.Get());

  const int veil_size = static_cast<int>(std::lround(size - 6.0f * scale_));
  ComPtr<IDCompositionVisual> veil_visual = compositor_.CreateVisual();
  veil_visual->SetContent(compositor_.CreateSurface(RenderBox(veil_size, veil_size, veil_size * 0.5f, veil, 1.0f), false).Get());
  const float veil_at = std::round((size - static_cast<float>(veil_size)) * 0.5f);
  veil_visual->SetOffsetX(veil_at);
  veil_visual->SetOffsetY(veil_at);
  cell.veil_effect = compositor_.CreateEffectGroup();
  veil_visual->SetEffect(cell.veil_effect.Get());
  cell.holder->AddVisual(veil_visual.Get(), FALSE, nullptr);

  const int icon_size = static_cast<int>(std::lround(config::kEditorIcon * scale_));
  ComPtr<IDCompositionVisual> icon_visual = compositor_.CreateVisual();
  icon_visual->SetContent(compositor_.CreateSurface(RenderEditorIcon(static_cast<EditorIcon>(icon), icon_size, scale_, ink), false).Get());
  const float icon_at = std::round((size - static_cast<float>(icon_size)) * 0.5f);
  icon_visual->SetOffsetX(icon_at);
  icon_visual->SetOffsetY(icon_at);
  cell.icon_effect = compositor_.CreateEffectGroup();
  icon_visual->SetEffect(cell.icon_effect.Get());
  cell.holder->AddVisual(icon_visual.Get(), FALSE, nullptr);
  owner.root->AddVisual(cell.holder.Get(), FALSE, nullptr);

  cells_.push_back(std::move(cell));
  Cell& added = cells_.back();
  added.veil.Bind(added.veil_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  added.icon.Bind(added.icon_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  added.scale.Bind(added.press.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  added.scale.Bind(added.press.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));
  added.veil.Snap(0.0f);
  added.icon.Snap(1.0f);
  added.scale.Snap(1.0f);
}

void EditorChrome::ShowGroup(Group& group, const TimeBase& now, double delay) {
  if (group.shown) return;
  group.shown = true;
  IDCompositionDevice* dc = compositor_.device();
  if (group.alpha.ValueAt(now.seconds) < 0.02f) group.rise.Snap(-config::kEditorChromeRise * scale_);
  group.alpha.To(dc, now, 1.0f, config::kEditorFade, delay);
  group.rise.To(dc, now, 0.0f, config::kEditorToolbar, delay);
}

void EditorChrome::Show(bool everything, const TimeBase& now, double delay) {
  ShowGroup(groups_[0], now, delay);
  if (!everything) return;
  for (size_t i = 1; i < groups_.size(); ++i) ShowGroup(groups_[i], now, delay + 0.03 * static_cast<double>(i));
}

void EditorChrome::Hide(const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  for (Group& group : groups_) {
    group.shown = false;
    group.alpha.To(dc, now, 0.0f, config::kEditorFade);
  }
  Hover(ChromeAction::kNone, now);
  Press(ChromeAction::kNone, now);
}

void EditorChrome::Enable(ChromeAction action, bool enabled, const TimeBase& now) {
  for (Cell& cell : cells_) {
    if (cell.action != action || cell.enabled == enabled) continue;
    cell.enabled = enabled;
    cell.icon.To(compositor_.device(), now, enabled ? 1.0f : palette_.disabled_alpha, config::kEditorFade);
    if (!enabled && hover_ == action) Hover(ChromeAction::kNone, now);
  }
}

ChromeAction EditorChrome::HitTest(POINT point) const {
  for (const Cell& cell : cells_) {
    if (cell.enabled && groups_[static_cast<size_t>(cell.group)].shown && PtInRect(&cell.rect, point)) return cell.action;
  }
  return ChromeAction::kNone;
}

bool EditorChrome::Contains(POINT point) const {
  return std::any_of(groups_.begin(), groups_.end(), [&](const Group& group) { return group.shown && PtInRect(&group.rect, point); });
}

void EditorChrome::Hover(ChromeAction action, const TimeBase& now) {
  if (action == hover_) return;
  hover_ = action;
  for (Cell& cell : cells_) {
    const float strength = cell.action == ChromeAction::kDone ? 0.10f : palette_.hover_alpha;
    cell.veil.To(compositor_.device(), now, cell.action == action && cell.enabled ? strength : 0.0f, config::kQuickFade);
  }
}

void EditorChrome::Press(ChromeAction action, const TimeBase& now) {
  for (Cell& cell : cells_) cell.scale.To(compositor_.device(), now, cell.action == action ? config::kEditorPressScale : 1.0f, config::kEditorPress);
}

}  // namespace sstk
