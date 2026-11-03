// The region selector pinned above the front card.
#pragma once

#include "compositor.h"
#include "motion.h"
#include "screen.h"

#include <memory>
#include <vector>

namespace sstk {

class Capsule {
 public:
  Capsule(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor);

  // Shows the options of the card now in front. Fewer than two options hides the capsule.
  void Bind(const std::vector<Region>& regions, const TaskbarInsets& insets, int selected, IDCompositionSurface* frost,
            const TimeBase& now);

  void MoveTo(float x, float y, const TimeBase& now);
  void Select(int index, const TimeBase& now);
  void SetHover(int index, const TimeBase& now);
  void Hide(const TimeBase& now);

  // Releases per-shot resources once the deck is empty and everything has faded.
  void Clear();
  void Tick(double now);

  bool visible() const { return visible_; }
  float height() const;
  float full_width() const;

  RECT Bounds() const;
  int HitTest(int x, int y) const;  // Coordinates relative to the stage. Returns -1 outside any segment.

 private:
  struct GlyphLayer {
    ComPtr<IDCompositionVisual> light;
    ComPtr<IDCompositionVisual> dark;
    ComPtr<IDCompositionSurface> light_surface;
    ComPtr<IDCompositionSurface> dark_surface;
    ComPtr<IDCompositionEffectGroup> effect;
    AnimFloat alpha;
    double retire_at = 0.0;
  };

  float SegmentLeft(int index) const;
  float WidthFor(int count) const;
  void PlacePill(int index, const TimeBase& now, bool immediate);

  Compositor& compositor_;
  float scale_;
  SIZE monitor_;

  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> shadow_;
  ComPtr<IDCompositionScaleTransform> shadow_scale_;
  ComPtr<IDCompositionSurface> shadow_surface_;
  ComPtr<IDCompositionVisual> frame_;
  ComPtr<IDCompositionRectangleClip> frame_clip_;
  ComPtr<IDCompositionVisual> ring_;
  ComPtr<IDCompositionVisual> inner_;
  ComPtr<IDCompositionRectangleClip> inner_clip_;
  ComPtr<IDCompositionVisual> frost_;
  ComPtr<IDCompositionVisual> fallback_fill_;
  ComPtr<IDCompositionVisual> light_layer_;
  ComPtr<IDCompositionVisual> veil_;
  ComPtr<IDCompositionRectangleClip> veil_clip_;
  ComPtr<IDCompositionVisual> veil_fill_;
  ComPtr<IDCompositionEffectGroup> veil_effect_;
  ComPtr<IDCompositionVisual> pill_;
  ComPtr<IDCompositionRectangleClip> pill_clip_;
  ComPtr<IDCompositionVisual> pill_fill_;
  ComPtr<IDCompositionVisual> dark_layer_;

  std::vector<std::unique_ptr<GlyphLayer>> glyph_layers_;

  AnimFloat x_, y_, opacity_, width_, shadow_sx_, pill_left_, pill_right_, veil_alpha_;

  int shadow_surface_width_ = 1;
  float shadow_pad_ = 0.0f;
  int option_count_ = 0;
  int selected_ = 0;
  int hover_ = -1;
  bool visible_ = false;
  float target_x_ = 0.0f;
  float target_y_ = 0.0f;
};

}  // namespace sstk
