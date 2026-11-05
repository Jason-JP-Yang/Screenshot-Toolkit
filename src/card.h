// One screenshot on screen: from the full-screen frame in flight to a thumbnail in the deck.
//
// The card is a moving window over an "image plane" holding the whole screenshot. Flying in and
// switching regions are therefore the same operation: animate the window's edges and the plane's
// origin and scale together. Each of those is linear in the spring's progress, so every point of
// the image travels in a straight line and the motion reads as a single continuous camera move.
//
// The plane holds three kinds of bitmap, all in exact registration, so crossfading between them
// only ever changes sharpness: the full frame for the start of the flight, a base copy filtered
// for thumbnail scales that is always underneath, and one crisp 1:1 thumbnail per region.
//
// A scrolling screenshot is one more such bitmap. It starts where the scrolled area sits in the
// screenshot and reaches past it, so the card shows it by moving its window over the plane too.
#pragma once

#include "compositor.h"
#include "motion.h"
#include "recorder.h"
#include "screen.h"
#include "store.h"

#include <array>
#include <memory>
#include <vector>

namespace sstk {

struct RectF {
  float left, top, right, bottom;
  float width() const { return right - left; }
  float height() const { return bottom - top; }
};

// One soft shadow cut into corners and edges, so that it can follow a card of any size without its
// blur or corners ever being stretched. Shared by every card on a monitor.
struct CardShadow {
  std::array<ComPtr<IDCompositionSurface>, 4> corners;  // Top left, top right, bottom left, bottom right.
  std::array<ComPtr<IDCompositionSurface>, 4> edges;    // Top, bottom, left, right; one pixel long.
  float outer = 0.0f;  // How far the bitmap reaches beyond the card.
  float reach = 0.0f;  // How far corners reach along the card's edges.
};

CardShadow BuildCardShadow(Compositor& compositor, float scale);

class Card {
 public:
  Card(Compositor& compositor, IDCompositionVisual* parent, std::shared_ptr<Shot> shot, const ScreenInfo& screen,
       float scale, ComPtr<IDCompositionSurface> frost, const CardShadow& shadow, IDCompositionClip* shadow_clip,
       double created_at);
  ~Card();
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;

  // Covers the whole monitor, pixel for pixel, as the starting pose of the flight.
  void PlaceFullScreen();

  // Prepares the reduced copies the flight fades into. Separate so the first frame is not delayed by them.
  void BuildDerivedAssets(const TimeBase& now);

  void MoveTo(const RectF& rect, int depth, const SpringSpec& spec, const TimeBase& now, double delay, bool in_flight);

  // Crossfades to another region's thumbnail. MoveTo carries out the matching camera move.
  void Select(int index, const TimeBase& now);
  bool CanSelect(int index) const;

  // The size the card takes for its selection, and the part of the plane it shows there.
  SIZE DisplaySize() const;
  RectF SourceRect() const;

  // Adds the option of a scrolling screenshot of the app window, once its scrollable areas are known. It shows the
  // app window until the screenshot starts. Returns false if there is no such window among the options.
  bool OfferScroll(std::vector<RECT> areas);
  const std::vector<RECT>& scroll_areas() const { return scroll_areas_; }
  HWND app() const { return app_; }

  // A scrolling screenshot growing in place of the selected scroll option: started, extended, and then kept as
  // the option's picture or dropped along with the option.
  bool BeginLong(const ScrollUpdate& update, int max_rows, const TimeBase& now);
  void DrawLong(const ScrollUpdate& update);
  void KeepLong(const ScrollUpdate& update);
  void DropLong(const TimeBase& now);
  float long_height() const;  // Of the scrolling screenshot's view, or 0 without one.
  RECT long_rect() const { return RECT{0, 0, long_size_.cx, long_size_.cy}; }

  void SetSwipe(float offset, float progress);
  void SnapBack(const TimeBase& now, float velocity);
  void SlideOut(const TimeBase& now, float velocity);

  // Takes the card off screen while its sprite is in the air, and puts it back on top of the deck.
  void Detach();
  void Attach();

  // Moves the card to the top of the deck's stacking order.
  void Raise();

  // Where the card is at a given moment, and where it is heading.
  RectF RectAt(double time) const;
  RectF target_rect() const;
  float resting_opacity() const { return resting_opacity_; }

  // The selected region's thumbnail in pixels, which is what a drag lifts off the deck. Null until built.
  const Image* thumbnail() const;

  // Drops finished crossfade layers and whatever the flight no longer needs.
  void Tick(double now, Store& store);

  bool FinishedExiting(double now) const { return now >= exit_end_time_; }

  // Called while the card is deep in the deck: only its current thumbnail shows, so everything else
  // can go. Coming back to the front rebuilds it from the shot's archive.
  void Hibernate(Store& store);
  void Wake(Store& store);

  const std::shared_ptr<Shot>& shot() const { return shot_; }
  const std::vector<Region>& regions() const { return regions_; }
  const TaskbarInsets& insets() const { return insets_; }
  int selected() const { return selected_; }
  IDCompositionSurface* frost() const { return frost_.Get(); }

  double expiry = 0.0;
  bool floating = false;  // Its sprite is still flying home, so the card can be neither grabbed nor dismissed.
  bool busy = false;      // A scrolling screenshot is recording or showing its outcome, which holds the card in place.

 private:
  struct CrispLayer {
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    AnimFloat alpha;
    double retire_at = 0.0;
  };

  // A scrolling screenshot's thumbnail: rows on a tall sparse surface, numbered as in recorder.h, and the part on show.
  struct Long {
    RECT area{};  // Monitor pixels of the scrolled area.
    int surface_left = 0;  // Monitor pixels of the surface's first column.
    int view_width = 0;
    double scale = 1.0;  // Thumbnail pixels per screen pixel.
    int content_top = 0;
    int content_bottom = 0;
    int view_top = 0;
    int view_bottom = 0;
    int max_rows = 0;
    ComPtr<IDCompositionVirtualSurface> surface;
    Image pixels;  // The finished view in memory, kept for the moment the card is lifted.
  };

  int CrispIndex(int index) const;  // The scroll option borrows the app window's thumbnail.
  bool ShowsLong(int index) const;
  void SetLongView(int content_top, int content_bottom, int view_bottom);
  void BuildCaches(const Image& frame, bool with_base);
  void ShowCrisp(int index, const TimeBase& now, double delay, const SpringSpec& spec);
  void BuildShadowVisuals(const CardShadow& shadow, IDCompositionClip* clip);
  IDCompositionDevice* device() const { return compositor_.device(); }

  Compositor& compositor_;
  ComPtr<IDCompositionVisual> parent_;
  std::shared_ptr<Shot> shot_;
  std::vector<Region> regions_;
  TaskbarInsets insets_;
  float scale_;
  float border_;
  ComPtr<IDCompositionSurface> frost_;
  int selected_ = 0;

  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> shadow_holder_;
  ComPtr<IDCompositionEffectGroup> shadow_effect_;
  std::array<ComPtr<IDCompositionVisual>, 8> shadow_slices_;
  ComPtr<IDCompositionScaleTransform> shadow_edge_x_;
  ComPtr<IDCompositionScaleTransform> shadow_edge_y_;
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
  ComPtr<IDCompositionVisual> base_visual_;
  ComPtr<IDCompositionEffectGroup> base_effect_;
  ComPtr<IDCompositionSurface> base_surface_;
  std::vector<ComPtr<IDCompositionSurface>> crisp_surfaces_;  // One per region; empty until built.
  std::vector<SIZE> crisp_sizes_;
  std::vector<Image> crisp_pixels_;  // The same thumbnails in memory, kept for the moment one is lifted.
  std::vector<std::unique_ptr<CrispLayer>> crisp_layers_;

  AnimFloat left_, top_, right_, bottom_, radius_;
  AnimFloat plane_x_, plane_y_, plane_sx_, plane_sy_;
  AnimFloat opacity_, slide_, ring_alpha_, base_alpha_;
  AnimFloat shadow_alpha_, shadow_edge_w_, shadow_edge_h_;

  HWND app_ = nullptr;
  RECT app_rect_{};
  std::vector<RECT> scroll_areas_;
  int scroll_source_ = -1;
  int before_scroll_ = 0;  // The selection to go back to if the scrolling screenshot fails.
  Long long_;
  SIZE long_size_{};

  float shadow_reach_ = 0.0f;
  bool hibernating_ = false;
  double full_release_at_ = 0.0;
  double exit_end_time_ = 0.0;
  bool attached_ = true;
  float resting_opacity_ = 1.0f;
};

}  // namespace sstk
