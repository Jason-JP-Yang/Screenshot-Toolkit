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
// So is a window read by itself, which lies where the window does and may reach past the screen.
//
// Past the screen only such a picture has anything to show, so it is drawn there twice: once to fade in and out with
// the others, and once beneath the screen's own copy at full strength, so that what lies past the screen slides in
// with the view instead of fading in. A region switch is still one straight move; where it passes over plane that no
// picture covers, beside such a picture, the pictures around that plane are carried over it, blurred.
#pragma once

#include "sstk/capture/capture.h"
#include "sstk/capture/screen.h"
#include "sstk/graphics/compositor.h"
#include "sstk/graphics/motion.h"
#include "sstk/scroll/recorder.h"
#include "sstk/storage/store.h"

#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace sstk {

struct EditDocument;

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
  Card(Compositor& compositor, IDCompositionVisual* parent, std::shared_ptr<Shot> shot, const CaptureResult& capture,
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

  // The same at once, with every picture already faded in, for a card that comes out of the editing panel.
  void PlaceAt(const RectF& rect, int depth, const TimeBase& now);

  // Out of sight while the editing panel shows the screenshot, and back, exactly where the panel's picture came to rest.
  void Conceal();
  void Reveal(const TimeBase& now);

  // The editing panel's picture in place of the options, until it is edited back to one of them: its size first, then
  // its thumbnail and its pixels as they are made.
  void SetEdited(SIZE size);
  void SetEditedThumbnail(Image thumbnail, const TimeBase& now);
  void SetEditedPicture(std::shared_ptr<const LongImage> picture) { edited_.picture = std::move(picture); }
  void ClearEdited(const TimeBase& now);
  bool edited() const { return edited_.active; }
  bool edited_shown() const { return edited_.surface != nullptr; }

  // Crossfades to another region's thumbnail. MoveTo carries out the matching camera move.
  void Select(int index, const TimeBase& now);
  bool CanSelect(int index) const;

  // The size the card takes for its selection, and the part of the plane it shows there.
  SIZE DisplaySize() const;
  RectF SourceRect() const;

  // The windows' own pictures for the options that show them, in place of what the screen showed of them. Returns
  // whether the selection is one of them.
  bool AttachWindows(std::vector<WindowShot>& shots, const TimeBase& now);

  // The selection when it is a picture of its own rather than a region of the frame.
  std::shared_ptr<const LongImage> whole() const;

  // Adds the option of a scrolling screenshot of the app window, once its scrollable areas are known. It shows the
  // app window until the screenshot starts. Returns false if there is no such window among the options.
  bool OfferScroll(ScrollHints hints);
  const ScrollHints& scroll_hints() const { return scroll_hints_; }
  HWND app() const { return app_; }

  // A scrolling screenshot can be had only of the app as the shot saw it. Once it has left that behind, the offer is
  // withdrawn for good, and the option goes unless it is in use. Returns whether the options changed.
  bool scroll_open() const { return app_ != nullptr && !scroll_closed_; }
  bool AppChanged() const;  // The app window has gone, moved, or changed size.
  bool WithdrawScroll();
  void CloseScroll() { scroll_closed_ = true; }
  const RECT& app_rect() const { return app_rect_; }

  uint64_t capture() const { return capture_; }
  double taken() const { return taken_; }

  // A scrolling screenshot growing in place of the selected scroll option: started, extended, and then kept as
  // the option's picture or dropped along with the option.
  bool BeginLong(const ScrollUpdate& update, int max_rows, const TimeBase& now);
  void DrawLong(const ScrollUpdate& update);
  void KeepLong(const ScrollUpdate& update);
  void DropLong(const TimeBase& now);
  // Before the image is done, makes the thumbnail lifted with the card the part on show as drawn so far.
  void FreezeView();
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

  // What the editing panel opens with: each option's own picture, and the scrolling screenshot with its place on the plane.
  const std::vector<std::shared_ptr<const LongImage>>& pictures() const { return sources_; }
  const std::shared_ptr<const LongImage>& long_image() const { return long_image_; }
  const RECT& long_plane() const { return long_plane_; }

  std::shared_ptr<EditDocument> edit;  // The markup and crop the editing panel left, to go on with next time.
  uint64_t edit_version = 0;           // Of the picture being made from it.
  bool editing = false;                // The editing panel has it, and it is out of sight.

  double expiry = 0.0;
  bool floating = false;  // Its sprite is still flying home, so the card can be neither grabbed nor dismissed.
  bool busy = false;      // A scrolling screenshot is recording or showing its outcome, which holds the card in place.
  // Input has reached the app since the shot, so what it shows is compared with the shot from time to time.
  bool disturbed = false;
  double next_check = 0.0;

 private:
  struct CrispLayer {
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    AnimFloat alpha;
    ComPtr<IDCompositionVisual> rim;  // The same picture beneath the screen's copy, for one that reaches past it.
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
    bool done = false;  // Whether the image is done, and `pixels` shows it.
    // Until then, every thumbnail row drawn so far, in memory as well, so that the card can be lifted at any moment.
    std::deque<std::vector<uint32_t>> drawn;
    int drawn_top = 0;
  };

  struct Edited {
    bool active = false;
    SIZE size{};  // Of the picture.
    Image pixels;  // Its thumbnail.
    ComPtr<IDCompositionSurface> surface;
    std::shared_ptr<const LongImage> picture;
  };

  void KeepDrawn(const ScrollPiece& piece);
  void ShowEdited(const TimeBase& now, bool immediate);
  RectF EditedRect() const { return RectF{0.0f, 0.0f, static_cast<float>(edited_.size.cx), static_cast<float>(edited_.size.cy)}; }

  int CrispIndex(int index) const;  // The scroll option borrows the app window's thumbnail.
  bool ShowsLong(int index) const;
  void SetLongView(int content_top, int content_bottom, int view_bottom);
  void BuildCaches(const Image& frame, bool with_base);
  void SetCrisp(size_t index, Image pixels);
  void PopOption();
  RectF Screen() const;            // The plane the screen's own pictures cover.
  RectF Valid(int index) const;    // Where the picture an option shows has something to show.
  bool OwnPicture(int index) const;
  RectF ViewAt(double time) const;  // The part of the plane the card shows.
  void Backfill(const RectF& span, double until);
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
  ComPtr<IDCompositionVisual> rims_;  // Beneath everything else on the plane, but for the backfill.
  ComPtr<IDCompositionVisual> backfill_;
  double backfill_until_ = 0.0;
  RectF shown_valid_{};  // Where the picture last moved to has something to show.
  ComPtr<IDCompositionVisual> full_visual_;
  ComPtr<IDCompositionSurface> full_surface_;
  ComPtr<IDCompositionVisual> base_visual_;
  ComPtr<IDCompositionEffectGroup> base_effect_;
  ComPtr<IDCompositionSurface> base_surface_;
  std::vector<ComPtr<IDCompositionSurface>> crisp_surfaces_;  // One per region; empty until built.
  std::vector<SIZE> crisp_sizes_;
  std::vector<Image> crisp_pixels_;  // The same thumbnails in memory, kept for the moment one is lifted.
  std::vector<std::shared_ptr<const LongImage>> sources_;  // Per region, its own picture, or null for a part of the frame.
  std::vector<std::unique_ptr<CrispLayer>> crisp_layers_;

  AnimFloat left_, top_, right_, bottom_, radius_;
  AnimFloat plane_x_, plane_y_, plane_sx_, plane_sy_;
  AnimFloat opacity_, slide_, ring_alpha_, base_alpha_;
  AnimFloat shadow_alpha_, shadow_edge_w_, shadow_edge_h_;

  uint64_t capture_ = 0;
  double taken_ = 0.0;
  HWND app_ = nullptr;
  RECT app_rect_{};
  RECT app_bounds_{};
  ScrollHints scroll_hints_;
  int scroll_source_ = -1;
  int before_scroll_ = 0;  // The selection to go back to if the scrolling screenshot fails.
  bool scroll_closed_ = false;
  Long long_;
  SIZE long_size_{};
  std::shared_ptr<const LongImage> long_image_;
  RECT long_plane_{};
  Edited edited_;

  float shadow_reach_ = 0.0f;
  float shadow_target_ = 0.0f;
  bool hibernating_ = false;
  double full_release_at_ = 0.0;
  double exit_end_time_ = 0.0;
  bool attached_ = true;
  float resting_opacity_ = 1.0f;
};

}  // namespace sstk
