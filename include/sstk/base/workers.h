// Threads a scrolling screenshot shares its heavy loops with, so that no one core carries them. They live only as long
// as the screenshot, and wait without running while there is nothing to share.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace sstk {

class Workers {
 public:
  Workers();
  ~Workers();
  Workers(const Workers&) = delete;
  Workers& operator=(const Workers&) = delete;

  // Threads that take part, the calling one included.
  int size() const { return static_cast<int>(threads_.size()) + 1; }

  // How many parts work on `items` things is worth splitting into, at no fewer than `grain` things each.
  int Parts(int64_t items, int64_t grain) const {
    return static_cast<int>(std::clamp<int64_t>(items / std::max<int64_t>(grain, 1), 1, static_cast<int64_t>(size()) * 3));
  }

  // Calls work(part) for each part in [0, parts), on the workers and the calling thread at once, and returns once every
  // call has. A call may change only what is its own, and must not share work itself.
  template <typename Work>
  void For(int parts, Work&& work) {
    if (parts <= 1 || threads_.empty()) {
      for (int part = 0; part < parts; ++part) work(part);
      return;
    }
    using Body = std::remove_reference_t<Work>;
    Run(parts, [](void* context, int part) { (*static_cast<Body*>(context))(part); }, const_cast<void*>(static_cast<const void*>(&work)));
  }

 private:
  using Call = void (*)(void*, int);

  void Run(int parts, Call call, void* context);
  int Take(Call call, void* context, int parts);
  void Main();

  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  bool stop_ = false;
  uint64_t generation_ = 0;
  Call call_ = nullptr;
  void* context_ = nullptr;
  int parts_ = 0;
  int finished_ = 0;
  int active_ = 0;
  std::atomic<int> next_{0};
};

// [first, last) cut into at most `most` bands, whose inner edges fall on multiples of `align`. `first` must not be
// negative unless `align` is 1.
class Bands {
 public:
  Bands(int first, int last, int align, int most) : first_(first), last_(last), align_(std::max(align, 1)) {
    if (last <= first) return;
    low_ = first / align_;
    units_ = (last - 1) / align_ - low_ + 1;
    count_ = std::clamp(most, 1, units_);
  }

  int count() const { return count_; }
  int begin(int band) const { return band == 0 ? first_ : (low_ + static_cast<int>(static_cast<int64_t>(units_) * band / count_)) * align_; }
  int end(int band) const { return band + 1 == count_ ? last_ : begin(band + 1); }

 private:
  int first_ = 0;
  int last_ = 0;
  int align_ = 1;
  int low_ = 0;
  int units_ = 0;
  int count_ = 0;
};

}  // namespace sstk
