#include "sstk/editor/jobs.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sstk {

namespace {

constexpr int kBandRows = 32;

}  // namespace

Image Sharpen(const CanvasSnapshot& canvas, int turns, const EditorView& view, const RECT& screen) {
  const int width = screen.right - screen.left;
  const int height = screen.bottom - screen.top;
  if (width <= 0 || height <= 0 || view.scale <= 0.0f || !canvas.base) return Image();
  const double left = (screen.left - view.x) / view.scale;
  const double right = (screen.right - view.x) / view.scale;
  const double top = (screen.top - view.y) / view.scale;
  const double bottom = (screen.bottom - view.y) / view.scale;
  // The footprint is read as a picture of its own, a band of rows at a time.
  const int first_column = static_cast<int>(std::floor(left));
  const int first_row = static_cast<int>(std::floor(top));
  const int columns = std::max(static_cast<int>(std::ceil(right)) - first_column, 1);
  const int rows = std::max(static_cast<int>(std::ceil(bottom)) - first_row, 1);
  Image out = Image::Allocate(width, height);
  if (!out) return out;
  const Resampler sampler(columns, rows, left - first_column, top - first_row, right - first_column, bottom - first_row, width, height);
  std::vector<uint32_t> band;
  for (int y = 0; y < height; y += kBandRows) {
    const int last = std::min(y + kBandRows, height);
    const int source_first = sampler.Row(y).first;
    const int source_last = sampler.Row(last - 1).last;
    band.resize(static_cast<size_t>(columns) * (source_last - source_first + 1));
    for (int row = source_first; row <= source_last; ++row) {
      canvas.TurnedRow(turns, first_row + row, first_column, columns, band.data() + static_cast<size_t>(row - source_first) * columns);
    }
    sampler.Make(y, last, reinterpret_cast<const uint8_t*>(band.data()), source_first, static_cast<ptrdiff_t>(columns) * 4, out.data(), out.stride());
  }
  return out;
}

EditorJobs::EditorJobs(HWND controller) : controller_(controller), thread_([this] { Main(); }) {}

EditorJobs::~EditorJobs() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void EditorJobs::Sharpen(SharpenJob job) {
  {
    std::lock_guard lock(mutex_);
    sharpen_ = std::move(job);
  }
  wake_.notify_all();
}

void EditorJobs::Export(ExportJob job) {
  {
    std::lock_guard lock(mutex_);
    exports_.push_back(std::move(job));
  }
  wake_.notify_all();
}

void EditorJobs::Post(std::unique_ptr<EditorJobResult> result) {
  if (PostMessageW(controller_, kMsgEditorJob, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
}

void EditorJobs::Main() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stopping_ || sharpen_.has_value() || !exports_.empty(); });
    if (stopping_) break;
    // The file waits on an export, and nothing waits on a sharper view.
    if (!exports_.empty()) {
      ExportJob job = std::move(exports_.front());
      exports_.pop_front();
      lock.unlock();
      Run(job);
      lock.lock();
      continue;
    }
    SharpenJob job = std::move(*sharpen_);
    sharpen_.reset();
    lock.unlock();
    Run(job);
    lock.lock();
  }
}

void EditorJobs::Run(SharpenJob& job) {
  auto result = std::make_unique<EditorJobResult>();
  result->kind = EditorJobResult::Kind::kSharp;
  result->ticket = job.ticket;
  result->screen = job.screen;
  result->pixels = sstk::Sharpen(job.canvas, job.turns, job.view, job.screen);
  job.canvas = CanvasSnapshot{};
  if (result->pixels) Post(std::move(result));
}

void EditorJobs::Run(ExportJob& job) {
  const double started = NowSeconds();
  const RECT turned = TurnRect(job.crop, job.turns);
  const int width = turned.right - turned.left;
  const int height = turned.bottom - turned.top;
  if (width <= 0 || height <= 0) return;

  // The thumbnail goes first, as the card waits for it to come back into the deck.
  auto thumbnail = std::make_unique<EditorJobResult>();
  thumbnail->kind = EditorJobResult::Kind::kThumbnail;
  thumbnail->ticket = job.version;
  thumbnail->shot = job.shot;
  const float scale = static_cast<float>(job.thumbnail.cx) / static_cast<float>(width);
  thumbnail->pixels = sstk::Sharpen(job.canvas, job.turns, EditorView{scale, -turned.left * scale, -turned.top * scale},
                                    RECT{0, 0, job.thumbnail.cx, job.thumbnail.cy});
  Post(std::move(thumbnail));

  auto picture = std::make_shared<LongImage>(width);
  std::vector<uint32_t> rows(static_cast<size_t>(width) * LongImage::kStripRows);
  for (int y = 0; y < height; y += LongImage::kStripRows) {
    const int count = std::min(LongImage::kStripRows, height - y);
    for (int row = 0; row < count; ++row) {
      job.canvas.TurnedRow(job.turns, turned.top + y + row, turned.left, width, rows.data() + static_cast<size_t>(row) * width);
    }
    picture->Add(y, count, rows.data());
  }
  picture->Seal(0, height, 0, width);
  job.canvas = CanvasSnapshot{};
  Trace("editor: %dx%d picture made in %.1f ms", width, height, (NowSeconds() - started) * 1000.0);

  auto result = std::make_unique<EditorJobResult>();
  result->kind = EditorJobResult::Kind::kPicture;
  result->ticket = job.version;
  result->shot = std::move(job.shot);
  result->picture = std::move(picture);
  Post(std::move(result));
}

}  // namespace sstk
