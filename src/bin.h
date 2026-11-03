// The rubbish bin at the bottom center of the screen while a thumbnail is held: a disc when the pointer
// is far, a labeled capsule when near, open-lidded when over it. Stepped per frame, as it follows the pointer.
#pragma once

#include "compositor.h"
#include "motion.h"

namespace sstk {

class RubbishBin {
 public:
  RubbishBin(Compositor& compositor, IDCompositionVisual* parent, float scale);

  // The bottom center of the free space. All points are in stage coordinates.
  void Place(float center_x, float bottom);

  void Show();
  void Hide();

  // Returns whether the pointer is over the bin.
  bool Track(POINT point);
  bool Contains(POINT point) const;

  // Gives the capsule a small elastic jolt. Positive squats, negative stretches.
  void Nudge(float impulse);

  // The thumbnail is gone for good: the lid drops.
  void Gulp(bool then_hide);

  // Returns whether the bin needs more frames.
  bool Frame(float dt);
  void Reset();

  RECT HotZone() const;
  float x() const { return center_x_.value + lean_x_.value; }
  float y() const { return center_y_.value + lean_y_.value; }

 private:
  void Apply();
  void SnapToDisc();

  Compositor& compositor_;
  float scale_;
  float height_;
  float full_width_;
  float icon_size_;

  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> shadow_holder_;
  ComPtr<IDCompositionVisual> shadow_left_;
  ComPtr<IDCompositionVisual> shadow_middle_;
  ComPtr<IDCompositionVisual> shadow_right_;
  ComPtr<IDCompositionVisual> body_;
  ComPtr<IDCompositionRectangleClip> body_clip_;
  ComPtr<IDCompositionVisual> ring_;
  ComPtr<IDCompositionVisual> inner_;
  ComPtr<IDCompositionRectangleClip> inner_clip_;
  ComPtr<IDCompositionVisual> fill_;
  ComPtr<IDCompositionVisual> veil_;
  ComPtr<IDCompositionEffectGroup> veil_effect_;
  ComPtr<IDCompositionVisual> icon_;
  ComPtr<IDCompositionVisual> can_;
  ComPtr<IDCompositionVisual> lid_;
  ComPtr<IDCompositionVisual> label_;
  ComPtr<IDCompositionEffectGroup> label_effect_;

  float shadow_pad_ = 0.0f;
  float shadow_left_width_ = 0.0f;
  float shadow_right_width_ = 0.0f;
  float label_height_ = 0.0f;
  float label_start_ = 0.0f;

  SpringValue presence_;
  SpringValue left_;
  SpringValue right_;
  SpringValue grow_;
  SpringValue squash_;
  SpringValue lid_angle_;
  SpringValue label_alpha_;
  SpringValue engaged_light_;
  SpringValue center_x_;
  SpringValue center_y_;
  SpringValue lean_x_;
  SpringValue lean_y_;

  bool showing_ = false;
  bool placed_ = false;
  bool near_ = false;
  bool engaged_ = false;
  bool slamming_ = false;
  bool right_leads_ = true;
  double hide_in_ = 0.0;
};

}  // namespace sstk
