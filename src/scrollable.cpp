#include "scrollable.h"

#include "config/core.h"
#include "screen.h"

#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace sstk {

namespace {

constexpr int kMaxTreeDepth = 256;
constexpr size_t kMaxSiblingsCompared = 64;

long Area(const RECT& rect) { return (rect.right - rect.left) * (rect.bottom - rect.top); }

bool SameArea(const RECT& a, const RECT& b) {
  constexpr int kTolerance = 4;
  return std::abs(a.left - b.left) <= kTolerance && std::abs(a.top - b.top) <= kTolerance &&
         std::abs(a.right - b.right) <= kTolerance && std::abs(a.bottom - b.bottom) <= kTolerance;
}

bool SameAreas(const std::vector<RECT>& a, const std::vector<RECT>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; });
}

// Which tiles of the window hold drawn content, reduced from the screenshot once so that it need not be kept.
struct ContentMap {
  RECT rect{};  // Virtual screen coordinates.
  int tile = 0;
  int columns = 0;
  int rows = 0;
  std::vector<uint8_t> drawn;
};

ContentMap MapContent(const Image& frame, const RECT& window, const RECT& monitor, float scale) {
  ContentMap map;
  map.rect = window;
  map.tile = std::max(static_cast<int>(std::lround(config::kScrollContentTile * scale)), 8);
  const int width = window.right - window.left;
  const int height = window.bottom - window.top;
  map.columns = width / map.tile;
  map.rows = height / map.tile;
  if (map.columns <= 0 || map.rows <= 0) return map;
  map.drawn.assign(static_cast<size_t>(map.columns) * map.rows, 0);
  const int left = window.left - monitor.left;
  const int top = window.top - monitor.top;
  for (int row = 0; row < map.rows; ++row) {
    for (int column = 0; column < map.columns; ++column) {
      int edges = 0;
      int samples = 0;
      for (int y = 0; y < map.tile; y += 2) {
        const uint8_t* pixels = frame.data() + static_cast<ptrdiff_t>(top + row * map.tile + y) * frame.stride() + static_cast<ptrdiff_t>(left + column * map.tile) * 4;
        for (int x = 0; x + 1 < map.tile; ++x, pixels += 4) {
          const int here = pixels[0] + 2 * pixels[1] + pixels[2];
          const int right = pixels[4] + 2 * pixels[5] + pixels[6];
          if (std::abs(here - right) > 96) ++edges;
          ++samples;
        }
      }
      map.drawn[static_cast<size_t>(row) * map.columns + column] = edges * 32 >= samples ? 1 : 0;
    }
  }
  return map;
}

struct Node {
  RECT rect{};
  bool offscreen = false;
  bool has_pattern = false;
  CONTROLTYPEID type = 0;
  int set_size = 0;
  std::vector<Node> children;
};

bool HasSize(const RECT& rect, int width, int height) { return rect.right - rect.left >= width && rect.bottom - rect.top >= height; }

bool Shows(const Node& node) { return !node.offscreen && node.rect.right > node.rect.left && node.rect.bottom > node.rect.top; }

// Control types that stand for what they draw. Panes and groups say nothing of it unless something inside them does.
bool DescribesItself(CONTROLTYPEID type) {
  switch (type) {
    case UIA_ButtonControlTypeId:
    case UIA_CalendarControlTypeId:
    case UIA_CheckBoxControlTypeId:
    case UIA_ComboBoxControlTypeId:
    case UIA_DataItemControlTypeId:
    case UIA_EditControlTypeId:
    case UIA_HeaderControlTypeId:
    case UIA_HeaderItemControlTypeId:
    case UIA_HyperlinkControlTypeId:
    case UIA_ImageControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_MenuBarControlTypeId:
    case UIA_MenuItemControlTypeId:
    case UIA_ProgressBarControlTypeId:
    case UIA_RadioButtonControlTypeId:
    case UIA_ScrollBarControlTypeId:
    case UIA_SeparatorControlTypeId:
    case UIA_SliderControlTypeId:
    case UIA_SpinnerControlTypeId:
    case UIA_SplitButtonControlTypeId:
    case UIA_StatusBarControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_TextControlTypeId:
    case UIA_ThumbControlTypeId:
    case UIA_TitleBarControlTypeId:
    case UIA_ToolBarControlTypeId:
    case UIA_ToolTipControlTypeId:
    case UIA_TreeItemControlTypeId:
    case UIA_AppBarControlTypeId:
      return true;
    default:
      return false;
  }
}

class Look {
 public:
  Look(HWND window, const RECT& limit, float scale, const ContentMap& content)
      : window_(window), limit_(limit), content_(content),
        min_width_(static_cast<int>(std::lround(config::kScrollAreaMinWidth * scale))),
        min_height_(static_cast<int>(std::lround(config::kScrollAreaMinHeight * scale))),
        hidden_min_(static_cast<int>(std::lround(config::kScrollHiddenMinHeight * scale))) {}

  void Run(IUIAutomation* automation) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(window_, &root)) || !root) return;
    ComPtr<IUIAutomationCondition> raw;
    if (FAILED(automation->get_RawViewCondition(&raw)) || !raw) return;
    FindScrollers(automation, root.Get(), raw.Get());
    // What a scroller says settles it. Anything else takes reading the whole tree.
    if (!overflows_) ReadTree(automation, root.Get(), raw.Get());
  }

  bool overflows() const { return overflows_; }
  bool undescribed() const { return undescribed_; }
  std::vector<RECT> TakeAreas() {
    std::sort(areas_.begin(), areas_.end(), [](const RECT& a, const RECT& b) { return Area(a) > Area(b); });
    for (RECT& area : areas_) OffsetRect(&area, -monitor_left_, -monitor_top_);
    return std::move(areas_);
  }
  void SetMonitor(const RECT& monitor) {
    monitor_left_ = monitor.left;
    monitor_top_ = monitor.top;
  }

 private:
  void Collect(RECT rect) {
    if (!IntersectRect(&rect, &rect, &limit_) || !HasSize(rect, min_width_, min_height_)) return;
    for (const RECT& existing : areas_) {
      if (SameArea(existing, rect)) return;
    }
    areas_.push_back(rect);
  }

  // Every scroller in the raw view, plain boxes included, and how much of its content it shows.
  void FindScrollers(IUIAutomation* automation, IUIAutomationElement* root, IUIAutomationCondition* raw) {
    VARIANT yes{};
    yes.vt = VT_BOOL;
    yes.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> has_pattern;
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, yes, &has_pattern)) || FAILED(automation->CreateCacheRequest(&cache))) return;
    cache->put_TreeFilter(raw);
    for (const PROPERTYID property : {UIA_BoundingRectanglePropertyId, UIA_ScrollVerticallyScrollablePropertyId, UIA_ScrollVerticalViewSizePropertyId, UIA_IsOffscreenPropertyId}) {
      if (FAILED(cache->AddProperty(property))) return;
    }
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAllBuildCache(TreeScope_Subtree, has_pattern.Get(), cache.Get(), &found)) || !found) return;
    int count = 0;
    found->get_Length(&count);
    for (int i = 0; i < count; ++i) {
      ComPtr<IUIAutomationElement> element;
      RECT rect{};
      if (FAILED(found->GetElement(i, &element)) || !element || FAILED(element->get_CachedBoundingRectangle(&rect))) continue;
      BOOL offscreen = FALSE;
      if (SUCCEEDED(element->get_CachedIsOffscreen(&offscreen)) && offscreen) continue;
      Collect(rect);
      RECT shown{};
      if (!IntersectRect(&shown, &rect, &limit_) || !HasSize(shown, min_width_, min_height_)) continue;
      VARIANT vertical{};
      VARIANT size{};
      const bool scrolls = SUCCEEDED(element->GetCachedPropertyValue(UIA_ScrollVerticallyScrollablePropertyId, &vertical)) && vertical.vt == VT_BOOL && vertical.boolVal != VARIANT_FALSE;
      const bool sized = SUCCEEDED(element->GetCachedPropertyValue(UIA_ScrollVerticalViewSizePropertyId, &size)) && size.vt == VT_R8;
      if (scrolls && (!sized || size.dblVal <= config::kScrollMaxViewPercent)) overflows_ = true;
    }
  }

  void ReadNode(IUIAutomationElement* element, Node& node, int depth) {
    element->get_CachedBoundingRectangle(&node.rect);
    BOOL offscreen = FALSE;
    element->get_CachedIsOffscreen(&offscreen);
    node.offscreen = offscreen != FALSE;
    element->get_CachedControlType(&node.type);
    VARIANT value{};
    if (SUCCEEDED(element->GetCachedPropertyValue(UIA_IsScrollPatternAvailablePropertyId, &value))) node.has_pattern = value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
    VariantClear(&value);
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

  void ReadTree(IUIAutomation* automation, IUIAutomationElement* root, IUIAutomationCondition* raw) {
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation->CreateCacheRequest(&cache))) return;
    cache->put_TreeFilter(raw);
    cache->put_TreeScope(TreeScope_Subtree);
    for (const PROPERTYID property : {UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId, UIA_ControlTypePropertyId, UIA_IsScrollPatternAvailablePropertyId, UIA_SizeOfSetPropertyId}) {
      if (FAILED(cache->AddProperty(property))) return;
    }
    ComPtr<IUIAutomationElement> snapshot;
    // A tree too large to read in time means a great deal of content, which is what this was to find out.
    if (FAILED(root->BuildUpdatedCache(cache.Get(), &snapshot)) || !snapshot) {
      undescribed_ = true;
      return;
    }
    Node tree;
    ReadNode(snapshot.Get(), tree, 0);
    covered_.assign(content_.drawn.size(), 0);
    Walk(tree, limit_);
    if (!overflows_) undescribed_ = CheckUndescribed();
  }

  // Children stacked one above the other, some of them out of sight, are content that overflows its container, and a
  // list claiming more items than it renders is drawing only the part in view.
  void CheckOverflow(const Node& node, const RECT& clip) {
    std::vector<const Node*> shown;
    std::vector<const Node*> hidden;
    for (const Node& child : node.children) {
      if (child.rect.right - child.rect.left < 8 || child.rect.bottom - child.rect.top < 4) continue;
      if (child.offscreen) {
        if (hidden.size() < kMaxSiblingsCompared) hidden.push_back(&child);
      } else if (shown.size() < kMaxSiblingsCompared) {
        shown.push_back(&child);
      }
    }
    if (shown.empty()) return;
    bool stacked = false;
    int hidden_height = 0;
    for (const Node* out : hidden) {
      for (const Node* in : shown) {
        const LONG overlap = std::min(out->rect.right, in->rect.right) - std::max(out->rect.left, in->rect.left);
        const LONG narrower = std::min(out->rect.right - out->rect.left, in->rect.right - in->rect.left);
        const bool apart = out->rect.top >= in->rect.bottom - 2 || out->rect.bottom <= in->rect.top + 2;
        if (apart && overlap * 2 >= narrower) {
          hidden_height += out->rect.bottom - out->rect.top;
          break;
        }
      }
    }
    if (hidden_height >= hidden_min_) stacked = true;

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
      if (largest - rendered >= config::kScrollHiddenSetItems && rising >= 3) stacked = true;
    }
    if (!stacked) return;
    RECT area = node.rect;
    if (!IntersectRect(&area, &area, &clip)) area = node.rect;
    RECT inside{};
    if (!IntersectRect(&inside, &area, &limit_) || !HasSize(inside, min_width_, min_height_)) return;
    overflows_ = true;
    Collect(area);
  }

  void Cover(const RECT& rect) {
    if (content_.drawn.empty()) return;
    RECT part{};
    if (!IntersectRect(&part, &rect, &content_.rect)) return;
    const int first_column = static_cast<int>(part.left - content_.rect.left) / content_.tile;
    const int last_column = std::min(static_cast<int>(part.right - 1 - content_.rect.left) / content_.tile, content_.columns - 1);
    const int first_row = static_cast<int>(part.top - content_.rect.top) / content_.tile;
    const int last_row = std::min(static_cast<int>(part.bottom - 1 - content_.rect.top) / content_.tile, content_.rows - 1);
    for (int row = first_row; row <= last_row; ++row) {
      for (int column = first_column; column <= last_column; ++column) covered_[static_cast<size_t>(row) * content_.columns + column] = 1;
    }
  }

  void Walk(const Node& node, const RECT& clip) {
    // A picture filling most of the window may as well be a canvas that an app draws everything on.
    const bool canvas = node.type == UIA_ImageControlTypeId && Area(node.rect) * 100 >= Area(limit_) * config::kScrollCanvasPercent;
    if (Shows(node) && !canvas && (DescribesItself(node.type) || (node.children.empty() && node.has_pattern))) Cover(node.rect);
    if (!node.children.empty()) CheckOverflow(node, clip);
    RECT inner{};
    const RECT& next = IntersectRect(&inner, &clip, &node.rect) ? inner : clip;
    for (const Node& child : node.children) Walk(child, next);
  }

  // A patch of the window with content nobody describes, big enough to be something that could scroll.
  bool CheckUndescribed() {
    if (content_.drawn.empty()) return false;
    const int columns = content_.columns;
    const int rows = content_.rows;
    std::vector<uint8_t> left(content_.drawn.size(), 0);
    for (size_t i = 0; i < left.size(); ++i) left[i] = content_.drawn[i] != 0 && covered_[i] == 0 ? 1 : 0;
    std::vector<int> stack;
    for (size_t start = 0; start < left.size(); ++start) {
      if (left[start] == 0) continue;
      int count = 0;
      int min_column = columns;
      int max_column = 0;
      int min_row = rows;
      int max_row = 0;
      stack.push_back(static_cast<int>(start));
      left[start] = 0;
      while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        const int row = at / columns;
        const int column = at % columns;
        ++count;
        min_column = std::min(min_column, column);
        max_column = std::max(max_column, column);
        min_row = std::min(min_row, row);
        max_row = std::max(max_row, row);
        for (int y = std::max(row - 2, 0); y <= std::min(row + 2, rows - 1); ++y) {
          for (int x = std::max(column - 2, 0); x <= std::min(column + 2, columns - 1); ++x) {
            const size_t next = static_cast<size_t>(y) * columns + x;
            if (left[next] == 0) continue;
            left[next] = 0;
            stack.push_back(static_cast<int>(next));
          }
        }
      }
      const int width = (max_column - min_column + 1) * content_.tile;
      const int height = (max_row - min_row + 1) * content_.tile;
      if (count >= config::kScrollContentMinTiles && width >= min_width_ && height >= min_height_) return true;
    }
    return false;
  }

  HWND window_;
  RECT limit_;
  const ContentMap& content_;
  int min_width_;
  int min_height_;
  int hidden_min_;
  int monitor_left_ = 0;
  int monitor_top_ = 0;
  bool overflows_ = false;
  bool undescribed_ = false;
  std::vector<RECT> areas_;
  std::vector<uint8_t> covered_;
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

void ScrollDetector::Request(uint64_t shot, HWND window, const RECT& monitor, float scale, std::shared_ptr<const Image> frame) {
  {
    std::lock_guard lock(mutex_);
    pending_ = Job{shot, window, monitor, scale, std::move(frame)};
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
    Job job = std::move(pending_);
    pending_ = Job{};
    has_pending_ = false;
    lock.unlock();

    RECT limit{};
    ContentMap content;
    const bool visible = IsWindow(job.window) && !IsIconic(job.window) && VisibleFrame(job.window, job.monitor, &limit);
    if (visible && job.frame) content = MapContent(*job.frame, limit, job.monitor, job.scale);
    job.frame.reset();
    lock.lock();
    if (!visible || !automation) continue;

    // Asked for the first time, a browser only starts to build what it can tell, so the first answer may be nothing.
    // It is asked again until the answer settles, or until there is nothing to wait for.
    std::vector<RECT> known;
    bool offered = false;
    bool described = false;
    for (size_t attempt = 0;; ++attempt) {
      lock.unlock();
      const double started = NowSeconds();
      Look look(job.window, limit, job.scale, content);
      look.SetMonitor(job.monitor);
      look.Run(automation.Get());
      // Content nobody describes may only be what the app has not got around to describing yet, and an app that has
      // described it once may let its tree lapse again.
      described = described || !look.undescribed();
      const bool scrollable = look.overflows() || (!described && static_cast<int>(attempt) >= config::kScrollDetectContentAttempt);
      auto result = std::make_unique<ScrollAreas>();
      result->shot = job.shot;
      result->areas = look.TakeAreas();
      Trace("scroll: look %zu: %zu areas, overflows %d, undescribed %d, in %.1f ms", attempt, result->areas.size(), look.overflows(), look.undescribed(), (NowSeconds() - started) * 1000.0);
      const bool settled = offered && SameAreas(known, result->areas);
      if (scrollable && !settled) {
        known = result->areas;
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
