#include "scrollable.h"

#include "config/core.h"

#include <dwmapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <memory>
#include <utility>

namespace sstk {

namespace {

struct Search {
  RECT limit;  // The window's frame on the monitor, in virtual screen coordinates.
  int min_width;
  int min_height;
  std::vector<RECT> areas;
};

long Area(const RECT& rect) { return (rect.right - rect.left) * (rect.bottom - rect.top); }

bool SameArea(const RECT& a, const RECT& b) {
  constexpr int kTolerance = 4;
  return std::abs(a.left - b.left) <= kTolerance && std::abs(a.top - b.top) <= kTolerance &&
         std::abs(a.right - b.right) <= kTolerance && std::abs(a.bottom - b.bottom) <= kTolerance;
}

// Keeps what is on the monitor and inside the window, if it is big enough to scroll through.
void Collect(Search& search, RECT rect) {
  if (!IntersectRect(&rect, &rect, &search.limit)) return;
  if (rect.right - rect.left < search.min_width || rect.bottom - rect.top < search.min_height) return;
  for (const RECT& existing : search.areas) {
    if (SameArea(existing, rect)) return;
  }
  search.areas.push_back(rect);
}

bool IsWebDocument(IUIAutomationElement* element) {
  CONTROLTYPEID type = 0;
  if (FAILED(element->get_CachedControlType(&type)) || type != UIA_DocumentControlTypeId) return false;
  BSTR framework = nullptr;
  if (FAILED(element->get_CachedFrameworkId(&framework))) return false;
  const bool web = framework != nullptr && wcscmp(framework, L"Chrome") == 0;
  SysFreeString(framework);
  return web;
}

void FindWithAutomation(HWND window, Search& search) {
  ComPtr<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) &&
      FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
    return;
  }
  // A hung app must not hold the search for long.
  ComPtr<IUIAutomation2> timed;
  if (SUCCEEDED(automation.As(&timed))) {
    timed->put_ConnectionTimeout(config::kScrollDetectTimeoutMs);
    timed->put_TransactionTimeout(config::kScrollDetectTimeoutMs);
  }

  ComPtr<IUIAutomationElement> root;
  if (FAILED(automation->ElementFromHandle(window, &root)) || !root) return;

  VARIANT yes{};
  yes.vt = VT_BOOL;
  yes.boolVal = VARIANT_TRUE;
  ComPtr<IUIAutomationCondition> has_pattern;
  if (FAILED(automation->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, yes, &has_pattern))) return;
  ComPtr<IUIAutomationCacheRequest> cache;
  if (FAILED(automation->CreateCacheRequest(&cache))) return;
  for (const PROPERTYID property : {UIA_BoundingRectanglePropertyId, UIA_ScrollVerticallyScrollablePropertyId, UIA_ControlTypePropertyId,
                                    UIA_FrameworkIdPropertyId, UIA_IsOffscreenPropertyId}) {
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
    VARIANT vertical{};
    const bool scrolls = SUCCEEDED(element->GetCachedPropertyValue(UIA_ScrollVerticallyScrollablePropertyId, &vertical)) &&
                         vertical.vt == VT_BOOL && vertical.boolVal != VARIANT_FALSE;
    // A browser tells about its page as a whole, and leaves out most of what scrolls inside one: plain boxes, and lists
    // and editors drawing only their visible part. So its documents count whatever they say, and the recorder finds
    // what really moves.
    if (scrolls || IsWebDocument(element.Get())) Collect(search, rect);
  }
}

void CollectScrollBar(HWND window, Search& search) {
  if (!IsWindowVisible(window) || (GetWindowLongPtrW(window, GWL_STYLE) & WS_VSCROLL) == 0) return;
  SCROLLBARINFO bar{};
  bar.cbSize = sizeof(bar);
  if (!GetScrollBarInfo(window, OBJID_VSCROLL, &bar)) return;
  if ((bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_UNAVAILABLE)) != 0) return;
  RECT rect;
  if (GetWindowRect(window, &rect)) Collect(search, rect);
}

std::vector<RECT> FindScrollAreas(HWND window, const RECT& monitor, float scale) {
  Search search{};
  if (!IsWindow(window) || IsIconic(window)) return {};
  RECT frame;
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && !GetWindowRect(window, &frame)) {
    return {};
  }
  if (!IntersectRect(&search.limit, &frame, &monitor)) return {};
  search.min_width = static_cast<int>(std::round(config::kScrollAreaMinWidth * scale));
  search.min_height = static_cast<int>(std::round(config::kScrollAreaMinHeight * scale));

  FindWithAutomation(window, search);
  CollectScrollBar(window, search);
  EnumChildWindows(window, [](HWND child, LPARAM context) -> BOOL {
    CollectScrollBar(child, *reinterpret_cast<Search*>(context));
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));

  // Outer areas first, so that a scroll is put down to the largest area that moved as a whole.
  std::sort(search.areas.begin(), search.areas.end(), [](const RECT& a, const RECT& b) { return Area(a) > Area(b); });
  for (RECT& area : search.areas) OffsetRect(&area, -monitor.left, -monitor.top);
  return std::move(search.areas);
}

bool SameAreas(const std::vector<RECT>& a, const std::vector<RECT>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const RECT& x, const RECT& y) { return EqualRect(&x, &y) != FALSE; });
}

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
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stopping_ || has_pending_; });
    if (stopping_) break;
    const Job job = pending_;
    has_pending_ = false;

    // Asked for the first time, a browser only starts to build what it can tell, so the first answer may be nothing.
    // It is asked again until the answer settles, or until there is nothing to wait for.
    std::vector<RECT> known;
    for (size_t attempt = 0;; ++attempt) {
      lock.unlock();
      const double started = NowSeconds();
      auto result = std::make_unique<ScrollAreas>();
      result->shot = job.shot;
      result->areas = FindScrollAreas(job.window, job.monitor, job.scale);
      Trace("scroll: %zu areas found in %.1f ms", result->areas.size(), (NowSeconds() - started) * 1000.0);
      const bool settled = !known.empty() && SameAreas(known, result->areas);
      if (!result->areas.empty() && !settled) {
        known = result->areas;
        if (PostMessageW(controller_, kMsgScrollAreas, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
      }
      lock.lock();
      if (settled || attempt == std::size(config::kScrollDetectRetryMs)) break;
      const auto delay = std::chrono::milliseconds(config::kScrollDetectRetryMs[attempt]);
      if (wake_.wait_for(lock, delay, [&] { return stopping_ || has_pending_; })) break;
    }
  }
  lock.unlock();
  CoUninitialize();
}

}  // namespace sstk
