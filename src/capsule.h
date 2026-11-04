// The region selector pinned above the front card. The selected option carries its label, so the capsule changes width
// as the selection moves. A scrolling screenshot turns it into a recording indicator, and then into its outcome.
#pragma once

#include "compositor.h"
#include "motion.h"
#include "recorder.h"
#include "screen.h"

#include <memory>
#include <vector>

namespace sstk {

class Capsule {
 public:
  Capsule(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor);
  ~Capsule();
  Capsule(const Capsule&) = delete;
  Capsule& operator=(const Capsule&) = delete;

  // Shows the options of the card now in front. Fewer than two options hides the capsule. After a scrolling
  // screenshot's outcome, this turns it back into the selector.
  void Bind(const std::vector<Region>& regions, const TaskbarInsets& insets, int selected, IDCompositionSurface* frost,
            const TimeBase& now);

  void MoveTo(float x, float y, const TimeBase& now);
  void Select(int index, const TimeBase& now);
  void SetHover(int index, const TimeBase& now);  // In the recording state any index lights the whole capsule.
  void Hide(const TimeBase& now);

  // A scrolling screenshot waiting for its first scroll, then capturing, and then its outcome.
  void ShowRecording(bool capturing, const TimeBase& now);
  void ShowOutcome(ScrollOutcome outcome, const TimeBase& now);

  // Releases per-shot resources once the deck is empty and everything has faded.
  void Clear();
  void Tick(double now);

  bool visible() const { return visible_; }
  bool selecting() const { return mode_ == Mode::kSelector; }
  bool recording() const { return mode_ == Mode::kRecording; }
  float height() const { return height_; }
  float frost_width() const;

  RECT Bounds() const;
  bool Contains(int x, int y) const;  // Coordinates relative to the stage, as for HitTest.
  int HitTest(int x, int y) const;    // Returns the option under the point, or -1 outside them or outside the selector.

 private:
  enum class Mode { kSelector, kRecording, kOutcome };

  // One option: its glyph on the glass and on the pill, and its label, which shows only while it is selected.
  struct Segment {
    RegionKind kind;
    RECT rect;
    ComPtr<IDCompositionVisual> light;
    ComPtr<IDCompositionVisual> dark;
    ComPtr<IDCompositionEffectGroup> icon_effect;
    ComPtr<IDCompositionVisual> label;
    ComPtr<IDCompositionEffectGroup> label_effect;
    AnimFloat x, icon_alpha, label_alpha;
    float label_x = 0.0f;       // From the segment's left edge.
    float active_width = 0.0f;  // With its label showing.
    bool placed = false;
    double retire_at = 0.0;
  };

  // A piece of light ink on the glass, for the recording and outcome states.
  struct Piece {
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    AnimFloat alpha;
    float ink_offset = 0.0f;  // Where its ink starts in its bitmap.
    float ink_width = 0.0f;
  };

  std::unique_ptr<Segment> MakeSegment(const Region& region, const TaskbarInsets& insets);
  void RemoveSegment(Segment& segment);
  void MakePiece(Piece& piece, const Image& image, float y);
  void PlacePiece(Piece& piece, float ink_x);
  void RemovePiece(Piece& piece);
  float TextTop(const TextLine& line) const;
  void BuildRecording();
  float SegmentWidth(int index) const;
  void Arrange(const TimeBase& now, bool immediate);
  void ShapePill(float left, float right, float top, float bottom, const TimeBase& now, const SpringSpec& spec);
  void Breathe(const TimeBase& now);
  void StopBreathing(const TimeBase& now);
  void LeaveStatus(const TimeBase& now);

  Compositor& compositor_;
  float scale_;
  SIZE monitor_;
  float height_;
  float pad_;
  float cell_;
  float segment_height_;
  float dot_;

  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> shadow_left_;
  ComPtr<IDCompositionVisual> shadow_middle_;
  ComPtr<IDCompositionVisual> shadow_right_;
  ComPtr<IDCompositionScaleTransform> shadow_stretch_;
  ComPtr<IDCompositionVisual> frame_;
  ComPtr<IDCompositionRectangleClip> frame_clip_;
  ComPtr<IDCompositionVisual> ring_;
  ComPtr<IDCompositionVisual> inner_;
  ComPtr<IDCompositionRectangleClip> inner_clip_;
  ComPtr<IDCompositionVisual> fallback_fill_;
  ComPtr<IDCompositionVisual> frost_;
  ComPtr<IDCompositionVisual> status_fill_;
  ComPtr<IDCompositionEffectGroup> status_effect_;
  ComPtr<IDCompositionVisual> light_layer_;
  ComPtr<IDCompositionVisual> veil_;
  ComPtr<IDCompositionRectangleClip> veil_clip_;
  ComPtr<IDCompositionEffectGroup> veil_effect_;
  ComPtr<IDCompositionVisual> pill_;
  ComPtr<IDCompositionRectangleClip> pill_clip_;
  ComPtr<IDCompositionEffectGroup> pill_effect_;
  ComPtr<IDCompositionVisual> dark_layer_;
  ComPtr<IDCompositionVisual> status_layer_;
  ComPtr<IDCompositionScaleTransform> icon_scale_;

  std::vector<std::unique_ptr<Segment>> segments_;
  std::vector<std::unique_ptr<Segment>> retiring_;
  Piece waiting_, capturing_, finish_, outcome_icon_, outcome_label_;
  bool recording_built_ = false;
  float recording_inset_ = 0.0f;  // The dot's distance from the left edge, which the right edge mirrors.
  float recording_text_ = 0.0f;   // Where the recording label's ink starts.

  AnimFloat x_, y_, opacity_, width_, veil_alpha_, status_alpha_;
  AnimFloat pill_left_, pill_right_, pill_top_, pill_bottom_, pill_radius_, pill_alpha_;
  AnimFloat finish_x_, icon_pop_;

  Mode mode_ = Mode::kSelector;
  int selected_ = 0;
  int hover_ = -1;
  bool visible_ = false;
  bool capturing_now_ = false;
  bool breathing_ = false;
  double breath_start_ = 0.0;
  float shadow_pad_ = 0.0f;
  float shadow_left_width_ = 0.0f;
  float shadow_right_width_ = 0.0f;
  float target_x_ = 0.0f;
  float target_y_ = 0.0f;
};

}  // namespace sstk
