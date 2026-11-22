// Tells whether an app still shows what a shot of it did, once something has reached it since: as it was, scrolled,
// or changed only in parts, as a playing video or a typed word changes it. Content replaced across nearly all of what
// scrolls, as when a link opens another page, can no longer be stitched to the shot. Looked at on a thread of its own,
// against the shot's own pixels, without touching the app.
#pragma once

#include "sstk/base/platform.h"
#include "sstk/storage/store.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace sstk {

class OfferGuard {
 public:
  struct Check {
    uint64_t shot = 0;
    std::shared_ptr<Shot> source;
    RECT monitor{};  // All of these in virtual screen coordinates.
    RECT view{};     // What the screen showed of the app.
    RECT area{};     // What scrolls in it.
  };

  OfferGuard(HWND controller, Store& store);
  ~OfferGuard();
  OfferGuard(const OfferGuard&) = delete;
  OfferGuard& operator=(const OfferGuard&) = delete;

  // Looks once, and posts kMsgScrollMismatch if the app has left the shot behind. A newer check of the same shot
  // replaces one still waiting.
  void Request(Check check);

 private:
  void Main();

  HWND controller_;
  Store& store_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Check> pending_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace sstk
