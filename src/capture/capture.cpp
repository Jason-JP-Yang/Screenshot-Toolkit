#include "sstk/capture/capture.h"

#include "sstk/config/core.h"

#include <shellscalingapi.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace sstk {

namespace {

float MonitorScale(HMONITOR monitor) {
  UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
  UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) return 1.0f;
  return static_cast<float>(dpi_x) / USER_DEFAULT_SCREEN_DPI;
}

// Reads the windows of each shot by themselves, apart from the capture thread, so that the next press never waits.
class WindowGrabber {
 public:
  struct Job {
    uint64_t capture = 0;
    std::vector<WindowFrame> windows;
    std::shared_ptr<const Image> screen;
    RECT bounds{};
    HMONITOR monitor = nullptr;
  };

  explicit WindowGrabber(HWND controller) : controller_(controller), thread_([this] { Main(); }) {}

  ~WindowGrabber() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
  }

  WindowGrabber(const WindowGrabber&) = delete;
  WindowGrabber& operator=(const WindowGrabber&) = delete;

  void Request(Job job) {
    {
      std::lock_guard lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    wake_.notify_all();
  }

 private:
  void Main() {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      CaptureDevice device;
      bool used = false;
      std::unique_lock lock(mutex_);
      const auto ready = [&] { return stopping_ || !jobs_.empty(); };
      for (;;) {
        // The capture device holds a good deal of memory, and goes after a while without a shot.
        if (!used) {
          wake_.wait(lock, ready);
        } else if (!wake_.wait_for(lock, std::chrono::seconds(config::kCaptureDeviceKeepSeconds), ready)) {
          lock.unlock();
          device.Close();
          lock.lock();
          used = false;
          continue;
        }
        if (stopping_) break;
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        lock.unlock();
        const double started = NowSeconds();
        auto result = std::make_unique<WindowShots>();
        result->capture = job.capture;
        result->shots = GrabWindows(job.windows, *job.screen, job.bounds, MonitorScale(job.monitor), device);
        used = true;
        job.screen.reset();
        Trace("capture: %zu of %zu windows read by themselves in %.1f ms", result->shots.size(), job.windows.size(), (NowSeconds() - started) * 1000.0);
        if (!result->shots.empty() && PostMessageW(controller_, kMsgWindowShots, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        lock.lock();
      }
    }
    CoUninitialize();
  }

  HWND controller_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace

void RunCaptureLoop(HWND controller, HANDLE trigger_event, HANDLE edit_event, HANDLE quit_event) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  WindowGrabber grabber(controller);
  const HANDLE waits[] = {trigger_event, edit_event, quit_event};
  uint64_t next_id = 1;

  for (;;) {
    const DWORD woken = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
    if (woken != WAIT_OBJECT_0 && woken != WAIT_OBJECT_0 + 1) return;

    auto result = std::make_unique<CaptureResult>();
    result->id = next_id++;
    result->trigger_time = NowSeconds();
    result->edit = woken == WAIT_OBJECT_0 + 1;
    GetLocalTime(&result->local_time);

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

    // The flash goes first: it gives feedback within a frame while the screen is being read.
    // Test tooling makes the overlay capturable, so there the flash has to wait for the read.
    if (!OverlayVisibleToCapture()) PostMessageW(controller, kMsgFlash, reinterpret_cast<WPARAM>(monitor), 0);

    result->screen = AnalyzeScreen(monitor);
    const double analyzed = NowSeconds();
    result->frame = CaptureScreen(result->screen.bounds);
    Trace("capture: analyze %.2f ms, blit %.2f ms, regions %zu", (analyzed - result->trigger_time) * 1000.0,
          (NowSeconds() - analyzed) * 1000.0, result->screen.regions.size());

    if (OverlayVisibleToCapture()) PostMessageW(controller, kMsgFlash, reinterpret_cast<WPARAM>(monitor), 0);
    WindowGrabber::Job job{result->id, result->screen.windows, result->frame, result->screen.bounds, monitor};
    if (!PostMessageW(controller, kMsgCaptured, 0, reinterpret_cast<LPARAM>(result.get()))) continue;
    result.release();
    if (job.screen && !job.windows.empty()) grabber.Request(std::move(job));
  }
}

}  // namespace sstk
