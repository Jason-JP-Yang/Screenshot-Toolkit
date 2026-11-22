#include "sstk/setup/window.h"

#include "sstk/base/product.h"
#include "sstk/config/animation.h"
#include "sstk/config/colors.h"
#include "sstk/config/layout.h"
#include "sstk/graphics/compositor.h"
#include "sstk/service/service.h"
#include "sstk/setup/installer.h"
#include "sstk/version.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <thread>

namespace sstk {

namespace {

constexpr wchar_t kWindowClass[] = L"ScreenshotToolkit.Setup";
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kExStyle = WS_EX_NOREDIRECTIONBITMAP;
constexpr UINT kMsgStarted = WM_APP + 65;     // wParam: 1 once the elevated steps run. lParam: 1 if the prompt was declined.
constexpr UINT kMsgFinished = WM_APP + 66;    // wParam: the steps' exit code.
constexpr UINT kMsgActionDone = WM_APP + 67;  // An About window's restart or quit is over.
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kFinishTimer = 2;
constexpr UINT kStatusPollMs = 500;
constexpr DWORD kRestartWaitMs = 8000;
// Window attributes from newer SDKs: dark title bar, and the caption's colors on Windows 11.
constexpr DWORD kDarkModeAttribute = 20;
constexpr DWORD kCaptionColorAttribute = 35;
constexpr DWORD kCaptionTextAttribute = 36;

enum class Look { kPrimary, kSecondary, kDanger };

uint32_t Argb(uint32_t rgb) { return 0xFF000000u | rgb; }

COLORREF ToColorRef(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

bool AppsUseLightTheme() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
               RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value != 0;
}

// Light ink on a dark face is drawn a little heavier, as the selector's labels are.
float InkPower(uint32_t ink) {
  const uint32_t sum = ((ink >> 16) & 0xFF) + ((ink >> 8) & 0xFF) + (ink & 0xFF);
  return sum > 384 ? config::kLabelWeight : 1.0f;
}

void DrawOver(Image& target, const Image& source, int left, int top) {
  for (int y = 0; y < source.height(); ++y) {
    const int ty = top + y;
    if (ty < 0 || ty >= target.height()) continue;
    for (int x = 0; x < source.width(); ++x) {
      const int tx = left + x;
      if (tx < 0 || tx >= target.width()) continue;
      const uint8_t* s = source.data() + (static_cast<size_t>(y) * source.width() + x) * 4;
      uint8_t* d = target.data() + (static_cast<size_t>(ty) * target.width() + tx) * 4;
      const int keep = 255 - s[3];
      for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(s[c] + (d[c] * keep + 127) / 255);
    }
  }
}

// "Screenshot Toolkit", the second word in italics, set on one baseline.
TextLine RenderTitle(int pixels, uint32_t color) {
  const TextStyle upright{config::kSetupTitleFaces, ARRAYSIZE(config::kSetupTitleFaces), FW_NORMAL, false, InkPower(color)};
  TextStyle italic = upright;
  italic.italic = true;
  TextLine first = RenderText(config::kSetupTitleUpright, pixels, color, upright);
  TextLine second = RenderText(config::kSetupTitleItalic, pixels, color, italic);
  if (!first.image || !second.image) return first;
  const int second_left = static_cast<int>(std::lround(first.origin + first.advance - second.origin));
  const float baseline = std::max(first.baseline, second.baseline);
  const int first_top = static_cast<int>(std::lround(baseline - first.baseline));
  const int second_top = static_cast<int>(std::lround(baseline - second.baseline));
  TextLine title;
  title.image = Image::Allocate(std::max(first.image.width(), second_left + second.image.width()),
                                std::max(first_top + first.image.height(), second_top + second.image.height()));
  if (!title.image) return first;
  DrawOver(title.image, first.image, 0, first_top);
  DrawOver(title.image, second.image, second_left, second_top);
  title.baseline = baseline;
  title.cap_height = first.cap_height;
  title.origin = first.origin;
  title.advance = second_left + second.origin + second.advance - first.origin;
  return title;
}

// The fallback when no service answers: the application then runs without UI Access.
void LaunchDirectly() {
  const std::wstring self = ExecutablePath();
  std::wstring command_line = L"\"" + self + L"\" --run";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(self.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

class SetupWindow {
 public:
  explicit SetupWindow(SetupWindowMode mode) : mode_(mode) {}
  ~SetupWindow();
  SetupWindow(const SetupWindow&) = delete;
  SetupWindow& operator=(const SetupWindow&) = delete;

  int Run();

 private:
  enum class Phase { kIdle, kWaiting, kWorking, kFinishing, kDone, kBusy };

  // Two layers, so that text changes cross-fade.
  struct Slot {
    ComPtr<IDCompositionVisual> visual[2];
    ComPtr<IDCompositionEffectGroup> effect[2];
    AnimFloat alpha[2];
    int active = 0;
  };

  struct ButtonState {
    std::wstring text;
    Look look = Look::kPrimary;
  };

  // A face under a clip that is a pill at rest and a thin bar while setup works, with the bar's track and fill, a veil
  // for hover and press, the label, and outside the clip the ring the keyboard's focus puts around it.
  struct ButtonVisuals {
    RECT rect{};
    ComPtr<IDCompositionVisual> root;
    ComPtr<IDCompositionScaleTransform> scale;
    ComPtr<IDCompositionRectangleClip> clip;
    ComPtr<IDCompositionVisual> face[2];
    ComPtr<IDCompositionEffectGroup> face_effect[2];
    AnimFloat face_alpha[2];
    int face_active = 0;
    ComPtr<IDCompositionVisual> track;
    ComPtr<IDCompositionEffectGroup> track_effect;
    AnimFloat track_alpha;
    ComPtr<IDCompositionVisual> fill;
    ComPtr<IDCompositionRectangleClip> fill_clip;
    ComPtr<IDCompositionEffectGroup> fill_effect;
    AnimFloat fill_alpha;
    AnimFloat fill_right;
    ComPtr<IDCompositionVisual> veil;
    ComPtr<IDCompositionEffectGroup> veil_effect;
    AnimFloat veil_alpha;
    AnimFloat top;
    AnimFloat bottom;
    AnimFloat radius;
    AnimFloat press;
    Slot label;
    float label_baseline = 0.0f;
    ComPtr<IDCompositionVisual> ring;
    ComPtr<IDCompositionEffectGroup> ring_effect;
    AnimFloat ring_alpha;
  };

  static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);

  const config::SetupPalette& palette() const { return light_ ? config::kSetupLight : config::kSetupDark; }
  const config::SetupButtonColors& Colors(Look look) const;
  float Px(float dip) const { return std::round(dip * scale_); }
  int button_count() const { return mode_ == SetupWindowMode::kAbout ? 2 : 1; }
  bool Interactive() const { return phase_ == Phase::kIdle || phase_ == Phase::kDone; }
  bool Busy() const { return !Interactive(); }
  SetupAction action() const { return mode_ == SetupWindowMode::kUninstall ? SetupAction::kUninstall : SetupAction::kInstall; }
  IDCompositionDevice* device() const { return compositor_.device(); }
  float ButtonWidth(int index) const { return static_cast<float>(buttons_visual_[index].rect.right - buttons_visual_[index].rect.left); }
  float ButtonHeight(int index) const { return static_cast<float>(buttons_visual_[index].rect.bottom - buttons_visual_[index].rect.top); }

  const wchar_t* Caption() const;
  void ApplyFrameTheme();
  void LoadIcons();
  void FitWindow(const RECT* work, const POINT* top_left);
  void Rebuild();
  void BuildSlot(Slot& slot, IDCompositionVisual* parent);
  void ShowText(Slot& slot, const TextLine& line, float center_x, float baseline, bool animate);
  TextLine LineText(const std::wstring& text) const;
  TextLine ButtonText(const ButtonState& state) const;
  void BuildButton(int index, const RECT& rect);
  void SetLine(int index, const std::wstring& text, bool animate);
  void SetButton(int index, const std::wstring& text, Look look, bool always);
  void FoldIntoBar();
  void UnfoldFromBar(const std::wstring& text, Look look);
  void SetProgress(float fraction);
  int HitTest(POINT point) const;
  void SetHover(int index);
  void ShowFocus(bool shown);
  void Activate(int index);
  void StartSetup();
  void BeginWorking();
  void OnStep(int step, int count);
  void OnFinished(int code);
  void OnFinishTimer();
  void ShowFailure(int code);
  void RunAboutAction(bool restart);
  void RefreshStatus(bool animate);

  SetupWindowMode mode_;
  HWND window_ = nullptr;
  HICON icon_big_ = nullptr;
  HICON icon_small_ = nullptr;
  Compositor compositor_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  float scale_ = 1.0f;
  bool light_ = false;
  int client_width_ = 0;
  int client_height_ = 0;
  float line_baseline_[2] = {};
  std::wstring line_text_[2];
  Slot lines_[2];
  ButtonState buttons_[2];
  ButtonVisuals buttons_visual_[2];
  Phase phase_ = Phase::kIdle;
  bool bar_ = false;
  float progress_ = 0.0f;
  bool succeeded_ = false;
  int last_code_ = 0;
  int exit_code_ = 0;
  int hover_ = -1;
  int pressed_ = -1;
  int focus_ = 0;
  bool focus_shown_ = false;
  bool tracking_ = false;
  std::thread worker_;
};

SetupWindow::~SetupWindow() {
  if (worker_.joinable()) worker_.join();
  if (icon_big_ != nullptr) DestroyIcon(icon_big_);
  if (icon_small_ != nullptr) DestroyIcon(icon_small_);
}

const config::SetupButtonColors& SetupWindow::Colors(Look look) const {
  const config::SetupPalette& colors = palette();
  return look == Look::kPrimary ? colors.primary : look == Look::kSecondary ? colors.secondary : colors.danger;
}

const wchar_t* SetupWindow::Caption() const {
  switch (mode_) {
    case SetupWindowMode::kInstall: return config::kSetupCaptionInstall;
    case SetupWindowMode::kUninstall: return config::kSetupCaptionUninstall;
    case SetupWindowMode::kAbout: return config::kSetupCaptionAbout;
  }
  return config::kSetupCaptionAbout;
}

int SetupWindow::Run() {
  light_ = AppsUseLightTheme();
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = Proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  RegisterClassExW(&window_class);

  // Created on the monitor under the pointer, so that it starts at that monitor's scale.
  POINT cursor{};
  GetCursorPos(&cursor);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
  const RECT work = monitor.rcWork;
  window_ = CreateWindowExW(kExStyle, kWindowClass, Caption(), kStyle, (work.left + work.right) / 2, (work.top + work.bottom) / 2, 1, 1,
                            nullptr, nullptr, instance, this);
  if (window_ == nullptr) return 1;
  scale_ = static_cast<float>(GetDpiForWindow(window_)) / USER_DEFAULT_SCREEN_DPI;
  ApplyFrameTheme();
  LoadIcons();
  if (!compositor_.Initialize() || FAILED(device()->CreateTargetForHwnd(window_, TRUE, &target_))) {
    DestroyWindow(window_);
    return 1;
  }

  line_text_[0] = std::wstring(L"Version ") + SSTK_VERSION_WIDE;
  line_text_[1] = SSTK_VERSION_DATE;
  switch (mode_) {
    case SetupWindowMode::kInstall:
      buttons_[0] = {config::kSetupInstall, Look::kPrimary};
      break;
    case SetupWindowMode::kUninstall:
      buttons_[0] = {config::kSetupUninstall, Look::kDanger};
      break;
    case SetupWindowMode::kAbout:
      buttons_[0] = {config::kSetupRestart, Look::kPrimary};
      buttons_[1] = {config::kSetupQuit, Look::kSecondary};
      line_text_[1] = IsApplicationRunning() ? config::kSetupRunning : config::kSetupStopped;
      SetTimer(window_, kStatusTimer, kStatusPollMs, nullptr);
      break;
  }
  Rebuild();
  FitWindow(&work, nullptr);
  ShowWindow(window_, SW_SHOWNORMAL);
  SetForegroundWindow(window_);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (worker_.joinable()) worker_.join();
  for (ButtonVisuals& button : buttons_visual_) button = ButtonVisuals{};
  for (Slot& slot : lines_) slot = Slot{};
  root_.Reset();
  target_.Reset();
  compositor_.Shutdown();
  return exit_code_;
}

void SetupWindow::ApplyFrameTheme() {
  const BOOL dark = light_ ? FALSE : TRUE;
  DwmSetWindowAttribute(window_, kDarkModeAttribute, &dark, sizeof(dark));
  const COLORREF caption = ToColorRef(palette().body);
  DwmSetWindowAttribute(window_, kCaptionColorAttribute, &caption, sizeof(caption));
  const COLORREF text = ToColorRef(palette().title);
  DwmSetWindowAttribute(window_, kCaptionTextAttribute, &text, sizeof(text));
}

void SetupWindow::LoadIcons() {
  const UINT dpi = GetDpiForWindow(window_);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  HICON large_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi),
                                                   GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
  HICON small_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                   GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
  SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
  SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
  if (icon_big_ != nullptr) DestroyIcon(icon_big_);
  if (icon_small_ != nullptr) DestroyIcon(icon_small_);
  icon_big_ = large_icon;
  icon_small_ = small_icon;
}

// Sizes the frame around the client area, centered in `work` or kept at `top_left`.
void SetupWindow::FitWindow(const RECT* work, const POINT* top_left) {
  RECT frame{0, 0, client_width_, client_height_};
  AdjustWindowRectExForDpi(&frame, kStyle, FALSE, kExStyle, GetDpiForWindow(window_));
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  int x = 0;
  int y = 0;
  if (top_left != nullptr) {
    x = top_left->x;
    y = top_left->y;
  } else {
    x = (work->left + work->right - width) / 2;
    y = std::max<int>(work->top, (work->top + work->bottom - height) / 2);
  }
  SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void SetupWindow::BuildSlot(Slot& slot, IDCompositionVisual* parent) {
  for (int i = 0; i < 2; ++i) {
    slot.visual[i] = compositor_.CreateVisual();
    slot.effect[i] = compositor_.CreateEffectGroup();
    slot.visual[i]->SetEffect(slot.effect[i].Get());
    parent->AddVisual(slot.visual[i].Get(), FALSE, nullptr);
    slot.alpha[i].Bind(slot.effect[i].Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
    slot.alpha[i].Snap(0.0f);
  }
  slot.active = 0;
}

void SetupWindow::ShowText(Slot& slot, const TextLine& line, float center_x, float baseline, bool animate) {
  const int next = animate ? 1 - slot.active : slot.active;
  IDCompositionVisual* visual = slot.visual[next].Get();
  visual->SetContent(compositor_.CreateSurface(line.image, false).Get());
  visual->SetOffsetX(std::round(center_x - line.advance * 0.5f - line.origin));
  visual->SetOffsetY(std::round(baseline - line.baseline));
  if (animate) {
    const TimeBase now = compositor_.Now();
    slot.alpha[next].To(device(), now, 1.0f, config::kSetupFade);
    slot.alpha[slot.active].To(device(), now, 0.0f, config::kSetupFade);
  } else {
    slot.alpha[next].Snap(1.0f);
    slot.alpha[1 - next].Snap(0.0f);
  }
  slot.active = next;
}

TextLine SetupWindow::LineText(const std::wstring& text) const {
  const uint32_t color = palette().text;
  const TextStyle style{config::kSetupTextFaces, ARRAYSIZE(config::kSetupTextFaces), FW_NORMAL, false, InkPower(color)};
  return RenderText(text.c_str(), static_cast<int>(Px(config::kSetupLineSize)), color, style);
}

TextLine SetupWindow::ButtonText(const ButtonState& state) const {
  const uint32_t ink = Colors(state.look).ink;
  const TextStyle style{config::kLabelFaces, ARRAYSIZE(config::kLabelFaces), FW_NORMAL, false, InkPower(ink)};
  return RenderText(state.text.c_str(), static_cast<int>(Px(config::kSetupButtonFontSize)), ink, style);
}

// Everything is drawn again at the current scale and theme, and placed where the current state has it, without motion.
void SetupWindow::Rebuild() {
  const config::SetupPalette& colors = palette();
  root_ = compositor_.CreateVisual();
  target_->SetRoot(root_.Get());
  hover_ = -1;

  const int logo_size = static_cast<int>(Px(config::kSetupLogoSize));
  const TextLine title = RenderTitle(static_cast<int>(Px(config::kSetupTitleSize)), colors.title);
  client_width_ = static_cast<int>(Px(config::kSetupWidth));
  const float logo_top = Px(config::kSetupLogoTop);
  const float title_baseline = std::round(logo_top + logo_size + Px(config::kSetupTitleGap) + title.cap_height);
  line_baseline_[0] = title_baseline + Px(config::kSetupFirstLineGap);
  line_baseline_[1] = line_baseline_[0] + Px(config::kSetupLineGap);
  const int button_width = static_cast<int>(Px(config::kSetupButtonWidth));
  const int button_height = static_cast<int>(Px(config::kSetupButtonHeight));
  RECT rects[2] = {};
  int top = static_cast<int>(line_baseline_[1] + Px(config::kSetupButtonGap));
  for (int i = 0; i < button_count(); ++i) {
    const int left = (client_width_ - button_width) / 2;
    rects[i] = RECT{left, top, left + button_width, top + button_height};
    top += button_height + static_cast<int>(Px(config::kSetupButtonSpacing));
  }
  client_height_ = static_cast<int>(rects[button_count() - 1].bottom + Px(config::kSetupBottom));

  // Larger than the client area, so that no edge shows while the frame catches up with a new size.
  const float margin = Px(64.0f);
  root_->AddVisual(compositor_.CreateSolidVisual(Argb(colors.body), client_width_ + margin, client_height_ + margin).Get(), FALSE, nullptr);

  ComPtr<IDCompositionVisual> logo = compositor_.CreateVisual();
  logo->SetContent(compositor_.CreateSurface(RenderLogo(logo_size), false).Get());
  logo->SetOffsetX(std::round((client_width_ - logo_size) * 0.5f));
  logo->SetOffsetY(logo_top);
  root_->AddVisual(logo.Get(), FALSE, nullptr);

  ComPtr<IDCompositionVisual> heading = compositor_.CreateVisual();
  heading->SetContent(compositor_.CreateSurface(title.image, false).Get());
  heading->SetOffsetX(std::round((client_width_ - title.advance) * 0.5f - title.origin));
  heading->SetOffsetY(std::round(title_baseline - title.baseline));
  root_->AddVisual(heading.Get(), FALSE, nullptr);

  for (int i = 0; i < 2; ++i) {
    lines_[i] = Slot{};
    BuildSlot(lines_[i], root_.Get());
    ShowText(lines_[i], LineText(line_text_[i]), client_width_ * 0.5f, line_baseline_[i], false);
  }
  for (int i = 0; i < button_count(); ++i) BuildButton(i, rects[i]);
  compositor_.Commit();
}

void SetupWindow::BuildButton(int index, const RECT& rect) {
  ButtonVisuals& b = buttons_visual_[index];
  b = ButtonVisuals{};
  b.rect = rect;
  const ButtonState& state = buttons_[index];
  const config::SetupButtonColors& colors = Colors(state.look);
  const float w = ButtonWidth(index);
  const float h = ButtonHeight(index);
  const bool bar = bar_ && index == 0;

  b.root = compositor_.CreateVisual();
  b.root->SetOffsetX(static_cast<float>(rect.left));
  b.root->SetOffsetY(static_cast<float>(rect.top));
  b.root->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  b.scale = compositor_.CreateScale();
  b.scale->SetCenterX(w * 0.5f);
  b.scale->SetCenterY(h * 0.5f);
  b.root->SetTransform(b.scale.Get());
  b.clip = compositor_.CreateClip();
  b.clip->SetLeft(0.0f);
  b.clip->SetRight(w);
  b.root->SetClip(b.clip.Get());
  root_->AddVisual(b.root.Get(), FALSE, nullptr);

  auto layer = [&](uint32_t color, ComPtr<IDCompositionEffectGroup>& effect, AnimFloat& alpha) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateSolidVisual(Argb(color), w, h);
    effect = compositor_.CreateEffectGroup();
    visual->SetEffect(effect.Get());
    b.root->AddVisual(visual.Get(), FALSE, nullptr);
    alpha.Bind(effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
    return visual;
  };
  for (int i = 0; i < 2; ++i) b.face[i] = layer(colors.face, b.face_effect[i], b.face_alpha[i]);
  b.track = layer(palette().track, b.track_effect, b.track_alpha);

  b.fill = compositor_.CreateVisual();
  b.fill_clip = compositor_.CreateClip();
  b.fill_clip->SetLeft(0.0f);
  b.fill_clip->SetTop(0.0f);
  b.fill_clip->SetBottom(h);
  b.fill->SetClip(b.fill_clip.Get());
  b.fill_effect = compositor_.CreateEffectGroup();
  b.fill->SetEffect(b.fill_effect.Get());
  b.fill->AddVisual(compositor_.CreateSolidVisual(Argb(palette().progress), w, h).Get(), FALSE, nullptr);
  b.root->AddVisual(b.fill.Get(), FALSE, nullptr);
  b.fill_alpha.Bind(b.fill_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  b.fill_right.Bind(b.fill_clip.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));

  b.veil = layer(colors.hover, b.veil_effect, b.veil_alpha);
  BuildSlot(b.label, b.root.Get());

  b.top.Bind(b.clip.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetTop));
  b.bottom.Bind(b.clip.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  b.radius.Bind(b.clip.Get(), SetClipRadii);
  b.press.Bind(b.scale.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  b.press.Bind(b.scale.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));

  const float gap = Px(config::kSetupFocusGap);
  const float stroke = std::max(Px(config::kSetupFocusStroke), 1.0f);
  const int ring_width = static_cast<int>(w + (gap + stroke) * 2.0f);
  const int ring_height = static_cast<int>(h + (gap + stroke) * 2.0f);
  b.ring = compositor_.CreateVisual();
  b.ring->SetContent(compositor_.CreateSurface(RenderRoundedOutline(ring_width, ring_height, ring_height * 0.5f, stroke, palette().focus), false).Get());
  b.ring->SetOffsetX(rect.left - gap - stroke);
  b.ring->SetOffsetY(rect.top - gap - stroke);
  b.ring_effect = compositor_.CreateEffectGroup();
  b.ring->SetEffect(b.ring_effect.Get());
  root_->AddVisual(b.ring.Get(), FALSE, nullptr);
  b.ring_alpha.Bind(b.ring_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));

  const float thickness = Px(config::kSetupProgressHeight);
  const float bar_top = std::round((h - thickness) * 0.5f);
  b.face_alpha[0].Snap(1.0f);
  b.face_alpha[1].Snap(0.0f);
  b.track_alpha.Snap(bar ? 1.0f : 0.0f);
  b.fill_alpha.Snap(bar ? 1.0f : 0.0f);
  b.fill_right.Snap(bar ? progress_ * w : 0.0f);
  b.veil_alpha.Snap(0.0f);
  b.press.Snap(1.0f);
  b.ring_alpha.Snap(focus_shown_ && focus_ == index && Interactive() ? 1.0f : 0.0f);
  b.top.Snap(bar ? bar_top : 0.0f);
  b.bottom.Snap(bar ? bar_top + thickness : h);
  b.radius.Snap(bar ? thickness * 0.5f : h * 0.5f);

  const TextLine label = ButtonText(state);
  b.label_baseline = std::round((h + label.cap_height) * 0.5f);
  ShowText(b.label, label, w * 0.5f, b.label_baseline, false);
  if (bar) b.label.alpha[b.label.active].Snap(0.0f);
}

void SetupWindow::SetLine(int index, const std::wstring& text, bool animate) {
  if (text == line_text_[index]) return;
  line_text_[index] = text;
  ShowText(lines_[index], LineText(text), client_width_ * 0.5f, line_baseline_[index], animate);
  compositor_.Commit();
}

void SetupWindow::SetButton(int index, const std::wstring& text, Look look, bool always) {
  ButtonState& state = buttons_[index];
  ButtonVisuals& b = buttons_visual_[index];
  const bool restyled = look != state.look;
  if (!restyled && text == state.text && !always) return;
  if (restyled) {
    const TimeBase now = compositor_.Now();
    const int next = 1 - b.face_active;
    b.face[next]->SetContent(compositor_.SolidColor(Argb(Colors(look).face)));
    b.face_alpha[next].To(device(), now, 1.0f, config::kSetupFade);
    b.face_alpha[b.face_active].To(device(), now, 0.0f, config::kSetupFade);
    b.face_active = next;
    b.veil->SetContent(compositor_.SolidColor(Argb(Colors(look).hover)));
  }
  state = {text, look};
  ShowText(b.label, ButtonText(state), ButtonWidth(index) * 0.5f, b.label_baseline, true);
  compositor_.Commit();
}

void SetupWindow::FoldIntoBar() {
  ButtonVisuals& b = buttons_visual_[0];
  const TimeBase now = compositor_.Now();
  const float h = ButtonHeight(0);
  const float thickness = Px(config::kSetupProgressHeight);
  const float bar_top = std::round((h - thickness) * 0.5f);
  bar_ = true;
  progress_ = 0.0f;
  b.top.To(device(), now, bar_top, config::kSetupMorph);
  b.bottom.To(device(), now, bar_top + thickness, config::kSetupMorph);
  b.radius.To(device(), now, thickness * 0.5f, config::kSetupMorph);
  b.track_alpha.To(device(), now, 1.0f, config::kSetupFade);
  b.label.alpha[b.label.active].To(device(), now, 0.0f, config::kSetupFade);
  b.veil_alpha.To(device(), now, 0.0f, config::kSetupHover);
  b.press.To(device(), now, 1.0f, config::kSetupPress);
  b.fill_right.Snap(0.0f);
  b.fill_alpha.Snap(1.0f);
  compositor_.Commit();
}

void SetupWindow::UnfoldFromBar(const std::wstring& text, Look look) {
  ButtonVisuals& b = buttons_visual_[0];
  const TimeBase now = compositor_.Now();
  const float h = ButtonHeight(0);
  bar_ = false;
  b.top.To(device(), now, 0.0f, config::kSetupMorph);
  b.bottom.To(device(), now, h, config::kSetupMorph);
  b.radius.To(device(), now, h * 0.5f, config::kSetupMorph);
  b.track_alpha.To(device(), now, 0.0f, config::kSetupFade);
  b.fill_alpha.To(device(), now, 0.0f, config::kSetupFade);
  SetButton(0, text, look, true);
}

void SetupWindow::SetProgress(float fraction) {
  progress_ = std::max(progress_, std::clamp(fraction, 0.0f, 1.0f));
  buttons_visual_[0].fill_right.To(device(), compositor_.Now(), progress_ * ButtonWidth(0), config::kSetupProgress);
  compositor_.Commit();
}

int SetupWindow::HitTest(POINT point) const {
  for (int i = 0; i < button_count(); ++i) {
    if (PtInRect(&buttons_visual_[i].rect, point)) return i;
  }
  return -1;
}

void SetupWindow::SetHover(int index) {
  if (index == hover_) return;
  const TimeBase now = compositor_.Now();
  if (hover_ >= 0) buttons_visual_[hover_].veil_alpha.To(device(), now, 0.0f, config::kSetupHover);
  hover_ = index;
  if (hover_ >= 0) {
    const float alpha = Colors(buttons_[hover_].look).hover_alpha * (pressed_ == hover_ ? 2.0f : 1.0f);
    buttons_visual_[hover_].veil_alpha.To(device(), now, alpha, config::kSetupHover);
  }
  compositor_.Commit();
}

void SetupWindow::ShowFocus(bool shown) {
  focus_shown_ = shown;
  const TimeBase now = compositor_.Now();
  for (int i = 0; i < button_count(); ++i) {
    buttons_visual_[i].ring_alpha.To(device(), now, shown && i == focus_ && Interactive() ? 1.0f : 0.0f, config::kSetupHover);
  }
  compositor_.Commit();
}

void SetupWindow::Activate(int index) {
  if (!Interactive() || index < 0 || index >= button_count()) return;
  if (mode_ == SetupWindowMode::kAbout) {
    RunAboutAction(index == 0);
  } else if (phase_ == Phase::kDone) {
    DestroyWindow(window_);
  } else {
    StartSetup();
  }
}

void SetupWindow::StartSetup() {
  phase_ = Phase::kWaiting;
  SetHover(-1);
  ShowFocus(false);
  if (worker_.joinable()) worker_.join();
  const HWND window = window_;
  const SetupAction requested = action();
  worker_ = std::thread([window, requested] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool declined = false;
    HANDLE process = StartElevatedSetup(requested, window, &declined);
    PostMessageW(window, kMsgStarted, process != nullptr, declined);
    DWORD code = declined ? kSetupCancelled : 1;
    if (process != nullptr) {
      WaitForSingleObject(process, INFINITE);
      GetExitCodeProcess(process, &code);
      CloseHandle(process);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    PostMessageW(window, kMsgFinished, code, 0);
  });
}

void SetupWindow::BeginWorking() {
  phase_ = Phase::kWorking;
  FoldIntoBar();
  SetLine(0, mode_ == SetupWindowMode::kUninstall ? config::kSetupUninstalling : config::kSetupInstalling, true);
  SetLine(1, SetupStepText(action(), 0), true);
}

void SetupWindow::OnStep(int step, int count) {
  if (phase_ == Phase::kWaiting) BeginWorking();
  if (phase_ != Phase::kWorking || count <= 0) return;
  SetLine(1, SetupStepText(action(), step), true);
  SetProgress((step + 0.6f) / count);
}

void SetupWindow::OnFinished(int code) {
  last_code_ = code;
  exit_code_ = code;
  if (phase_ == Phase::kWaiting) {
    // It never ran: the prompt was declined, or the program could not start.
    phase_ = Phase::kIdle;
    ShowFailure(code);
    return;
  }
  phase_ = Phase::kFinishing;
  succeeded_ = code == kSetupDone;
  if (succeeded_) SetProgress(1.0f);
  const double wait = (succeeded_ ? SpringSettleTime(config::kSetupProgress) * 0.6 : 0.0) + config::kSetupFinishHold;
  SetTimer(window_, kFinishTimer, static_cast<UINT>(wait * 1000.0), nullptr);
}

void SetupWindow::OnFinishTimer() {
  KillTimer(window_, kFinishTimer);
  if (phase_ != Phase::kFinishing) return;
  const bool uninstalling = mode_ == SetupWindowMode::kUninstall;
  if (succeeded_) {
    phase_ = Phase::kDone;
    SetLine(0, uninstalling ? config::kSetupUninstalled : config::kSetupInstalled, true);
    SetLine(1, uninstalling ? config::kSetupUninstalledHint : config::kSetupInstalledHint, true);
    UnfoldFromBar(uninstalling ? config::kSetupClose : config::kSetupDone, uninstalling ? Look::kSecondary : Look::kPrimary);
  } else {
    phase_ = Phase::kIdle;
    ShowFailure(last_code_);
    UnfoldFromBar(buttons_[0].text, buttons_[0].look);
  }
}

void SetupWindow::ShowFailure(int code) {
  SetLine(0, mode_ == SetupWindowMode::kUninstall ? config::kSetupUninstallFailed : config::kSetupInstallFailed, true);
  SetLine(1, SetupFailureText(code), true);
}

void SetupWindow::RunAboutAction(bool restart) {
  phase_ = Phase::kBusy;
  SetHover(-1);
  ShowFocus(focus_shown_);
  SetLine(1, restart ? config::kSetupRestarting : config::kSetupQuitting, true);
  if (worker_.joinable()) worker_.join();
  const HWND window = window_;
  worker_ = std::thread([window, restart] {
    RequestQuit();
    WaitForApplicationExit(5000);
    if (restart) {
      if (!AskServiceToStart()) LaunchDirectly();
      const ULONGLONG deadline = GetTickCount64() + kRestartWaitMs;
      while (!IsApplicationRunning() && GetTickCount64() < deadline) Sleep(100);
    }
    PostMessageW(window, kMsgActionDone, 0, 0);
  });
}

void SetupWindow::RefreshStatus(bool animate) {
  SetLine(1, IsApplicationRunning() ? config::kSetupRunning : config::kSetupStopped, animate);
}

LRESULT CALLBACK SetupWindow::Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  return self != nullptr && self->root_ != nullptr ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SetupWindow::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEMOVE: {
      if (!tracking_) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        tracking_ = TrackMouseEvent(&track) != FALSE;
      }
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SetHover(Interactive() && (pressed_ < 0 || HitTest(point) == pressed_) ? HitTest(point) : -1);
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      if (pressed_ < 0) SetHover(-1);
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int index = Interactive() ? HitTest(point) : -1;
      if (index < 0) return 0;
      pressed_ = index;
      focus_ = index;
      SetCapture(window_);
      ShowFocus(false);
      ButtonVisuals& b = buttons_visual_[index];
      const TimeBase now = compositor_.Now();
      b.press.To(device(), now, config::kSetupPressScale, config::kSetupPress);
      b.veil_alpha.To(device(), now, Colors(buttons_[index].look).hover_alpha * 2.0f, config::kSetupHover);
      hover_ = index;
      compositor_.Commit();
      return 0;
    }
    case WM_LBUTTONUP: {
      if (pressed_ < 0) return 0;
      const int index = pressed_;
      pressed_ = -1;
      ReleaseCapture();
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool inside = HitTest(point) == index;
      ButtonVisuals& b = buttons_visual_[index];
      const TimeBase now = compositor_.Now();
      b.press.To(device(), now, 1.0f, config::kSetupPress);
      b.veil_alpha.To(device(), now, inside ? Colors(buttons_[index].look).hover_alpha : 0.0f, config::kSetupHover);
      hover_ = inside ? index : -1;
      compositor_.Commit();
      if (inside) Activate(index);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (pressed_ >= 0 && reinterpret_cast<HWND>(lparam) != window_) {
        ButtonVisuals& b = buttons_visual_[pressed_];
        const TimeBase now = compositor_.Now();
        b.press.To(device(), now, 1.0f, config::kSetupPress);
        b.veil_alpha.To(device(), now, 0.0f, config::kSetupHover);
        pressed_ = -1;
        hover_ = -1;
        compositor_.Commit();
      }
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_TAB && Interactive()) {
        if (focus_shown_) focus_ = (focus_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? button_count() - 1 : 1)) % button_count();
        ShowFocus(true);
      } else if ((wparam == VK_RETURN || wparam == VK_SPACE) && Interactive()) {
        Activate(focus_);
      } else if (wparam == VK_ESCAPE) {
        PostMessageW(window_, WM_CLOSE, 0, 0);
      }
      return 0;
    case kMsgSetupStep:
      OnStep(static_cast<int>(wparam), static_cast<int>(lparam));
      return 0;
    case kMsgStarted:
      if (wparam != 0 && phase_ == Phase::kWaiting) BeginWorking();
      return 0;
    case kMsgFinished:
      OnFinished(static_cast<int>(wparam));
      return 0;
    case kMsgActionDone:
      phase_ = Phase::kIdle;
      RefreshStatus(true);
      ShowFocus(focus_shown_);
      return 0;
    case WM_TIMER:
      if (wparam == kFinishTimer) {
        OnFinishTimer();
      } else if (wparam == kStatusTimer && phase_ == Phase::kIdle) {
        RefreshStatus(true);
      }
      return 0;
    case WM_SETTINGCHANGE:
      if (lparam != 0 && wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
        const bool light = AppsUseLightTheme();
        if (light != light_) {
          light_ = light;
          ApplyFrameTheme();
          Rebuild();
        }
      }
      return 0;
    case WM_DPICHANGED: {
      scale_ = static_cast<float>(HIWORD(wparam)) / USER_DEFAULT_SCREEN_DPI;
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      const POINT top_left{suggested->left, suggested->top};
      Rebuild();
      LoadIcons();
      FitWindow(nullptr, &top_left);
      return 0;
    }
    case WM_CLOSE:
      if (!Busy()) DestroyWindow(window_);  // Setup under way cannot be left.
      return 0;
    case WM_DESTROY:
      KillTimer(window_, kStatusTimer);
      KillTimer(window_, kFinishTimer);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

}  // namespace

int RunSetupWindow(SetupWindowMode mode) {
  SetupWindow window(mode);
  return window.Run();
}

}  // namespace sstk
