#include "sstk/editor/editor.h"

#include "sstk/capture/hook.h"
#include "sstk/config/animation.h"
#include "sstk/config/colors.h"
#include "sstk/config/core.h"
#include "sstk/config/layout.h"
#include "sstk/editor/art.h"
#include "sstk/editor/brush.h"
#include "sstk/overlay/bin.h"
#include "sstk/overlay/capsule.h"
#include "sstk/overlay/sprite.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <random>
#include <unordered_set>

namespace sstk {

namespace {

constexpr wchar_t kEditorClass[] = L"ScreenshotToolkit.Editor";
constexpr wchar_t kShieldClass[] = L"ScreenshotToolkit.Shield";
constexpr UINT kFrameMessage = WM_APP + 60;
constexpr UINT_PTR kBeatTimer = 1;
constexpr float kDegrees = 3.14159265f / 180.0f;
constexpr int kUploadRows = 512;

LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (editor != nullptr) return editor->Handle(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK ShieldProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_POINTERACTIVATE:
      return PA_NOACTIVATE;
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

void RegisterClasses() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  WNDCLASSW editor{};
  editor.lpfnWndProc = EditorProc;
  editor.hInstance = GetModuleHandleW(nullptr);
  editor.lpszClassName = kEditorClass;
  RegisterClassW(&editor);
  WNDCLASSW shield{};
  shield.lpfnWndProc = ShieldProc;
  shield.hInstance = GetModuleHandleW(nullptr);
  shield.lpszClassName = kShieldClass;
  RegisterClassW(&shield);
}

bool AppsUseLightTheme() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD,
               nullptr, &value, &size);
  return value != 0;
}

void ExcludeFromCapture(HWND window) {
  if (!OverlayVisibleToCapture()) SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
}

RectF ToRectF(const RECT& rect) {
  return RectF{static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right), static_cast<float>(rect.bottom)};
}

RECT Outward(const RectF& rect) {
  return RECT{static_cast<LONG>(std::floor(rect.left)), static_cast<LONG>(std::floor(rect.top)), static_cast<LONG>(std::ceil(rect.right)),
              static_cast<LONG>(std::ceil(rect.bottom))};
}

RECT Intersect(const RECT& a, const RECT& b) {
  RECT out{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
  if (out.right < out.left) out.right = out.left;
  if (out.bottom < out.top) out.bottom = out.top;
  return out;
}

bool Empty(const RECT& rect) { return rect.right <= rect.left || rect.bottom <= rect.top; }

RectF Inset(const RectF& rect, float by) { return RectF{rect.left + by, rect.top + by, rect.right - by, rect.bottom - by}; }

uint32_t Random() {
  static std::mt19937 engine{static_cast<unsigned>(GetTickCount64())};
  return engine();
}

bool PointInPolygon(const std::vector<PointF>& polygon, PointF point) {
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const PointF& a = polygon[i];
    const PointF& b = polygon[j];
    if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
  }
  return inside;
}

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by, float* along) {
  const float abx = bx - ax;
  const float aby = by - ay;
  const float t = std::clamp(((px - ax) * abx + (py - ay) * aby) / std::max(abx * abx + aby * aby, 1e-6f), 0.0f, 1.0f);
  *along = t;
  return std::hypot(px - (ax + abx * t), py - (ay + aby * t));
}

uint8_t ToByte(float value) { return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f)); }

}  // namespace

Editor::Editor(EditorHost& host, Compositor& compositor, Store& store, EditorJobs& jobs, HMONITOR monitor, const RECT& bounds, float scale)
    : host_(host), compositor_(compositor), store_(store), jobs_(jobs), monitor_(monitor), bounds_(bounds),
      size_{bounds.right - bounds.left, bounds.bottom - bounds.top}, scale_(scale) {
  RegisterClasses();
  light_ = AppsUseLightTheme();
  palette_ = light_ ? &config::kEditorLight : &config::kEditorDark;
  window_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kEditorClass, L"", WS_POPUP, bounds.left,
                            bounds.top, size_.cx, size_.cy, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (window_ == nullptr) return;
  SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ExcludeFromCapture(window_);
  if (FAILED(compositor_.device()->CreateTargetForHwnd(window_, TRUE, &target_))) {
    DestroyWindow(window_);
    window_ = nullptr;
    return;
  }
  cursor_ = LoadCursorW(nullptr, IDC_ARROW);
  BuildVisuals();
}

Editor::~Editor() {
  pacer_.Stop();
  if (window_ != nullptr) {
    KillTimer(window_, kBeatTimer);
    SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
  }
  sprite_.reset();
  bin_.reset();
  capsule_.reset();
  toolbar_.reset();
  chrome_.reset();
  if (target_) target_->SetRoot(nullptr);
  target_.Reset();
  for (Shield& shield : shields_) {
    if (shield.target) shield.target->SetRoot(nullptr);
    shield.target.Reset();
    if (shield.window != nullptr) DestroyWindow(shield.window);
  }
  if (window_ != nullptr) DestroyWindow(window_);
  if (brush_cursor_ != nullptr) DestroyCursor(brush_cursor_);
  compositor_.Commit();
}

void Editor::BuildVisuals() {
  IDCompositionDevice* dc = compositor_.device();
  const config::EditorPalette& palette = *palette_;
  const float w = static_cast<float>(size_.cx);
  const float h = static_cast<float>(size_.cy);
  const auto effect_of = [&](IDCompositionVisual* visual) {
    ComPtr<IDCompositionEffectGroup> effect = compositor_.CreateEffectGroup();
    visual->SetEffect(effect.Get());
    return effect;
  };

  root_ = compositor_.CreateVisual();
  root_effect_ = effect_of(root_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  target_->SetRoot(root_.Get());

  body_ = compositor_.CreateSolidVisual(0xFF000000u | palette.body, w, h);
  body_effect_ = effect_of(body_.Get());
  root_->AddVisual(body_.Get(), FALSE, nullptr);

  rotor_ = compositor_.CreateVisual();
  rotor_effect_ = effect_of(rotor_.Get());
  dc->CreateRotateTransform(&rotate_);
  rotor_->SetTransform(rotate_.Get());
  root_->AddVisual(rotor_.Get(), FALSE, nullptr);

  dc->CreateScaleTransform(&view_scale_);
  dc->CreateTranslateTransform(&view_shift_);
  IDCompositionTransform* parts[] = {view_scale_.Get(), view_shift_.Get()};
  dc->CreateTransformGroup(parts, 2, &view_transform_);

  outside_holder_ = compositor_.CreateVisual();
  outside_effect_ = effect_of(outside_holder_.Get());
  rotor_->AddVisual(outside_holder_.Get(), FALSE, nullptr);
  outside_view_ = compositor_.CreateVisual();
  outside_view_->SetTransform(view_transform_.Get());
  outside_holder_->AddVisual(outside_view_.Get(), FALSE, nullptr);
  outside_plane_ = compositor_.CreateVisual();
  outside_view_->AddVisual(outside_plane_.Get(), FALSE, nullptr);

  const auto big_solid = [&](uint32_t argb) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateSolidVisual(argb, w * 3.0f, h * 3.0f);
    visual->SetOffsetX(-w);
    visual->SetOffsetY(-h);
    return visual;
  };

  frame_holder_ = compositor_.CreateVisual();
  frame_clip_ = compositor_.CreateClip();
  frame_holder_->SetClip(frame_clip_.Get());
  frame_holder_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  ComPtr<IDCompositionVisual> line = big_solid(0xFF000000u | palette.frame);
  line_effect_ = effect_of(line.Get());
  frame_holder_->AddVisual(line.Get(), FALSE, nullptr);
  rotor_->AddVisual(frame_holder_.Get(), FALSE, nullptr);

  ring_holder_ = compositor_.CreateVisual();
  ring_clip_ = compositor_.CreateClip();
  ring_holder_->SetClip(ring_clip_.Get());
  ring_holder_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  ComPtr<IDCompositionVisual> ring = big_solid(0xFF000000u | config::kCardBorderColor);
  ring_effect_ = effect_of(ring.Get());
  ring_holder_->AddVisual(ring.Get(), FALSE, nullptr);
  rotor_->AddVisual(ring_holder_.Get(), FALSE, nullptr);

  inner_holder_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_holder_->SetClip(inner_clip_.Get());
  inner_holder_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  rotor_->AddVisual(inner_holder_.Get(), FALSE, nullptr);
  inside_view_ = compositor_.CreateVisual();
  inside_view_->SetTransform(view_transform_.Get());
  inner_holder_->AddVisual(inside_view_.Get(), FALSE, nullptr);
  inside_plane_ = compositor_.CreateVisual();
  inside_view_->AddVisual(inside_plane_.Get(), FALSE, nullptr);
  preview_ = compositor_.CreateVisual();
  preview_effect_ = effect_of(preview_.Get());
  inside_view_->AddVisual(preview_.Get(), FALSE, nullptr);
  sharp_ = compositor_.CreateVisual();
  sharp_effect_ = effect_of(sharp_.Get());
  inner_holder_->AddVisual(sharp_.Get(), FALSE, nullptr);

  inside_ink_ = compositor_.CreateVisual();
  outside_ink_ = compositor_.CreateVisual();
  inside_float_ = compositor_.CreateVisual();
  outside_float_ = compositor_.CreateVisual();
  inside_plane_->AddVisual(inside_ink_.Get(), FALSE, nullptr);
  inside_plane_->AddVisual(inside_float_.Get(), FALSE, nullptr);
  outside_plane_->AddVisual(outside_ink_.Get(), FALSE, nullptr);
  outside_plane_->AddVisual(outside_float_.Get(), FALSE, nullptr);

  // The crop frame's handles sit just outside it, so that they never hide what is kept.
  handles_ = compositor_.CreateVisual();
  handles_effect_ = effect_of(handles_.Get());
  rotor_->AddVisual(handles_.Get(), FALSE, nullptr);
  const int thickness = static_cast<int>(std::max(std::round(config::kEditorHandleWidth * scale_), 2.0f));
  const int arm = static_cast<int>(std::round(config::kEditorCornerArm * scale_));
  const int edge = static_cast<int>(std::round(config::kEditorEdgeArm * scale_));
  const int pad = static_cast<int>(std::ceil(3.0f * scale_));
  const float t = static_cast<float>(thickness);
  const float p = static_cast<float>(pad);
  const float corner_size = static_cast<float>(arm + pad * 2);
  const auto handle = [&](const Image& image) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateVisual();
    visual->SetContent(compositor_.CreateSurface(image, false).Get());
    handles_->AddVisual(visual.Get(), FALSE, nullptr);
    handle_visuals_.push_back(visual);
    return visual;
  };
  const float line_width = std::max(std::round(config::kEditorFrameLine * scale_), 1.0f);
  ComPtr<IDCompositionVisual> corners[4];
  for (int i = 0; i < 4; ++i) corners[i] = handle(RenderCornerHandle(i, arm, thickness, pad, palette.frame, palette.halo, palette.halo_alpha));
  ComPtr<IDCompositionVisual> top_bar = handle(RenderEdgeHandle(true, edge, thickness, pad, palette.frame, palette.halo, palette.halo_alpha));
  ComPtr<IDCompositionVisual> bottom_bar = handle(RenderEdgeHandle(true, edge, thickness, pad, palette.frame, palette.halo, palette.halo_alpha));
  ComPtr<IDCompositionVisual> left_bar = handle(RenderEdgeHandle(false, edge, thickness, pad, palette.frame, palette.halo, palette.halo_alpha));
  ComPtr<IDCompositionVisual> right_bar = handle(RenderEdgeHandle(false, edge, thickness, pad, palette.frame, palette.halo, palette.halo_alpha));
  const float half_edge = static_cast<float>(edge) * 0.5f;

  const auto x_of = SSTK_SETTER(IDCompositionVisual, SetOffsetX);
  const auto y_of = SSTK_SETTER(IDCompositionVisual, SetOffsetY);
  fl_.Bind(frame_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetLeft), -line_width);
  fl_.Bind(ring_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetLeft));
  fl_.Bind(corners[0].Get(), x_of, -(t + p));
  fl_.Bind(corners[2].Get(), x_of, -(t + p));
  fl_.Bind(left_bar.Get(), x_of, -(t + p));
  fr_.Bind(frame_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight), line_width);
  fr_.Bind(ring_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  fr_.Bind(corners[1].Get(), x_of, t + p - corner_size);
  fr_.Bind(corners[3].Get(), x_of, t + p - corner_size);
  fr_.Bind(right_bar.Get(), x_of, -p);
  ft_.Bind(frame_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetTop), -line_width);
  ft_.Bind(ring_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetTop));
  ft_.Bind(corners[0].Get(), y_of, -(t + p));
  ft_.Bind(corners[1].Get(), y_of, -(t + p));
  ft_.Bind(top_bar.Get(), y_of, -(t + p));
  fb_.Bind(frame_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom), line_width);
  fb_.Bind(ring_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  fb_.Bind(corners[2].Get(), y_of, t + p - corner_size);
  fb_.Bind(corners[3].Get(), y_of, t + p - corner_size);
  fb_.Bind(bottom_bar.Get(), y_of, -p);
  fcx_.Bind(top_bar.Get(), x_of, -half_edge - p);
  fcx_.Bind(bottom_bar.Get(), x_of, -half_edge - p);
  fcy_.Bind(left_bar.Get(), y_of, -half_edge - p);
  fcy_.Bind(right_bar.Get(), y_of, -half_edge - p);
  il_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetLeft));
  it_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetTop));
  ir_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  ib_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  radius_.Bind(frame_clip_.Get(), SetClipRadii, line_width);
  radius_.Bind(ring_clip_.Get(), SetClipRadii);
  inner_radius_.Bind(inner_clip_.Get(), SetClipRadii);
  s_.Bind(view_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  s_.Bind(view_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));
  x_.Bind(view_shift_.Get(), SSTK_SETTER(IDCompositionTranslateTransform, SetOffsetX));
  y_.Bind(view_shift_.Get(), SSTK_SETTER(IDCompositionTranslateTransform, SetOffsetY));
  angle_.Bind(rotate_.Get(), SSTK_SETTER(IDCompositionRotateTransform, SetAngle));

  lasso_ = compositor_.CreateVisual();
  root_->AddVisual(lasso_.Get(), FALSE, nullptr);

  ruler_ = compositor_.CreateVisual();
  ruler_effect_ = effect_of(ruler_.Get());
  ruler_band_ = compositor_.CreateVisual();
  ruler_label_ = compositor_.CreateVisual();
  ruler_->AddVisual(ruler_band_.Get(), FALSE, nullptr);
  ruler_->AddVisual(ruler_label_.Get(), FALSE, nullptr);
  root_->AddVisual(ruler_.Get(), FALSE, nullptr);

  flash_ = compositor_.CreateSolidVisual(0xFF000000u | config::kFlashColor, w, h);
  flash_effect_ = effect_of(flash_.Get());
  root_->AddVisual(flash_.Get(), FALSE, nullptr);

  chrome_layer_ = compositor_.CreateVisual();
  root_->AddVisual(chrome_layer_.Get(), FALSE, nullptr);
  capsule_ = std::make_unique<Capsule>(compositor_, chrome_layer_.Get(), scale_, size_, palette.capsule);
  chrome_ = std::make_unique<EditorChrome>(compositor_, chrome_layer_.Get(), scale_, size_, palette);
  toolbar_ = std::make_unique<MarkupToolbar>(compositor_, chrome_layer_.Get(), scale_, size_, palette);
  bin_layer_ = compositor_.CreateVisual();
  root_->AddVisual(bin_layer_.Get(), FALSE, nullptr);

  root_alpha_.Bind(root_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  body_alpha_.Bind(body_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  flash_alpha_.Bind(flash_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  photo_alpha_.Bind(rotor_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  outside_alpha_.Bind(outside_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  line_alpha_.Bind(line_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  ring_alpha_.Bind(ring_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  handles_alpha_.Bind(handles_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  preview_alpha_.Bind(preview_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  sharp_alpha_.Bind(sharp_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  ruler_alpha_.Bind(ruler_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));

  root_alpha_.Snap(1.0f);
  body_alpha_.Snap(0.0f);
  flash_alpha_.Snap(0.0f);
  photo_alpha_.Snap(1.0f);
  outside_alpha_.Snap(0.0f);
  line_alpha_.Snap(0.0f);
  ring_alpha_.Snap(0.0f);
  handles_alpha_.Snap(0.0f);
  preview_alpha_.Snap(0.0f);
  sharp_alpha_.Snap(0.0f);
  ruler_alpha_.Snap(0.0f);
  angle_.Snap(0.0f);
  radius_.Snap(0.0f);
  inner_radius_.Snap(0.0f);
  ApplyView(EditorView{1.0f, 0.0f, 0.0f}, RectF{0.0f, 0.0f, w, h}, nullptr, TimeBase{});
  SetTurns(0);
}

void Editor::BuildShields() {
  std::vector<std::pair<HMONITOR, RECT>> monitors;
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT rect, LPARAM context) -> BOOL {
    reinterpret_cast<std::vector<std::pair<HMONITOR, RECT>>*>(context)->emplace_back(handle, *rect);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&monitors));
  for (const auto& [handle, rect] : monitors) {
    if (handle == monitor_) continue;
    Shield shield;
    shield.window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kShieldClass, L"", WS_POPUP, rect.left,
                                    rect.top, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (shield.window == nullptr) continue;
    ExcludeFromCapture(shield.window);
    if (FAILED(compositor_.device()->CreateTargetForHwnd(shield.window, TRUE, &shield.target))) {
      DestroyWindow(shield.window);
      continue;
    }
    shield.visual = compositor_.CreateSolidVisual(0xFF000000u, static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    shield.effect = compositor_.CreateEffectGroup();
    shield.visual->SetEffect(shield.effect.Get());
    shield.target->SetRoot(shield.visual.Get());
    shields_.push_back(std::move(shield));
    Shield& added = shields_.back();
    added.alpha.Bind(added.effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
    added.alpha.Snap(0.0f);
  }
}

int Editor::AddSource(std::shared_ptr<const Image> pixels, std::shared_ptr<const LongImage> packed, const RECT& rect) {
  if (packed) {
    for (size_t i = 0; i < sources_.size(); ++i) {
      if (sources_[i].packed == packed) return static_cast<int>(i);
    }
  }
  Source source;
  source.pixels = std::move(pixels);
  source.packed = std::move(packed);
  source.rect = rect;
  if (source.packed) source.rect = RECT{rect.left, rect.top, rect.left + source.packed->width(), rect.top + source.packed->height()};
  sources_.push_back(std::move(source));
  return static_cast<int>(sources_.size()) - 1;
}

void Editor::BuildOptions(const EditorRequest& request) {
  const RECT screen = request.regions.empty() ? RECT{0, 0, size_.cx, size_.cy} : request.regions.front().rect;
  AddSource(request.frame, nullptr, RECT{0, 0, screen.right - screen.left, screen.bottom - screen.top});
  options_.push_back(Option{});
  for (size_t i = 0; i < request.regions.size(); ++i) {
    const Region& region = request.regions[i];
    Option option;
    option.kind = region.kind;
    option.glyph = region.rect;
    option.region = static_cast<int>(i);
    option.window = region.window;
    const std::shared_ptr<const LongImage> picture = i < request.pictures.size() ? request.pictures[i] : nullptr;
    if (region.kind == RegionKind::kLong) {
      if (!request.long_image) continue;
      option.source = AddSource(nullptr, request.long_image, request.long_plane);
      option.crop = sources_[static_cast<size_t>(option.source)].rect;
    } else if (picture) {
      option.source = AddSource(nullptr, picture, region.rect);
      option.crop = sources_[static_cast<size_t>(option.source)].rect;
    } else {
      option.source = 0;
      option.crop = Intersect(region.rect, sources_[0].rect);
    }
    if (region.kind == RegionKind::kScroll) scroll_offered_ = true;
    if (Empty(option.crop)) continue;
    options_.push_back(option);
  }
}

bool Editor::LoadSource(int index) {
  if (index < 0 || index >= static_cast<int>(sources_.size())) return false;
  Source& source = sources_[static_cast<size_t>(index)];
  if (source.surface) return true;
  if (!source.pixels) {
    if (source.packed) {
      const LongImage& packed = *source.packed;
      auto image = std::make_shared<Image>(Image::Allocate(packed.width(), packed.height()));
      if (!*image) return false;
      for (int row = 0; row < packed.height(); row += kUploadRows) {
        packed.Read(row, std::min(kUploadRows, packed.height() - row), image->data() + static_cast<ptrdiff_t>(row) * image->stride(), image->stride(), false);
      }
      source.pixels = std::move(image);
    } else if (shot_) {
      source.pixels = store_.AcquireFrame(shot_);
    }
  }
  if (!source.pixels || !*source.pixels) return false;
  const Image& pixels = *source.pixels;
  source.rect.right = source.rect.left + pixels.width();
  source.rect.bottom = source.rect.top + pixels.height();
  source.surface = compositor_.CreateVirtualSurface(pixels.width(), pixels.height(), true);
  if (!source.surface) return false;
  for (int row = 0; row < pixels.height(); row += kUploadRows) {
    const int count = std::min(kUploadRows, pixels.height() - row);
    compositor_.Upload(source.surface.Get(), POINT{0, row}, PixelView{pixels.data() + static_cast<ptrdiff_t>(row) * pixels.stride(), pixels.width(), count, pixels.stride()});
  }
  const auto place = [&](ComPtr<IDCompositionVisual>& visual, ComPtr<IDCompositionEffectGroup>& effect, IDCompositionVisual* plane) {
    visual = compositor_.CreateVisual();
    visual->SetContent(source.surface.Get());
    visual->SetOffsetX(static_cast<float>(source.rect.left));
    visual->SetOffsetY(static_cast<float>(source.rect.top));
    effect = compositor_.CreateEffectGroup();
    visual->SetEffect(effect.Get());
    plane->AddVisual(visual.Get(), TRUE, nullptr);
  };
  place(source.inside, source.inside_effect, inside_plane_.Get());
  place(source.outside, source.outside_effect, outside_plane_.Get());
  source.alpha.Bind(source.inside_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  source.alpha.Bind(source.outside_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  source.alpha.Snap(0.0f);
  return true;
}

void Editor::ShowSource(int index, const TimeBase& now, bool fade) {
  for (size_t i = 0; i < sources_.size(); ++i) {
    Source& source = sources_[i];
    if (!source.surface) continue;
    const float target = static_cast<int>(i) == index ? 1.0f : 0.0f;
    if (fade) {
      source.alpha.To(compositor_.device(), now, target, config::kEditorFade);
    } else {
      source.alpha.Snap(target);
    }
  }
}

void Editor::MakeCanvas(int index) {
  Source& source = sources_[static_cast<size_t>(index)];
  canvas_ = std::make_unique<InkCanvas>(source.pixels, POINT{source.rect.left, source.rect.top});
  active_source_ = index;
  ink_surface_ = compositor_.CreateVirtualSurface(canvas_->width(), canvas_->height(), false);
  for (IDCompositionVisual* ink : {inside_ink_.Get(), outside_ink_.Get()}) {
    ink->SetContent(ink_surface_.Get());
    ink->SetOffsetX(static_cast<float>(source.rect.left));
    ink->SetOffsetY(static_cast<float>(source.rect.top));
  }
  canvas_->Redraw(doc_.strokes, source.rect);
  FlushCanvas();
}

void Editor::BindCapsule(const TimeBase& now) {
  std::vector<Region> regions;
  regions.reserve(options_.size());
  for (const Option& option : options_) regions.push_back(Region{option.kind, option.glyph, option.window});
  capsule_->Bind(regions, insets_, phase_ == Phase::kChoosing ? 0 : CapsuleIndex(doc_.option), nullptr, now);
}

int Editor::CapsuleIndex(int option) const { return std::clamp(option, 0, static_cast<int>(options_.size()) - 1); }

void Editor::Open(EditorRequest request) {
  if (window_ == nullptr) return;
  const TimeBase now = compositor_.Now();
  IDCompositionDevice* dc = compositor_.device();
  const config::EditorPalette& palette = *palette_;
  shot_ = request.shot;
  capture_ = std::move(request.capture);
  if (capture_) {
    capture_id_ = capture_->id;
    app_ = capture_->screen.app;
    app_rect_ = capture_->screen.app_rect;
  }
  insets_ = request.insets;
  BuildOptions(request);

  int chosen = 1;
  for (size_t i = 1; i < options_.size(); ++i) {
    if (options_[i].region == request.selected) chosen = static_cast<int>(i);
  }
  chosen = std::min(chosen, static_cast<int>(options_.size()) - 1);
  original_ = EditDocument{};
  original_.option = chosen;
  original_.source = options_[static_cast<size_t>(chosen)].source;
  original_.crop = options_[static_cast<size_t>(chosen)].crop;
  doc_ = original_;
  if (request.document && request.document->source >= 0 && request.document->source < static_cast<int>(sources_.size()) &&
      request.document->option >= 0 && request.document->option < static_cast<int>(options_.size())) {
    doc_ = *request.document;
  }
  if (doc_.option == 0) {
    custom_crop_ = doc_.crop;
    custom_source_ = doc_.source;
  }
  SetTurns(doc_.turns);
  BuildShields();
  capsule_->CenterOn(static_cast<float>(size_.cx) * 0.5f, std::round(chrome_->middle() - capsule_->height() * 0.5f), now);

  if (request.choose) {
    phase_ = Phase::kChoosing;
    if (LoadSource(0)) {
      MakeCanvas(0);
      ShowSource(0, now, false);
    }
    const RECT whole = sources_[0].rect;
    // It starts exactly where the screen showed it, and the flash goes on over it.
    ApplyView(EditorView{1.0f, 0.0f, 0.0f}, ToRectF(whole), nullptr, now);
    flash_alpha_.Snap(request.flash);
    flash_alpha_.To(dc, now, 0.0f, config::kFlashFall);
    body_alpha_.To(dc, now, 1.0f, config::kEditorFade);
    const EditorView fit = Fit(whole, 0, Area());
    ApplyView(fit, FrameOf(fit, whole, 0), &config::kEditorOpen, now);
    chrome_->Show(false, now, config::kEditorChromeDelay);
    BindCapsule(now);
    input_from_ = now.seconds + 0.12;
  } else {
    phase_ = Phase::kEditing;
    const float start = request.from.width() / std::max(request.from_view.width(), 1.0f);
    ApplyView(EditorView{start, request.from.left - start * request.from_view.left, request.from.top - start * request.from_view.top}, request.from,
              nullptr, now);
    radius_.Snap(request.from_radius);
    ApplyInner(Inset(request.from, request.from_border), std::max(request.from_radius - request.from_border, 0.0f), nullptr, now);
    ring_alpha_.Snap(1.0f);
    if (request.preview) {
      preview_->SetContent(compositor_.CreateSurface(request.preview, true).Get());
      preview_->SetTransform(D2D_MATRIX_3X2_F{request.from_view.width() / request.preview.width(), 0.0f, 0.0f,
                                              request.from_view.height() / request.preview.height(), request.from_view.left, request.from_view.top});
      preview_alpha_.Snap(1.0f);
    }
    const EditorView fit = Fit(doc_.crop, doc_.turns, Area());
    ApplyView(fit, FrameOf(fit, doc_.crop, doc_.turns), &config::kEditorOpen, now);
    radius_.To(dc, now, 0.0f, config::kEditorOpen);
    ring_alpha_.To(dc, now, 0.0f, config::kEditorFade, 0.05);
    body_alpha_.To(dc, now, 1.0f, config::kEditorFade);
    outside_alpha_.To(dc, now, palette.outside_alpha, config::kEditorFade, config::kEditorOutsideDelay);
    line_alpha_.To(dc, now, 1.0f, config::kEditorFade, config::kEditorChromeDelay);
    handles_alpha_.To(dc, now, 1.0f, config::kEditorFade, config::kEditorChromeDelay);
    chrome_->Show(true, now, config::kEditorChromeDelay);
    toolbar_->Show(now, config::kEditorChromeDelay);
    BindCapsule(now);
    UpdateChrome(now);
    input_from_ = now.seconds + 0.22;
    covering_frames_ = 2;
  }
  compositor_.Commit();
  SetWindowPos(window_, HWND_TOPMOST, bounds_.left, bounds_.top, size_.cx, size_.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
  for (Shield& shield : shields_) {
    SetWindowPos(shield.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE);
    shield.alpha.To(dc, now, palette.shield_alpha, config::kEditorFade);
  }
  SetTimer(window_, kBeatTimer, config::kEditorHeartbeatMs, nullptr);
  EditorHeartbeat();

  if (phase_ == Phase::kEditing) {
    // The picture itself, once the thumbnail is already on its way out of the deck.
    const TimeBase later = compositor_.Now();
    if (LoadSource(doc_.source)) {
      MakeCanvas(doc_.source);
      ShowSource(doc_.source, later, false);
      preview_alpha_.To(dc, later, 0.0f, config::kQuickFade, 0.05);
    }
  }
  compositor_.Commit();
  last_frame_ = now.seconds;
  WantFrames();
}

RectF Editor::Area() const {
  const float top = chrome_->bottom() + config::kEditorPictureTop * scale_;
  const float side = config::kEditorPictureSide * scale_;
  const float height = static_cast<float>(size_.cy);
  float bottom = height - top;
  if (phase_ != Phase::kChoosing && toolbar_->shown() && !toolbar_->folded()) bottom = toolbar_->top() - config::kEditorPictureBottom * scale_;
  return RectF{side, top, static_cast<float>(size_.cx) - side, std::max(bottom, top + 1.0f)};
}

EditorView Editor::Fit(const RECT& crop, int turns, const RectF& area) const {
  const RECT turned = TurnRect(crop, turns);
  const float w = static_cast<float>(std::max(turned.right - turned.left, 1L));
  const float h = static_cast<float>(std::max(turned.bottom - turned.top, 1L));
  const float s = std::min({area.width() / w, area.height() / h, config::kEditorMaxFit});
  EditorView view;
  view.scale = s;
  view.x = std::round((area.left + area.right) * 0.5f - s * w * 0.5f) - s * static_cast<float>(turned.left);
  view.y = std::round((area.top + area.bottom) * 0.5f - s * h * 0.5f) - s * static_cast<float>(turned.top);
  return view;
}

RectF Editor::FrameOf(const EditorView& view, const RECT& crop, int turns) const {
  const RECT turned = TurnRect(crop, turns);
  return RectF{view.scale * turned.left + view.x, view.scale * turned.top + view.y, view.scale * turned.right + view.x, view.scale * turned.bottom + view.y};
}

void Editor::ApplyView(const EditorView& view, const RectF& frame, const SpringSpec* spec, const TimeBase& now, double delay) {
  view_ = view;
  frame_ = frame;
  IDCompositionDevice* dc = compositor_.device();
  const auto set = [&](AnimFloat& value, float target) {
    if (spec != nullptr && dc != nullptr) {
      value.To(dc, now, target, *spec, delay);
    } else {
      value.Snap(target);
    }
  };
  set(s_, view.scale);
  set(x_, view.x);
  set(y_, view.y);
  set(fl_, frame.left);
  set(ft_, frame.top);
  set(fr_, frame.right);
  set(fb_, frame.bottom);
  set(fcx_, (frame.left + frame.right) * 0.5f);
  set(fcy_, (frame.top + frame.bottom) * 0.5f);
  set(radius_, 0.0f);
  ApplyInner(frame, 0.0f, spec, now);
  view_until_ = spec != nullptr ? now.seconds + delay + SpringSettleTime(*spec) : now.seconds;
  HideSharp();
  sharpen_at_ = std::max(view_until_, now.seconds) + config::kEditorSharpDelay;
  if (!lasso_path_.empty()) RenderLasso();
  if (window_ != nullptr && dc != nullptr) WantFrames();
}

void Editor::ApplyInner(const RectF& inner, float radius, const SpringSpec* spec, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const auto set = [&](AnimFloat& value, float target) {
    if (spec != nullptr && dc != nullptr) {
      value.To(dc, now, target, *spec);
    } else {
      value.Snap(target);
    }
  };
  set(il_, inner.left);
  set(it_, inner.top);
  set(ir_, inner.right);
  set(ib_, inner.bottom);
  set(inner_radius_, radius);
}

void Editor::Refit(const SpringSpec& spec, const TimeBase& now) {
  if (phase_ != Phase::kEditing) return;
  const EditorView fit = Fit(doc_.crop, doc_.turns, Area());
  ApplyView(fit, FrameOf(fit, doc_.crop, doc_.turns), &spec, now);
}

void Editor::SetTurns(int turns) {
  const D2D_MATRIX_3X2_F matrix = TurnMatrix(turns);
  inside_plane_->SetTransform(matrix);
  outside_plane_->SetTransform(matrix);
}

void Editor::AnimateTurn(int from, int to, const TimeBase& now) {
  const int delta = ((to - from) % 4 + 4) % 4;
  SetTurns(to);
  if (delta == 0) {
    Refit(config::kEditorView, now);
    return;
  }
  const float degrees = delta == 3 ? -90.0f : static_cast<float>(delta) * 90.0f;
  const RectF area = Area();
  const PointF pivot{(area.left + area.right) * 0.5f, (area.top + area.bottom) * 0.5f};
  rotate_->SetCenterX(pivot.x);
  rotate_->SetCenterY(pivot.y);
  // The picture in its new way up, turned back by the same angle, which looks just as it did a moment ago.
  const float c = std::cos(degrees * kDegrees);
  const float s = std::sin(degrees * kDegrees);
  const auto turn = [&](float x, float y) { return PointF{pivot.x + (x - pivot.x) * c - (y - pivot.y) * s, pivot.y + (x - pivot.x) * s + (y - pivot.y) * c}; };
  const PointF shift = turn(view_.x, view_.y);
  const PointF a = turn(frame_.left, frame_.top);
  const PointF b = turn(frame_.right, frame_.bottom);
  const RectF frame{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
  ApplyView(EditorView{view_.scale, shift.x, shift.y}, frame, nullptr, now);
  angle_.Snap(-degrees);
  angle_.To(compositor_.device(), now, 0.0f, config::kEditorView);
  ClearSelection();
  Refit(config::kEditorView, now);
}

PointF Editor::ToPlane(float x, float y) const {
  const PointF turned{(x - view_.x) / view_.scale, (y - view_.y) / view_.scale};
  return Turn(turned, 4 - doc_.turns);
}

PointF Editor::ToScreen(PointF plane) const {
  const PointF turned = Turn(plane, doc_.turns);
  return PointF{view_.scale * turned.x + view_.x, view_.scale * turned.y + view_.y};
}

RECT Editor::SourceTurned() const {
  const int index = active_source_ >= 0 ? active_source_ : doc_.source;
  return TurnRect(sources_[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(sources_.size()) - 1))].rect, doc_.turns);
}

RectF Editor::PictureOnScreen() const {
  const RECT turned = SourceTurned();
  return RectF{view_.scale * turned.left + view_.x, view_.scale * turned.top + view_.y, view_.scale * turned.right + view_.x,
               view_.scale * turned.bottom + view_.y};
}

void Editor::HideSharp() {
  if (!sharp_shown_ && !sharp_waiting_) return;
  sharp_alpha_.Snap(0.0f);
  sharp_shown_ = false;
  sharp_waiting_ = false;
  ++sharp_ticket_;
  sharp_dirty_.clear();
}

void Editor::RequestSharp() {
  if (!canvas_ || (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) || view_.scale >= 0.999f) return;
  if (drag_ == Drag::kCrop || drag_ == Drag::kPanView || drag_ == Drag::kPanCrop || crop_gesture_ || touches_.size() >= 2) return;
  const RECT screen = Intersect(Outward(frame_), RECT{0, 0, size_.cx, size_.cy});
  if (Empty(screen)) return;
  SharpenJob job;
  job.ticket = ++sharp_ticket_;
  job.canvas = canvas_->Snapshot();
  job.turns = doc_.turns;
  job.view = view_;
  job.screen = screen;
  jobs_.Sharpen(std::move(job));
  sharp_waiting_ = true;
  sharp_rect_ = screen;
  sharp_dirty_.clear();
}

void Editor::OnJob(EditorJobResult& result) {
  if (result.kind != EditorJobResult::Kind::kSharp || result.ticket != sharp_ticket_ || !sharp_waiting_ || !result.pixels) return;
  sharp_waiting_ = false;
  sharp_surface_ = compositor_.CreateSurface(result.pixels, true);
  if (!sharp_surface_) return;
  sharp_->SetContent(sharp_surface_.Get());
  sharp_->SetOffsetX(static_cast<float>(result.screen.left));
  sharp_->SetOffsetY(static_cast<float>(result.screen.top));
  sharp_rect_ = result.screen;
  sharp_shown_ = true;
  sharp_alpha_.Snap(0.0f);
  sharp_alpha_.To(compositor_.device(), compositor_.Now(), 1.0f, config::kQuickFade);
  std::vector<RECT> dirty;
  dirty.swap(sharp_dirty_);
  for (const RECT& rect : dirty) PatchSharp(rect);
  compositor_.Commit();
}

void Editor::PatchSharp(const RECT& plane) {
  if (sharp_waiting_) {
    sharp_dirty_.push_back(plane);
    return;
  }
  if (!sharp_shown_ || !canvas_ || !sharp_surface_) return;
  const PointF a = ToScreen(PointF{static_cast<float>(plane.left), static_cast<float>(plane.top)});
  const PointF b = ToScreen(PointF{static_cast<float>(plane.right), static_cast<float>(plane.bottom)});
  RECT screen = Outward(RectF{std::min(a.x, b.x) - 1.0f, std::min(a.y, b.y) - 1.0f, std::max(a.x, b.x) + 1.0f, std::max(a.y, b.y) + 1.0f});
  screen = Intersect(screen, sharp_rect_);
  if (Empty(screen)) return;
  const Image pixels = Sharpen(canvas_->Snapshot(), doc_.turns, view_, screen);
  if (pixels) compositor_.Upload(sharp_surface_.Get(), POINT{screen.left - sharp_rect_.left, screen.top - sharp_rect_.top}, pixels);
}

void Editor::FlushCanvas() {
  if (!canvas_ || !ink_surface_) return;
  const POINT origin = canvas_->origin();
  for (const RECT& change : canvas_->TakeChanges()) {
    const PixelView view = canvas_->View(change);
    if (view.data != nullptr) compositor_.Upload(ink_surface_.Get(), POINT{change.left, change.top}, view);
    PatchSharp(RECT{change.left + origin.x, change.top + origin.y, change.right + origin.x, change.bottom + origin.y});
  }
}

void Editor::Commit(const EditDocument& before, const TimeBase& now) {
  undo_.push_back(before);
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  redo_.clear();
  UpdateChrome(now);
}

bool Editor::Changed() const {
  return !doc_.strokes.empty() || doc_.turns != 0 || doc_.source != original_.source || !Same(doc_.crop, original_.crop);
}

void Editor::UpdateChrome(const TimeBase& now) {
  chrome_->Enable(ChromeAction::kUndo, !undo_.empty(), now);
  chrome_->Enable(ChromeAction::kRedo, !redo_.empty(), now);
  chrome_->Enable(ChromeAction::kReset, Changed(), now);
}

void Editor::ApplyDocument(const EditDocument& target, const TimeBase& now) {
  const EditDocument from = doc_;
  ClearSelection();
  doc_ = target;
  if (target.source != active_source_ && LoadSource(target.source)) {
    MakeCanvas(target.source);
    ShowSource(target.source, now, true);
  } else if (canvas_) {
    std::unordered_set<const Stroke*> before;
    std::unordered_set<const Stroke*> after;
    for (const auto& stroke : from.strokes) before.insert(stroke.get());
    for (const auto& stroke : target.strokes) after.insert(stroke.get());
    RECT dirty{};
    for (const auto& stroke : from.strokes) {
      if (after.count(stroke.get()) == 0) dirty = Union(dirty, stroke->bounds);
    }
    for (const auto& stroke : target.strokes) {
      if (before.count(stroke.get()) == 0) dirty = Union(dirty, stroke->bounds);
    }
    if (!Empty(dirty)) canvas_->Redraw(doc_.strokes, dirty);
    FlushCanvas();
  }
  if (from.turns != target.turns) {
    AnimateTurn(from.turns, target.turns, now);
  } else if (!Same(from.crop, target.crop) || from.source != target.source) {
    Refit(config::kEditorView, now);
  }
  if (target.option == 0) {
    custom_crop_ = target.crop;
    custom_source_ = target.source;
  }
  capsule_->Select(CapsuleIndex(target.option), now);
  UpdateChrome(now);
}

void Editor::Undo(const TimeBase& now) {
  if (undo_.empty() || live_) return;
  const EditDocument target = undo_.back();
  undo_.pop_back();
  redo_.push_back(doc_);
  ApplyDocument(target, now);
}

void Editor::Redo(const TimeBase& now) {
  if (redo_.empty() || live_) return;
  const EditDocument target = redo_.back();
  redo_.pop_back();
  undo_.push_back(doc_);
  ApplyDocument(target, now);
}

void Editor::Reset(const TimeBase& now) {
  if (!Changed()) return;
  const EditDocument before = doc_;
  ApplyDocument(original_, now);
  Commit(before, now);
}

void Editor::Rotate(int direction, const TimeBase& now) {
  const EditDocument before = doc_;
  doc_.turns = ((doc_.turns + direction) % 4 + 4) % 4;
  Commit(before, now);
  AnimateTurn(before.turns, doc_.turns, now);
}

int Editor::OptionFor(const RECT& crop, int source) const {
  for (size_t i = 1; i < options_.size(); ++i) {
    const Option& option = options_[i];
    if (option.kind != RegionKind::kScroll && option.source == source && Same(option.crop, crop)) return static_cast<int>(i);
  }
  return 0;
}

void Editor::ChooseOption(int option, const TimeBase& now) {
  if (option < 0 || option >= static_cast<int>(options_.size())) return;
  const Option& chosen = options_[static_cast<size_t>(option)];
  if (chosen.kind == RegionKind::kScroll) {
    Land(EditorEnd::kScroll, now);
    return;
  }
  if (phase_ == Phase::kChoosing) {
    if (option != 0) EnterEditing(option, chosen.crop, now);
    return;
  }
  if (option == 0) {
    if (doc_.option == 0) return;
    if (custom_source_ >= 0 && !Empty(custom_crop_)) {
      EditDocument target = doc_;
      target.option = 0;
      target.crop = custom_crop_;
      target.source = custom_source_;
      const EditDocument before = doc_;
      ApplyDocument(target, now);
      Commit(before, now);
    } else {
      EnterChoosing(now);
    }
    return;
  }
  if (option == doc_.option) return;
  EditDocument target = doc_;
  target.option = option;
  target.source = chosen.source;
  target.crop = chosen.crop;
  const EditDocument before = doc_;
  ApplyDocument(target, now);
  Commit(before, now);
}

void Editor::EnterChoosing(const TimeBase& now) {
  choosing_again_ = true;
  before_choosing_ = doc_;
  ClearSelection();
  phase_ = Phase::kChoosing;
  choose_hover_ = -1;
  IDCompositionDevice* dc = compositor_.device();
  toolbar_->Hide(now);
  ShowRuler(false, now);
  handles_alpha_.To(dc, now, 0.0f, config::kEditorFade);
  outside_alpha_.To(dc, now, 0.0f, config::kEditorFade);
  line_alpha_.To(dc, now, 0.0f, config::kEditorFade);
  const RECT whole = sources_[static_cast<size_t>(std::max(active_source_, 0))].rect;
  const EditorView fit = Fit(whole, doc_.turns, Area());
  ApplyView(fit, FrameOf(fit, whole, doc_.turns), &config::kEditorView, now);
  capsule_->Select(0, now);
}

void Editor::EnterEditing(int option, const RECT& crop, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const EditDocument before = choosing_again_ ? before_choosing_ : doc_;
  doc_.option = option;
  doc_.crop = crop;
  doc_.source = option > 0 ? options_[static_cast<size_t>(option)].source : std::max(active_source_, 0);
  if (doc_.source != active_source_ && LoadSource(doc_.source)) {
    MakeCanvas(doc_.source);
    ShowSource(doc_.source, now, true);
  }
  if (option == 0) {
    custom_crop_ = crop;
    custom_source_ = doc_.source;
  }
  phase_ = Phase::kEditing;
  if (choosing_again_) {
    Commit(before, now);
  } else {
    original_ = doc_;
    undo_.clear();
    redo_.clear();
  }
  choosing_again_ = false;
  choose_hover_ = -1;
  const EditorView fit = Fit(doc_.crop, doc_.turns, Area());
  ApplyView(fit, FrameOf(fit, doc_.crop, doc_.turns), &config::kEditorView, now);
  outside_alpha_.To(dc, now, palette_->outside_alpha, config::kEditorFade);
  line_alpha_.To(dc, now, 1.0f, config::kEditorFade);
  handles_alpha_.To(dc, now, 1.0f, config::kEditorFade, 0.08);
  chrome_->Show(true, now, 0.04);
  toolbar_->Show(now, 0.08);
  if (toolbar_->ruler()) ShowRuler(true, now);
  capsule_->Select(CapsuleIndex(option), now);
  UpdateChrome(now);
  input_from_ = now.seconds + 0.12;
}

int Editor::WindowAt(POINT point) const {
  int best = -1;
  float best_area = 0.0f;
  for (size_t i = 1; i < options_.size(); ++i) {
    const Option& option = options_[i];
    if (option.kind != RegionKind::kApp && option.kind != RegionKind::kZone) continue;
    const RectF rect = FrameOf(view_, option.crop, doc_.turns);
    const float x = static_cast<float>(point.x);
    const float y = static_cast<float>(point.y);
    if (x < rect.left || x >= rect.right || y < rect.top || y >= rect.bottom) continue;
    const float area = rect.width() * rect.height();
    if (best < 0 || area < best_area) {
      best = static_cast<int>(i);
      best_area = area;
    }
  }
  return best;
}

void Editor::PreviewChoice(POINT point, const TimeBase& now) {
  const int over = WindowAt(point);
  if (over == choose_hover_) return;
  choose_hover_ = over;
  IDCompositionDevice* dc = compositor_.device();
  const EditorView view = view_;
  if (over >= 0) {
    const RectF frame = FrameOf(view_, options_[static_cast<size_t>(over)].crop, doc_.turns);
    ApplyView(view, frame, &config::kEditorZoom, now);
    outside_alpha_.To(dc, now, palette_->outside_alpha, config::kQuickFade);
    line_alpha_.To(dc, now, 1.0f, config::kQuickFade);
  } else {
    ApplyView(view, PictureOnScreen(), &config::kEditorZoom, now);
    outside_alpha_.To(dc, now, 0.0f, config::kQuickFade);
    line_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  }
}

LRESULT Editor::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_POINTERACTIVATE:
      return PA_NOACTIVATE;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        SetCursor(cursor_);
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      Contact contact;
      contact.x = static_cast<float>(GET_X_LPARAM(lparam));
      contact.y = static_cast<float>(GET_Y_LPARAM(lparam));
      contact.time = NowSeconds();
      if (drag_ != Drag::kNone) {
        Move(&contact, 1);
      } else {
        Hover(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
      if (drag_ != Drag::kNone) return 0;
      Contact contact;
      contact.x = static_cast<float>(GET_X_LPARAM(lparam));
      contact.y = static_cast<float>(GET_Y_LPARAM(lparam));
      contact.time = NowSeconds();
      panning_button_ = message != WM_LBUTTONDOWN;
      Press(contact, panning_button_);
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
      if (drag_ == Drag::kNone || panning_button_ != (message != WM_LBUTTONUP)) return 0;
      Contact contact;
      contact.x = static_cast<float>(GET_X_LPARAM(lparam));
      contact.y = static_cast<float>(GET_Y_LPARAM(lparam));
      contact.time = NowSeconds();
      Release(contact);
      return 0;
    }
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
      if (OnPointer(message, wparam)) return 0;
      break;
    case WM_CAPTURECHANGED:
      if (drag_ != Drag::kNone && reinterpret_cast<HWND>(lparam) != window_) {
        const TimeBase now = compositor_.Now();
        EndDrag(now);
        compositor_.Commit();
      }
      return 0;
    case WM_TIMER:
      if (wparam == kBeatTimer) EditorHeartbeat();
      return 0;
    case kFrameMessage:
      OnFrame();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

PointF Editor::PointerPixel(const POINTER_INFO& info) {
  auto found = std::find_if(devices_.begin(), devices_.end(), [&](const auto& entry) { return entry.first == info.sourceDevice; });
  if (found == devices_.end()) {
    RECT device{};
    RECT display{};
    if (GetPointerDeviceRects(info.sourceDevice, &device, &display)) {
      devices_.push_back({info.sourceDevice, {device, display}});
      found = devices_.end() - 1;
    }
  }
  float x = static_cast<float>(info.ptPixelLocation.x);
  float y = static_cast<float>(info.ptPixelLocation.y);
  if (found != devices_.end()) {
    const RECT& device = found->second.first;
    const RECT& display = found->second.second;
    if (device.right > device.left && device.bottom > device.top) {
      x = static_cast<float>(display.left) + static_cast<float>(info.ptHimetricLocation.x - device.left) * static_cast<float>(display.right - display.left) /
                                                 static_cast<float>(device.right - device.left);
      y = static_cast<float>(display.top) + static_cast<float>(info.ptHimetricLocation.y - device.top) * static_cast<float>(display.bottom - display.top) /
                                                static_cast<float>(device.bottom - device.top);
    }
  }
  return PointF{x - static_cast<float>(bounds_.left), y - static_cast<float>(bounds_.top)};
}

bool Editor::PenContacts(UINT32 id, std::vector<Contact>& contacts) {
  POINTER_PEN_INFO latest{};
  if (!GetPointerPenInfo(id, &latest)) return false;
  UINT32 count = std::max<UINT32>(latest.pointerInfo.historyCount, 1);
  std::vector<POINTER_PEN_INFO> history(count);
  if (count > 1 && GetPointerPenInfoHistory(id, &count, history.data())) {
    history.resize(count);
  } else {
    history.assign(1, latest);
  }
  const double frequency = static_cast<double>(QpcFrequency());
  // The history comes newest first.
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    const POINTER_PEN_INFO& pen = *it;
    Contact contact;
    const PointF at = PointerPixel(pen.pointerInfo);
    contact.x = at.x;
    contact.y = at.y;
    contact.pressure = (pen.penMask & PEN_MASK_PRESSURE) != 0 ? static_cast<float>(pen.pressure) / 1024.0f : 0.5f;
    if ((pen.penMask & (PEN_MASK_TILT_X | PEN_MASK_TILT_Y)) != 0) {
      const float tx = static_cast<float>(pen.tiltX);
      const float ty = static_cast<float>(pen.tiltY);
      contact.tilt = std::clamp(std::hypot(tx, ty) / 60.0f, 0.0f, 1.0f);
      contact.azimuth = std::atan2(ty, tx);
    }
    contact.mouse = false;
    contact.eraser = (pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
    contact.time = pen.pointerInfo.PerformanceCount != 0 ? static_cast<double>(pen.pointerInfo.PerformanceCount) / frequency : NowSeconds();
    contacts.push_back(contact);
  }
  return true;
}

bool Editor::OnPointer(UINT message, WPARAM wparam) {
  const UINT32 id = GET_POINTERID_WPARAM(wparam);
  POINTER_INPUT_TYPE type = PT_POINTER;
  if (!GetPointerType(id, &type)) return false;
  if (type == PT_PEN) {
    std::vector<Contact> contacts;
    if (!PenContacts(id, contacts) || contacts.empty()) return true;
    const Contact& latest = contacts.back();
    const POINT point{std::lround(latest.x), std::lround(latest.y)};
    if (message == WM_POINTERDOWN) {
      if (drag_ == Drag::kNone) {
        pen_ = id;
        panning_button_ = false;
        Press(latest, false);
      }
    } else if (message == WM_POINTERUPDATE) {
      if (pen_ == id && IS_POINTER_INCONTACT_WPARAM(wparam) && drag_ != Drag::kNone) {
        Move(contacts.data(), contacts.size());
      } else if (drag_ == Drag::kNone) {
        Hover(point);
      }
    } else if (pen_ == id) {
      pen_ = 0;
      if (drag_ != Drag::kNone) Release(latest);
    }
    return true;
  }
  if (type != PT_TOUCH) return false;
  POINTER_INFO info{};
  if (!GetPointerInfo(id, &info)) return true;
  const TimeBase now = compositor_.Now();
  Contact contact;
  const PointF at = PointerPixel(info);
  contact.x = at.x;
  contact.y = at.y;
  contact.time = NowSeconds();
  const auto found = std::find_if(touches_.begin(), touches_.end(), [&](const auto& touch) { return touch.first == id; });
  if (message == WM_POINTERDOWN) {
    if (found == touches_.end()) touches_.push_back({id, at});
    if (touches_.size() == 1) {
      if (drag_ == Drag::kNone) {
        panning_button_ = false;
        Press(contact, false);
      }
    } else if (touches_.size() == 2) {
      // A second finger turns the touch into a pinch, and whatever the first had begun is taken back.
      if (drag_ == Drag::kDraw) CancelStroke();
      if (drag_ != Drag::kNone && drag_ != Drag::kDraw) EndDrag(now);
      drag_ = Drag::kSwallow;
      const PointF a = touches_[0].second;
      const PointF b = touches_[1].second;
      pinch_distance_ = std::max(std::hypot(a.x - b.x, a.y - b.y), 1.0f);
      pinch_center_ = PointF{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
      if (toolbar_->folded() && !crop_gesture_) {
        crop_before_ = doc_;
        crop_gesture_ = true;
      }
    }
  } else if (message == WM_POINTERUPDATE) {
    if (found != touches_.end()) found->second = at;
    if (touches_.size() >= 2) {
      OnPinch(now);
    } else if (touches_.size() == 1 && found != touches_.end() && drag_ != Drag::kNone) {
      Move(&contact, 1);
    }
  } else {
    const bool was_pinch = touches_.size() >= 2;
    if (found != touches_.end()) touches_.erase(found);
    if (touches_.empty()) {
      if (drag_ != Drag::kNone) Release(contact);
      if (was_pinch && crop_gesture_) CommitCropGesture(now);
    }
  }
  compositor_.Commit();
  return true;
}

void Editor::OnPinch(const TimeBase& now) {
  if (phase_ != Phase::kEditing || touches_.size() < 2) return;
  const PointF a = touches_[0].second;
  const PointF b = touches_[1].second;
  const float distance = std::max(std::hypot(a.x - b.x, a.y - b.y), 1.0f);
  const PointF center{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
  const float factor = distance / pinch_distance_;
  const float old_scale = view_.scale;
  if (toolbar_->folded()) {
    ZoomCrop(factor, POINT{std::lround(pinch_center_.x), std::lround(pinch_center_.y)}, now);
    PanCrop(center.x - pinch_center_.x, center.y - pinch_center_.y);
  } else {
    const float fit = Fit(doc_.crop, doc_.turns, Area()).scale;
    const float scale = std::clamp(old_scale * factor, fit, std::max(config::kEditorMaxZoom, fit));
    EditorView view{scale, center.x - (pinch_center_.x - view_.x) * scale / old_scale, center.y - (pinch_center_.y - view_.y) * scale / old_scale};
    ApplyView(view, FrameOf(view, doc_.crop, doc_.turns), nullptr, now);
  }
  pinch_distance_ = distance;
  pinch_center_ = center;
}

void Editor::Press(const Contact& contact, bool pan) {
  if (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) return;
  const TimeBase now = compositor_.Now();
  const POINT point{std::lround(contact.x), std::lround(contact.y)};
  press_ = point;
  last_point_ = point;
  moved_ = false;
  SetCapture(window_);

  if (const ChromeAction action = chrome_->HitTest(point); action != ChromeAction::kNone) {
    drag_ = Drag::kChrome;
    pressed_action_ = action;
    chrome_->Press(action, now);
    compositor_.Commit();
    return;
  }
  if (chrome_->Contains(point)) {
    drag_ = Drag::kSwallow;
    return;
  }
  if (capsule_->visible() && capsule_->Contains(point.x, point.y)) {
    drag_ = Drag::kSwallow;
    const int segment = capsule_->HitTest(point.x, point.y);
    if (segment >= 0) ChooseOption(segment, now);
    compositor_.Commit();
    return;
  }
  if (toolbar_->Contains(point)) {
    drag_ = Drag::kToolbar;
    HandleToolbar(toolbar_->Press(point, now), now);
    compositor_.Commit();
    return;
  }
  if (toolbar_->popover_open()) {
    toolbar_->ClosePopover(now);
    drag_ = Drag::kSwallow;
    compositor_.Commit();
    return;
  }
  if (now.seconds < input_from_) {
    drag_ = Drag::kSwallow;
    return;
  }
  if (phase_ == Phase::kChoosing) {
    if (pan) {
      drag_ = Drag::kSwallow;
      return;
    }
    drag_ = Drag::kChoose;
    const RectF picture = PictureOnScreen();
    choose_from_ = POINT{std::lround(std::clamp(contact.x, picture.left, picture.right)), std::lround(std::clamp(contact.y, picture.top, picture.bottom))};
    return;
  }
  if (pan || space_) {
    drag_ = Drag::kPanView;
    UpdateCursor(point);
    return;
  }
  if (const int edges = HandleAt(point); edges != 0) {
    BeginCrop(edges, point, now);
    return;
  }
  if (ruler_on_ && OnRuler(point)) {
    drag_ = Drag::kRuler;
    ruler_grab_ = point;
    return;
  }
  if (toolbar_->folded()) {
    drag_ = Drag::kPanCrop;
    if (!crop_gesture_) {
      crop_before_ = doc_;
      crop_gesture_ = true;
    }
    return;
  }
  if (!canvas_) {
    drag_ = Drag::kSwallow;
    return;
  }
  const Tool tool = contact.eraser ? Tool::kEraser : Tools().tool;
  const PointF plane = ToPlane(contact.x, contact.y);
  if (tool == Tool::kLasso) {
    if (!selection_.empty() && InSelection(plane)) {
      BeginMove(plane);
    } else {
      BeginLasso(plane);
    }
    compositor_.Commit();
    return;
  }
  ClearSelection();
  if (tool == Tool::kEraser && Tools().object_eraser) {
    drag_ = Drag::kObjectErase;
    erase_before_ = doc_;
    erase_last_ = plane;
    EraseAlong(plane, plane);
    compositor_.Commit();
    return;
  }
  if (IsInk(tool) || tool == Tool::kEraser) {
    BeginStroke(tool, contact);
    WantFrames();
  }
}

void Editor::Move(const Contact* contacts, size_t count) {
  if (count == 0) return;
  const Contact& latest = contacts[count - 1];
  const POINT point{std::lround(latest.x), std::lround(latest.y)};
  const TimeBase now = compositor_.Now();
  const LONG slop = std::lround(config::kEditorPressSlop * scale_);
  if (!moved_ && (std::abs(point.x - press_.x) > slop || std::abs(point.y - press_.y) > slop)) moved_ = true;
  const float dx = static_cast<float>(point.x - last_point_.x);
  const float dy = static_cast<float>(point.y - last_point_.y);
  switch (drag_) {
    case Drag::kChrome:
      chrome_->Press(chrome_->HitTest(point) == pressed_action_ ? pressed_action_ : ChromeAction::kNone, now);
      break;
    case Drag::kToolbar:
      HandleToolbar(toolbar_->Drag(point, now), now);
      break;
    case Drag::kChoose:
      if (moved_) {
        const RectF picture = PictureOnScreen();
        const float x = std::clamp(latest.x, picture.left, picture.right);
        const float y = std::clamp(latest.y, picture.top, picture.bottom);
        const float fx = static_cast<float>(choose_from_.x);
        const float fy = static_cast<float>(choose_from_.y);
        ApplyView(view_, RectF{std::min(fx, x), std::min(fy, y), std::max(fx, x), std::max(fy, y)}, nullptr, now);
        outside_alpha_.To(compositor_.device(), now, palette_->outside_alpha, config::kQuickFade);
        line_alpha_.To(compositor_.device(), now, 1.0f, config::kQuickFade);
        choose_hover_ = -2;
      }
      break;
    case Drag::kCrop:
      last_point_ = point;
      DragCrop(point);
      break;
    case Drag::kDraw:
      for (size_t i = 0; i < count; ++i) AddPoint(contacts[i], false);
      live_pending_ = true;
      break;
    case Drag::kObjectErase: {
      const PointF plane = ToPlane(latest.x, latest.y);
      EraseAlong(erase_last_, plane);
      erase_last_ = plane;
      break;
    }
    case Drag::kLasso: {
      const PointF plane = ToPlane(latest.x, latest.y);
      const PointF& last = lasso_path_.back();
      if (std::hypot(plane.x - last.x, plane.y - last.y) * view_.scale >= 2.0f) {
        lasso_path_.push_back(plane);
        RenderLasso();
      }
      break;
    }
    case Drag::kMove: {
      const PointF plane = ToPlane(latest.x, latest.y);
      move_delta_ = POINT{std::lround(plane.x - move_from_.x), std::lround(plane.y - move_from_.y)};
      for (IDCompositionVisual* visual : {inside_float_.Get(), outside_float_.Get()}) {
        visual->SetOffsetX(static_cast<float>(float_bounds_.left + move_delta_.x));
        visual->SetOffsetY(static_cast<float>(float_bounds_.top + move_delta_.y));
      }
      RenderLasso();
      break;
    }
    case Drag::kPanView: {
      EditorView view = view_;
      view.x += dx;
      view.y += dy;
      ApplyView(view, FrameOf(view, doc_.crop, doc_.turns), nullptr, now);
      break;
    }
    case Drag::kPanCrop:
      PanCrop(dx, dy);
      break;
    case Drag::kRuler:
      ruler_center_.x += dx;
      ruler_center_.y += dy;
      PlaceRuler();
      break;
    default:
      break;
  }
  if (drag_ != Drag::kCrop) last_point_ = point;
  compositor_.Commit();
}

void Editor::Release(const Contact& contact) {
  const TimeBase now = compositor_.Now();
  const POINT point{std::lround(contact.x), std::lround(contact.y)};
  const Drag drag = drag_;
  switch (drag) {
    case Drag::kChrome:
      chrome_->Press(ChromeAction::kNone, now);
      drag_ = Drag::kNone;
      if (chrome_->HitTest(point) == pressed_action_) Act(pressed_action_, now);
      break;
    case Drag::kToolbar:
      toolbar_->Release();
      break;
    case Drag::kChoose: {
      drag_ = Drag::kNone;
      const float least = 8.0f * scale_;
      if (moved_ && frame_.width() >= least && frame_.height() >= least) {
        const PointF a = ToPlane(frame_.left, frame_.top);
        const PointF b = ToPlane(frame_.right, frame_.bottom);
        RECT crop{std::lround(std::min(a.x, b.x)), std::lround(std::min(a.y, b.y)), std::lround(std::max(a.x, b.x)), std::lround(std::max(a.y, b.y))};
        crop = Intersect(crop, sources_[static_cast<size_t>(std::max(active_source_, 0))].rect);
        if (!Empty(crop)) EnterEditing(OptionFor(crop, std::max(active_source_, 0)), crop, now);
      } else {
        int option = WindowAt(point);
        if (option < 0) {
          for (size_t i = 1; i < options_.size() && option < 0; ++i) {
            if (options_[i].kind == RegionKind::kFull) option = static_cast<int>(i);
          }
        }
        if (option < 0) option = std::min(1, static_cast<int>(options_.size()) - 1);
        if (option > 0) EnterEditing(option, options_[static_cast<size_t>(option)].crop, now);
      }
      break;
    }
    case Drag::kCrop:
      EndCrop(now);
      break;
    case Drag::kDraw:
      EndStroke(contact, now);
      break;
    case Drag::kObjectErase:
      if (doc_.strokes.size() != erase_before_.strokes.size()) Commit(erase_before_, now);
      break;
    case Drag::kLasso:
      EndLasso(now);
      break;
    case Drag::kMove:
      EndMove(now);
      break;
    case Drag::kPanCrop:
      CommitCropGesture(now);
      break;
    default:
      break;
  }
  drag_ = Drag::kNone;
  if (GetCapture() == window_) ReleaseCapture();
  Hover(point);
  WantFrames();
  compositor_.Commit();
}

void Editor::EndDrag(const TimeBase& now) {
  const Drag drag = drag_;
  drag_ = Drag::kNone;
  switch (drag) {
    case Drag::kChrome:
      chrome_->Press(ChromeAction::kNone, now);
      break;
    case Drag::kToolbar:
      toolbar_->Release();
      break;
    case Drag::kChoose:
      choose_hover_ = -1;
      ApplyView(view_, PictureOnScreen(), &config::kEditorZoom, now);
      outside_alpha_.To(compositor_.device(), now, 0.0f, config::kQuickFade);
      line_alpha_.To(compositor_.device(), now, 0.0f, config::kQuickFade);
      break;
    case Drag::kCrop:
      EndCrop(now);
      break;
    case Drag::kDraw: {
      if (!live_ || live_->points.empty()) {
        CancelStroke();
        break;
      }
      const InkPoint& last = live_->points.back();
      const PointF at = ToScreen(PointF{last.x, last.y});
      Contact contact;
      contact.x = at.x;
      contact.y = at.y;
      contact.pressure = last.pressure;
      contact.time = last.time;
      contact.mouse = live_->mouse;
      EndStroke(contact, now);
      break;
    }
    case Drag::kObjectErase:
      if (doc_.strokes.size() != erase_before_.strokes.size()) Commit(erase_before_, now);
      break;
    case Drag::kLasso:
      EndLasso(now);
      break;
    case Drag::kMove:
      EndMove(now);
      break;
    case Drag::kPanCrop:
      CommitCropGesture(now);
      break;
    default:
      break;
  }
}

void Editor::Hover(POINT point) {
  if (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) return;
  const TimeBase now = compositor_.Now();
  chrome_->Hover(chrome_->HitTest(point), now);
  capsule_->SetHover(capsule_->visible() ? capsule_->HitTest(point.x, point.y) : -1, now);
  toolbar_->Hover(point, now);
  if (phase_ == Phase::kChoosing && now.seconds >= input_from_ && !chrome_->Contains(point) && !capsule_->Contains(point.x, point.y)) {
    PreviewChoice(point, now);
  }
  last_point_ = point;
  UpdateCursor(point);
  compositor_.Commit();
}

void Editor::HandleToolbar(ToolbarEvent event, const TimeBase& now) {
  switch (event) {
    case ToolbarEvent::kTool:
      if (Tools().tool != Tool::kLasso) ClearSelection();
      break;
    case ToolbarEvent::kColor:
      if (!selection_.empty()) {
        const ToolBox& tools = Tools();
        Recolor(tools.inks[static_cast<size_t>(IsInk(tools.tool) ? tools.tool : tools.last_ink)].color, now);
      }
      break;
    case ToolbarEvent::kRuler:
      ShowRuler(toolbar_->ruler(), now);
      break;
    case ToolbarEvent::kFolded:
    case ToolbarEvent::kUnfolded:
      Refit(config::kEditorToolbar, now);
      break;
    default:
      break;
  }
  UpdateCursor(last_point_);
}

void Editor::Act(ChromeAction action, const TimeBase& now) {
  switch (action) {
    case ChromeAction::kClose: Delete(now); break;
    case ChromeAction::kReset: Reset(now); break;
    case ChromeAction::kUndo: Undo(now); break;
    case ChromeAction::kRedo: Redo(now); break;
    case ChromeAction::kRotateRight: Rotate(1, now); break;
    case ChromeAction::kRotateLeft: Rotate(-1, now); break;
    case ChromeAction::kDone: Land(EditorEnd::kKeep, now); break;
    case ChromeAction::kNone: break;
  }
}

HCURSOR Editor::BrushCursor(int diameter) {
  diameter = std::clamp(diameter, static_cast<int>(config::kEditorBrushCursorMin * scale_), 124);
  if (brush_cursor_ != nullptr && diameter == brush_cursor_size_) return brush_cursor_;
  const int size = (diameter + 7) & ~1;
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color == nullptr) return LoadCursorW(nullptr, IDC_CROSS);
  // A white ring on a dark one, which shows on any picture.
  const float center = static_cast<float>(size) * 0.5f;
  const float radius = static_cast<float>(diameter) * 0.5f;
  auto* pixels = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float d = std::hypot(static_cast<float>(x) + 0.5f - center, static_cast<float>(y) + 0.5f - center);
      const float dark = std::clamp(1.6f - std::fabs(d - radius), 0.0f, 1.0f) * 0.55f;
      const float light = std::clamp(0.9f - std::fabs(d - radius), 0.0f, 1.0f);
      const float dot = diameter < 10 ? std::clamp(1.2f - d, 0.0f, 1.0f) : 0.0f;
      const float white = std::max(light, dot);
      const float alpha = white + dark * (1.0f - white);
      const uint8_t a = ToByte(alpha * 255.0f);
      const uint8_t v = ToByte(white * 255.0f);
      pixels[y * size + x] = static_cast<uint32_t>(a) << 24 | static_cast<uint32_t>(v) << 16 | static_cast<uint32_t>(v) << 8 | v;
    }
  }
  std::vector<BYTE> zeros(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
  ICONINFO info{FALSE, static_cast<DWORD>(size / 2), static_cast<DWORD>(size / 2), mask, color};
  HCURSOR cursor = CreateIconIndirect(&info);
  DeleteObject(color);
  DeleteObject(mask);
  if (cursor == nullptr) return LoadCursorW(nullptr, IDC_CROSS);
  // The old one may still be on screen, and goes only once the next one replaces it.
  if (brush_cursor_ != nullptr && brush_cursor_ != cursor_) DestroyCursor(brush_cursor_);
  brush_cursor_ = cursor;
  brush_cursor_size_ = diameter;
  return cursor;
}

void Editor::UpdateCursor(POINT point) {
  HCURSOR cursor = LoadCursorW(nullptr, IDC_ARROW);
  const bool over_controls = chrome_->Contains(point) || capsule_->Contains(point.x, point.y) || toolbar_->Contains(point);
  if ((phase_ == Phase::kEditing || phase_ == Phase::kChoosing) && !over_controls) {
    const int edges = drag_ == Drag::kCrop ? crop_edges_ : (phase_ == Phase::kEditing ? HandleAt(point) : 0);
    if (phase_ == Phase::kChoosing) {
      cursor = LoadCursorW(nullptr, IDC_CROSS);
    } else if (drag_ == Drag::kPanView || drag_ == Drag::kPanCrop || drag_ == Drag::kRuler || space_) {
      cursor = LoadCursorW(nullptr, IDC_SIZEALL);
    } else if (edges != 0) {
      const bool horizontal = (edges & 5) != 0;
      const bool vertical = (edges & 10) != 0;
      if (horizontal && vertical) {
        cursor = LoadCursorW(nullptr, (edges == 3 || edges == 12) ? IDC_SIZENWSE : IDC_SIZENESW);
      } else {
        cursor = LoadCursorW(nullptr, horizontal ? IDC_SIZEWE : IDC_SIZENS);
      }
    } else if (ruler_on_ && OnRuler(point)) {
      cursor = LoadCursorW(nullptr, IDC_SIZEALL);
    } else if (toolbar_->folded()) {
      const float x = static_cast<float>(point.x);
      const float y = static_cast<float>(point.y);
      if (x >= frame_.left && x < frame_.right && y >= frame_.top && y < frame_.bottom) cursor = LoadCursorW(nullptr, IDC_SIZEALL);
    } else {
      const Tool tool = Tools().tool;
      if (tool == Tool::kLasso) {
        const bool inside = !selection_.empty() && InSelection(ToPlane(static_cast<float>(point.x), static_cast<float>(point.y)));
        cursor = LoadCursorW(nullptr, inside ? IDC_SIZEALL : IDC_CROSS);
      } else if (IsInk(tool) || tool == Tool::kEraser) {
        cursor = BrushCursor(static_cast<int>(std::lround(Tools().CurrentWidth() * scale_ * view_.scale)));
      }
    }
  }
  if (cursor == cursor_) return;
  cursor_ = cursor;
  POINT screen{};
  GetCursorPos(&screen);
  if (WindowFromPoint(screen) == window_) SetCursor(cursor_);
}

int Editor::HandleAt(POINT point) const {
  if (phase_ != Phase::kEditing) return 0;
  const float x = static_cast<float>(point.x);
  const float y = static_cast<float>(point.y);
  const float corner = config::kEditorCornerReach * scale_;
  const float edge = config::kEditorEdgeReach * scale_;
  const RectF& f = frame_;
  const struct {
    float x, y;
    int edges;
  } corners[] = {{f.left, f.top, 3}, {f.right, f.top, 6}, {f.left, f.bottom, 9}, {f.right, f.bottom, 12}};
  for (const auto& c : corners) {
    if (std::hypot(x - c.x, y - c.y) <= corner) return c.edges;
  }
  // An edge takes the pointer just outside it anywhere along it, and from inside only at its middle bar.
  const float bar = config::kEditorEdgeArm * scale_ * 0.5f + 8.0f * scale_;
  const float cx = (f.left + f.right) * 0.5f;
  const float cy = (f.top + f.bottom) * 0.5f;
  const bool along_x = x > f.left && x < f.right;
  const bool along_y = y > f.top && y < f.bottom;
  const float inside_x = std::fabs(y - cy) < bar ? edge : 0.0f;
  const float inside_y = std::fabs(x - cx) < bar ? edge : 0.0f;
  if (along_y && x >= f.left - edge && x <= f.left + inside_x) return 1;
  if (along_y && x <= f.right + edge && x >= f.right - inside_x) return 4;
  if (along_x && y >= f.top - edge && y <= f.top + inside_y) return 2;
  if (along_x && y <= f.bottom + edge && y >= f.bottom - inside_y) return 8;
  return 0;
}

void Editor::BeginCrop(int edges, POINT point, const TimeBase& now) {
  drag_ = Drag::kCrop;
  crop_edges_ = edges;
  crop_before_ = doc_;
  crop_turned_ = ToRectF(TurnRect(doc_.crop, doc_.turns));
  refit_at_ = 0.0;
  ClearSelection();
  // The view stops where its springs were heading, which is where the handles are measured from.
  ApplyView(view_, frame_, nullptr, now);
  crop_start_ = crop_turned_;
  crop_view_ = view_;
  crop_frame_ = frame_;
  crop_grab_.x = ((edges & 1) ? frame_.left : (edges & 4) ? frame_.right : static_cast<float>(point.x)) - static_cast<float>(point.x);
  crop_grab_.y = ((edges & 2) ? frame_.top : (edges & 8) ? frame_.bottom : static_cast<float>(point.y)) - static_cast<float>(point.y);
  last_point_ = point;
  UpdateCursor(point);
}

// The handle follows the pointer one for one, anywhere on the screen. Within the picture's space the view stays as it
// was. Drawn on into the margin, the view narrows about the opposite side in step with the handle, so that by the
// screen's edge the whole picture on that side has come within reach. Nothing changes while the pointer rests.
void Editor::DragCrop(POINT point) {
  const RectF area = Area();
  const RECT bound = SourceTurned();
  const float inset = config::kEditorEdgeInset * scale_;
  const float width = static_cast<float>(size_.cx);
  const float height = static_cast<float>(size_.cy);
  const float hx = std::clamp(static_cast<float>(point.x) + crop_grab_.x, inset, width - inset);
  const float hy = std::clamp(static_cast<float>(point.y) + crop_grab_.y, inset, height - inset);
  const float s0 = crop_view_.scale;
  const RectF& c0 = crop_start_;
  const RectF& f0 = crop_frame_;

  // How far into the margin the handle has gone, and how small the view must be by its far end.
  const auto narrowed = [&](float depth, float room, float anchor_screen, float limit, float anchor_turned, float far_turned) {
    const float reach = std::fabs(far_turned - anchor_turned);
    const float smallest = std::fabs(limit - anchor_screen) / std::max(reach, 1.0f);
    if (smallest >= s0 || depth <= 0.0f || room <= 1.0f) return s0;
    const float t = std::clamp(depth / room, 0.0f, 1.0f);
    return s0 * std::pow(smallest / s0, std::pow(t, config::kEditorEdgeEase));
  };
  // The margin starts at the picture's space, or where the handle was taken if that was already past it.
  const float right = std::max(area.right, f0.right);
  const float left = std::min(area.left, f0.left);
  const float bottom = std::max(area.bottom, f0.bottom);
  const float top = std::min(area.top, f0.top);
  float scale = s0;
  if (crop_edges_ & 4) scale = std::min(scale, narrowed(hx - right, width - inset - right, f0.left, width - inset, c0.left, static_cast<float>(bound.right)));
  if (crop_edges_ & 1) scale = std::min(scale, narrowed(left - hx, left - inset, f0.right, inset, c0.right, static_cast<float>(bound.left)));
  if (crop_edges_ & 8) scale = std::min(scale, narrowed(hy - bottom, height - inset - bottom, f0.top, height - inset, c0.top, static_cast<float>(bound.bottom)));
  if (crop_edges_ & 2) scale = std::min(scale, narrowed(top - hy, top - inset, f0.bottom, inset, c0.bottom, static_cast<float>(bound.top)));

  // The opposite side stays where it was on screen, and an axis not being dragged stays centered.
  EditorView view{scale, 0.0f, 0.0f};
  if (crop_edges_ & 4) {
    view.x = f0.left - scale * c0.left;
  } else if (crop_edges_ & 1) {
    view.x = f0.right - scale * c0.right;
  } else {
    view.x = (f0.left + f0.right) * 0.5f - scale * (c0.left + c0.right) * 0.5f;
  }
  if (crop_edges_ & 8) {
    view.y = f0.top - scale * c0.top;
  } else if (crop_edges_ & 2) {
    view.y = f0.bottom - scale * c0.bottom;
  } else {
    view.y = (f0.top + f0.bottom) * 0.5f - scale * (c0.top + c0.bottom) * 0.5f;
  }

  const float least = std::max(config::kEditorMinCrop * scale_ / scale, 4.0f);
  RectF crop = c0;
  if (crop_edges_ & 1) crop.left = std::clamp((hx - view.x) / scale, static_cast<float>(bound.left), std::max(crop.right - least, static_cast<float>(bound.left)));
  if (crop_edges_ & 4) crop.right = std::clamp((hx - view.x) / scale, std::min(crop.left + least, static_cast<float>(bound.right)), static_cast<float>(bound.right));
  if (crop_edges_ & 2) crop.top = std::clamp((hy - view.y) / scale, static_cast<float>(bound.top), std::max(crop.bottom - least, static_cast<float>(bound.top)));
  if (crop_edges_ & 8) crop.bottom = std::clamp((hy - view.y) / scale, std::min(crop.top + least, static_cast<float>(bound.bottom)), static_cast<float>(bound.bottom));
  crop_turned_ = crop;
  const RectF frame{scale * crop.left + view.x, scale * crop.top + view.y, scale * crop.right + view.x, scale * crop.bottom + view.y};
  ApplyView(view, frame, nullptr, compositor_.Now());
}

void Editor::EndCrop(const TimeBase& now) {
  drag_ = Drag::kNone;
  const RECT turned{std::lround(crop_turned_.left), std::lround(crop_turned_.top), std::lround(crop_turned_.right), std::lround(crop_turned_.bottom)};
  RECT crop = Intersect(TurnRect(turned, 4 - doc_.turns), sources_[static_cast<size_t>(std::max(active_source_, 0))].rect);
  if (!Empty(crop) && !Same(crop, doc_.crop)) {
    doc_.crop = crop;
    doc_.option = OptionFor(crop, doc_.source);
    if (doc_.option == 0) {
      custom_crop_ = crop;
      custom_source_ = doc_.source;
    }
    Commit(crop_before_, now);
    capsule_->Select(CapsuleIndex(doc_.option), now);
  }
  ApplyView(view_, FrameOf(view_, doc_.crop, doc_.turns), nullptr, now);
  refit_at_ = now.seconds + config::kEditorRefitDelay;
  WantFrames();
}

void Editor::PanCrop(float dx, float dy) {
  const RECT bound = SourceTurned();
  EditorView view = view_;
  view.x += dx;
  view.y += dy;
  const float s = view.scale;
  if ((frame_.left - view.x) / s < bound.left) view.x = frame_.left - s * bound.left;
  if ((frame_.right - view.x) / s > bound.right) view.x = frame_.right - s * bound.right;
  if ((frame_.top - view.y) / s < bound.top) view.y = frame_.top - s * bound.top;
  if ((frame_.bottom - view.y) / s > bound.bottom) view.y = frame_.bottom - s * bound.bottom;
  const TimeBase now = compositor_.Now();
  ApplyView(view, frame_, nullptr, now);
  crop_gesture_until_ = now.seconds + 0.35;
  WantFrames();
}

void Editor::ZoomCrop(float factor, POINT anchor, const TimeBase& now) {
  if (!crop_gesture_) {
    crop_before_ = doc_;
    crop_gesture_ = true;
  }
  const RECT bound = SourceTurned();
  const float least = std::max(frame_.width() / static_cast<float>(std::max(bound.right - bound.left, 1L)),
                               frame_.height() / static_cast<float>(std::max(bound.bottom - bound.top, 1L)));
  const float scale = std::clamp(view_.scale * factor, least, std::max(config::kEditorMaxZoom, least));
  const float ax = static_cast<float>(anchor.x);
  const float ay = static_cast<float>(anchor.y);
  EditorView view{scale, ax - (ax - view_.x) * scale / view_.scale, ay - (ay - view_.y) * scale / view_.scale};
  if ((frame_.left - view.x) / scale < bound.left) view.x = frame_.left - scale * bound.left;
  if ((frame_.right - view.x) / scale > bound.right) view.x = frame_.right - scale * bound.right;
  if ((frame_.top - view.y) / scale < bound.top) view.y = frame_.top - scale * bound.top;
  if ((frame_.bottom - view.y) / scale > bound.bottom) view.y = frame_.bottom - scale * bound.bottom;
  ApplyView(view, frame_, touches_.size() >= 2 ? nullptr : &config::kEditorZoom, now);
  crop_gesture_until_ = now.seconds + 0.35;
  WantFrames();
}

void Editor::CommitCropGesture(const TimeBase& now) {
  if (!crop_gesture_) return;
  crop_gesture_ = false;
  const RectF turned{(frame_.left - view_.x) / view_.scale, (frame_.top - view_.y) / view_.scale, (frame_.right - view_.x) / view_.scale,
                     (frame_.bottom - view_.y) / view_.scale};
  const RECT rounded{std::lround(turned.left), std::lround(turned.top), std::lround(turned.right), std::lround(turned.bottom)};
  const RECT crop = Intersect(TurnRect(rounded, 4 - doc_.turns), sources_[static_cast<size_t>(std::max(active_source_, 0))].rect);
  if (!Empty(crop) && !Same(crop, doc_.crop)) {
    doc_.crop = crop;
    doc_.option = OptionFor(crop, doc_.source);
    if (doc_.option == 0) {
      custom_crop_ = crop;
      custom_source_ = doc_.source;
    }
    Commit(crop_before_, now);
    capsule_->Select(CapsuleIndex(doc_.option), now);
  }
  ApplyView(view_, FrameOf(view_, doc_.crop, doc_.turns), nullptr, now);
}

void Editor::Zoom(float factor, POINT anchor, const TimeBase& now) {
  const float fit = Fit(doc_.crop, doc_.turns, Area()).scale;
  const float scale = std::clamp(view_.scale * factor, fit, std::max(config::kEditorMaxZoom, fit));
  if (std::fabs(scale - view_.scale) < 1e-5f) return;
  const float ax = static_cast<float>(anchor.x);
  const float ay = static_cast<float>(anchor.y);
  EditorView view{scale, ax - (ax - view_.x) * scale / view_.scale, ay - (ay - view_.y) * scale / view_.scale};
  if (scale <= fit * 1.0001f) view = Fit(doc_.crop, doc_.turns, Area());
  ApplyView(view, FrameOf(view, doc_.crop, doc_.turns), &config::kEditorZoom, now);
}

PointF Editor::SnapToRuler(float x, float y) const {
  if (ruler_edge_ == 0) return PointF{x, y};
  const float c = std::cos(ruler_angle_ * kDegrees);
  const float s = std::sin(ruler_angle_ * kDegrees);
  const float dx = x - ruler_center_.x;
  const float dy = y - ruler_center_.y;
  const float u = dx * c + dy * s;
  const float v = static_cast<float>(ruler_edge_) * (static_cast<float>(ruler_thickness_) * 0.5f + ruler_offset_);
  return PointF{ruler_center_.x + u * c - v * s, ruler_center_.y + u * s + v * c};
}

void Editor::BeginStroke(Tool tool, const Contact& contact) {
  const ToolBox& tools = Tools();
  live_ = std::make_shared<Stroke>();
  Stroke& stroke = *live_;
  stroke.tool = tool;
  if (IsInk(tool)) {
    const ToolBox::Ink& ink = tools.inks[static_cast<size_t>(tool)];
    stroke.color = ink.color;
    stroke.opacity = ink.opacity;
    stroke.width = tools.WidthOf(tool, ink.width) * scale_;
  } else {
    stroke.width = tools.WidthOf(Tool::kEraser, tools.eraser_width) * scale_;
  }
  stroke.scale = scale_;
  stroke.mouse = contact.mouse;
  stroke.seed = Random();
  trail_ = StampTrail{};
  ruler_edge_ = 0;
  if (ruler_on_ && IsInk(tool)) {
    const float c = std::cos(ruler_angle_ * kDegrees);
    const float s = std::sin(ruler_angle_ * kDegrees);
    const float v = -(contact.x - ruler_center_.x) * s + (contact.y - ruler_center_.y) * c;
    const float half = static_cast<float>(ruler_thickness_) * 0.5f;
    const float reach = config::kRulerSnap * scale_;
    const float inside = 6.0f * scale_;
    if (v < -half + inside && v > -half - reach) ruler_edge_ = -1;
    if (v > half - inside && v < half + reach) ruler_edge_ = 1;
    ruler_offset_ = stroke.width * view_.scale * 0.5f + 1.5f * scale_;
  }
  canvas_->Begin();
  smoothed_ = SnapToRuler(contact.x, contact.y);
  const PointF plane = ToPlane(smoothed_.x, smoothed_.y);
  stroke.points.push_back(InkPoint{plane.x, plane.y, contact.pressure, contact.tilt, contact.azimuth, contact.time});
  drag_ = Drag::kDraw;
  live_pending_ = true;
}

void Editor::AddPoint(const Contact& contact, bool last) {
  if (!live_) return;
  const PointF screen = SnapToRuler(contact.x, contact.y);
  if (last) {
    smoothed_ = screen;
  } else {
    const float k = contact.mouse ? config::kSmoothingMouse : config::kSmoothingPen;
    smoothed_.x += (screen.x - smoothed_.x) * k;
    smoothed_.y += (screen.y - smoothed_.y) * k;
  }
  const PointF plane = ToPlane(smoothed_.x, smoothed_.y);
  if (!live_->points.empty()) {
    const InkPoint& previous = live_->points.back();
    if (std::hypot(plane.x - previous.x, plane.y - previous.y) < 0.3f) {
      if (!last) return;
      live_->points.back().pressure = contact.pressure;
      return;
    }
  }
  live_->points.push_back(InkPoint{plane.x, plane.y, contact.pressure, contact.tilt, contact.azimuth, contact.time});
}

void Editor::EndStroke(const Contact& contact, const TimeBase& now) {
  if (!live_ || !canvas_) {
    live_.reset();
    return;
  }
  AddPoint(contact, true);
  const size_t first = LayStamps(*live_, trail_, true);
  if (live_->mouse && (live_->tool == Tool::kPen || live_->tool == Tool::kBrush)) {
    RelayStamps(*live_);
    canvas_->Restroke(*live_);
  } else if (live_->stamps.size() > first) {
    canvas_->Extend(*live_, first);
  }
  canvas_->End();
  const EditDocument before = doc_;
  doc_.strokes.push_back(std::move(live_));
  live_.reset();
  live_pending_ = false;
  ruler_edge_ = 0;
  Commit(before, now);
  FlushCanvas();
}

void Editor::CancelStroke() {
  if (canvas_ && live_) canvas_->Cancel();
  live_.reset();
  live_pending_ = false;
  ruler_edge_ = 0;
  drag_ = Drag::kNone;
  FlushCanvas();
}

void Editor::EraseAlong(PointF from, PointF to) {
  const float reach = Tools().WidthOf(Tool::kEraser, Tools().eraser_width) * scale_ * 0.5f;
  const float length = std::hypot(to.x - from.x, to.y - from.y);
  const int steps = std::max(1, static_cast<int>(std::ceil(length / std::max(reach * 0.5f, 1.0f))));
  RECT dirty{};
  for (int i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / steps;
    const float x = from.x + (to.x - from.x) * t;
    const float y = from.y + (to.y - from.y) * t;
    for (size_t k = doc_.strokes.size(); k-- > 0;) {
      const Stroke& stroke = *doc_.strokes[k];
      if (stroke.tool == Tool::kEraser || !Touches(stroke, x, y, reach)) continue;
      dirty = Union(dirty, stroke.bounds);
      doc_.strokes.erase(doc_.strokes.begin() + static_cast<ptrdiff_t>(k));
    }
  }
  if (!Empty(dirty) && canvas_) {
    canvas_->Redraw(doc_.strokes, dirty);
    FlushCanvas();
  }
}

void Editor::BeginLasso(PointF plane) {
  ClearSelection();
  lasso_path_.push_back(plane);
  drag_ = Drag::kLasso;
}

void Editor::EndLasso(const TimeBase& now) {
  drag_ = Drag::kNone;
  if (lasso_path_.size() < 3) {
    ClearSelection();
    return;
  }
  selection_.clear();
  for (size_t i = 0; i < doc_.strokes.size(); ++i) {
    const Stroke& stroke = *doc_.strokes[i];
    if (!IsInk(stroke.tool) || stroke.points.empty()) continue;
    size_t inside = 0;
    for (const InkPoint& point : stroke.points) {
      if (PointInPolygon(lasso_path_, PointF{point.x, point.y})) ++inside;
    }
    if (static_cast<float>(inside) >= config::kLassoShare * static_cast<float>(stroke.points.size())) selection_.push_back(i);
  }
  if (selection_.empty()) {
    ClearSelection();
    return;
  }
  RenderLasso();
  (void)now;
}

void Editor::ClearSelection() {
  selection_.clear();
  lasso_path_.clear();
  move_delta_ = POINT{};
  if (lasso_) lasso_->SetContent(nullptr);
}

bool Editor::InSelection(PointF plane) const {
  if (lasso_path_.size() < 3) return false;
  return PointInPolygon(lasso_path_, PointF{plane.x - static_cast<float>(move_delta_.x), plane.y - static_cast<float>(move_delta_.y)});
}

RECT Editor::SelectionBounds() const {
  RECT bounds{};
  for (const size_t index : selection_) {
    if (index < doc_.strokes.size()) bounds = Union(bounds, doc_.strokes[index]->bounds);
  }
  return bounds;
}

void Editor::BeginMove(PointF plane) {
  drag_ = Drag::kMove;
  move_from_ = plane;
  move_delta_ = POINT{};
  float_bounds_ = SelectionBounds();
  const int width = float_bounds_.right - float_bounds_.left;
  const int height = float_bounds_.bottom - float_bounds_.top;
  if (width <= 0 || height <= 0 || !canvas_) {
    drag_ = Drag::kNone;
    return;
  }
  // The strokes lift off as one floating picture, and the canvas is drawn without them until they are put down.
  Image floating = Image::Allocate(width, height);
  if (floating) {
    std::fill_n(reinterpret_cast<uint32_t*>(floating.data()), static_cast<size_t>(width) * height, 0u);
    std::vector<float> mask(static_cast<size_t>(width) * height);
    for (const size_t index : selection_) {
      const Stroke& stroke = *doc_.strokes[index];
      std::fill(mask.begin(), mask.end(), 0.0f);
      DrawStroke(stroke, float_bounds_, mask.data(), width);
      PaintStroke(stroke, mask.data(), width, reinterpret_cast<uint32_t*>(floating.data()), width, width, height);
    }
    ComPtr<IDCompositionSurface> surface = compositor_.CreateSurface(floating, false);
    for (IDCompositionVisual* visual : {inside_float_.Get(), outside_float_.Get()}) {
      visual->SetContent(surface.Get());
      visual->SetOffsetX(static_cast<float>(float_bounds_.left));
      visual->SetOffsetY(static_cast<float>(float_bounds_.top));
    }
  }
  StrokeList rest;
  for (size_t i = 0; i < doc_.strokes.size(); ++i) {
    if (std::find(selection_.begin(), selection_.end(), i) == selection_.end()) rest.push_back(doc_.strokes[i]);
  }
  canvas_->Redraw(rest, float_bounds_);
  FlushCanvas();
}

void Editor::EndMove(const TimeBase& now) {
  drag_ = Drag::kNone;
  inside_float_->SetContent(nullptr);
  outside_float_->SetContent(nullptr);
  if (!canvas_) return;
  const POINT delta = move_delta_;
  move_delta_ = POINT{};
  if (delta.x == 0 && delta.y == 0) {
    canvas_->Redraw(doc_.strokes, float_bounds_);
    FlushCanvas();
    RenderLasso();
    return;
  }
  const EditDocument before = doc_;
  const float fx = static_cast<float>(delta.x);
  const float fy = static_cast<float>(delta.y);
  for (const size_t index : selection_) {
    auto moved = std::make_shared<Stroke>(*doc_.strokes[index]);
    for (InkPoint& point : moved->points) {
      point.x += fx;
      point.y += fy;
    }
    for (Stamp& stamp : moved->stamps) {
      stamp.x += fx;
      stamp.y += fy;
    }
    OffsetRect(&moved->bounds, delta.x, delta.y);
    doc_.strokes[index] = std::move(moved);
  }
  for (PointF& point : lasso_path_) {
    point.x += fx;
    point.y += fy;
  }
  RECT moved_bounds = float_bounds_;
  OffsetRect(&moved_bounds, delta.x, delta.y);
  canvas_->Redraw(doc_.strokes, Union(float_bounds_, moved_bounds));
  FlushCanvas();
  Commit(before, now);
  RenderLasso();
}

void Editor::RenderLasso() {
  if (lasso_path_.empty()) {
    lasso_->SetContent(nullptr);
    return;
  }
  std::vector<PointF> points;
  points.reserve(lasso_path_.size() + 1);
  for (const PointF& plane : lasso_path_) {
    points.push_back(ToScreen(PointF{plane.x + static_cast<float>(move_delta_.x), plane.y + static_cast<float>(move_delta_.y)}));
  }
  const bool closed = !selection_.empty();
  if (closed) points.push_back(points.front());
  float left = points[0].x;
  float top = points[0].y;
  float right = left;
  float bottom = top;
  for (const PointF& point : points) {
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  }
  const float pad = 4.0f * scale_;
  const RECT box = Intersect(Outward(RectF{left - pad, top - pad, right + pad, bottom + pad}), RECT{0, 0, size_.cx, size_.cy});
  const int width = box.right - box.left;
  const int height = box.bottom - box.top;
  if (width <= 0 || height <= 0) {
    lasso_->SetContent(nullptr);
    return;
  }
  std::vector<float> line(static_cast<size_t>(width) * height, 0.0f);
  std::vector<float> halo(static_cast<size_t>(width) * height, 0.0f);
  const float dash = 6.0f * scale_;
  const float gap = 4.0f * scale_;
  const float core = std::max(0.8f * scale_, 0.8f);
  const float rim = core + 1.2f * scale_;
  float travelled = 0.0f;
  for (size_t i = 0; i + 1 < points.size(); ++i) {
    const PointF a{points[i].x - box.left, points[i].y - box.top};
    const PointF b{points[i + 1].x - box.left, points[i + 1].y - box.top};
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    const int x0 = std::max(static_cast<int>(std::floor(std::min(a.x, b.x) - rim - 1.0f)), 0);
    const int x1 = std::min(static_cast<int>(std::ceil(std::max(a.x, b.x) + rim + 1.0f)), width);
    const int y0 = std::max(static_cast<int>(std::floor(std::min(a.y, b.y) - rim - 1.0f)), 0);
    const int y1 = std::min(static_cast<int>(std::ceil(std::max(a.y, b.y) + rim + 1.0f)), height);
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        float along = 0.0f;
        const float d = SegmentDistance(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, a.x, a.y, b.x, b.y, &along);
        const size_t at = static_cast<size_t>(y) * width + x;
        halo[at] = std::max(halo[at], std::clamp(rim - d + 0.5f, 0.0f, 1.0f));
        const bool on = std::fmod(travelled + along * length, dash + gap) < dash;
        if (on) line[at] = std::max(line[at], std::clamp(core - d + 0.5f, 0.0f, 1.0f));
      }
    }
    travelled += length;
  }
  Image image = Image::Allocate(width, height);
  if (!image) return;
  const uint32_t ink = palette_->frame;
  const uint32_t shade = palette_->halo;
  auto* pixels = reinterpret_cast<uint32_t*>(image.data());
  for (size_t i = 0; i < line.size(); ++i) {
    const float h = halo[i] * palette_->halo_alpha;
    const float l = line[i];
    const float alpha = l + h * (1.0f - l);
    uint32_t out = static_cast<uint32_t>(ToByte(alpha * 255.0f)) << 24;
    for (int c = 0; c < 3; ++c) {
      const float value = static_cast<float>((ink >> (c * 8)) & 0xFF) * l + static_cast<float>((shade >> (c * 8)) & 0xFF) * h * (1.0f - l);
      out |= static_cast<uint32_t>(ToByte(value)) << (c * 8);
    }
    pixels[i] = out;
  }
  lasso_->SetContent(compositor_.CreateSurface(image, false).Get());
  lasso_->SetOffsetX(static_cast<float>(box.left));
  lasso_->SetOffsetY(static_cast<float>(box.top));
}

void Editor::Recolor(uint32_t color, const TimeBase& now) {
  if (selection_.empty() || !canvas_) return;
  const EditDocument before = doc_;
  for (const size_t index : selection_) {
    auto recolored = std::make_shared<Stroke>(*doc_.strokes[index]);
    recolored->color = color;
    doc_.strokes[index] = std::move(recolored);
  }
  canvas_->Redraw(doc_.strokes, SelectionBounds());
  FlushCanvas();
  Commit(before, now);
}

void Editor::DeleteSelection(const TimeBase& now) {
  if (selection_.empty() || !canvas_) return;
  const EditDocument before = doc_;
  const RECT dirty = SelectionBounds();
  std::vector<size_t> doomed = selection_;
  std::sort(doomed.begin(), doomed.end());
  for (size_t i = doomed.size(); i-- > 0;) doc_.strokes.erase(doc_.strokes.begin() + static_cast<ptrdiff_t>(doomed[i]));
  ClearSelection();
  canvas_->Redraw(doc_.strokes, dirty);
  FlushCanvas();
  Commit(before, now);
}

void Editor::ShowRuler(bool show, const TimeBase& now) {
  if (show && phase_ != Phase::kEditing) show = false;
  ruler_on_ = show;
  if (show && ruler_length_ == 0) {
    ruler_length_ = static_cast<int>(std::ceil(std::hypot(static_cast<float>(size_.cx), static_cast<float>(size_.cy)) * 2.0f));
    ruler_thickness_ = static_cast<int>(std::round(config::kRulerWidth * scale_));
    ruler_band_->SetContent(compositor_.CreateSurface(RenderRuler(ruler_length_, ruler_thickness_, scale_), false).Get());
    const RectF area = Area();
    ruler_center_ = PointF{(area.left + area.right) * 0.5f, (area.top + area.bottom) * 0.5f};
  }
  if (show) PlaceRuler();
  ruler_alpha_.To(compositor_.device(), now, show ? 1.0f : 0.0f, config::kEditorFade);
}

void Editor::PlaceRuler() {
  if (ruler_length_ == 0) return;
  const float c = std::cos(ruler_angle_ * kDegrees);
  const float s = std::sin(ruler_angle_ * kDegrees);
  const float half_l = static_cast<float>(ruler_length_) * 0.5f;
  const float half_t = static_cast<float>(ruler_thickness_) * 0.5f;
  ruler_band_->SetTransform(D2D_MATRIX_3X2_F{c, s, -s, c, ruler_center_.x - (half_l * c - half_t * s), ruler_center_.y - (half_l * s + half_t * c)});
  wchar_t text[16];
  float shown = std::fmod(std::fabs(ruler_angle_), 180.0f);
  if (shown > 90.0f) shown = 180.0f - shown;
  swprintf_s(text, L"%d°", static_cast<int>(std::lround(shown)));
  const TextLine label = RenderText(text, static_cast<int>(std::lround(config::kRulerFontSize * scale_)), config::kRulerInk);
  const RECT ink = InkBounds(label.image);
  ruler_label_->SetContent(compositor_.CreateSurface(label.image, false).Get());
  ruler_label_->SetOffsetX(std::round(ruler_center_.x - static_cast<float>(ink.left + ink.right) * 0.5f));
  ruler_label_->SetOffsetY(std::round(ruler_center_.y - static_cast<float>(ink.top + ink.bottom) * 0.5f));
}

bool Editor::OnRuler(POINT point) const {
  if (!ruler_on_ || ruler_length_ == 0) return false;
  const float c = std::cos(ruler_angle_ * kDegrees);
  const float s = std::sin(ruler_angle_ * kDegrees);
  const float dx = static_cast<float>(point.x) - ruler_center_.x;
  const float dy = static_cast<float>(point.y) - ruler_center_.y;
  const float u = dx * c + dy * s;
  const float v = -dx * s + dy * c;
  return std::fabs(v) <= static_cast<float>(ruler_thickness_) * 0.5f && std::fabs(u) <= static_cast<float>(ruler_length_) * 0.5f;
}

void Editor::TurnRuler(float degrees) {
  float angle = ruler_angle_ + degrees;
  while (angle > 180.0f) angle -= 360.0f;
  while (angle <= -180.0f) angle += 360.0f;
  const float nearest = std::round(angle / 45.0f) * 45.0f;
  if (std::fabs(angle - nearest) < config::kRulerSnapDegrees && std::fabs(degrees) > config::kRulerSnapDegrees * 0.5f) angle = nearest;
  ruler_angle_ = angle;
  PlaceRuler();
}

void Editor::OnKey(UINT key, LPARAM flags) {
  if (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) return;
  const bool down = (flags & kEditorKeyDown) != 0;
  const bool control = (flags & kEditorKeyControl) != 0;
  const bool shift = (flags & kEditorKeyShift) != 0;
  if (key == VK_SPACE) {
    space_ = down;
    UpdateCursor(last_point_);
    return;
  }
  if (!down) return;
  const TimeBase now = compositor_.Now();
  const RectF area = Area();
  const POINT middle{std::lround((area.left + area.right) * 0.5f), std::lround((area.top + area.bottom) * 0.5f)};
  switch (key) {
    case VK_ESCAPE:
      if (toolbar_->popover_open()) {
        toolbar_->ClosePopover(now);
      } else if (drag_ == Drag::kDraw) {
        CancelStroke();
        if (GetCapture() == window_) ReleaseCapture();
      } else if (!selection_.empty()) {
        ClearSelection();
      } else if (phase_ == Phase::kChoosing) {
        if (choosing_again_) {
          const EditDocument back = before_choosing_;
          choosing_again_ = false;
          phase_ = Phase::kEditing;
          ApplyDocument(back, now);
          Refit(config::kEditorView, now);
          outside_alpha_.To(compositor_.device(), now, palette_->outside_alpha, config::kEditorFade);
          line_alpha_.To(compositor_.device(), now, 1.0f, config::kEditorFade);
          handles_alpha_.To(compositor_.device(), now, 1.0f, config::kEditorFade);
          toolbar_->Show(now, 0.0);
        } else {
          Delete(now);
        }
      } else {
        Land(EditorEnd::kKeep, now);
      }
      break;
    case VK_RETURN:
      if (phase_ == Phase::kEditing) Land(EditorEnd::kKeep, now);
      break;
    case 'Z':
      if (control && phase_ == Phase::kEditing) {
        if (shift) {
          Redo(now);
        } else {
          Undo(now);
        }
      }
      break;
    case 'Y':
      if (control && phase_ == Phase::kEditing) Redo(now);
      break;
    case VK_OEM_PLUS:
    case VK_ADD:
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
      if (control && phase_ == Phase::kEditing) {
        const bool in = key == VK_OEM_PLUS || key == VK_ADD;
        const float factor = static_cast<float>(in ? config::kEditorZoomStep : 1.0 / config::kEditorZoomStep);
        if (toolbar_->folded()) {
          ZoomCrop(factor, POINT{std::lround((frame_.left + frame_.right) * 0.5f), std::lround((frame_.top + frame_.bottom) * 0.5f)}, now);
        } else {
          Zoom(factor, middle, now);
        }
      }
      break;
    case '0':
    case VK_NUMPAD0:
      if (control) Refit(config::kEditorZoom, now);
      break;
    case VK_DELETE:
    case VK_BACK:
      DeleteSelection(now);
      break;
    default:
      break;
  }
  UpdateCursor(last_point_);
  compositor_.Commit();
}

void Editor::OnWheel(POINT screen, int delta, LPARAM flags) {
  if (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) return;
  const POINT point{screen.x - bounds_.left, screen.y - bounds_.top};
  if (point.x < 0 || point.y < 0 || point.x >= size_.cx || point.y >= size_.cy) return;
  if (chrome_->Contains(point) || toolbar_->Contains(point) || capsule_->Contains(point.x, point.y)) return;
  if (drag_ != Drag::kNone && drag_ != Drag::kPanView) return;
  const TimeBase now = compositor_.Now();
  const bool shift = (flags & kEditorKeyShift) != 0;
  if (ruler_on_ && OnRuler(point)) {
    const float step = shift ? 1.0f : config::kRulerStepDegrees;
    TurnRuler(delta > 0 ? -step : step);
    compositor_.Commit();
    return;
  }
  if (phase_ != Phase::kEditing) return;
  if ((flags & kEditorWheelHorizontal) != 0 || shift) {
    const float amount = static_cast<float>(delta) / 120.0f * 48.0f * scale_ * ((flags & kEditorWheelHorizontal) != 0 ? -1.0f : 1.0f);
    if (toolbar_->folded()) {
      if (!crop_gesture_) {
        crop_before_ = doc_;
        crop_gesture_ = true;
      }
      PanCrop(amount, 0.0f);
    } else {
      EditorView view = view_;
      view.x += amount;
      ApplyView(view, FrameOf(view, doc_.crop, doc_.turns), &config::kEditorZoom, now);
    }
    compositor_.Commit();
    return;
  }
  const float factor = static_cast<float>(std::pow(config::kEditorZoomStep, static_cast<double>(delta) / 120.0));
  if (toolbar_->folded()) {
    ZoomCrop(factor, point, now);
  } else {
    Zoom(factor, point, now);
  }
  UpdateCursor(point);
  compositor_.Commit();
}

void Editor::ThumbnailReady() {
  thumbnail_ready_ = true;
  WantFrames();
}

void Editor::AttachWindows(WindowShots& shots) {
  if (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) return;
  const TimeBase now = compositor_.Now();
  bool changed = false;
  for (WindowShot& shot : shots.shots) {
    if (shot.image == nullptr) continue;
    for (size_t i = 1; i < options_.size(); ++i) {
      Option& option = options_[i];
      const bool own = option.kind == RegionKind::kApp || option.kind == RegionKind::kZone || option.kind == RegionKind::kScroll;
      if (!own || option.window != shot.window) continue;
      option.source = AddSource(nullptr, shot.image, shot.rect);
      option.crop = sources_[static_cast<size_t>(option.source)].rect;
      option.glyph = shot.rect;
      changed = true;
      // Still untouched, the window's own picture takes the place of what the screen showed of it.
      if (phase_ == Phase::kEditing && doc_.option == static_cast<int>(i) && doc_.strokes.empty() && doc_.turns == 0 && undo_.empty()) {
        EditDocument target = doc_;
        target.source = option.source;
        target.crop = option.crop;
        ApplyDocument(target, now);
        original_ = doc_;
      }
    }
    window_shots_.push_back(std::move(shot));
  }
  if (changed) BindCapsule(now);
  compositor_.Commit();
}

void Editor::OfferScroll(const ScrollHints& hints) {
  hints_ = hints;
  if (scroll_offered_ || (phase_ != Phase::kEditing && phase_ != Phase::kChoosing)) return;
  int source_option = -1;
  for (size_t i = 1; i < options_.size() && source_option < 0; ++i) {
    if (app_ != nullptr && options_[i].window == app_) source_option = static_cast<int>(i);
  }
  for (size_t i = 1; i < options_.size() && source_option < 0; ++i) {
    const RECT& rect = options_[i].glyph;
    if (options_[i].window == nullptr && std::abs(rect.left - app_rect_.left) <= config::kSameRectTolerance &&
        std::abs(rect.top - app_rect_.top) <= config::kSameRectTolerance && std::abs(rect.right - app_rect_.right) <= config::kSameRectTolerance &&
        std::abs(rect.bottom - app_rect_.bottom) <= config::kSameRectTolerance) {
      source_option = static_cast<int>(i);
    }
  }
  if (source_option < 0) return;
  Option scroll = options_[static_cast<size_t>(source_option)];
  scroll.kind = RegionKind::kScroll;
  scroll.region = -1;
  options_.push_back(scroll);
  scroll_offered_ = true;
  BindCapsule(compositor_.Now());
  compositor_.Commit();
}

void Editor::Land(EditorEnd end, const TimeBase& now) {
  if (phase_ != Phase::kEditing && !(phase_ == Phase::kChoosing && end == EditorEnd::kScroll)) return;
  if (drag_ != Drag::kNone) EndDrag(now);
  if (GetCapture() == window_) ReleaseCapture();
  if (crop_gesture_) CommitCropGesture(now);
  ClearSelection();
  IDCompositionDevice* dc = compositor_.device();

  EditorOutcome outcome;
  outcome.end = end;
  if (end == EditorEnd::kScroll) {
    int index = -1;
    for (size_t i = 1; i < options_.size(); ++i) {
      if (options_[i].kind == RegionKind::kScroll) index = static_cast<int>(i);
    }
    if (index < 0) return;
    const Option& scroll = options_[static_cast<size_t>(index)];
    outcome.region = scroll.region;
    doc_.crop = scroll.crop;
    doc_.turns = 0;
    SetTurns(0);
    if (scroll.source != active_source_ && LoadSource(scroll.source)) {
      MakeCanvas(scroll.source);
      ShowSource(scroll.source, now, false);
    }
    doc_.source = scroll.source;
  } else {
    const Option& option = options_[static_cast<size_t>(doc_.option)];
    const bool edited = !doc_.strokes.empty() || doc_.turns != 0 || doc_.option == 0 || option.source != doc_.source || !Same(doc_.crop, option.crop);
    if (edited) {
      outcome.document = std::make_shared<EditDocument>(doc_);
      const RECT turned = TurnRect(doc_.crop, doc_.turns);
      outcome.size = SIZE{turned.right - turned.left, turned.bottom - turned.top};
    } else {
      outcome.region = option.region;
    }
  }
  phase_ = Phase::kLanding;
  const EditorLanding landing = host_.EditorLand(*this, outcome);
  const RectF view = outcome.document ? ToRectF(TurnRect(doc_.crop, doc_.turns)) : landing.view;
  const float s = landing.rect.width() / std::max(view.width(), 1.0f);
  ApplyView(EditorView{s, landing.rect.left - s * view.left, landing.rect.top - s * view.top}, landing.rect, &config::kEditorLand, now);
  const float border = std::max(std::round(config::kCardBorder * scale_), 1.0f);
  const float radius = config::kCardRadius * scale_;
  radius_.To(dc, now, radius, config::kEditorLand);
  ApplyInner(Inset(landing.rect, border), radius - border, &config::kEditorLand, now);
  ring_alpha_.To(dc, now, 1.0f, config::kEditorFade);
  line_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  handles_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  outside_alpha_.To(dc, now, 0.0f, config::kEditorFade);
  body_alpha_.To(dc, now, 0.0f, config::kEditorFade, 0.06);
  HideSharp();
  sharpen_at_ = 0.0;
  refit_at_ = 0.0;
  chrome_->Hide(now);
  toolbar_->Hide(now);
  capsule_->Hide(now);
  ShowRuler(false, now);
  for (Shield& shield : shields_) shield.alpha.To(dc, now, 0.0f, config::kEditorFade);
  arrive_at_ = view_until_;
  outcome_ = outcome;
  thumbnail_ready_ = outcome.document == nullptr;
  if (outcome.document && canvas_) {
    ExportJob job;
    job.version = landing.version;
    job.shot = shot_;
    job.canvas = canvas_->Snapshot();
    job.turns = doc_.turns;
    job.crop = doc_.crop;
    job.thumbnail = ThumbnailSize(RECT{0, 0, outcome.size.cx, outcome.size.cy}, scale_);
    jobs_.Export(std::move(job));
  }
  UpdateCursor(last_point_);
  WantFrames();
}

Image Editor::Snapshot(const RectF& frame) {
  const RECT screen = Intersect(Outward(frame), RECT{0, 0, size_.cx, size_.cy});
  if (!canvas_ || Empty(screen)) return Image();
  return Sharpen(canvas_->Snapshot(), doc_.turns, view_, screen);
}

void Editor::Delete(const TimeBase& now) {
  if (phase_ != Phase::kEditing && phase_ != Phase::kChoosing) return;
  if (drag_ == Drag::kDraw) CancelStroke();
  if (drag_ != Drag::kNone) EndDrag(now);
  if (GetCapture() == window_) ReleaseCapture();
  crop_gesture_ = false;
  ClearSelection();
  IDCompositionDevice* dc = compositor_.device();
  const RectF frame = phase_ == Phase::kChoosing ? PictureOnScreen() : frame_;
  phase_ = Phase::kDeleting;
  HideSharp();
  sharpen_at_ = 0.0;
  refit_at_ = 0.0;
  chrome_->Hide(now);
  toolbar_->Hide(now);
  capsule_->Hide(now);
  ShowRuler(false, now);

  // The picture as it shows becomes a sprite, which the bin draws in as it does a thumbnail.
  const Image picture = Snapshot(frame);
  const RECT shown = Intersect(Outward(frame), RECT{0, 0, size_.cx, size_.cy});
  bin_ = std::make_unique<RubbishBin>(compositor_, bin_layer_.Get(), scale_);
  bin_->Place(static_cast<float>(size_.cx) * 0.5f, static_cast<float>(size_.cy));
  bin_->Show();
  if (picture) {
    sprite_ = std::make_unique<DragSprite>(compositor_, picture, static_cast<float>(picture.width()) * 0.5f, static_cast<float>(picture.height()) * 0.5f,
                                           scale_, 0.0f, 0.0f);
    sprite_->AddView(bin_layer_.Get(), bounds_, bounds_);
    sprite_height_ = static_cast<float>(picture.height());
    bin_x_.Snap(static_cast<float>(bounds_.left) + static_cast<float>(shown.left + shown.right) * 0.5f);
    bin_y_.Snap(static_cast<float>(bounds_.top) + static_cast<float>(shown.top + shown.bottom) * 0.5f);
    bin_scale_.Snap(1.0f);
    bin_scale_.target = std::min(config::kEditorBinScale * static_cast<float>(size_.cy) / std::max(sprite_height_, 1.0f), 1.0f);
    bin_pull_.Snap(0.0f);
    photo_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  } else {
    photo_alpha_.To(dc, now, 0.0f, config::kEditorFade);
  }
  body_alpha_.To(dc, now, 0.0f, config::kEditorFade, 0.12);
  for (Shield& shield : shields_) shield.alpha.To(dc, now, 0.0f, config::kEditorFade);
  delete_started_ = now.seconds;
  gulped_ = false;
  StepDelete(0.0f);
  UpdateCursor(last_point_);
  WantFrames();
}

void Editor::StepDelete(float dt) {
  const double elapsed = compositor_.Now().seconds - delete_started_;
  const float mouth_x = static_cast<float>(bounds_.left) + bin_->mouth_x();
  const float mouth_y = static_cast<float>(bounds_.top) + bin_->mouth_y();
  if (sprite_) {
    // Drawn toward a point above the bin while it shrinks, and then into it.
    const float hover = config::kEditorBinScale * static_cast<float>(size_.cy) * 0.5f + 34.0f * scale_;
    bin_x_.target = mouth_x;
    bin_y_.target = mouth_y - hover;
    bin_x_.Step(config::kEditorToBin, dt);
    bin_y_.Step(config::kEditorToBin, dt);
    bin_scale_.Step(config::kEditorToBin, dt);
    if (elapsed >= config::kEditorBinPullDelay) {
      bin_pull_.target = 1.0f;
      bin_pull_.Step(config::kBinSwallow, dt);
    }
    if (!gulped_) {
      bin_->Track(POINT{std::lround(bin_->mouth_x()), std::lround(bin_->mouth_y())});
      bin_->Aim(bin_x_.value - static_cast<float>(bounds_.left), bin_y_.value - static_cast<float>(bounds_.top));
    }
    const float pull = std::clamp(bin_pull_.value, 0.0f, 1.0f);
    if (!gulped_ && pull >= config::kBinGulpAt) {
      gulped_ = true;
      bin_->Gulp(true);
    }
    SpritePose pose{};
    pose.x = bin_x_.value;
    pose.y = bin_y_.value;
    pose.scale = bin_scale_.value;
    pose.pull = pull;
    pose.pull_x = mouth_x;
    pose.pull_y = mouth_y;
    pose.opacity = std::clamp((1.0f - pull) / config::kBinHideMargin, 0.0f, 1.0f);
    pose.shadow = 0.75f * std::clamp(static_cast<float>(elapsed) / 0.15f, 0.0f, 1.0f) * std::clamp(1.0f - pull * 3.0f, 0.0f, 1.0f);
    pose.shadow_free = 1.0f;
    sprite_->Render(pose);
  } else if (!gulped_ && elapsed >= 0.2) {
    gulped_ = true;
    bin_->Gulp(true);
  }
  const bool moving = bin_->Frame(dt);
  if (gulped_ && (!sprite_ || bin_pull_.value > 0.99f) && !moving) {
    host_.EditorDeleted(*this);
    sprite_.reset();
    Vanish();
  }
}

void Editor::Vanish() {
  phase_ = Phase::kGone;
  vanish_frames_ = 2;
  WantFrames();
}

void Editor::Abort() {
  pacer_.Stop();
  if (GetCapture() == window_) ReleaseCapture();
  if (window_ != nullptr) {
    KillTimer(window_, kBeatTimer);
    ShowWindow(window_, SW_HIDE);
  }
  for (Shield& shield : shields_) {
    if (shield.window != nullptr) ShowWindow(shield.window, SW_HIDE);
  }
  phase_ = Phase::kGone;
  vanish_frames_ = 0;
}

void Editor::WantFrames() {
  if (window_ == nullptr || pacer_.running()) return;
  last_frame_ = compositor_.Now().seconds;
  pacer_.Start(window_, kFrameMessage);
}

bool Editor::NeedsFrames() const {
  return vanish_frames_ > 0 || covering_frames_ > 0 || live_ != nullptr || crop_gesture_ || refit_at_ > 0.0 || sharpen_at_ > 0.0 ||
         phase_ == Phase::kDeleting || phase_ == Phase::kLanding;
}

void Editor::OnFrame() {
  pacer_.Acknowledge();
  const TimeBase now = compositor_.Now();
  const float dt = std::clamp(static_cast<float>(now.seconds - last_frame_), 0.0f, config::kMaxFrameTime);
  last_frame_ = now.seconds;

  if (vanish_frames_ > 0) {
    // The card took the picture's place in the last frame. The panel goes in the next two: first its pixels, then its windows.
    if (vanish_frames_ == 2) {
      root_alpha_.Snap(0.0f);
      for (Shield& shield : shields_) shield.alpha.Snap(0.0f);
    } else {
      KillTimer(window_, kBeatTimer);
      ShowWindow(window_, SW_HIDE);
      for (Shield& shield : shields_) {
        if (shield.window != nullptr) ShowWindow(shield.window, SW_HIDE);
      }
    }
    --vanish_frames_;
    compositor_.Commit();
    if (vanish_frames_ == 0) pacer_.Stop();
    return;
  }
  if (phase_ == Phase::kGone) {
    pacer_.Stop();
    return;
  }

  if (covering_frames_ > 0 && --covering_frames_ == 0) host_.EditorCovering(*this);
  if (live_ && live_pending_ && canvas_) {
    const size_t first = LayStamps(*live_, trail_, false);
    if (live_->stamps.size() > first) canvas_->Extend(*live_, first);
    live_pending_ = false;
  }
  if (crop_gesture_ && drag_ != Drag::kPanCrop && touches_.size() < 2 && now.seconds >= crop_gesture_until_) CommitCropGesture(now);
  if (refit_at_ > 0.0 && now.seconds >= refit_at_ && drag_ == Drag::kNone) {
    refit_at_ = 0.0;
    Refit(config::kEditorView, now);
  }
  if (sharpen_at_ > 0.0 && now.seconds >= sharpen_at_) {
    const bool still = drag_ != Drag::kCrop && drag_ != Drag::kPanView && drag_ != Drag::kPanCrop && !crop_gesture_ && touches_.size() < 2;
    if (still) {
      sharpen_at_ = 0.0;
      RequestSharp();
    }
  }
  if (phase_ == Phase::kDeleting && bin_) StepDelete(dt);
  if (phase_ == Phase::kLanding && now.seconds >= arrive_at_ && thumbnail_ready_) {
    host_.EditorArrived(*this, outcome_);
    phase_ = Phase::kGone;
    vanish_frames_ = 2;
  }
  FlushCanvas();
  compositor_.Commit();
  if (!NeedsFrames()) pacer_.Stop();
}

}  // namespace sstk
