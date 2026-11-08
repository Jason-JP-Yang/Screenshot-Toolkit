// Decides whether the top app window really has content to scroll to, without touching it. Only what an app reports
// counts, read in UI Automation's raw view, where the plain boxes that the default view leaves out can be seen too:
// scrollers that show only part of their content, content stacked beyond the edge of the box that clips it, lists
// longer than what they render, and text documents with text out of view. Each must also be what the screen really
// shows at that place, so that something covered by an overlay, a fullscreen element, or another window does not count.
// A window that reports none of this, such as a game, is not offered a scrolling screenshot.
#pragma once

#include "platform.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace sstk {

struct ScrollAreas {
  uint64_t shot = 0;
  std::vector<RECT> areas;  // What the app reports as scrolling, in monitor pixels, largest first.
};

class ScrollDetector {
 public:
  explicit ScrollDetector(HWND controller);
  ~ScrollDetector();
  ScrollDetector(const ScrollDetector&) = delete;
  ScrollDetector& operator=(const ScrollDetector&) = delete;

  // Looks into the window in the background, and posts kMsgScrollAreas once it knows there is something to scroll to, and
  // again whenever it learns more. Posts nothing otherwise. A newer request replaces this one.
  void Request(uint64_t shot, HWND window, const RECT& monitor, float scale);

 private:
  struct Job {
    uint64_t shot = 0;
    HWND window = nullptr;
    RECT monitor{};
    float scale = 1.0f;
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
