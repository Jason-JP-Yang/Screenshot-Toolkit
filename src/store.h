// Background worker that keeps the Gallery file and the clipboard in step with each shot's selection.
#pragma once

#include "image.h"
#include "platform.h"

#include <wincodec.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sstk {

// One screenshot's durable state. Fields other than frame and local_time belong to the Store.
struct Shot {
  std::shared_ptr<const Image> frame;
  SYSTEMTIME local_time{};
  std::wstring path;
  RECT region{};
  uint64_t requested_version = 0;
  uint64_t saved_version = 0;
};

class Store {
 public:
  void Start(HWND controller);
  void Stop();

  // Loads the codec and resolves the Gallery folder so that the first real shot pays no first-use cost.
  void Warmup();

  // Records region as the shot's selection. The newest request wins; stale work is skipped.
  void Submit(const std::shared_ptr<Shot>& shot, const RECT& region);

  // Blocks until the shot's latest selection is on disk, then returns its path.
  std::wstring WaitUntilSaved(const std::shared_ptr<Shot>& shot);

  bool IsIdle();

  // A posted clipboard payload is current only if it carries the latest sequence number.
  uint64_t latest_clipboard_sequence() const { return clipboard_sequence_.load(std::memory_order_relaxed); }

 private:
  void WorkerMain();
  void PrepareClipboard(const std::shared_ptr<Shot>& shot, const RECT& region, uint64_t sequence);
  bool SaveToDisk(Shot& shot, const RECT& region);

  HWND controller_ = nullptr;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable saved_;
  std::vector<std::shared_ptr<Shot>> pending_saves_;
  std::shared_ptr<Shot> pending_clipboard_;
  RECT pending_clipboard_region_{};
  std::atomic<uint64_t> clipboard_sequence_{0};
  bool busy_ = false;
  bool warmup_requested_ = false;
  bool stopping_ = false;
  ComPtr<IWICImagingFactory> codec_factory_;
};

}  // namespace sstk
