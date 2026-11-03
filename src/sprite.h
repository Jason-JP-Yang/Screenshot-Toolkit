// The thumbnail in the air: a free-standing bitmap that follows the pointer. To go into the bin it is cut
// into thin bands: its near edge stretches out to a narrow neck at the bin, then the whole card runs down
// that funnel and through the neck. The same motion backward draws it out again.
#pragma once

#include "compositor.h"

#include <array>
#include <vector>

namespace sstk {

struct SpritePose {
  float x, y;  // The grabbed point, in virtual screen coordinates.
  float scale;
  float angle;  // Radians, about the grabbed point.
  float pull;   // 0 is free, 1 is entirely inside the pull point.
  float pull_x, pull_y;
  float opacity;
  float shadow;
  float shadow_free;  // 0 keeps the shadow off the taskbar, as on a resting card; 1 lets it fall anywhere.
  float shadow_drop;
  float badge;
};

class DragSprite {
 public:
  // The grabbed point is in pixels of the thumbnail.
  DragSprite(Compositor& compositor, const Image& thumbnail, float grab_x, float grab_y, float scale);
  ~DragSprite();
  DragSprite(const DragSprite&) = delete;
  DragSprite& operator=(const DragSprite&) = delete;

  // Shows the sprite on one monitor's overlay. `free` is its space off the taskbar.
  void AddView(IDCompositionVisual* layer, const RECT& bounds, const RECT& free);

  void Render(const SpritePose& pose);

  float reach() const { return reach_; }

 private:
  static constexpr int kBandCount = 28;

  struct Band {
    ComPtr<IDCompositionVisual> holder;
    ComPtr<IDCompositionEffectGroup> effect;
    ComPtr<IDCompositionMatrixTransform3D> transform;
    ComPtr<IDCompositionVisual> image;
  };

  struct View {
    ComPtr<IDCompositionVisual> layer;
    ComPtr<IDCompositionVisual> root;
    ComPtr<IDCompositionEffectGroup> root_effect;
    ComPtr<IDCompositionVisual> shadow_holder;
    ComPtr<IDCompositionRectangleClip> shadow_clip;
    ComPtr<IDCompositionVisual> shadow;
    ComPtr<IDCompositionEffectGroup> shadow_effect;
    ComPtr<IDCompositionVisual> whole;
    ComPtr<IDCompositionVisual> bands_root;
    std::array<Band, kBandCount> bands;
    ComPtr<IDCompositionVisual> badge;
    ComPtr<IDCompositionEffectGroup> badge_effect;
    RECT bounds{};
    RECT free{};
    bool bent = false;
  };

  void ComputeBands(const D2D_MATRIX_3X2_F& rigid, const SpritePose& pose);

  Compositor& compositor_;
  std::vector<View> views_;
  ComPtr<IDCompositionSurface> surface_;
  ComPtr<IDCompositionSurface> shadow_surface_;
  ComPtr<IDCompositionSurface> badge_surface_;
  D2D_MATRIX_3X2_F shadow_local_{};
  std::array<int, kBandCount + 1> band_edges_{};
  std::array<D3DMATRIX, kBandCount> band_matrices_{};
  float width_ = 0.0f;
  float height_ = 0.0f;
  float grab_x_ = 0.0f;
  float grab_y_ = 0.0f;
  float reach_ = 0.0f;
  float badge_half_ = 0.0f;
  float corner_inset_ = 0.0f;
  float neck_ = 0.0f;
};

}  // namespace sstk
