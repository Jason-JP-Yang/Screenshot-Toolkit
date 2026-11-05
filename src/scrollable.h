// Decides whether the top app window really has content to scroll to, without touching it. What an app reports is read
// in UI Automation's raw view, where the plain boxes that the default view leaves out can be seen too: how much of its
// content each scroller shows, content hidden beyond its container, and lists longer than what is rendered of them.
// Content that an app draws and describes nothing about cannot be judged, so it may scroll.
#pragma once

#include "image.h"
#include "platform.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sstk {

struct ScrollAreas {
  uint64_t shot = 0;
  std::vector<RECT> areas;  // What the app reports as scrolling or holding a scroller, in monitor pixels, largest first.
};

class ScrollDetector {
 public:
  explicit ScrollDetector(HWND controller);
  ~ScrollDetector();
  ScrollDetector(const ScrollDetector&) = delete;
  ScrollDetector& operator=(const ScrollDetector&) = delete;

  // Looks into the window in the background, and posts kMsgScrollAreas once it knows there is something to scroll to, and
  // again whenever it learns more. Posts nothing otherwise. A newer request replaces this one.
  void Request(uint64_t shot, HWND window, const RECT& monitor, float scale, std::shared_ptr<const Image> frame);

 private:
  struct Job {
    uint64_t shot = 0;
    HWND window = nullptr;
    RECT monitor{};
    float scale = 1.0f;
    std::shared_ptr<const Image> frame;
  };

  void Main();

  HWND controller_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  Job pending_;
  bool has_pending_ = false;
  bool stopping_ = false;
};

}  // namespace sstk
