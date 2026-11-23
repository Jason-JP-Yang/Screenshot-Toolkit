// The editing panel's controls along the top: closing at the left with the history beside it, and turning and
// finishing at the right. The selector between them is a Capsule of its own.
#pragma once

#include "sstk/config/colors.h"
#include "sstk/graphics/compositor.h"
#include "sstk/graphics/motion.h"

#include <vector>

namespace sstk {

enum class ChromeAction { kNone, kClose, kReset, kUndo, kRedo, kRotateRight, kRotateLeft, kDone };

class EditorChrome {
 public:
  EditorChrome(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor, const config::EditorPalette& palette);
  EditorChrome(const EditorChrome&) = delete;
  EditorChrome& operator=(const EditorChrome&) = delete;

  // The close button always, and the rest once there is a picture to work on.
  void Show(bool everything, const TimeBase& now, double delay);
  void Hide(const TimeBase& now);
  void Enable(ChromeAction action, bool enabled, const TimeBase& now);

  ChromeAction HitTest(POINT point) const;  // The enabled control under the point.
  bool Contains(POINT point) const;
  void Hover(ChromeAction action, const TimeBase& now);
  void Press(ChromeAction action, const TimeBase& now);

  float bottom() const { return bottom_; }
  float middle() const { return middle_; }

 private:
  struct Group {
    RECT rect{};
    ComPtr<IDCompositionVisual> root;
    ComPtr<IDCompositionEffectGroup> effect;
    AnimFloat alpha, rise;
    bool shown = false;
  };

  struct Cell {
    ChromeAction action = ChromeAction::kNone;
    int group = 0;
    RECT rect{};
    ComPtr<IDCompositionVisual> holder;
    ComPtr<IDCompositionScaleTransform> press;
    ComPtr<IDCompositionEffectGroup> veil_effect;
    ComPtr<IDCompositionEffectGroup> icon_effect;
    AnimFloat veil, icon, scale;
    bool enabled = true;
  };

  void AddGroup(const RECT& rect, uint32_t face);
  void AddCell(int group, ChromeAction action, const RECT& rect, int icon, uint32_t ink, uint32_t veil);
  void ShowGroup(Group& group, const TimeBase& now, double delay);

  Compositor& compositor_;
  ComPtr<IDCompositionVisual> layer_;
  float scale_;
  const config::EditorPalette& palette_;
  std::vector<Group> groups_;
  std::vector<Cell> cells_;
  ChromeAction hover_ = ChromeAction::kNone;
  float bottom_ = 0.0f;
  float middle_ = 0.0f;
};

}  // namespace sstk
