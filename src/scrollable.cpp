#include "scrollable.h"

#include "config/core.h"
#include "screen.h"

#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <utility>

namespace sstk {

namespace {

constexpr int kMaxTreeDepth = 256;
constexpr size_t kMaxSiblingsCompared = 64;
constexpr size_t kMaxVerified = 12;
constexpr size_t kMaxEnclosed = 4;
constexpr int kMaxAncestorSteps = 160;
constexpr int kTextProbe = 2048;
// Where to look for an area on screen, as shares of its width and height, center first.
constexpr double kProbes[][2] = {{0.5, 0.5}, {0.25, 0.3}, {0.75, 0.3}, {0.25, 0.7}, {0.75, 0.7}, {0.5, 0.12}, {0.5, 0.88}, {0.1, 0.5}, {0.9, 0.5}};

using RuntimeId = std::vector<int>;

long Area(const RECT& rect) { return (rect.right - rect.left) * (rect.bottom - rect.top); }

bool SameArea(const RECT& a, const RECT& b) {
  constexpr int kTolerance = 4;
  return std::abs(a.left - b.left) <= kTolerance && std::abs(a.top - b.top) <= kTolerance &&
         std::abs(a.right - b.right) <= kTolerance && std::abs(a.bottom - b.bottom) <= kTolerance;
}

bool SameAreas(const std::vector<RECT>& a, const std::vector<RECT>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; });
}

bool HasSize(const RECT& rect, int width, int height) { return rect.right - rect.left >= width && rect.bottom - rect.top >= height; }

RuntimeId ReadRuntimeId(IUIAutomationElement* element) {
  RuntimeId id;
  VARIANT value{};
  if (FAILED(element->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &value)) || value.vt != (VT_I4 | VT_ARRAY)) {
    VariantClear(&value);
    if (FAILED(element->GetCurrentPropertyValue(UIA_RuntimeIdPropertyId, &value))) return id;
  }
  if (value.vt == (VT_I4 | VT_ARRAY) && value.parray != nullptr) {
    LONG lower = 0;
    LONG upper = -1;
    int* data = nullptr;
    if (SUCCEEDED(SafeArrayGetLBound(value.parray, 1, &lower)) && SUCCEEDED(SafeArrayGetUBound(value.parray, 1, &upper)) && upper >= lower &&
        SUCCEEDED(SafeArrayAccessData(value.parray, reinterpret_cast<void**>(&data)))) {
      id.assign(data, data + (upper - lower + 1));
      SafeArrayUnaccessData(value.parray);
    }
  }
  VariantClear(&value);
  return id;
}

bool CachedTrue(IUIAutomationElement* element, PROPERTYID property) {
  VARIANT value{};
  const bool yes = SUCCEEDED(element->GetCachedPropertyValue(property, &value)) && value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
  VariantClear(&value);
  return yes;
}

bool CachedNumber(IUIAutomationElement* element, PROPERTYID property, double* number) {
  VARIANT value{};
  const bool known = SUCCEEDED(element->GetCachedPropertyValue(property, &value)) && value.vt == VT_R8;
  if (known) *number = value.dblVal;
  VariantClear(&value);
  return known;
}

// Browsers report text everywhere, clipped or not, and say what scrolls in other ways.
bool DescribesTextLikeABrowser(IUIAutomationElement* element) {
  VARIANT value{};
  bool browser = false;
  if (SUCCEEDED(element->GetCachedPropertyValue(UIA_FrameworkIdPropertyId, &value)) && value.vt == VT_BSTR && value.bstrVal != nullptr) {
    browser = _wcsicmp(value.bstrVal, L"Chrome") == 0 || _wcsicmp(value.bstrVal, L"Gecko") == 0;
  }
  VariantClear(&value);
  return browser;
}

bool HasText(IUIAutomationTextRange* range) {
  BSTR text = nullptr;
  if (FAILED(range->GetText(kTextProbe, &text)) || text == nullptr) return false;
  bool any = false;
  for (const wchar_t* c = text; *c != L'\0' && !any; ++c) any = !std::iswspace(*c);
  SysFreeString(text);
  return any;
}

struct Node {
  RECT rect{};
  bool offscreen = false;
  int set_size = 0;
  ComPtr<IUIAutomationElement> element;
  std::vector<Node> children;
};

bool Shows(const Node& node) { return !node.offscreen && node.rect.right > node.rect.left && node.rect.bottom > node.rect.top; }

struct Candidate {
  RECT area{};  // What shows of it, in virtual screen coordinates.
  ComPtr<IUIAutomationElement> element;
};

class Look {
 public:
  Look(IUIAutomation* automation, HWND window, const RECT& limit, float scale, std::vector<RECT>& shown)
      : automation_(automation), window_(window), limit_(limit), shown_(shown),
        min_width_(static_cast<int>(std::lround(config::kScrollAreaMinWidth * scale))),
        min_height_(static_cast<int>(std::lround(config::kScrollAreaMinHeight * scale))),
        hidden_min_(static_cast<int>(std::lround(config::kScrollHiddenMinHeight * scale))),
        reach_(static_cast<int>(std::lround(config::kScrollEdgeReach * scale))) {}

  void Run() {
    if (!Prepare()) return;
    FindScrollers();
    Settle();
    // What a scroller says settles it. Anything else takes reading the whole tree.
    if (!overflows_) {
      ReadTree();
      Settle();
    }
    if (!overflows_) {
      FindText();
      Settle();
    }
    // An app that describes nothing of its content still has windows that say how far they scroll.
    if (!overflows_) {
      FindWindows();
      Settle();
    }
  }

  bool overflows() const { return overflows_; }

  ScrollHints TakeHints(const RECT& monitor) {
    std::sort(areas_.begin(), areas_.end(), [](const RECT& a, const RECT& b) { return Area(a) > Area(b); });
    for (RECT& area : areas_) OffsetRect(&area, -monitor.left, -monitor.top);
    for (RECT& frame : frames_) OffsetRect(&frame, -monitor.left, -monitor.top);
    return ScrollHints{std::move(areas_), std::move(frames_)};
  }

 private:
  bool Prepare() {
    if (FAILED(automation_->ElementFromHandle(window_, &root_)) || !root_) return false;
    if (FAILED(automation_->get_RawViewCondition(&raw_)) || !raw_) return false;
    if (FAILED(automation_->get_RawViewWalker(&walker_)) || !walker_) return false;
    if (FAILED(automation_->CreateCacheRequest(&identity_)) || FAILED(identity_->put_TreeFilter(raw_.Get())) ||
        FAILED(identity_->AddProperty(UIA_RuntimeIdPropertyId))) {
      return false;
    }
    if (FAILED(automation_->CreateCacheRequest(&placed_)) || FAILED(placed_->put_TreeFilter(raw_.Get())) ||
        FAILED(placed_->AddProperty(UIA_RuntimeIdPropertyId)) || FAILED(placed_->AddProperty(UIA_BoundingRectanglePropertyId))) {
      return false;
    }
    root_id_ = ReadRuntimeId(root_.Get());
    return !root_id_.empty();
  }

  RECT Clip(const RECT& rect) const {
    RECT shown{};
    return IntersectRect(&shown, &rect, &limit_) ? shown : RECT{};
  }

  void Consider(const RECT& rect, IUIAutomationElement* element) {
    const RECT area = Clip(rect);
    if (!HasSize(area, min_width_, min_height_) || element == nullptr) return;
    candidates_.push_back(Candidate{area, element});
  }

  // Every scroller in the raw view, plain boxes included, that shows only part of what it holds.
  void FindScrollers() {
    VARIANT yes{};
    yes.vt = VT_BOOL;
    yes.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> has_pattern;
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation_->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, yes, &has_pattern)) || FAILED(automation_->CreateCacheRequest(&cache))) return;
    cache->put_TreeFilter(raw_.Get());
    for (const PROPERTYID property : {UIA_BoundingRectanglePropertyId, UIA_ScrollVerticallyScrollablePropertyId, UIA_ScrollVerticalViewSizePropertyId,
                                      UIA_ScrollVerticalScrollPercentPropertyId, UIA_IsOffscreenPropertyId, UIA_RuntimeIdPropertyId}) {
      if (FAILED(cache->AddProperty(property))) return;
    }
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root_->FindAllBuildCache(TreeScope_Subtree, has_pattern.Get(), cache.Get(), &found)) || !found) return;
    int count = 0;
    found->get_Length(&count);
    for (int i = 0; i < count; ++i) {
      ComPtr<IUIAutomationElement> element;
      RECT rect{};
      if (FAILED(found->GetElement(i, &element)) || !element || FAILED(element->get_CachedBoundingRectangle(&rect))) continue;
      BOOL offscreen = FALSE;
      if (SUCCEEDED(element->get_CachedIsOffscreen(&offscreen)) && offscreen) continue;
      // Scrolling sideways only, or not at all, it is no part of what scrolls up and down.
      if (!CachedTrue(element.Get(), UIA_ScrollVerticallyScrollablePropertyId)) continue;
      double view = 100.0;
      double percent = 0.0;
      if (CachedNumber(element.Get(), UIA_ScrollVerticalViewSizePropertyId, &view) && view > config::kScrollMaxViewPercent) continue;
      if (CachedNumber(element.Get(), UIA_ScrollVerticalScrollPercentPropertyId, &percent) && percent < 0.0) continue;
      Consider(rect, element.Get());
    }
  }

  void ReadNode(IUIAutomationElement* element, Node& node, int depth) {
    element->get_CachedBoundingRectangle(&node.rect);
    BOOL offscreen = FALSE;
    element->get_CachedIsOffscreen(&offscreen);
    node.offscreen = offscreen != FALSE;
    node.element = element;
    VARIANT value{};
    if (SUCCEEDED(element->GetCachedPropertyValue(UIA_SizeOfSetPropertyId, &value)) && value.vt == VT_I4) node.set_size = value.lVal;
    VariantClear(&value);
    ComPtr<IUIAutomationElementArray> children;
    if (depth >= kMaxTreeDepth || FAILED(element->GetCachedChildren(&children)) || !children) return;
    int count = 0;
    children->get_Length(&count);
    node.children.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      ComPtr<IUIAutomationElement> child;
      if (SUCCEEDED(children->GetElement(i, &child)) && child) ReadNode(child.Get(), node.children[static_cast<size_t>(i)], depth + 1);
    }
  }

  void ReadTree() {
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation_->CreateCacheRequest(&cache))) return;
    cache->put_TreeFilter(raw_.Get());
    cache->put_TreeScope(TreeScope_Subtree);
    for (const PROPERTYID property : {UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId, UIA_SizeOfSetPropertyId, UIA_RuntimeIdPropertyId}) {
      if (FAILED(cache->AddProperty(property))) return;
    }
    ComPtr<IUIAutomationElement> snapshot;
    if (FAILED(root_->BuildUpdatedCache(cache.Get(), &snapshot)) || !snapshot) return;
    Node tree;
    ReadNode(snapshot.Get(), tree, 0);
    Walk(tree, limit_);
  }

  void Walk(const Node& node, const RECT& clip) {
    if (!node.children.empty()) CheckOverflow(node, clip);
    RECT inner{};
    const RECT& next = IntersectRect(&inner, &clip, &node.rect) ? inner : clip;
    for (const Node& child : node.children) Walk(child, next);
  }

  // What shows of a box reaching its clipped edge, with more of it lined up past that edge out of sight, is content that
  // runs on beyond the box. So is a list claiming more items than it renders, rendered up to an edge. Content hidden
  // anywhere else, as in a collapsed panel, a closed menu, or the other pages of a carousel, is not.
  void CheckOverflow(const Node& node, const RECT& clip) {
    RECT view{};
    if (!IntersectRect(&view, &node.rect, &clip)) return;
    view = Clip(view);
    if (!HasSize(view, min_width_, min_height_)) return;
    std::vector<const Node*> shown;
    std::vector<const Node*> hidden;
    for (const Node& child : node.children) {
      if (child.rect.right - child.rect.left < 8 || child.rect.bottom - child.rect.top < 4) continue;
      if (child.offscreen) {
        if (hidden.size() < kMaxSiblingsCompared) hidden.push_back(&child);
      } else if (Shows(child) && shown.size() < kMaxSiblingsCompared) {
        shown.push_back(&child);
      }
    }
    if (shown.empty()) return;
    LONG shown_top = LONG_MAX;
    LONG shown_bottom = LONG_MIN;
    for (const Node* in : shown) {
      shown_top = std::min(shown_top, in->rect.top);
      shown_bottom = std::max(shown_bottom, in->rect.bottom);
    }
    const bool reaches_bottom = shown_bottom >= view.bottom - reach_;
    const bool reaches_top = shown_top <= view.top + reach_;
    if (!reaches_bottom && !reaches_top) return;

    int hidden_height = 0;
    for (const Node* out : hidden) {
      const bool past = (reaches_bottom && out->rect.top >= view.bottom - reach_) || (reaches_top && out->rect.bottom <= view.top + reach_);
      if (!past) continue;
      for (const Node* in : shown) {
        const LONG overlap = std::min(out->rect.right, in->rect.right) - std::max(out->rect.left, in->rect.left);
        const LONG narrower = std::min(out->rect.right - out->rect.left, in->rect.right - in->rect.left);
        if (overlap * 2 >= narrower) {
          hidden_height += out->rect.bottom - out->rect.top;
          break;
        }
      }
    }
    bool stacked = hidden_height >= hidden_min_;
    if (!stacked) {
      int largest = 0;
      for (const Node& child : node.children) largest = std::max(largest, child.set_size);
      int rendered = 0;
      LONG lowest = LONG_MIN;
      int rising = 0;
      for (const Node& child : node.children) {
        if (child.set_size != largest || largest == 0) continue;
        ++rendered;
        if (Shows(child) && child.rect.top >= lowest) {
          lowest = child.rect.bottom - 2;
          ++rising;
        }
      }
      stacked = largest - rendered >= config::kScrollHiddenSetItems && rising >= 3;
    }
    if (stacked) Consider(view, node.element.Get());
  }

  // Text documents that say what part of their text shows, as terminals and editors do, with text beyond it.
  void FindText() {
    VARIANT yes{};
    yes.vt = VT_BOOL;
    yes.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> has_pattern;
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation_->CreatePropertyCondition(UIA_IsTextPatternAvailablePropertyId, yes, &has_pattern)) || FAILED(automation_->CreateCacheRequest(&cache))) return;
    cache->put_TreeFilter(raw_.Get());
    for (const PROPERTYID property : {UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId, UIA_FrameworkIdPropertyId, UIA_RuntimeIdPropertyId}) {
      if (FAILED(cache->AddProperty(property))) return;
    }
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root_->FindAllBuildCache(TreeScope_Subtree, has_pattern.Get(), cache.Get(), &found)) || !found) return;
    const double least = static_cast<double>(Area(limit_)) * config::kScrollTextMinShare;
    int count = 0;
    found->get_Length(&count);
    for (int i = 0; i < count; ++i) {
      ComPtr<IUIAutomationElement> element;
      RECT rect{};
      if (FAILED(found->GetElement(i, &element)) || !element || FAILED(element->get_CachedBoundingRectangle(&rect))) continue;
      BOOL offscreen = FALSE;
      if ((SUCCEEDED(element->get_CachedIsOffscreen(&offscreen)) && offscreen) || DescribesTextLikeABrowser(element.Get())) continue;
      if (static_cast<double>(Area(Clip(rect))) < least) continue;
      ComPtr<IUIAutomationTextPattern> text;
      ComPtr<IUIAutomationTextRange> document;
      ComPtr<IUIAutomationTextRangeArray> visible;
      if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) || !text || FAILED(text->get_DocumentRange(&document)) || !document ||
          FAILED(text->GetVisibleRanges(&visible)) || !visible) {
        continue;
      }
      int ranges = 0;
      visible->get_Length(&ranges);
      ComPtr<IUIAutomationTextRange> first;
      ComPtr<IUIAutomationTextRange> last;
      if (ranges <= 0 || FAILED(visible->GetElement(0, &first)) || FAILED(visible->GetElement(ranges - 1, &last)) || !first || !last) continue;
      ComPtr<IUIAutomationTextRange> before;
      ComPtr<IUIAutomationTextRange> after;
      if (FAILED(document->Clone(&before)) || FAILED(document->Clone(&after)) || !before || !after) continue;
      const bool above = SUCCEEDED(before->MoveEndpointByRange(TextPatternRangeEndpoint_End, first.Get(), TextPatternRangeEndpoint_Start)) && HasText(before.Get());
      const bool below = !above && SUCCEEDED(after->MoveEndpointByRange(TextPatternRangeEndpoint_Start, last.Get(), TextPatternRangeEndpoint_End)) && HasText(after.Get());
      if (above || below) Consider(rect, element.Get());
    }
  }

  // A window that keeps a range to scroll through longer than the page it shows, itself or through a scroll bar
  // window of its own, holds more than it shows. Its own word for it, where it has no other.
  void FindWindows() {
    EnumChildWindows(
        window_,
        [](HWND child, LPARAM self) -> BOOL {
          reinterpret_cast<Look*>(self)->ConsiderWindow(child);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
  }

  void ConsiderWindow(HWND child) {
    if (!IsWindowVisible(child)) return;
    wchar_t name[16]{};
    GetClassNameW(child, name, static_cast<int>(std::size(name)));
    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    const bool bar = _wcsicmp(name, L"ScrollBar") == 0;
    if (bar ? (style & SBS_VERT) == 0 || !IsWindowEnabled(child) : (style & WS_VSCROLL) == 0) return;
    // A range a window was only born with, and has no use for, says nothing.
    SCROLLBARINFO state{sizeof(state)};
    if (!bar && (!GetScrollBarInfo(child, OBJID_VSCROLL, &state) || (state.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE)) != 0)) return;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
    if (!GetScrollInfo(child, bar ? SB_CTL : SB_VERT, &info) || info.nMax - info.nMin + 1 <= static_cast<int>(std::max<UINT>(info.nPage, 1))) return;
    const HWND scrolled = bar ? GetParent(child) : child;
    RECT rect{};
    ComPtr<IUIAutomationElement> element;
    if (scrolled == nullptr || !GetWindowRect(scrolled, &rect) || FAILED(automation_->ElementFromHandle(scrolled, &element))) return;
    Consider(rect, element.Get());
  }

  // The parts of the window around an area, each as far as the ones around it let it show.
  void Enclose(const Candidate& candidate) {
    std::vector<RECT> chain;
    ComPtr<IUIAutomationElement> current = candidate.element;
    for (int step = 0; current && step < kMaxAncestorSteps && ReadRuntimeId(current.Get()) != root_id_; ++step) {
      ComPtr<IUIAutomationElement> parent;
      RECT rect{};
      if (FAILED(walker_->GetParentElementBuildCache(current.Get(), placed_.Get(), &parent)) || !parent || FAILED(parent->get_CachedBoundingRectangle(&rect))) break;
      chain.push_back(rect);
      current = parent;
    }
    RECT clip = limit_;
    for (auto outer = chain.rbegin(); outer != chain.rend(); ++outer) {
      RECT shown{};
      if (IntersectRect(&shown, &*outer, &clip)) clip = shown;
      *outer = clip;
    }
    for (const RECT& frame : chain) {
      if (EqualRect(&frame, &candidate.area) || std::any_of(frames_.begin(), frames_.end(), [&](const RECT& kept) { return EqualRect(&kept, &frame) != FALSE; })) continue;
      frames_.push_back(frame);
    }
  }

  // An area counts only where the screen really shows it: at some place inside it, the window there is this one, and
  // what the app says is at that place lies within the area.
  bool Visible(const Candidate& candidate) {
    const RuntimeId id = ReadRuntimeId(candidate.element.Get());
    if (id.empty()) return false;
    const RECT& area = candidate.area;
    for (const auto& probe : kProbes) {
      const POINT point{area.left + static_cast<LONG>((area.right - area.left) * probe[0]), area.top + static_cast<LONG>((area.bottom - area.top) * probe[1])};
      const HWND hit = WindowFromPoint(point);
      if (hit == nullptr || GetAncestor(hit, GA_ROOT) != window_) continue;
      ComPtr<IUIAutomationElement> at;
      if (FAILED(automation_->ElementFromPointBuildCache(point, identity_.Get(), &at)) || !at) continue;
      if (Within(at.Get(), id)) return true;
    }
    return false;
  }

  bool Within(IUIAutomationElement* element, const RuntimeId& id) {
    ComPtr<IUIAutomationElement> current = element;
    for (int step = 0; current && step < kMaxAncestorSteps; ++step) {
      const RuntimeId here = ReadRuntimeId(current.Get());
      if (here.empty() || here == root_id_) return here == id;
      if (here == id) return true;
      ComPtr<IUIAutomationElement> parent;
      if (FAILED(walker_->GetParentElementBuildCache(current.Get(), identity_.Get(), &parent))) return false;
      current = parent;
    }
    return false;
  }

  void Settle() {
    std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) { return Area(a.area) > Area(b.area); });
    for (size_t i = 0; i < candidates_.size() && i < kMaxVerified; ++i) {
      const RECT& area = candidates_[i].area;
      if (std::any_of(areas_.begin(), areas_.end(), [&](const RECT& kept) { return SameArea(kept, area); })) continue;
      const bool known = std::any_of(shown_.begin(), shown_.end(), [&](const RECT& seen) { return SameArea(seen, area); });
      const bool visible = known || Visible(candidates_[i]);
      Trace("scroll: candidate %ld,%ld-%ld,%ld %s", area.left, area.top, area.right, area.bottom, visible ? "on screen" : "hidden");
      if (!visible) continue;
      if (!known) shown_.push_back(area);
      areas_.push_back(area);
      overflows_ = true;
      if (areas_.size() <= kMaxEnclosed) Enclose(candidates_[i]);
    }
    candidates_.clear();
  }

  IUIAutomation* automation_;
  HWND window_;
  RECT limit_;
  std::vector<RECT>& shown_;  // Areas earlier looks at the same window found on screen.
  int min_width_;
  int min_height_;
  int hidden_min_;
  int reach_;
  ComPtr<IUIAutomationElement> root_;
  ComPtr<IUIAutomationCondition> raw_;
  ComPtr<IUIAutomationTreeWalker> walker_;
  ComPtr<IUIAutomationCacheRequest> identity_;
  ComPtr<IUIAutomationCacheRequest> placed_;
  RuntimeId root_id_;
  std::vector<Candidate> candidates_;
  std::vector<RECT> areas_;
  std::vector<RECT> frames_;
  bool overflows_ = false;
};

}  // namespace

ScrollDetector::ScrollDetector(HWND controller) : controller_(controller) {}

ScrollDetector::~ScrollDetector() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void ScrollDetector::Request(uint64_t shot, HWND window, const RECT& monitor, float scale) {
  {
    std::lock_guard lock(mutex_);
    pending_ = Job{shot, window, monitor, scale};
    has_pending_ = true;
  }
  if (!thread_.joinable()) thread_ = std::thread([this] { Main(); });
  wake_.notify_all();
}

void ScrollDetector::Main() {
  (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
    (void)CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
  }
  // A hung app must not hold the search for long.
  ComPtr<IUIAutomation2> timed;
  if (automation && SUCCEEDED(automation.As(&timed))) {
    timed->put_ConnectionTimeout(config::kScrollDetectTimeoutMs);
    timed->put_TransactionTimeout(config::kScrollDetectTimeoutMs);
  }

  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stopping_ || has_pending_; });
    if (stopping_) break;
    const Job job = pending_;
    pending_ = Job{};
    has_pending_ = false;
    lock.unlock();
    RECT limit{};
    const bool visible = IsWindow(job.window) && !IsIconic(job.window) && VisibleFrame(job.window, job.monitor, &limit);
    lock.lock();
    if (!visible || !automation) continue;

    // Asked for the first time, a browser only starts to build what it can tell, so the first answer may be nothing.
    // It is asked again until the answer settles, or until there is nothing to wait for.
    std::vector<RECT> known;
    std::vector<RECT> shown;
    bool offered = false;
    for (size_t attempt = 0;; ++attempt) {
      lock.unlock();
      const double started = NowSeconds();
      Look look(automation.Get(), job.window, limit, job.scale, shown);
      look.Run();
      auto result = std::make_unique<ScrollAreas>();
      result->shot = job.shot;
      result->hints = look.TakeHints(job.monitor);
      Trace("scroll: look %zu: %zu areas in %zu frames, overflows %d, in %.1f ms", attempt, result->hints.areas.size(), result->hints.frames.size(), look.overflows(),
            (NowSeconds() - started) * 1000.0);
      const bool settled = offered && SameAreas(known, result->hints.areas);
      if (look.overflows() && !settled) {
        known = result->hints.areas;
        offered = true;
        if (PostMessageW(controller_, kMsgScrollAreas, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
      }
      lock.lock();
      if (settled || attempt == std::size(config::kScrollDetectRetryMs)) break;
      const auto delay = std::chrono::milliseconds(config::kScrollDetectRetryMs[attempt]);
      if (wake_.wait_for(lock, delay, [&] { return stopping_ || has_pending_; })) break;
    }
  }
  lock.unlock();
  timed.Reset();
  automation.Reset();
  CoUninitialize();
}

}  // namespace sstk
