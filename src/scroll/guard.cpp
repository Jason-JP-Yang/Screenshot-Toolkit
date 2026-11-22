#include "sstk/scroll/guard.h"

#include "sstk/base/workers.h"
#include "sstk/config/core.h"
#include "sstk/graphics/pixels.h"
#include "sstk/scroll/tracker.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <iterator>
#include <utility>
#include <vector>

namespace sstk {

namespace {

constexpr uint32_t kQuietPixels = 24;
constexpr double kKeepSeconds = 3.0;  // A shot's pixels stay this long after the last look at them.

struct Reference {
  uint64_t shot = 0;
  RECT view{};
  Image pixels;
  double used = 0.0;
};

// What the screen shows there now, without CAPTUREBLT so that the pointer never flickers.
bool ReadScreen(Frame& frame, const RECT& rect) {
  HDC screen = GetDC(nullptr);
  if (screen == nullptr) return false;
  HDC memory = CreateCompatibleDC(screen);
  bool read = false;
  if (memory != nullptr) {
    const HGDIOBJ previous = SelectObject(memory, frame.bitmap());
    if (previous != nullptr) {
      read = BitBlt(memory, 0, 0, frame.width(), frame.height(), screen, rect.left, rect.top, SRCCOPY) != FALSE;
      SelectObject(memory, previous);
    }
    DeleteDC(memory);
  }
  ReleaseDC(nullptr, screen);
  GdiFlush();
  return read;
}

// Whether the area shows the shot still: as it was, scrolled, or changed in parts only. A scroll too far to overlap
// what the shot showed is as good as a new page.
bool Continues(const Image& before, const Frame& now, const RECT& area, Workers& workers) {
  Tracker tracker;
  tracker.Reset(before.width(), before.height(), &workers);
  tracker.SetRef(before.view());
  if (tracker.Load(now.view(), area, INT_MAX, -1, kQuietPixels) <= kQuietPixels) return true;
  Tracker::Match match;
  bool sure = false;
  if (tracker.Register(0, &match, &sure)) return true;
  const double rows = area.bottom - area.top;
  const double columns = area.right - area.left;
  return tracker.changed_rows() < rows * config::kScrollReplacedShare || tracker.changed_width() < columns * config::kScrollReplacedShare;
}

}  // namespace

OfferGuard::OfferGuard(HWND controller, Store& store) : controller_(controller), store_(store), thread_([this] { Main(); }) {}

OfferGuard::~OfferGuard() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void OfferGuard::Request(Check check) {
  {
    std::lock_guard lock(mutex_);
    std::erase_if(pending_, [&](const Check& waiting) { return waiting.shot == check.shot; });
    pending_.push_back(std::move(check));
  }
  wake_.notify_all();
}

void OfferGuard::Main() {
  (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    std::unique_ptr<Workers> workers;
    std::vector<Reference> references;
    std::unique_lock lock(mutex_);
    const auto ready = [&] { return stopping_ || !pending_.empty(); };
    for (;;) {
      // What is held goes after a while with nothing to look at, and nothing wakes the thread after that.
      if (!workers && references.empty()) {
        wake_.wait(lock, ready);
      } else if (!wake_.wait_for(lock, std::chrono::duration<double>(kKeepSeconds), ready)) {
        lock.unlock();
        workers.reset();
        references.clear();
        lock.lock();
        continue;
      }
      if (stopping_) break;
      Check check = std::move(pending_.front());
      pending_.pop_front();
      lock.unlock();

      const double now = NowSeconds();
      std::erase_if(references, [&](const Reference& reference) { return now - reference.used > kKeepSeconds; });
      auto found = std::find_if(references.begin(), references.end(),
                                [&](const Reference& reference) { return reference.shot == check.shot && EqualRect(&reference.view, &check.view); });
      const int width = check.view.right - check.view.left;
      const int height = check.view.bottom - check.view.top;
      if (found == references.end()) {
        Reference reference;
        reference.shot = check.shot;
        reference.view = check.view;
        const std::shared_ptr<const Image> frame = width > 0 && height > 0 ? store_.PeekFrame(check.source) : nullptr;
        const int left = check.view.left - check.monitor.left;
        const int top = check.view.top - check.monitor.top;
        if (frame && left >= 0 && top >= 0 && left + width <= frame->width() && top + height <= frame->height()) {
          reference.pixels = CopyRect(*frame, left, top, width, height);
        }
        references.push_back(std::move(reference));
        found = std::prev(references.end());
      }
      found->used = now;

      bool mismatch = false;
      RECT area{};
      if (found->pixels && IntersectRect(&area, &check.area, &check.view)) {
        OffsetRect(&area, -check.view.left, -check.view.top);
        Frame reading(width, height);
        if (reading && ReadScreen(reading, check.view)) {
          if (!workers) workers = std::make_unique<Workers>();
          mismatch = !Continues(found->pixels, reading, area, *workers);
        }
      }
      if (mismatch) {
        Trace("scroll: the app no longer shows what shot %llu did", static_cast<unsigned long long>(check.shot));
        PostMessageW(controller_, kMsgScrollMismatch, static_cast<WPARAM>(check.shot), 0);
      }
      lock.lock();
    }
  }
  CoUninitialize();
}

}  // namespace sstk
