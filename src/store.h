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

// One screenshot's durable state. Every field except local_time is guarded by the Store's lock
// while a request for this shot may be in flight.
struct Shot {
  std::shared_ptr<const Image> frame;  // Null once archived; see Store::ReleaseFrame.
  std::vector<uint8_t> archive;        // Lossless copy of the whole frame, far smaller than the raw pixels.
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

  // Frees the raw pixels of a shot once they are safely archived and nothing is still using them.
  // Returns whether the shot is free of raw pixels now. Later crops decode the archive on the worker.
  bool ReleaseFrame(const std::shared_ptr<Shot>& shot);

  // Returns the shot's raw pixels, restoring them if released. Blocks for the short time decoding takes.
  std::shared_ptr<const Image> AcquireFrame(const std::shared_ptr<Shot>& shot);

  // Unloads the codec on the worker thread. It costs a few milliseconds to reload for the next save,
  // which nobody waits on, and it is the largest thing left in memory between shots.
  void ReleaseCodec();

  bool IsIdle();

  // A posted clipboard payload is current only if it carries the latest sequence number.
  uint64_t latest_clipboard_sequence() const { return clipboard_sequence_.load(std::memory_order_relaxed); }

 private:
  void WorkerMain();
  std::shared_ptr<const Image> FrameForWork(const std::shared_ptr<Shot>& shot);
  void PrepareClipboard(const std::shared_ptr<Shot>& shot, const RECT& region, uint64_t sequence);
  bool SaveToDisk(Shot& shot, const std::shared_ptr<const Image>& frame, const RECT& region);
  std::shared_ptr<const Image> DecodeArchive(const std::vector<uint8_t>& archive);

  HWND controller_ = nullptr;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable saved_;
  std::vector<std::shared_ptr<Shot>> pending_saves_;
  std::shared_ptr<Shot> pending_clipboard_;
  std::shared_ptr<Shot> pending_restore_;
  RECT pending_clipboard_region_{};
  std::atomic<uint64_t> clipboard_sequence_{0};
  bool busy_ = false;
  const Shot* active_shot_ = nullptr;
  bool warmup_requested_ = false;
  bool codec_release_requested_ = false;
  bool stopping_ = false;
  ComPtr<IWICImagingFactory> codec_factory_;
  // Decoded on demand for crops of a released shot, and kept until the worker runs out of work.
  std::shared_ptr<const Image> work_frame_;
  const Shot* work_frame_shot_ = nullptr;
};

}  // namespace sstk
