// One screenshot on screen: from the full-screen frame in flight to a thumbnail in the deck.
//
// The card is a moving window over an "image plane" holding the whole screenshot. Flying in and
// switching regions are therefore the same operation: animate the window's edges and the plane's
// origin and scale together. Each of those is linear in the spring's progress, so every point of
// the image travels in a straight line and the motion reads as a single continuous camera move.
#pragma once

#include "compositor.h"
#include "motion.h"
#include "screen.h"
#include "store.h"

#include <memory>
#include <vector>

namespace sstk {

struct RectF {
  float left, top, right, bottom;
  float width() const { return right - left; }
  float height() const { return bottom - top; }
};

class Card {
 public:
  Card(Compositor& compositor, IDCompositionVisual* parent, std::shared_ptr<Shot> shot, std::vector<Region> regions,
       float scale, ComPtr<IDCompositionSurface> frost, double created_at);
  ~Card();
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;

  // Covers the whole monitor, pixel for pixel, as the starting pose of the flight.
  void PlaceFullScreen();

  // Prepares the reduced copies the flight fades into. Separate so the first frame is not delayed by them.
  void BuildDerivedAssets(const RectF& landing, const TimeBase& now);

  void MoveTo(const RectF& rect, int depth, const SpringSpec& spec, const TimeBase& now, double delay, bool in_flight);

  void Select(int index, const RectF& rect, const TimeBase& now);

  void SetSwipe(float offset, float opacity);
  void SnapBack(const TimeBase& now, float velocity);
  void SlideOut(const TimeBase& now, float distance, float velocity);
  void SetDimmed(bool dimmed, const TimeBase& now);

  // Drops finished crossfade layers and the transient full-resolution surface.
  void Tick(double now);

  bool FinishedExiting(double now) const { return now >= exit_end_time_; }

  Image RenderDragPreview(int width) const;

  const std::shared_ptr<Shot>& shot() const { return shot_; }
  const std::vector<Region>& regions() const { return regions_; }
  int selected() const { return selected_; }
  const RECT& selected_rect() const { return regions_[selected_].rect; }
  IDCompositionSurface* frost() const { return frost_.Get(); }

  double expiry = 0.0;
  bool held_by_drag = false;

 private:
  struct CrispLayer {
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    ComPtr<IDCompositionSurface> surface;
    AnimFloat alpha;
    double retire_at = 0.0;
  };

  void AddCrisp(const RECT& region, int width, int height, const TimeBase& now, double delay);
  void UpdateShadow(int width, int height);
  IDCompositionDevice* device() const { return compositor_.device(); }

  Compositor& compositor_;
  ComPtr<IDCompositionVisual> parent_;
  std::shared_ptr<Shot> shot_;
  std::vector<Region> regions_;
  float scale_;
  ComPtr<IDCompositionSurface> frost_;
  int selected_ = 0;

  // Reduced copy kept in memory so that switching regions never touches every full-resolution pixel.
  Image half_;

  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> shadow_holder_;
  ComPtr<IDCompositionVisual> shadow_visual_;
  ComPtr<IDCompositionEffectGroup> shadow_effect_;
  ComPtr<IDCompositionScaleTransform> shadow_scale_;
  ComPtr<IDCompositionSurface> shadow_surface_;
  ComPtr<IDCompositionVisual> frame_;
  ComPtr<IDCompositionRectangleClip> frame_clip_;
  ComPtr<IDCompositionVisual> ring_;
  ComPtr<IDCompositionEffectGroup> ring_effect_;
  ComPtr<IDCompositionVisual> inner_;
  ComPtr<IDCompositionRectangleClip> inner_clip_;
  ComPtr<IDCompositionVisual> plane_;
  ComPtr<IDCompositionVisual> plane_scaled_;
  ComPtr<IDCompositionScaleTransform> plane_scale_;
  ComPtr<IDCompositionVisual> full_visual_;
  ComPtr<IDCompositionSurface> full_surface_;
  ComPtr<IDCompositionVisual> half_visual_;
  ComPtr<IDCompositionEffectGroup> half_effect_;
  ComPtr<IDCompositionSurface> half_surface_;
  std::vector<std::unique_ptr<CrispLayer>> crisp_layers_;

  AnimFloat left_, top_, right_, bottom_, radius_;
  AnimFloat plane_x_, plane_y_, plane_sx_, plane_sy_;
  AnimFloat opacity_, slide_, ring_alpha_, half_alpha_;
  AnimFloat shadow_x_, shadow_y_, shadow_sx_, shadow_sy_, shadow_alpha_;

  int shadow_width_ = 0;
  int shadow_height_ = 0;
  float shadow_pad_ = 0.0f;
  int shadow_surface_width_ = 1;
  int shadow_surface_height_ = 1;

  double full_release_at_ = 0.0;
  double exit_end_time_ = 0.0;
  bool dimmed_ = false;
  float resting_opacity_ = 1.0f;
};

}  // namespace sstk
