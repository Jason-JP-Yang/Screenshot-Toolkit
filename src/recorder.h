// A scrolling screenshot in progress. Its own thread watches the scrolled area, works out how far the content
// moved, and stitches what came into view onto the screenshot it started from, sending the thumbnail as it grows.
//
// Places are in plane coordinates: monitor pixels, reaching above and below the monitor as the capture grows.
// Thumbnail rows are numbered on a tall sparse surface, whose row kLongOrigin shows the area's first row.
#pragma once

#include "image.h"
#include "platform.h"

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sstk {

inline constexpr int kLongOrigin = 1 << 15;
inline constexpr int kLongRows = 1 << 16;

enum class ScrollOutcome { kSucceeded, kPartial, kFailed };

struct ScrollUpdate {
  uint64_t session = 0;
  bool locked = false;  // The first update once the scrolled area is known.
  RECT area{};          // Monitor pixels: the scrolled area, less any scroll bar.
  int thumb_width = 0;
  Image rows;           // Thumbnail rows to draw, the first of them at row `at`.
  int at = 0;
  int content_top = 0;  // Thumbnail rows holding the capture so far.
  int content_bottom = 0;
  int view_bottom = 0;  // Where the part on screen now ends.
  bool finished = false;
  ScrollOutcome outcome = ScrollOutcome::kFailed;
  std::shared_ptr<const Image> image;  // The scrolling screenshot, unless it failed.
  RECT image_rect{};                   // Its place in plane coordinates.
};

struct ScrollRequest {
  uint64_t session = 0;
  HWND controller = nullptr;
  HWND window = nullptr;     // The app being scrolled.
  RECT monitor{};            // Virtual screen coordinates.
  std::vector<RECT> areas;   // Monitor pixels, largest first.
  std::shared_ptr<const Image> frame;  // The screenshot it continues.
  float scale = 1.0f;
};

// Posts kMsgScrollUpdate as the capture changes, and one finished update at the end.
class ScrollRecorder {
 public:
  explicit ScrollRecorder(ScrollRequest request);
  ~ScrollRecorder();  // Calls it off without a word, if it is still running.
  ScrollRecorder(const ScrollRecorder&) = delete;
  ScrollRecorder& operator=(const ScrollRecorder&) = delete;

  // A wheel turned, in virtual screen coordinates. Turning it outside the scrolled area ends the capture early.
  void Wheel(POINT point);

  // Ends the capture the normal way.
  void Finish();

 private:
  void Main(ScrollRequest request);

  std::mutex mutex_;
  std::vector<POINT> wheels_;
  bool finish_ = false;
  bool stop_ = false;
  HANDLE wake_ = nullptr;
  std::thread thread_;
};

}  // namespace sstk
