#include "workers.h"

#include <windows.h>

namespace sstk {

namespace {

// Beyond this many besides the caller, memory, not cores, limits the loops shared.
constexpr int kMostThreads = 7;

}  // namespace

Workers::Workers() {
  const int cores = static_cast<int>(std::thread::hardware_concurrency());
  const int count = std::clamp(cores - 1, 0, kMostThreads);
  threads_.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) threads_.emplace_back([this] { Main(); });
}

Workers::~Workers() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (std::thread& thread : threads_) thread.join();
}

int Workers::Take(Call call, void* context, int parts) {
  int taken = 0;
  for (int part = next_.fetch_add(1, std::memory_order_relaxed); part < parts; part = next_.fetch_add(1, std::memory_order_relaxed)) {
    call(context, part);
    ++taken;
  }
  return taken;
}

void Workers::Run(int parts, Call call, void* context) {
  {
    std::unique_lock lock(mutex_);
    // A worker late for the last task may still be about to look for a part of it.
    done_.wait(lock, [&] { return active_ == 0; });
    call_ = call;
    context_ = context;
    parts_ = parts;
    finished_ = 0;
    next_.store(0, std::memory_order_relaxed);
    ++generation_;
  }
  wake_.notify_all();
  const int taken = Take(call, context, parts);
  std::unique_lock lock(mutex_);
  finished_ += taken;
  done_.wait(lock, [&] { return finished_ == parts; });
}

void Workers::Main() {
  // Above the apps around, below the thread that waits on the work.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  uint64_t seen = 0;
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
    if (stop_) return;
    seen = generation_;
    const Call call = call_;
    void* const context = context_;
    const int parts = parts_;
    ++active_;
    lock.unlock();
    const int taken = Take(call, context, parts);
    lock.lock();
    --active_;
    finished_ += taken;
    done_.notify_all();
  }
}

}  // namespace sstk
