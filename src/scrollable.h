// Finds the parts of the top app window that scroll vertically, nested ones included. UI Automation sees into
// every kind of app, and classic scroll bars fill in where it sees nothing. It only reads, so nothing on screen reacts.
#pragma once

#include "platform.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace sstk {

struct ScrollAreas {
  uint64_t shot = 0;
  std::vector<RECT> areas;  // Monitor pixels, largest first.
};

class ScrollDetector {
 public:
  explicit ScrollDetector(HWND controller);
  ~ScrollDetector();
  ScrollDetector(const ScrollDetector&) = delete;
  ScrollDetector& operator=(const ScrollDetector&) = delete;

  // Looks into the window in the background, and posts kMsgScrollAreas if anything scrolls. A newer request
  // replaces one still waiting.
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
