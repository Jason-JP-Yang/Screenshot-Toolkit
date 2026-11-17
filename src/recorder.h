// A scrolling screenshot in progress. Its own thread reads the app's window, finds what really moves when it is scrolled,
// and builds the long image from what it sees of that (see stitcher.h), sending the thumbnail as it grows. The heavy
// loops are shared with worker threads that live as long as the screenshot (see workers.h), and the rows are packed as
// they are made (see longimage.h), so the image is never held as a bitmap.
// Whatever the window shows right above and below the scrolled area, across the same columns, goes on the two ends of
// the image as it is, and takes no part in the stitching.
//
// Places are in plane coordinates: monitor pixels, reaching above and below the monitor as the capture grows.
// Thumbnail rows are numbered on a tall sparse surface, whose row kLongOrigin shows the area's first row.
#pragma once

#include "image.h"
#include "longimage.h"
#include "platform.h"

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sstk {

inline constexpr int kLongOrigin = 1 << 15;
inline constexpr int kLongRows = 1 << 16;

enum class ScrollOutcome { kSucceeded, kPartial, kFailed };

// Thumbnail rows to draw, the first of them at row `at`.
struct ScrollPiece {
  int at = 0;
  Image rows;
};

struct ScrollUpdate {
  uint64_t session = 0;
  bool locked = false;  // The first update once the scrolled area is known.
  RECT area{};          // Monitor pixels: the scrolled area.
  int thumb_width = 0;  // Of that area.
  int surface_left = 0;   // Monitor pixels: the first column drawn on the surface,
  int surface_width = 0;  // and how wide it is.
  std::vector<ScrollPiece> pieces;  // Drawn in order, later ones over earlier ones.
  int content_top = 0;  // Thumbnail rows holding the capture so far.
  int content_bottom = 0;
  int view_bottom = 0;  // Where the part on screen now ends.
  bool finished = false;
  ScrollOutcome outcome = ScrollOutcome::kFailed;
  std::shared_ptr<const LongImage> image;  // The scrolling screenshot, unless it failed.
  RECT image_rect{};                       // Its place in plane coordinates.
};

// What an app reports of its scrolling, in monitor pixels. Only a hint at the edges of what moves.
struct ScrollHints {
  std::vector<RECT> areas;   // What scrolls, largest first.
  std::vector<RECT> frames;  // The parts of the window around each of them, as far as they show.
};

struct ScrollRequest {
  uint64_t session = 0;
  HWND controller = nullptr;
  HWND window = nullptr;     // The app being scrolled.
  RECT monitor{};            // Virtual screen coordinates.
  ScrollHints hints;
  float scale = 1.0f;
};

// Posts kMsgScrollUpdate as the capture changes, and one finished update at the end.
class ScrollRecorder {
 public:
  explicit ScrollRecorder(ScrollRequest request);
  ~ScrollRecorder();  // Calls it off without a word, if it is still running.
  ScrollRecorder(const ScrollRecorder&) = delete;
  ScrollRecorder& operator=(const ScrollRecorder&) = delete;

  // A wheel turned, in virtual screen coordinates. Turning it outside the app's window ends the capture early.
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
