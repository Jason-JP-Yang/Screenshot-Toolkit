// The markup toolbar along the bottom of the editing panel: the tools standing in a row with the chosen one raised, the
// colors, and a button that folds it away into a round one at the bottom left. Pressing the chosen tool again opens
// its popover: widths and opacity for one that lays down color, widths and kind for the eraser. The last swatch opens
// a grid of colors.
#pragma once

#include "sstk/config/colors.h"
#include "sstk/editor/document.h"
#include "sstk/graphics/compositor.h"
#include "sstk/graphics/motion.h"

#include <array>
#include <vector>

namespace sstk {

enum class ToolbarEvent { kNone, kTool, kInk, kColor, kRuler, kFolded, kUnfolded };

class MarkupToolbar {
 public:
  MarkupToolbar(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor, const config::EditorPalette& palette);
  MarkupToolbar(const MarkupToolbar&) = delete;
  MarkupToolbar& operator=(const MarkupToolbar&) = delete;

  void Show(const TimeBase& now, double delay);
  void Hide(const TimeBase& now);
  void Fold(bool fold, const TimeBase& now);
  bool folded() const { return folded_; }
  bool shown() const { return shown_; }

  // The ruler is a tool that stays on while another draws.
  void SetRuler(bool on, const TimeBase& now);
  bool ruler() const { return ruler_; }

  // The top edge of the unfolded toolbar.
  float top() const { return top_; }

  bool Contains(POINT point) const;
  bool popover_open() const { return popover_ != Popover::kNone; }
  ToolbarEvent Press(POINT point, const TimeBase& now);
  ToolbarEvent Drag(POINT point, const TimeBase& now);
  void Release();
  void Hover(POINT point, const TimeBase& now);
  void ClosePopover(const TimeBase& now);

  // Brings the tools and colors in line with Tools(), after a change made elsewhere.
  void Refresh(const TimeBase& now);

 private:
  enum class Popover { kNone, kInk, kEraser, kColors };

  struct Slot {
    Tool tool;
    RECT rect{};
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionSurface> art;
    AnimFloat lift;
    float raised = 0.0f;
  };

  struct Swatch {
    RECT rect{};
    ComPtr<IDCompositionVisual> visual;
    bool hue = false;
    uint32_t color = 0;
    int shown_state = -1;
  };

  void RenderTool(Slot& slot);
  void RenderBadge();
  void Morph(bool fold, const TimeBase& now, bool immediate);
  void RenderSwatches();
  void Lift(const TimeBase& now);
  int ChosenSwatch() const;  // -1 for none, the swatch count for the grid's color.
  Tool InkTool() const;      // The tool a color applies to.
  void SetColor(uint32_t color, bool custom, const TimeBase& now);
  void OpenPopover(Popover kind, int anchor_x, const TimeBase& now);
  void RenderPopover();
  void PlaceKnob();
  RECT Inner(float left, float top, float width, float height) const;

  Compositor& compositor_;
  float scale_;
  SIZE monitor_;
  const config::EditorPalette& palette_;

  // One shape that is the toolbar unfolded and the round button folded, and slides and narrows from one to the other.
  ComPtr<IDCompositionVisual> layer_;
  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> shape_;
  ComPtr<IDCompositionVisual> wide_shadow_;
  ComPtr<IDCompositionEffectGroup> wide_shadow_effect_;
  ComPtr<IDCompositionVisual> round_shadow_;
  ComPtr<IDCompositionEffectGroup> round_shadow_effect_;
  ComPtr<IDCompositionVisual> outer_;
  ComPtr<IDCompositionRectangleClip> outer_clip_;
  ComPtr<IDCompositionVisual> inner_;
  ComPtr<IDCompositionRectangleClip> inner_clip_;
  ComPtr<IDCompositionVisual> contents_;
  ComPtr<IDCompositionEffectGroup> contents_effect_;
  ComPtr<IDCompositionVisual> tools_;
  ComPtr<IDCompositionRectangleClip> tools_clip_;
  ComPtr<IDCompositionVisual> fold_veil_;
  ComPtr<IDCompositionEffectGroup> fold_veil_effect_;
  ComPtr<IDCompositionVisual> badge_;
  ComPtr<IDCompositionEffectGroup> badge_effect_;
  ComPtr<IDCompositionVisual> badge_art_;
  ComPtr<IDCompositionVisual> badge_line_;
  ComPtr<IDCompositionVisual> badge_veil_;
  ComPtr<IDCompositionEffectGroup> badge_veil_effect_;
  AnimFloat alpha_, slide_, fold_hover_, badge_hover_;
  AnimFloat shape_x_, shape_y_, shape_w_, shape_h_, shape_r_;
  AnimFloat contents_alpha_, badge_alpha_, wide_shadow_alpha_, round_shadow_alpha_;
  uint32_t badge_state_ = 0xFFFFFFFFu;

  ComPtr<IDCompositionVisual> popover_root_;
  ComPtr<IDCompositionEffectGroup> popover_effect_;
  ComPtr<IDCompositionScaleTransform> popover_scale_;
  ComPtr<IDCompositionVisual> popover_body_;
  ComPtr<IDCompositionVisual> knob_;
  AnimFloat popover_alpha_, popover_pop_;

  std::array<Slot, kToolCount> slots_;
  std::vector<Swatch> swatches_;
  RECT panel_{};
  RECT fold_{};
  RECT button_rect_{};
  float top_ = 0.0f;
  float width_ = 0.0f;
  float height_ = 0.0f;

  Popover popover_ = Popover::kNone;
  RECT popover_rect_{};
  std::array<RECT, 5> width_cells_{};
  RECT track_{};
  std::array<RECT, 2> modes_{};
  RECT grid_{};
  bool dragging_opacity_ = false;
  int hover_slot_ = -1;
  bool shown_ = false;
  bool folded_ = false;
  bool ruler_ = false;
};

}  // namespace sstk
