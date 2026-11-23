// The editing panel's heavy work, on a thread of its own: the picture sharpened for a view at rest, which the
// compositor's own filtering would leave rough when it shrinks the picture, and the finished picture packed for the file.
#pragma once

#include "sstk/editor/canvas.h"
#include "sstk/scroll/longimage.h"
#include "sstk/storage/store.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace sstk {

struct EditorJobResult {
  enum class Kind { kSharp, kThumbnail, kPicture };
  Kind kind = Kind::kSharp;
  uint64_t ticket = 0;  // The sharpening it answers, or the version of the edit it was made from.
  std::shared_ptr<Shot> shot;
  Image pixels;  // The sharpened view, or the thumbnail.
  RECT screen{};  // Where the sharpened view goes, in monitor pixels.
  std::shared_ptr<const LongImage> picture;
};

// A view of the turned picture: screen = scale * turned + offset.
struct EditorView {
  float scale = 1.0f;
  float x = 0.0f;
  float y = 0.0f;
};

struct SharpenJob {
  uint64_t ticket = 0;
  CanvasSnapshot canvas;
  int turns = 0;
  EditorView view;
  RECT screen{};  // The monitor pixels to fill.
};

struct ExportJob {
  uint64_t version = 0;
  std::shared_ptr<Shot> shot;
  CanvasSnapshot canvas;
  int turns = 0;
  RECT crop{};  // Plane pixels.
  SIZE thumbnail{};
};

// Area-averaged pixels of the turned picture under `screen`, for a view that shrinks it.
Image Sharpen(const CanvasSnapshot& canvas, int turns, const EditorView& view, const RECT& screen);

class EditorJobs {
 public:
  explicit EditorJobs(HWND controller);
  ~EditorJobs();
  EditorJobs(const EditorJobs&) = delete;
  EditorJobs& operator=(const EditorJobs&) = delete;

  // Replaces a sharpening not started yet. Results come back to the controller as kMsgEditorJob.
  void Sharpen(SharpenJob job);
  void Export(ExportJob job);

 private:
  void Main();
  void Run(SharpenJob& job);
  void Run(ExportJob& job);
  void Post(std::unique_ptr<EditorJobResult> result);

  HWND controller_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<SharpenJob> sharpen_;
  std::deque<ExportJob> exports_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace sstk
