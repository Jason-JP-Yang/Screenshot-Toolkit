// The editing panel: the screenshot over the whole monitor, to crop, turn, and mark up. It opens out of a thumbnail,
// or straight from the capture keys that choose the part afterwards, and settles back into the deck when done or into
// the rubbish bin when closed. While it is open, nothing under it can be reached, the other monitors are dimmed and
// blocked, and the hook gives it every key and wheel turn.
//
// The picture lies in plane pixels, as card.h numbers them, turned by quarter turns and shown at
// screen = scale * turned + offset, inside a rotor that animates a quarter turn about the middle of the picture's space.
// The crop frame is drawn in screen pixels and moves with the same springs as the picture, so that both agree at rest.
#pragma once

#include "sstk/capture/capture.h"
#include "sstk/capture/screen.h"
#include "sstk/editor/brush.h"
#include "sstk/editor/canvas.h"
#include "sstk/editor/chrome.h"
#include "sstk/editor/document.h"
#include "sstk/editor/jobs.h"
#include "sstk/editor/toolbar.h"
#include "sstk/graphics/compositor.h"
#include "sstk/overlay/card.h"
#include "sstk/scroll/recorder.h"
#include "sstk/storage/store.h"

#include <memory>
#include <vector>

namespace sstk {

class Capsule;
class DragSprite;
class RubbishBin;

struct EditorRequest {
  std::shared_ptr<Shot> shot;
  std::unique_ptr<CaptureResult> capture;  // From the keys that open the panel, for the card made once it is done.
  std::shared_ptr<const Image> frame;      // The screen as shot, or null to have the store bring it back.
  std::vector<Region> regions;
  std::vector<std::shared_ptr<const LongImage>> pictures;  // Per region, its window's own picture, or null.
  std::shared_ptr<const LongImage> long_image;
  RECT long_plane{};
  TaskbarInsets insets;
  int selected = 0;  // The region chosen in the deck.
  std::shared_ptr<const EditDocument> document;  // What the last editing left, to go on with.
  bool choose = false;  // The part is chosen first, by dragging across the screen.
  // Where it opens from: a thumbnail showing `from_view` of the turned picture, or the whole screen.
  RectF from{};
  RectF from_view{};
  float from_radius = 0.0f;
  float from_border = 0.0f;
  Image preview;       // The thumbnail's pixels, shown until the picture itself is ready.
  float flash = 0.0f;  // How much of the capture flash still shows.
};

enum class EditorEnd { kKeep, kScroll, kDelete };

struct EditorOutcome {
  EditorEnd end = EditorEnd::kKeep;
  std::shared_ptr<EditDocument> document;  // Null when the picture is one of the regions as it is.
  int region = 0;                          // That region, then.
  SIZE size{};                             // The edited picture's pixels.
};

// Where the card waits: its rectangle on the monitor, the part of the plane it shows, and the edit it shows.
struct EditorLanding {
  RectF rect{};
  RectF view{};
  uint64_t version = 0;
};

class Editor;

class EditorHost {
 public:
  // The card is made or updated for the outcome, out of sight, and laid out where it will rest.
  virtual EditorLanding EditorLand(Editor& editor, const EditorOutcome& outcome) = 0;
  // The panel's picture lies exactly over the thumbnail it opened from, which can go.
  virtual void EditorCovering(Editor& editor) = 0;
  // The panel's picture has come to rest exactly where the card is, which can show.
  virtual void EditorArrived(Editor& editor, const EditorOutcome& outcome) = 0;
  // The screenshot went into the bin.
  virtual void EditorDeleted(Editor& editor) = 0;

 protected:
  ~EditorHost() = default;
};

class Editor {
 public:
  Editor(EditorHost& host, Compositor& compositor, Store& store, EditorJobs& jobs, HMONITOR monitor, const RECT& bounds, float scale);
  ~Editor();
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;

  bool ready() const { return window_ != nullptr; }
  void Open(EditorRequest request);

  HMONITOR monitor() const { return monitor_; }
  HWND window() const { return window_; }
  const std::shared_ptr<Shot>& shot() const { return shot_; }
  uint64_t capture_id() const { return capture_id_; }
  bool finished() const { return phase_ == Phase::kGone && vanish_frames_ == 0; }

  // What a card made from the panel's capture needs.
  std::unique_ptr<CaptureResult> TakeCapture() { return std::move(capture_); }
  std::vector<WindowShot> TakeWindowShots() { return std::move(window_shots_); }
  const ScrollHints& scroll_hints() const { return hints_; }
  bool scroll_offered() const { return scroll_offered_; }

  void OnKey(UINT key, LPARAM flags);
  void OnWheel(POINT screen, int delta, LPARAM flags);
  void OnJob(EditorJobResult& result);
  void ThumbnailReady();
  void AttachWindows(WindowShots& shots);
  void OfferScroll(const ScrollHints& hints);
  void Abort();  // Gone at once, as when the screens change.

  LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);

 private:
  enum class Phase { kOpening, kChoosing, kEditing, kLanding, kDeleting, kGone };
  enum class Drag { kNone, kSwallow, kChrome, kToolbar, kChoose, kCrop, kDraw, kObjectErase, kLasso, kMove, kPanView, kPanCrop, kRuler };

  struct Contact {
    float x = 0.0f;
    float y = 0.0f;
    float pressure = 0.5f;
    float tilt = 0.0f;
    float azimuth = 0.0f;
    double time = 0.0;
    bool mouse = true;
    bool eraser = false;  // The pen's other end.
  };

  struct Source {
    std::shared_ptr<const Image> pixels;
    std::shared_ptr<const LongImage> packed;
    RECT rect{};  // Plane pixels.
    ComPtr<IDCompositionVirtualSurface> surface;
    ComPtr<IDCompositionVisual> inside;
    ComPtr<IDCompositionVisual> outside;
    ComPtr<IDCompositionEffectGroup> inside_effect;
    ComPtr<IDCompositionEffectGroup> outside_effect;
    AnimFloat alpha;
  };

  struct Option {
    RegionKind kind = RegionKind::kCustom;
    RECT crop{};   // What choosing it keeps, in plane pixels.
    RECT glyph{};  // What the selector draws for it, in monitor pixels.
    int source = 0;
    int region = -1;
    HWND window = nullptr;
  };

  struct Shield {
    HWND window = nullptr;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    AnimFloat alpha;
  };

  // Building.
  void BuildVisuals();
  void BuildShields();
  void BuildOptions(const EditorRequest& request);
  int AddSource(std::shared_ptr<const Image> pixels, std::shared_ptr<const LongImage> packed, const RECT& rect);
  bool LoadSource(int index);
  void ShowSource(int index, const TimeBase& now, bool fade);
  void MakeCanvas(int index);
  void BindCapsule(const TimeBase& now);
  int CapsuleIndex(int option) const;

  // The view.
  RectF Area() const;
  EditorView Fit(const RECT& crop, int turns, const RectF& area) const;
  RectF FrameOf(const EditorView& view, const RECT& crop, int turns) const;
  void ApplyView(const EditorView& view, const RectF& frame, const SpringSpec* spec, const TimeBase& now, double delay = 0.0);
  void ApplyInner(const RectF& inner, float radius, const SpringSpec* spec, const TimeBase& now);
  void Refit(const SpringSpec& spec, const TimeBase& now);
  void AnimateTurn(int from, int to, const TimeBase& now);
  PointF ToPlane(float x, float y) const;
  PointF ToScreen(PointF plane) const;
  RECT SourceTurned() const;
  RectF PictureOnScreen() const;
  void SetTurns(int turns);

  // Sharpening the view at rest.
  void HideSharp();
  void RequestSharp();
  void PatchSharp(const RECT& plane);
  void FlushCanvas();

  // The document.
  void Commit(const EditDocument& before, const TimeBase& now);
  void ApplyDocument(const EditDocument& target, const TimeBase& now);
  void Undo(const TimeBase& now);
  void Redo(const TimeBase& now);
  void Reset(const TimeBase& now);
  void Rotate(int direction, const TimeBase& now);
  void ChooseOption(int option, const TimeBase& now);
  int OptionFor(const RECT& crop, int source) const;
  void UpdateChrome(const TimeBase& now);
  bool Changed() const;

  // Choosing the part.
  void EnterChoosing(const TimeBase& now);
  void EnterEditing(int option, const RECT& crop, const TimeBase& now);
  int WindowAt(POINT point) const;
  void PreviewChoice(POINT point, const TimeBase& now);

  // Input.
  void Press(const Contact& contact, bool pan);
  void Move(const Contact* contacts, size_t count);
  void Release(const Contact& contact);
  void Hover(POINT point);
  void EndDrag(const TimeBase& now);
  bool OnPointer(UINT message, WPARAM wparam);
  bool PenContacts(UINT32 id, std::vector<Contact>& contacts);
  PointF PointerPixel(const POINTER_INFO& info);
  void OnPinch(const TimeBase& now);
  void HandleToolbar(ToolbarEvent event, const TimeBase& now);
  void Act(ChromeAction action, const TimeBase& now);
  void UpdateCursor(POINT point);
  HCURSOR BrushCursor(int diameter);
  void Zoom(float factor, POINT anchor, const TimeBase& now);

  // Cropping.
  int HandleAt(POINT point) const;
  void BeginCrop(int edges, POINT point, const TimeBase& now);
  void DragCrop(POINT point);
  void EndCrop(const TimeBase& now);
  void EdgeZoom(float dt);
  void PanCrop(float dx, float dy);
  void ZoomCrop(float factor, POINT anchor, const TimeBase& now);
  void CommitCropGesture(const TimeBase& now);

  // Marking up.
  void BeginStroke(Tool tool, const Contact& contact);
  void AddPoint(const Contact& contact, bool last);
  void EndStroke(const Contact& contact, const TimeBase& now);
  void CancelStroke();
  PointF SnapToRuler(float x, float y) const;
  void EraseAlong(PointF from, PointF to);
  void BeginLasso(PointF plane);
  void EndLasso(const TimeBase& now);
  void ClearSelection();
  bool InSelection(PointF plane) const;
  RECT SelectionBounds() const;
  void BeginMove(PointF plane);
  void EndMove(const TimeBase& now);
  void RenderLasso();
  void Recolor(uint32_t color, const TimeBase& now);
  void DeleteSelection(const TimeBase& now);

  // The ruler.
  void ShowRuler(bool show, const TimeBase& now);
  void PlaceRuler();
  bool OnRuler(POINT point) const;
  void TurnRuler(float degrees);

  // Endings.
  void Land(EditorEnd end, const TimeBase& now);
  void Delete(const TimeBase& now);
  void StepDelete(float dt);
  void Vanish();
  Image Snapshot(const RectF& frame);

  // Frames.
  void OnFrame();
  void WantFrames();
  bool NeedsFrames() const;

  EditorHost& host_;
  Compositor& compositor_;
  Store& store_;
  EditorJobs& jobs_;
  HMONITOR monitor_;
  RECT bounds_;
  SIZE size_;
  float scale_;
  bool light_ = false;
  const config::EditorPalette* palette_ = nullptr;

  HWND window_ = nullptr;
  std::vector<Shield> shields_;
  ComPtr<IDCompositionTarget> target_;
  FramePacer pacer_;
  double last_frame_ = 0.0;

  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionEffectGroup> root_effect_;
  ComPtr<IDCompositionVisual> body_;
  ComPtr<IDCompositionEffectGroup> body_effect_;
  ComPtr<IDCompositionVisual> rotor_;
  ComPtr<IDCompositionEffectGroup> rotor_effect_;
  ComPtr<IDCompositionRotateTransform> rotate_;
  ComPtr<IDCompositionScaleTransform> view_scale_;
  ComPtr<IDCompositionTranslateTransform> view_shift_;
  ComPtr<IDCompositionTransform> view_transform_;
  ComPtr<IDCompositionVisual> outside_holder_;
  ComPtr<IDCompositionEffectGroup> outside_effect_;
  ComPtr<IDCompositionVisual> outside_view_;
  ComPtr<IDCompositionVisual> outside_plane_;
  ComPtr<IDCompositionVisual> frame_holder_;
  ComPtr<IDCompositionRectangleClip> frame_clip_;
  ComPtr<IDCompositionEffectGroup> line_effect_;
  ComPtr<IDCompositionVisual> ring_holder_;
  ComPtr<IDCompositionRectangleClip> ring_clip_;
  ComPtr<IDCompositionEffectGroup> ring_effect_;
  ComPtr<IDCompositionVisual> inner_holder_;
  ComPtr<IDCompositionRectangleClip> inner_clip_;
  ComPtr<IDCompositionVisual> inside_view_;
  ComPtr<IDCompositionVisual> inside_plane_;
  ComPtr<IDCompositionVisual> preview_;
  ComPtr<IDCompositionEffectGroup> preview_effect_;
  ComPtr<IDCompositionVisual> sharp_;
  ComPtr<IDCompositionEffectGroup> sharp_effect_;
  ComPtr<IDCompositionVisual> handles_;
  ComPtr<IDCompositionEffectGroup> handles_effect_;
  ComPtr<IDCompositionVisual> lasso_;
  ComPtr<IDCompositionVisual> ruler_;
  ComPtr<IDCompositionVisual> ruler_band_;
  ComPtr<IDCompositionVisual> ruler_label_;
  ComPtr<IDCompositionEffectGroup> ruler_effect_;
  ComPtr<IDCompositionVisual> flash_;
  ComPtr<IDCompositionEffectGroup> flash_effect_;
  ComPtr<IDCompositionVisual> chrome_layer_;
  ComPtr<IDCompositionVisual> bin_layer_;
  ComPtr<IDCompositionVirtualSurface> ink_surface_;
  ComPtr<IDCompositionVisual> inside_ink_;
  ComPtr<IDCompositionVisual> outside_ink_;
  ComPtr<IDCompositionVisual> inside_float_;
  ComPtr<IDCompositionVisual> outside_float_;
  ComPtr<IDCompositionSurface> sharp_surface_;

  AnimFloat root_alpha_, body_alpha_, flash_alpha_, photo_alpha_, outside_alpha_, line_alpha_, ring_alpha_, handles_alpha_;
  AnimFloat preview_alpha_, sharp_alpha_, ruler_alpha_;
  AnimFloat s_, x_, y_, angle_;
  AnimFloat fl_, ft_, fr_, fb_, fcx_, fcy_;
  AnimFloat il_, it_, ir_, ib_, radius_, inner_radius_;

  std::unique_ptr<EditorChrome> chrome_;
  std::unique_ptr<MarkupToolbar> toolbar_;
  std::unique_ptr<Capsule> capsule_;
  std::unique_ptr<RubbishBin> bin_;
  std::unique_ptr<DragSprite> sprite_;

  // What is being edited.
  std::shared_ptr<Shot> shot_;
  std::unique_ptr<CaptureResult> capture_;
  uint64_t capture_id_ = 0;
  HWND app_ = nullptr;
  RECT app_rect_{};
  std::vector<WindowShot> window_shots_;
  ScrollHints hints_;
  bool scroll_offered_ = false;
  std::vector<Source> sources_;
  std::vector<Option> options_;
  int active_source_ = -1;
  std::unique_ptr<InkCanvas> canvas_;
  EditDocument doc_;
  EditDocument original_;
  std::vector<EditDocument> undo_;
  std::vector<EditDocument> redo_;
  RECT custom_crop_{};
  int custom_source_ = -1;
  TaskbarInsets insets_;

  Phase phase_ = Phase::kOpening;
  bool choosing_again_ = false;
  EditDocument before_choosing_;
  EditorView view_{};
  RectF frame_{};
  double view_until_ = 0.0;
  double input_from_ = 0.0;
  double refit_at_ = 0.0;
  double sharpen_at_ = 0.0;
  int covering_frames_ = -1;

  uint64_t sharp_ticket_ = 0;
  bool sharp_shown_ = false;
  bool sharp_waiting_ = false;
  RECT sharp_rect_{};
  std::vector<RECT> sharp_dirty_;

  // The pointer.
  Drag drag_ = Drag::kNone;
  ChromeAction pressed_action_ = ChromeAction::kNone;
  POINT press_{};
  POINT last_point_{};
  bool moved_ = false;
  bool space_ = false;
  bool panning_button_ = false;
  HCURSOR cursor_ = nullptr;
  HCURSOR brush_cursor_ = nullptr;
  int brush_cursor_size_ = 0;
  UINT32 pen_ = 0;
  std::vector<std::pair<UINT32, PointF>> touches_;
  float pinch_distance_ = 0.0f;
  PointF pinch_center_{};
  std::vector<std::pair<HANDLE, std::pair<RECT, RECT>>> devices_;

  // Choosing.
  POINT choose_from_{};
  int choose_hover_ = -1;

  // Cropping.
  int crop_edges_ = 0;
  RectF crop_turned_{};
  PointF crop_grab_{};
  bool crop_gesture_ = false;
  double crop_gesture_until_ = 0.0;
  EditDocument crop_before_;

  // Marking up.
  std::shared_ptr<Stroke> live_;
  StampTrail trail_;
  bool live_pending_ = false;
  PointF smoothed_{};
  int ruler_edge_ = 0;
  float ruler_offset_ = 0.0f;
  EditDocument erase_before_;
  PointF erase_last_{};
  std::vector<PointF> lasso_path_;
  std::vector<size_t> selection_;
  PointF move_from_{};
  POINT move_delta_{};
  RECT float_bounds_{};

  // The ruler, on screen.
  bool ruler_on_ = false;
  PointF ruler_center_{};
  float ruler_angle_ = 45.0f;
  int ruler_length_ = 0;
  int ruler_thickness_ = 0;
  POINT ruler_grab_{};

  // Endings.
  EditorOutcome outcome_;
  bool thumbnail_ready_ = true;
  double arrive_at_ = 0.0;
  int vanish_frames_ = -1;
  SpringValue bin_x_, bin_y_, bin_scale_, bin_pull_;
  double delete_started_ = 0.0;
  bool gulped_ = false;
  float sprite_height_ = 0.0f;
};

}  // namespace sstk
