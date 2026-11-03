// Hands a saved screenshot to other applications through the system's drag and drop. The loop calls into
// whichever window is under the pointer and can block there, so it runs apart from the UI thread.
#pragma once

#include "image.h"
#include "platform.h"
#include "store.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sstk {

// Prepares the UI thread for shell and OLE calls. Deferred to first use so idle memory stays minimal.
void EnsureOleInitialized();

struct FileDragOutcome {
  uint64_t id = 0;
  DWORD effect = DROPEFFECT_NONE;
  POINT release{};  // Virtual screen coordinates.
  bool cancelled = false;
};

class FileDragThread {
 public:
  FileDragThread(HWND controller, Store& store);
  ~FileDragThread();
  FileDragThread(const FileDragThread&) = delete;
  FileDragThread& operator=(const FileDragThread&) = delete;

  // Posts kMsgDragFeedback as the window under the pointer changes its mind, and one kMsgDragFinished at the end.
  void Begin(std::shared_ptr<Shot> shot, uint64_t id);
  void Cancel() { cancel_.store(true, std::memory_order_relaxed); }

 private:
  void Main();
  void Run(const std::shared_ptr<Shot>& shot, uint64_t id);

  HWND controller_;
  Store& store_;
  DWORD ui_thread_;
  HANDLE wake_;
  std::thread thread_;
  std::mutex mutex_;
  std::shared_ptr<Shot> pending_;
  uint64_t pending_id_ = 0;
  bool stopping_ = false;
  std::atomic<bool> cancel_{false};
};

}  // namespace sstk
