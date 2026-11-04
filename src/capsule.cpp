#include "capsule.h"

#include "config/animation.h"
#include "config/colors.h"
#include "config/core.h"
#include "config/layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sstk {

namespace {

constexpr double kTwoPi = 6.283185307179586;

uint32_t Argb(uint32_t rgb, float alpha) { return static_cast<uint32_t>(alpha * 255.0f + 0.5f) << 24 | rgb; }

// Lays one image over another, both premultiplied.
void Blend(Image& canvas, const Image& part, int at_x, int at_y) {
  for (int y = 0; y < part.height(); ++y) {
    const int to_y = at_y + y;
    if (to_y < 0 || to_y >= canvas.height()) continue;
    for (int x = 0; x < part.width(); ++x) {
      const int to_x = at_x + x;
      if (to_x < 0 || to_x >= canvas.width()) continue;
      const uint8_t* from = part.data() + static_cast<ptrdiff_t>(y) * part.stride() + static_cast<ptrdiff_t>(x) * 4;
      uint8_t* to = canvas.data() + static_cast<ptrdiff_t>(to_y) * canvas.stride() + static_cast<ptrdiff_t>(to_x) * 4;
      const int keep = 255 - from[3];
      for (int c = 0; c < 4; ++c) to[c] = static_cast<uint8_t>(from[c] + (to[c] * keep + 127) / 255);
    }
  }
}

struct Part {
  const Image* image;
  bool keycap;
  float gap;  // Before it, between what is visibly drawn.
};

// Sets text and keycaps on one line: the text on a shared baseline, and the keycaps centered on its capitals.
TextLine Compose(const std::vector<Part>& parts, float baseline, float cap_height) {
  std::vector<POINT> places;
  int pen = 0;
  int top = 0;
  int bottom = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    const Image& image = *parts[i].image;
    const RECT ink = InkBounds(image);
    const int x = (i == 0 ? 0 : pen + static_cast<int>(std::lround(parts[i].gap))) - ink.left;
    const int y = parts[i].keycap ? static_cast<int>(std::lround(baseline - cap_height * 0.5f - image.height() * 0.5f)) : 0;
    places.push_back(POINT{x, y});
    pen = x + ink.right;
    top = std::min(top, y);
    bottom = std::max(bottom, y + image.height());
  }
  TextLine line;
  line.image = Image::Allocate(std::max(pen, 1), std::max(bottom - top, 1));
  line.baseline = baseline - static_cast<float>(top);
  line.cap_height = cap_height;
  if (!line.image) return line;
  for (size_t i = 0; i < parts.size(); ++i) Blend(line.image, *parts[i].image, places[i].x, places[i].y - top);
  return line;
}

// "Win" and "Enter" as two keycaps, then text, or text, the keycaps, and more text.
TextLine ChordLine(const wchar_t* before, const wchar_t* after, uint32_t color, float scale) {
  const int font = static_cast<int>(std::lround(config::kCapsuleFontSize * scale));
  const int key_height = static_cast<int>(std::lround(config::kKeycapHeight * scale));
  const Image win = RenderKeycap(Keycap::kWindows, key_height, scale, color);
  const Image enter = RenderKeycap(Keycap::kEnter, key_height, scale, color);
  const TextLine tail = RenderText(after, font, color);
  const float text_gap = config::kKeycapTextGap * scale;
  std::vector<Part> parts;
  TextLine head;
  if (before != nullptr) {
    head = RenderText(before, font, color);
    parts.push_back(Part{&head.image, false, 0.0f});
  }
  parts.push_back(Part{&win, true, text_gap});
  parts.push_back(Part{&enter, true, config::kKeycapGap * scale});
  parts.push_back(Part{&tail.image, false, text_gap});
  return Compose(parts, tail.baseline, tail.cap_height);
}

// The scroll option has none: it says how to start a scrolling screenshot instead.
const wchar_t* LabelFor(RegionKind kind) {
  switch (kind) {
    case RegionKind::kFull: return config::kLabelFullscreen;
    case RegionKind::kApp:
    case RegionKind::kZone: return config::kLabelWindow;
    case RegionKind::kNoTaskbar: return config::kLabelNoTaskbar;
    case RegionKind::kLong: return config::kLabelScrolling;
    case RegionKind::kScroll: break;
  }
  return nullptr;
}

}  // namespace

Capsule::Capsule(Compositor& compositor, IDCompositionVisual* parent, float scale, SIZE monitor)
    : compositor_(compositor), scale_(scale), monitor_(monitor),
      height_(std::round((config::kSegmentHeight + config::kCapsulePadding * 2.0f) * scale)),
      pad_(std::round(config::kCapsulePadding * scale)), cell_(std::round(config::kSegmentWidth * scale)),
      segment_height_(std::round(config::kSegmentHeight * scale)), dot_(std::round(config::kRecordingDot * scale)) {
  const float wide = frost_width() * 2.0f;

  root_ = compositor_.CreateVisual();
  root_effect_ = compositor_.CreateEffectGroup();
  root_->SetEffect(root_effect_.Get());
  root_->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
  parent->AddVisual(root_.Get(), FALSE, nullptr);

  // The shadow is cut into two caps and a stretching middle, so that it follows any width without being stretched.
  const config::ShadowSpec& cast = config::kCapsuleShadow;
  const ShadowLayer layers[] = {{cast.sigma * scale, cast.alpha, cast.offset * scale}};
  shadow_pad_ = std::ceil(28.0f * scale);
  const Image shadow = RenderShadow(static_cast<int>(height_), static_cast<int>(height_), height_ * 0.5f, shadow_pad_, layers, ARRAYSIZE(layers));
  const int left_columns = shadow.width() / 2;
  const int right_columns = shadow.width() - left_columns - 1;
  shadow_left_width_ = static_cast<float>(left_columns);
  shadow_right_width_ = static_cast<float>(right_columns);
  auto slice = [&](int left, int width) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateVisual();
    visual->SetContent(compositor_.CreateSurface(CopyRect(shadow, left, 0, width, shadow.height()), false).Get());
    visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
    visual->SetOffsetY(-shadow_pad_);
    root_->AddVisual(visual.Get(), FALSE, nullptr);
    return visual;
  };
  shadow_left_ = slice(0, left_columns);
  shadow_middle_ = slice(left_columns, 1);
  shadow_right_ = slice(left_columns + 1, right_columns);
  shadow_left_->SetOffsetX(-shadow_pad_);
  shadow_middle_->SetOffsetX(shadow_left_width_ - shadow_pad_);
  shadow_stretch_ = compositor_.CreateScale();
  shadow_middle_->SetTransform(shadow_stretch_.Get());

  frame_ = compositor_.CreateVisual();
  frame_clip_ = compositor_.CreateClip();
  frame_->SetClip(frame_clip_.Get());
  frame_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  frame_clip_->SetLeft(0.0f);
  frame_clip_->SetTop(0.0f);
  frame_clip_->SetBottom(height_);
  SetClipRadii(frame_clip_.Get(), height_ * 0.5f, nullptr);
  root_->AddVisual(frame_.Get(), FALSE, nullptr);

  ring_ = compositor_.CreateSolidVisual(Argb(config::kCapsuleHairlineColor, config::kCapsuleHairlineAlpha), wide, height_);
  frame_->AddVisual(ring_.Get(), FALSE, nullptr);

  inner_ = compositor_.CreateVisual();
  inner_clip_ = compositor_.CreateClip();
  inner_->SetClip(inner_clip_.Get());
  inner_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  inner_clip_->SetLeft(1.0f);
  inner_clip_->SetTop(1.0f);
  inner_clip_->SetBottom(height_ - 1.0f);
  SetClipRadii(inner_clip_.Get(), height_ * 0.5f - 1.0f, nullptr);
  frame_->AddVisual(inner_.Get(), FALSE, nullptr);

  fallback_fill_ = compositor_.CreateSolidVisual(Argb(config::kGlassTint, 1.0f), wide, height_);
  inner_->AddVisual(fallback_fill_.Get(), FALSE, nullptr);
  frost_ = compositor_.CreateVisual();
  inner_->AddVisual(frost_.Get(), FALSE, nullptr);

  // The outcome of a scrolling screenshot floods the glass with its color.
  status_fill_ = compositor_.CreateSolidVisual(Argb(config::kScrollSucceeded, 1.0f), wide, height_);
  status_effect_ = compositor_.CreateEffectGroup();
  status_fill_->SetEffect(status_effect_.Get());
  inner_->AddVisual(status_fill_.Get(), FALSE, nullptr);

  light_layer_ = compositor_.CreateVisual();
  inner_->AddVisual(light_layer_.Get(), FALSE, nullptr);

  veil_ = compositor_.CreateVisual();
  veil_clip_ = compositor_.CreateClip();
  veil_->SetClip(veil_clip_.Get());
  veil_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  veil_effect_ = compositor_.CreateEffectGroup();
  veil_->SetEffect(veil_effect_.Get());
  veil_->AddVisual(compositor_.CreateSolidVisual(Argb(config::kHoverVeilColor, config::kHoverVeilAlpha), wide, height_).Get(), FALSE, nullptr);
  inner_->AddVisual(veil_.Get(), FALSE, nullptr);

  // Dark glyphs and labels live inside the pill's clip, so the sliding pill inverts each glyph exactly as it passes.
  // The same pill gathers into the recording dot.
  pill_ = compositor_.CreateVisual();
  pill_clip_ = compositor_.CreateClip();
  pill_->SetClip(pill_clip_.Get());
  pill_->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
  pill_effect_ = compositor_.CreateEffectGroup();
  pill_->SetEffect(pill_effect_.Get());
  pill_->AddVisual(compositor_.CreateSolidVisual(Argb(config::kPorcelain, 1.0f), wide, height_).Get(), FALSE, nullptr);
  dark_layer_ = compositor_.CreateVisual();
  pill_->AddVisual(dark_layer_.Get(), FALSE, nullptr);
  inner_->AddVisual(pill_.Get(), FALSE, nullptr);

  status_layer_ = compositor_.CreateVisual();
  inner_->AddVisual(status_layer_.Get(), FALSE, nullptr);
  icon_scale_ = compositor_.CreateScale();

  x_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  y_.Bind(root_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetY));
  opacity_.Bind(root_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  width_.Bind(frame_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  width_.Bind(inner_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight), -1.0f);
  width_.Bind(shadow_right_.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX), shadow_pad_ - shadow_right_width_);
  width_.Bind(shadow_stretch_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX),
              shadow_pad_ * 2.0f - shadow_left_width_ - shadow_right_width_);
  veil_alpha_.Bind(veil_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  status_alpha_.Bind(status_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  pill_left_.Bind(pill_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetLeft));
  pill_right_.Bind(pill_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetRight));
  pill_top_.Bind(pill_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetTop));
  pill_bottom_.Bind(pill_clip_.Get(), SSTK_SETTER(IDCompositionRectangleClip, SetBottom));
  pill_radius_.Bind(pill_clip_.Get(), SetClipRadii);
  pill_alpha_.Bind(pill_effect_.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  icon_pop_.Bind(icon_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleX));
  icon_pop_.Bind(icon_scale_.Get(), SSTK_SETTER(IDCompositionScaleTransform, SetScaleY));

  opacity_.Snap(0.0f);
  veil_alpha_.Snap(0.0f);
  status_alpha_.Snap(0.0f);
  width_.Snap(height_);
  pill_left_.Snap(pad_);
  pill_right_.Snap(pad_ + cell_);
  pill_top_.Snap(pad_);
  pill_bottom_.Snap(pad_ + segment_height_);
  pill_radius_.Snap(segment_height_ * 0.5f);
  pill_alpha_.Snap(1.0f);
}

Capsule::~Capsule() = default;

float Capsule::frost_width() const { return std::round(config::kFrostWidth * scale_); }

float Capsule::TextTop(const TextLine& line) const {
  // The capitals sit centered, whatever the descenders do.
  return std::round(height_ * 0.5f - (line.baseline - line.cap_height * 0.5f));
}

std::unique_ptr<Capsule::Segment> Capsule::MakeSegment(const Region& region, const TaskbarInsets& insets) {
  auto segment = std::make_unique<Segment>();
  segment->kind = region.kind;
  segment->rect = region.rect;

  // Glyphs come from the shot's real geometry. A maximized window merges with the "without taskbar" option, so the
  // taskbar's gap goes by area rather than by kind.
  GlyphGap taskbar_edge = GlyphGap::kNone;
  if (insets.bottom > 0) taskbar_edge = GlyphGap::kBottom;
  else if (insets.top > 0) taskbar_edge = GlyphGap::kTop;
  else if (insets.left > 0) taskbar_edge = GlyphGap::kLeft;
  else if (insets.right > 0) taskbar_edge = GlyphGap::kRight;
  const RECT usable{insets.left, insets.top, monitor_.cx - insets.right, monitor_.cy - insets.bottom};
  const RECT& rect = region.rect;
  const bool without_taskbar = std::abs(rect.left - usable.left) <= config::kSameRectTolerance &&
                               std::abs(rect.top - usable.top) <= config::kSameRectTolerance &&
                               std::abs(rect.right - usable.right) <= config::kSameRectTolerance &&
                               std::abs(rect.bottom - usable.bottom) <= config::kSameRectTolerance;
  const float mw = static_cast<float>(monitor_.cx);
  const float mh = static_cast<float>(monitor_.cy);
  const bool scroll = region.kind == RegionKind::kScroll || region.kind == RegionKind::kLong;
  const GlyphSpec spec{mw / mh, rect.left / mw, rect.top / mh, rect.right / mw, rect.bottom / mh,
                       without_taskbar ? taskbar_edge : GlyphGap::kNone, scroll};
  const int cell_w = static_cast<int>(cell_);
  const int cell_h = static_cast<int>(segment_height_);
  const Image light = RenderGlyph(spec, cell_w, cell_h, scale_, config::kGlyphOnGlass, config::kGlyphOnGlassAlpha);
  const Image dark = RenderGlyph(spec, cell_w, cell_h, scale_, config::kGlyphOnPorcelain, 1.0f);
  const int font = static_cast<int>(std::lround(config::kCapsuleFontSize * scale_));
  const TextLine label = region.kind == RegionKind::kScroll
                             ? ChordLine(config::kScrollHintBefore, config::kScrollHintAfter, config::kGlyphOnPorcelain, scale_)
                             : RenderText(LabelFor(region.kind), font, config::kGlyphOnPorcelain);

  // Laid out by what is visibly drawn: the label ends as far from the pill's edge as the glyph starts from the other.
  const RECT ink = InkBounds(dark);
  const RECT label_ink = InkBounds(label.image);
  segment->label_x = static_cast<float>(ink.right) + std::round(config::kLabelGap * scale_) - static_cast<float>(label_ink.left);
  segment->active_width = segment->label_x + static_cast<float>(label_ink.right + ink.left);

  segment->icon_effect = compositor_.CreateEffectGroup();
  const auto glyph = [&](const Image& image, IDCompositionVisual* layer) {
    ComPtr<IDCompositionVisual> visual = compositor_.CreateVisual();
    visual->SetContent(compositor_.CreateSurface(image, false).Get());
    visual->SetEffect(segment->icon_effect.Get());
    visual->SetOffsetY(pad_);
    layer->AddVisual(visual.Get(), FALSE, nullptr);
    return visual;
  };
  segment->light = glyph(light, light_layer_.Get());
  segment->dark = glyph(dark, dark_layer_.Get());

  segment->label = compositor_.CreateVisual();
  segment->label->SetContent(compositor_.CreateSurface(label.image, false).Get());
  segment->label_effect = compositor_.CreateEffectGroup();
  segment->label->SetEffect(segment->label_effect.Get());
  segment->label->SetOffsetY(TextTop(label));
  dark_layer_->AddVisual(segment->label.Get(), FALSE, nullptr);

  segment->x.Bind(segment->light.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  segment->x.Bind(segment->dark.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX));
  segment->x.Bind(segment->label.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX), segment->label_x);
  segment->icon_alpha.Bind(segment->icon_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  segment->label_alpha.Bind(segment->label_effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  segment->label_alpha.Snap(0.0f);
  return segment;
}

void Capsule::RemoveSegment(Segment& segment) {
  light_layer_->RemoveVisual(segment.light.Get());
  dark_layer_->RemoveVisual(segment.dark.Get());
  dark_layer_->RemoveVisual(segment.label.Get());
}

void Capsule::MakePiece(Piece& piece, const Image& image, float y) {
  RemovePiece(piece);
  piece.visual = compositor_.CreateVisual();
  piece.visual->SetContent(compositor_.CreateSurface(image, false).Get());
  piece.effect = compositor_.CreateEffectGroup();
  piece.visual->SetEffect(piece.effect.Get());
  piece.visual->SetOffsetY(y);
  const RECT ink = InkBounds(image);
  piece.ink_offset = static_cast<float>(ink.left);
  piece.ink_width = static_cast<float>(ink.right - ink.left);
  piece.alpha = AnimFloat{};
  piece.alpha.Bind(piece.effect.Get(), SSTK_SETTER(IDCompositionEffectGroup, SetOpacity));
  piece.alpha.Snap(0.0f);
  status_layer_->AddVisual(piece.visual.Get(), FALSE, nullptr);
}

void Capsule::PlacePiece(Piece& piece, float ink_x) { piece.visual->SetOffsetX(ink_x - piece.ink_offset); }

void Capsule::RemovePiece(Piece& piece) {
  if (piece.visual) status_layer_->RemoveVisual(piece.visual.Get());
  piece.visual.Reset();
  piece.effect.Reset();
}

void Capsule::BuildRecording() {
  if (recording_built_) return;
  recording_built_ = true;
  const int font = static_cast<int>(std::lround(config::kCapsuleFontSize * scale_));
  const float center = height_ * 0.5f;
  recording_inset_ = center - dot_ * 0.5f;
  recording_text_ = std::round(center + dot_ * 0.5f + config::kStatusGap * scale_);
  const TextLine waiting = RenderText(config::kRecordingWaiting, font, config::kStatusInk);
  const TextLine capturing = RenderText(config::kRecordingActive, font, config::kStatusInk);
  const TextLine finish = ChordLine(nullptr, config::kRecordingFinish, config::kStatusInk, scale_);
  MakePiece(waiting_, waiting.image, TextTop(waiting));
  PlacePiece(waiting_, recording_text_);
  MakePiece(capturing_, capturing.image, TextTop(capturing));
  PlacePiece(capturing_, recording_text_);
  MakePiece(finish_, finish.image, TextTop(finish));
  finish_x_ = AnimFloat{};
  finish_x_.Bind(finish_.visual.Get(), SSTK_SETTER(IDCompositionVisual, SetOffsetX), -finish_.ink_offset);
}

float Capsule::SegmentWidth(int index) const { return index == selected_ ? segments_[index]->active_width : cell_; }

void Capsule::Arrange(const TimeBase& now, bool immediate) {
  IDCompositionDevice* dc = compositor_.device();
  float x = pad_;
  for (size_t i = 0; i < segments_.size(); ++i) {
    Segment& segment = *segments_[i];
    // A newcomer appears in its place rather than sliding in from the edge.
    if (immediate || !segment.placed) {
      segment.x.Snap(x);
      segment.placed = true;
    } else {
      segment.x.To(dc, now, x, config::kSegmentSlide);
    }
    x += SegmentWidth(static_cast<int>(i));
  }
  const float width = x + pad_;
  const Segment& chosen = *segments_[selected_];
  const float left = chosen.x.target();
  const float right = left + chosen.active_width;
  if (immediate) {
    width_.Snap(width);
    pill_left_.Snap(left);
    pill_right_.Snap(right);
    pill_top_.Snap(pad_);
    pill_bottom_.Snap(pad_ + segment_height_);
    pill_radius_.Snap(segment_height_ * 0.5f);
  } else {
    width_.To(dc, now, width, config::kCapsuleResize);
    ShapePill(left, right, pad_, pad_ + segment_height_, now, config::kPillMorph);
  }
}

// The leading edge springs ahead of the trailing one, which gives the pill a soft, liquid stretch.
void Capsule::ShapePill(float left, float right, float top, float bottom, const TimeBase& now, const SpringSpec& spec) {
  IDCompositionDevice* dc = compositor_.device();
  const bool moving_right = right > pill_right_.target();
  pill_left_.To(dc, now, left, moving_right ? config::kPillTrailing : config::kPillLeading);
  pill_right_.To(dc, now, right, moving_right ? config::kPillLeading : config::kPillTrailing);
  pill_top_.To(dc, now, top, spec);
  pill_bottom_.To(dc, now, bottom, spec);
  pill_radius_.To(dc, now, (bottom - top) * 0.5f, spec);
}

void Capsule::Bind(const std::vector<Region>& regions, const TaskbarInsets& insets, int selected, IDCompositionSurface* frost,
                   const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const bool was_visible = visible_;
  if (mode_ != Mode::kSelector) LeaveStatus(now);
  if (regions.size() < 2) {
    Hide(now);
    return;
  }

  frost_->SetContent(frost);
  if (frost != nullptr) {
    // The frost bitmap spans the capsule's height and its widest reach at a fixed downscale.
    const float frost_w = static_cast<float>(std::max(static_cast<int>(frost_width()) / config::kFrostDownscale, 1));
    const float frost_h = static_cast<float>(std::max(static_cast<int>(height_) / config::kFrostDownscale, 1));
    frost_->SetTransform(D2D_MATRIX_3X2_F{frost_width() / frost_w, 0.0f, 0.0f, height_ / frost_h, 0.0f, 0.0f});
  }

  // Options already on show keep their visuals, so that whatever stays the same does not flicker.
  std::vector<std::unique_ptr<Segment>> next;
  next.reserve(regions.size());
  for (const Region& region : regions) {
    const auto same = std::find_if(segments_.begin(), segments_.end(), [&](const std::unique_ptr<Segment>& segment) {
      return segment != nullptr && segment->kind == region.kind && EqualRect(&segment->rect, &region.rect);
    });
    if (same != segments_.end()) {
      next.push_back(std::move(*same));
    } else {
      next.push_back(MakeSegment(region, insets));
      next.back()->icon_alpha.Snap(was_visible ? 0.0f : 1.0f);
    }
  }
  for (auto& left_over : segments_) {
    if (left_over == nullptr) continue;
    left_over->icon_alpha.To(dc, now, 0.0f, config::kQuickFade);
    left_over->label_alpha.To(dc, now, 0.0f, config::kQuickFade);
    left_over->retire_at = std::max(left_over->icon_alpha.end_time(), left_over->label_alpha.end_time());
    retiring_.push_back(std::move(left_over));
  }
  segments_ = std::move(next);
  selected_ = std::clamp(selected, 0, static_cast<int>(segments_.size()) - 1);
  visible_ = true;
  hover_ = -1;
  veil_alpha_.To(dc, now, 0.0f, config::kQuickFade);

  for (size_t i = 0; i < segments_.size(); ++i) {
    Segment& segment = *segments_[i];
    const bool chosen = static_cast<int>(i) == selected_;
    if (was_visible) {
      segment.icon_alpha.To(dc, now, 1.0f, config::kFade, config::kLabelDelay);
      segment.label_alpha.To(dc, now, chosen ? 1.0f : 0.0f, chosen ? config::kFade : config::kQuickFade, chosen ? config::kLabelDelay : 0.0);
    } else {
      segment.icon_alpha.Snap(1.0f);
      segment.label_alpha.Snap(chosen ? 1.0f : 0.0f);
    }
  }

  if (was_visible) {
    Arrange(now, false);
    opacity_.To(dc, now, 1.0f, config::kFade);
  } else if (opacity_.ValueAt(now.seconds) > 0.02f) {
    // Called back while still leaving: it turns around from where it is.
    Arrange(now, true);
    x_.To(dc, now, target_x_, config::kReflow);
    opacity_.To(dc, now, 1.0f, config::kFade);
    y_.To(dc, now, target_y_, config::kCapsuleEnter);
  } else {
    Arrange(now, true);
    // The capsule rises into place just after the card starts flying.
    x_.Snap(target_x_);
    y_.Snap(target_y_ + config::kCapsuleRise * scale_);
    opacity_.Snap(0.0f);
    opacity_.To(dc, now, 1.0f, config::kFade, config::kCapsuleEnterDelay);
    y_.To(dc, now, target_y_, config::kCapsuleEnter, config::kCapsuleEnterDelay);
  }
}

void Capsule::MoveTo(float x, float y, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  const bool first = target_x_ == 0.0f && target_y_ == 0.0f;
  target_x_ = x;
  target_y_ = y;
  if (first) {
    x_.Snap(x);
    y_.Snap(y);
    return;
  }
  // Out of sight it only remembers: the next showing starts from these.
  if (!visible_) return;
  x_.To(dc, now, x, config::kReflow);
  y_.To(dc, now, y, config::kReflow);
}

void Capsule::Select(int index, const TimeBase& now) {
  if (mode_ != Mode::kSelector || index < 0 || index >= static_cast<int>(segments_.size()) || index == selected_) return;
  IDCompositionDevice* dc = compositor_.device();
  segments_[selected_]->label_alpha.To(dc, now, 0.0f, config::kQuickFade);
  selected_ = index;
  segments_[index]->label_alpha.To(dc, now, 1.0f, config::kFade, config::kLabelDelay);
  Arrange(now, false);
  if (hover_ == index) SetHover(-1, now);
}

void Capsule::SetHover(int index, const TimeBase& now) {
  if (mode_ == Mode::kOutcome || (mode_ == Mode::kSelector && index == selected_)) index = -1;
  if (index == hover_) return;
  hover_ = index;
  IDCompositionDevice* dc = compositor_.device();
  if (index < 0) {
    veil_alpha_.To(dc, now, 0.0f, config::kQuickFade);
    return;
  }
  if (mode_ == Mode::kRecording) {
    // The whole capsule is one button now: pressing it finishes.
    veil_clip_->SetLeft(0.0f);
    veil_clip_->SetRight(frost_width());
    veil_clip_->SetTop(0.0f);
    veil_clip_->SetBottom(height_);
    SetClipRadii(veil_clip_.Get(), height_ * 0.5f, nullptr);
  } else {
    const float left = segments_[index]->x.target();
    veil_clip_->SetLeft(left);
    veil_clip_->SetRight(left + cell_);
    veil_clip_->SetTop(pad_);
    veil_clip_->SetBottom(pad_ + segment_height_);
    SetClipRadii(veil_clip_.Get(), segment_height_ * 0.5f, nullptr);
  }
  veil_alpha_.To(dc, now, 1.0f, config::kQuickFade);
}

void Capsule::Hide(const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  visible_ = false;
  hover_ = -1;
  // It sinks the way it came as it fades, the mirror of its entrance.
  opacity_.To(dc, now, 0.0f, config::kCapsuleLeave);
  y_.To(dc, now, target_y_ + config::kCapsuleRise * scale_, config::kCapsuleLeave);
  veil_alpha_.To(dc, now, 0.0f, config::kQuickFade);
}

void Capsule::ShowRecording(bool capturing, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  BuildRecording();
  const bool entering = mode_ != Mode::kRecording;
  if (!entering && capturing == capturing_now_) return;
  if (entering) {
    mode_ = Mode::kRecording;
    hover_ = -1;
    veil_alpha_.To(dc, now, 0.0f, config::kQuickFade);
    for (auto& segment : segments_) {
      segment->icon_alpha.To(dc, now, 0.0f, config::kQuickFade);
      segment->label_alpha.To(dc, now, 0.0f, config::kQuickFade);
    }
    // The pill gathers into a dot, concentric with the left cap, and starts to breathe once it is there.
    const float center = height_ * 0.5f;
    const float radius = dot_ * 0.5f;
    ShapePill(center - radius, center + radius, center - radius, center + radius, now, config::kPillMorph);
    Breathe(now);
    finish_.alpha.To(dc, now, config::kFinishHintAlpha, config::kFade, config::kLabelDelay * 2.0);
  }
  capturing_now_ = capturing;
  Piece& shown = capturing ? capturing_ : waiting_;
  Piece& gone = capturing ? waiting_ : capturing_;
  gone.alpha.To(dc, now, 0.0f, config::kQuickFade);
  shown.alpha.To(dc, now, config::kStatusInkAlpha, config::kFade, config::kLabelDelay);
  const float finish_left = std::round(recording_text_ + shown.ink_width + config::kFinishHintGap * scale_);
  if (entering) {
    finish_x_.Snap(finish_left);
  } else {
    finish_x_.To(dc, now, finish_left, config::kSegmentSlide);
  }
  width_.To(dc, now, finish_left + finish_.ink_width + recording_inset_, config::kCapsuleResize);
}

void Capsule::ShowOutcome(ScrollOutcome outcome, const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  mode_ = Mode::kOutcome;
  hover_ = -1;
  veil_alpha_.To(dc, now, 0.0f, config::kQuickFade);
  StopBreathing(now);
  for (Piece* piece : {&waiting_, &capturing_, &finish_}) piece->alpha.To(dc, now, 0.0f, config::kQuickFade);

  // The dot shrinks away into the outcome's mark, which pops in at the same place as the color floods the glass.
  const float center = height_ * 0.5f;
  ShapePill(center, center, center, center, now, config::kFade);
  pill_alpha_.To(dc, now, 0.0f, config::kQuickFade);

  uint32_t color = config::kScrollFailed;
  StatusIcon mark = StatusIcon::kCross;
  const wchar_t* text = config::kResultFailed;
  if (outcome == ScrollOutcome::kSucceeded) {
    color = config::kScrollSucceeded;
    mark = StatusIcon::kCheck;
    text = config::kResultSucceeded;
  } else if (outcome == ScrollOutcome::kPartial) {
    color = config::kScrollPartial;
    mark = StatusIcon::kExclamation;
    text = config::kResultPartial;
  }
  status_fill_->SetContent(compositor_.SolidColor(Argb(color, 1.0f)));
  status_alpha_.To(dc, now, 1.0f, config::kFade);

  const int size = static_cast<int>(std::round(config::kStatusIconSize * scale_));
  const float corner = std::round((height_ - static_cast<float>(size)) * 0.5f);
  MakePiece(outcome_icon_, RenderStatusIcon(mark, size, scale_, config::kStatusInk), corner);
  outcome_icon_.visual->SetOffsetX(corner);
  icon_scale_->SetCenterX(static_cast<float>(size) * 0.5f);
  icon_scale_->SetCenterY(static_cast<float>(size) * 0.5f);
  outcome_icon_.visual->SetTransform(icon_scale_.Get());
  icon_pop_.Snap(config::kResultPopFrom);
  icon_pop_.To(dc, now, 1.0f, config::kResultPop);
  outcome_icon_.alpha.To(dc, now, config::kStatusInkAlpha, config::kFade);

  const TextLine label = RenderText(text, static_cast<int>(std::lround(config::kCapsuleFontSize * scale_)), config::kStatusInk);
  MakePiece(outcome_label_, label.image, TextTop(label));
  const float inset = corner + outcome_icon_.ink_offset;
  const float label_left = std::round(inset + outcome_icon_.ink_width + config::kStatusGap * scale_);
  PlacePiece(outcome_label_, label_left);
  outcome_label_.alpha.To(dc, now, config::kStatusInkAlpha, config::kFade, config::kLabelDelay);
  width_.To(dc, now, label_left + outcome_label_.ink_width + inset, config::kCapsuleResize);
}

void Capsule::Breathe(const TimeBase& now) {
  ComPtr<IDCompositionAnimation> breath;
  if (FAILED(compositor_.device()->CreateAnimation(&breath))) return;
  // DWM runs the breathing on its own, for as long as the recording lasts.
  breath->SetAbsoluteBeginTime(now.ticks);
  breath->AddCubic(0.0, 1.0f, 0.0f, 0.0f, 0.0f);
  breath->AddSinusoidal(config::kBreathDelay, (1.0f + config::kBreathLow) * 0.5f, (1.0f - config::kBreathLow) * 0.5f,
                        config::kBreathHertz, 90.0f);
  breath->End(config::kBreathDelay + 3600.0, 1.0f);
  pill_effect_->SetOpacity(breath.Get());
  breath_start_ = now.seconds + config::kBreathDelay;
  breathing_ = true;
}

// Takes the breathing over where it is, so that whatever comes next starts from there.
void Capsule::StopBreathing(const TimeBase& now) {
  if (!breathing_) return;
  breathing_ = false;
  float value = 1.0f;
  if (now.seconds > breath_start_) {
    const double phase = kTwoPi * config::kBreathHertz * (now.seconds - breath_start_) + kTwoPi * 0.25;
    value = (1.0f + config::kBreathLow) * 0.5f + (1.0f - config::kBreathLow) * 0.5f * static_cast<float>(std::sin(phase));
  }
  pill_alpha_.Snap(value);
}

void Capsule::LeaveStatus(const TimeBase& now) {
  IDCompositionDevice* dc = compositor_.device();
  mode_ = Mode::kSelector;
  StopBreathing(now);
  status_alpha_.To(dc, now, 0.0f, config::kFade);
  for (Piece* piece : {&waiting_, &capturing_, &finish_, &outcome_icon_, &outcome_label_}) {
    piece->alpha.To(dc, now, 0.0f, config::kQuickFade);
  }
  // The pill grows back out of the left cap toward the selected option.
  pill_alpha_.To(dc, now, 1.0f, config::kFade, config::kResultReturnDelay);
}

void Capsule::Clear() {
  for (auto& segment : segments_) RemoveSegment(*segment);
  for (auto& segment : retiring_) RemoveSegment(*segment);
  segments_.clear();
  retiring_.clear();
  for (Piece* piece : {&waiting_, &capturing_, &finish_, &outcome_icon_, &outcome_label_}) RemovePiece(*piece);
  recording_built_ = false;
  frost_->SetContent(nullptr);
  mode_ = Mode::kSelector;
  breathing_ = false;
  capturing_now_ = false;
  pill_alpha_.Snap(1.0f);
  status_alpha_.Snap(0.0f);
  opacity_.Snap(0.0f);
  veil_alpha_.Snap(0.0f);
  visible_ = false;
  hover_ = -1;
  selected_ = 0;
}

void Capsule::Tick(double now) {
  for (auto it = retiring_.begin(); it != retiring_.end();) {
    if (now >= (*it)->retire_at) {
      RemoveSegment(**it);
      it = retiring_.erase(it);
    } else {
      ++it;
    }
  }
}

RECT Capsule::Bounds() const {
  return RECT{static_cast<LONG>(target_x_), static_cast<LONG>(target_y_), static_cast<LONG>(target_x_ + width_.target()),
              static_cast<LONG>(target_y_ + height_)};
}

bool Capsule::Contains(int x, int y) const {
  if (!visible_) return false;
  const RECT bounds = Bounds();
  return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

int Capsule::HitTest(int x, int y) const {
  if (mode_ != Mode::kSelector || segments_.empty() || !Contains(x, y)) return -1;
  const int count = static_cast<int>(segments_.size());
  for (int i = 0; i < count; ++i) {
    const float left = target_x_ + segments_[i]->x.target();
    if (x >= left && x < left + SegmentWidth(i)) return i;
  }
  // The padding at either end still belongs to the nearest option.
  return x < target_x_ + segments_.front()->x.target() ? 0 : count - 1;
}

}  // namespace sstk
