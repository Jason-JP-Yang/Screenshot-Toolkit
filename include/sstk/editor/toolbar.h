// The markup toolbar: the tools standing in a row with the chosen one raised, the colors, and a button that folds it
// away into a round one in the bottom corner nearer to it. Pressing the chosen tool again opens its popover: widths and
// opacity for one that lays down color, widths and kind for the eraser. The last swatch opens a grid of colors.
//
// Dragged, it lands at the nearest of a few places: along the bottom or the top, at the left, middle, or right, or
// standing up along either side. Its tools always point into the screen, and their labels stay upright.
#pragma once

#include "sstk/config/colors.h"
#include "sstk/editor/document.h"
#include "sstk/graphics/compositor.h"
#include "sstk/graphics/motion.h"

#include <array>
#include <vector>

namespace sstk {

enum class ToolbarEvent { kNone, kTool, kInk, kColor, kRuler, kFolded, kUnfolded, kDocked };

enum class ToolbarDock { kBottomLeft, kBottom, kBottomRight, kTopLeft, kTop, kTopRight, kLeft, kRight };
inline constexpr int kToolbarDocks = 8;

// The edge of the screen a dock keeps the toolbar along.
enum class ToolbarSide { kBottom, kTop, kLeft, kRight };

class MarkupToolbar {
 public:
  // `ceiling` is the top of the space below the controls along the top, where the toolbar may dock.
  MarkupToolbar(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor, float ceiling, const config::EditorPalette& palette);
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

  // Where the unfolded toolbar rests, and the edge it rests along.
  RECT panel() const { return panel_; }
  ToolbarSide side() const;

  bool Contains(POINT point) const;
  bool popover_open() const { return popover_ != Popover::kNone; }
  ToolbarEvent Press(POINT point, const TimeBase& now);
  ToolbarEvent Drag(POINT point, const TimeBase& now);
  ToolbarEvent Release(const TimeBase& now);
  void Hover(POINT point, const TimeBase& now);
  void ClosePopover(const TimeBase& now);

  // Brings the tools and colors in line with Tools(), after a change made elsewhere.
  void Refresh(const TimeBase& now);

 private:
  enum class Popover { kNone, kInk, kEraser, kColors };

  struct Slot {
    Tool tool;
    RECT rect{};
    ComPtr<IDCompositionVisual> holder;  // At the tool's resting place,
    ComPtr<IDCompositionVisual> visual;  // and the tool, raised from it along the toolbar's depth.
    ComPtr<IDCompositionSurface> art;
    AnimFloat lift_x, lift_y;
    float raised = 0.0f;
  };

  struct Swatch {
    RECT rect{};
    ComPtr<IDCompositionVisual> visual;
    bool hue = false;
    uint32_t color = 0;
    int shown_state = -1;
  };

  Image ToolImage(Tool tool, bool percent, int turns) const;  // With its opacity printed on it below 100, when `percent`.
  void RenderTool(Slot& slot);
  void RenderBadge();
  void Morph(bool fold, const TimeBase& now, bool immediate);
  void RenderSwatches();
  void Lift(const TimeBase& now);
  int ChosenSwatch() const;  // -1 for none, the swatch count for the grid's color.
  Tool InkTool() const;      // The tool a color applies to.
  void SetColor(uint32_t color, bool custom, const TimeBase& now);
  void OpenPopover(Popover kind, POINT anchor, const TimeBase& now);
  void RenderPopover();
  void PlaceKnob();
  RECT Inner(float left, float top, float width, float height) const;

  // Docking.
  bool Upright(ToolbarDock dock) const { return dock == ToolbarDock::kLeft || dock == ToolbarDock::kRight; }
  bool Fits(ToolbarDock dock) const;
  RECT DockRect(ToolbarDock dock) const;
  ToolbarDock NearestDock(POINT center) const;
  RECT ButtonRect(bool right) const;
  int Turns() const;  // How the tools are turned from standing upright, to point into the screen.
  void Layout();      // Places everything inside the toolbar for its dock.
  void Land(ToolbarDock dock, const TimeBase& now);

  Compositor& compositor_;
  float scale_;
  SIZE monitor_;
  float ceiling_;
  const config::EditorPalette& palette_;

  // One shape that is the toolbar unfolded and the round button folded, and slides and narrows from one to the other.
  ComPtr<IDCompositionVisual> layer_;
  ComPtr<IDCompositionVisual> landing_;
  ComPtr<IDCompositionEffectGroup> landing_effect_;
  ComPtr<IDCompositionRectangleClip> landing_clip_;
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
  ComPtr<IDCompositionVisual> fold_face_;
  ComPtr<IDCompositionVisual> fold_veil_;
  ComPtr<IDCompositionEffectGroup> fold_veil_effect_;
  ComPtr<IDCompositionVisual> fold_icon_;
  ComPtr<IDCompositionVisual> badge_;
  ComPtr<IDCompositionEffectGroup> badge_effect_;
  ComPtr<IDCompositionVisual> badge_art_;
  ComPtr<IDCompositionVisual> badge_line_;
  ComPtr<IDCompositionVisual> badge_veil_;
  ComPtr<IDCompositionEffectGroup> badge_veil_effect_;
  AnimFloat alpha_, slide_, fold_hover_, badge_hover_;
  AnimFloat shape_x_, shape_y_, shape_w_, shape_h_, shape_r_;
  AnimFloat contents_alpha_, badge_alpha_, wide_shadow_alpha_, round_shadow_alpha_;
  AnimFloat landing_x_, landing_y_, landing_w_, landing_h_, landing_alpha_;
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
  float width_ = 0.0f;   // Lying along the bottom or the top.
  float height_ = 0.0f;
  ToolbarDock dock_ = ToolbarDock::kBottom;
  bool button_right_ = false;  // Folded into the bottom right corner rather than the bottom left.
  int laid_turns_ = -1;
  int laid_upright_ = -1;

  // A press on the toolbar or the button, which carries it once it moves far enough.
  POINT pressed_{};
  POINT grab_{};  // From the shape's corner to the press.
  POINT last_point_{};
  bool pressing_ = false;
  bool carrying_ = false;
  ToolbarDock landing_dock_ = ToolbarDock::kBottom;

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
