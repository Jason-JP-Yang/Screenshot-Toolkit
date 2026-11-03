#include "store.h"

#include "config.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cwchar>

namespace sstk {

namespace {

std::wstring GalleryFolder() {
  // Test tooling redirects output so that automated runs never touch the real Gallery.
  wchar_t override_path[MAX_PATH];
  if (GetEnvironmentVariableW(L"SSTK_GALLERY_DIR", override_path, MAX_PATH) > 0) return override_path;

  PWSTR folder = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &folder))) result = folder;
  CoTaskMemFree(folder);
  return result;
}

std::wstring AllocateFilePath(const SYSTEMTIME& time) {
  const std::wstring folder = GalleryFolder();
  if (folder.empty()) return {};

  wchar_t stem[64];
  swprintf_s(stem, L"Screenshot %04d-%02d-%02d %02d%02d%02d", time.wYear, time.wMonth, time.wDay, time.wHour,
             time.wMinute, time.wSecond);
  std::wstring path = folder + L"\\" + stem + L".png";
  for (int suffix = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix) {
    path = folder + L"\\" + stem + L" (" + std::to_wstring(suffix) + L").png";
  }
  return path;
}

bool WriteFileAtomically(const std::wstring& path, IStream* stream) {
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
  HGLOBAL memory = nullptr;
  if (FAILED(GetHGlobalFromStream(stream, &memory))) return false;
  const void* bytes = GlobalLock(memory);
  if (bytes == nullptr) return false;

  const std::wstring temporary = path + L".tmp";
  bool written = false;
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD count = 0;
    written = WriteFile(file, bytes, static_cast<DWORD>(stat.cbSize.QuadPart), &count, nullptr) &&
              count == stat.cbSize.QuadPart;
    CloseHandle(file);
  }
  GlobalUnlock(memory);
  if (!written) {
    DeleteFileW(temporary.c_str());
    return false;
  }

  // A viewer may be holding the previous version for a moment, so a replace is retried briefly.
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
      return true;
    }
    Sleep(20);
  }
  DeleteFileW(temporary.c_str());
  return false;
}

void RemoveShotFile(const std::wstring& path) {
  if constexpr (config::kDeleteToRecycleBin) {
    // The list of paths ends with an empty string, which c_str() supplies after the embedded terminator.
    std::wstring list = path;
    list.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = list.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&operation);
    if (result == 0) return;
    Trace("discard: recycling failed with %d", result);
  }
  if (!DeleteFileW(path.c_str())) Trace("discard: could not delete the file, error %lu", GetLastError());
}

// Packed bottom-up 24-bit DIB: the form every clipboard consumer understands without alpha surprises.
HGLOBAL BuildDib(const Image& frame, const RECT& region) {
  const int width = region.right - region.left;
  const int height = region.bottom - region.top;
  const size_t row_bytes = (static_cast<size_t>(width) * 3 + 3) & ~static_cast<size_t>(3);
  const size_t image_bytes = row_bytes * height;

  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + image_bytes);
  if (memory == nullptr) return nullptr;
  auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
  if (header == nullptr) {
    GlobalFree(memory);
    return nullptr;
  }
  *header = BITMAPINFOHEADER{};
  header->biSize = sizeof(BITMAPINFOHEADER);
  header->biWidth = width;
  header->biHeight = height;
  header->biPlanes = 1;
  header->biBitCount = 24;
  header->biCompression = BI_RGB;
  header->biSizeImage = static_cast<DWORD>(image_bytes);

  uint8_t* pixels = reinterpret_cast<uint8_t*>(header + 1);
  for (int y = 0; y < height; ++y) {
    const uint8_t* source = frame.data() + static_cast<ptrdiff_t>(region.top + y) * frame.stride() + static_cast<ptrdiff_t>(region.left) * 4;
    uint8_t* target = pixels + static_cast<ptrdiff_t>(height - 1 - y) * row_bytes;
    for (int x = 0; x < width; ++x, source += 4, target += 3) {
      target[0] = source[0];
      target[1] = source[1];
      target[2] = source[2];
    }
  }
  GlobalUnlock(memory);
  return memory;
}

}  // namespace

void Store::Start(HWND controller) {
  controller_ = controller;
  worker_ = std::thread([this] { WorkerMain(); });
}

void Store::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void Store::Warmup() {
  {
    std::lock_guard lock(mutex_);
    warmup_requested_ = true;
  }
  wake_.notify_all();
}

void Store::ReleaseCodec() {
  std::unique_lock lock(mutex_);
  codec_release_requested_ = true;
  wake_.notify_all();
  saved_.wait(lock, [&] { return !codec_release_requested_ || stopping_; });
}

void Store::Submit(const std::shared_ptr<Shot>& shot, const RECT& region) {
  {
    std::lock_guard lock(mutex_);
    shot->region = region;
    ++shot->requested_version;
    if (std::find(pending_saves_.begin(), pending_saves_.end(), shot) == pending_saves_.end()) {
      pending_saves_.push_back(shot);
    }
    pending_clipboard_ = shot;
    pending_clipboard_region_ = region;
    clipboard_shot_ = shot.get();
    clipboard_sequence_.fetch_add(1, std::memory_order_relaxed);
  }
  wake_.notify_all();
}

bool Store::Discard(const std::shared_ptr<Shot>& shot) {
  bool on_clipboard = false;
  {
    std::lock_guard lock(mutex_);
    pending_saves_.erase(std::remove(pending_saves_.begin(), pending_saves_.end(), shot), pending_saves_.end());
    if (pending_clipboard_ == shot) pending_clipboard_ = nullptr;
    if (clipboard_shot_ == shot.get()) {
      on_clipboard = true;
      clipboard_shot_ = nullptr;
      // Whatever is still on its way to the clipboard is now out of date.
      clipboard_sequence_.fetch_add(1, std::memory_order_relaxed);
    }
    // Nobody should keep waiting for a save that is no longer coming.
    shot->saved_version = shot->requested_version;
    pending_discards_.push_back(shot);
  }
  saved_.notify_all();
  wake_.notify_all();
  return on_clipboard;
}

std::wstring Store::WaitUntilSaved(const std::shared_ptr<Shot>& shot) {
  std::unique_lock lock(mutex_);
  saved_.wait(lock, [&] { return shot->saved_version >= shot->requested_version || stopping_; });
  return shot->path;
}

bool Store::IsIdle() {
  std::lock_guard lock(mutex_);
  return !busy_ && pending_saves_.empty() && pending_clipboard_ == nullptr && pending_discards_.empty();
}

std::shared_ptr<const Image> Store::FrameForWork(const std::shared_ptr<Shot>& shot) {
  {
    std::lock_guard lock(mutex_);
    if (shot->frame) return shot->frame;
  }
  if (work_frame_shot_ != shot.get()) {
    work_frame_ = DecodeArchive(shot->archive);
    work_frame_shot_ = shot.get();
  }
  return work_frame_;
}

void Store::PrepareClipboard(const std::shared_ptr<Shot>& shot, const RECT& region, uint64_t sequence) {
  std::shared_ptr<const Image> frame = FrameForWork(shot);
  if (!frame) return;
  HGLOBAL dib = BuildDib(*frame, region);
  if (dib == nullptr) return;
  if (!PostMessageW(controller_, kMsgClipboardReady, static_cast<WPARAM>(sequence), reinterpret_cast<LPARAM>(dib))) {
    GlobalFree(dib);
  }
}

bool Store::SaveToDisk(Shot& shot, const std::shared_ptr<const Image>& frame_pointer, const RECT& region) {
  if (shot.path.empty()) shot.path = AllocateFilePath(shot.local_time);
  if (shot.path.empty()) {
    Trace("save: no gallery path");
    return false;
  }

  HRESULT hr = S_OK;
  auto ok = [&hr](HRESULT result, const char* step) {
    hr = result;
    if (FAILED(result)) Trace("save: %s failed with %08lx", step, result);
    return SUCCEEDED(result);
  };

  if (!codec_factory_ && !ok(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&codec_factory_)), "factory")) {
    return false;
  }

  ComPtr<IStream> stream;
  if (!ok(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "stream")) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (!ok(codec_factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "encoder")) return false;
  if (!ok(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder init")) return false;

  ComPtr<IWICBitmapFrameEncode> target;
  ComPtr<IPropertyBag2> options;
  if (!ok(encoder->CreateNewFrame(&target, &options), "new frame")) return false;
  if (!ok(target->Initialize(options.Get()), "frame init")) return false;

  const int width = region.right - region.left;
  const int height = region.bottom - region.top;
  if (!ok(target->SetSize(width, height), "size")) return false;
  if (!ok(target->SetResolution(96.0, 96.0), "resolution")) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (!ok(target->SetPixelFormat(&format), "pixel format")) return false;

  const Image& frame = *frame_pointer;
  ComPtr<IWICBitmap> source;
  if (!ok(codec_factory_->CreateBitmapFromMemory(frame.width(), frame.height(), GUID_WICPixelFormat32bppBGR,
                                                 frame.stride(), frame.stride() * frame.height(),
                                                 const_cast<BYTE*>(frame.data()), &source), "source")) {
    return false;
  }
  ComPtr<IWICFormatConverter> converter;
  if (!ok(codec_factory_->CreateFormatConverter(&converter), "converter")) return false;
  if (!ok(converter->Initialize(source.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom), "converter init")) {
    return false;
  }

  WICRect crop{region.left, region.top, width, height};
  if (!ok(target->WriteSource(converter.Get(), &crop), "write")) return false;
  if (!ok(target->Commit(), "frame commit") || !ok(encoder->Commit(), "encoder commit")) return false;
  if (!WriteFileAtomically(shot.path, stream.Get())) {
    Trace("save: could not write the file, error %lu", GetLastError());
    return false;
  }

  // The first encode of the whole frame doubles as the in-memory archive.
  const bool whole_frame = width == frame.width() && height == frame.height();
  STATSTG stat{};
  HGLOBAL memory = nullptr;
  if (whole_frame && SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
      SUCCEEDED(GetHGlobalFromStream(stream.Get(), &memory))) {
    if (const void* bytes = GlobalLock(memory)) {
      const auto* first = static_cast<const uint8_t*>(bytes);
      std::vector<uint8_t> archive(first, first + static_cast<size_t>(stat.cbSize.QuadPart));
      GlobalUnlock(memory);
      std::lock_guard lock(mutex_);
      if (shot.archive.empty()) shot.archive = std::move(archive);
    }
  }
  return true;
}

std::shared_ptr<const Image> Store::DecodeArchive(const std::vector<uint8_t>& archive) {
  if (!codec_factory_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&codec_factory_)))) {
    return nullptr;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(codec_factory_->CreateStream(&stream))) return nullptr;
  if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(archive.data()), static_cast<DWORD>(archive.size())))) {
    return nullptr;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(codec_factory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
    return nullptr;
  }
  ComPtr<IWICBitmapFrameDecode> source;
  if (FAILED(decoder->GetFrame(0, &source))) return nullptr;
  ComPtr<IWICBitmapSource> converted;
  if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGR, source.Get(), &converted))) return nullptr;
  UINT width = 0;
  UINT height = 0;
  if (FAILED(converted->GetSize(&width, &height))) return nullptr;
  auto image = std::make_shared<Image>(Image::Allocate(static_cast<int>(width), static_cast<int>(height)));
  if (!*image) return nullptr;
  if (FAILED(converted->CopyPixels(nullptr, image->stride(), image->stride() * image->height(), image->data()))) {
    return nullptr;
  }
  return image;
}

bool Store::ReleaseFrame(const std::shared_ptr<Shot>& shot) {
  std::lock_guard lock(mutex_);
  if (!shot->frame) return true;
  if (shot->archive.empty() || shot->saved_version < shot->requested_version) return false;
  if (pending_clipboard_ == shot || active_shot_ == shot.get()) return false;
  shot->frame.reset();
  return true;
}

std::shared_ptr<const Image> Store::AcquireFrame(const std::shared_ptr<Shot>& shot) {
  std::unique_lock lock(mutex_);
  if (shot->frame) return shot->frame;
  pending_restore_ = shot;
  wake_.notify_all();
  saved_.wait(lock, [&] { return shot->frame != nullptr || pending_restore_ != shot || stopping_; });
  return shot->frame;
}

void Store::WorkerMain() {
  (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  std::unique_lock lock(mutex_);
  for (;;) {
    busy_ = false;
    active_shot_ = nullptr;
    if (pending_clipboard_ == nullptr && pending_saves_.empty() && pending_discards_.empty()) {
      work_frame_.reset();
      work_frame_shot_ = nullptr;
    }
    wake_.wait(lock, [&] {
      return stopping_ || warmup_requested_ || codec_release_requested_ || pending_restore_ != nullptr ||
             pending_clipboard_ != nullptr || !pending_discards_.empty() || !pending_saves_.empty();
    });
    busy_ = true;

    if (codec_release_requested_) {
      lock.unlock();
      codec_factory_.Reset();
      CoFreeUnusedLibrariesEx(0, 0);
      lock.lock();
      codec_release_requested_ = false;
      saved_.notify_all();
      continue;
    }

    if (warmup_requested_) {
      warmup_requested_ = false;
      lock.unlock();
      GalleryFolder();
      if (!codec_factory_) {
        (void)CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&codec_factory_));
      }
      lock.lock();
      continue;
    }

    // The UI thread is waiting on a restore, so it goes ahead of everything else.
    if (pending_restore_ != nullptr) {
      std::shared_ptr<Shot> shot = pending_restore_;
      lock.unlock();
      std::shared_ptr<const Image> frame = DecodeArchive(shot->archive);
      lock.lock();
      shot->frame = std::move(frame);
      pending_restore_ = nullptr;
      saved_.notify_all();
      continue;
    }

    // The clipboard comes next because it is what the next keystroke may depend on.
    if (pending_clipboard_ != nullptr) {
      std::shared_ptr<Shot> shot = std::move(pending_clipboard_);
      pending_clipboard_ = nullptr;
      active_shot_ = shot.get();
      const RECT region = pending_clipboard_region_;
      const uint64_t sequence = clipboard_sequence_.load(std::memory_order_relaxed);
      lock.unlock();
      PrepareClipboard(shot, region, sequence);
      shot.reset();
      lock.lock();
      continue;
    }

    // Any save of this shot that was already running has finished by now, so the file is complete.
    if (!pending_discards_.empty()) {
      std::shared_ptr<Shot> shot = std::move(pending_discards_.back());
      pending_discards_.pop_back();
      const std::wstring path = shot->path;
      if (work_frame_shot_ == shot.get()) {
        work_frame_.reset();
        work_frame_shot_ = nullptr;
      }
      lock.unlock();
      if (!path.empty()) RemoveShotFile(path);
      shot.reset();
      lock.lock();
      continue;
    }

    if (!pending_saves_.empty()) {
      std::shared_ptr<Shot> shot = pending_saves_.front();
      pending_saves_.erase(pending_saves_.begin());
      active_shot_ = shot.get();
      const RECT region = shot->region;
      const uint64_t version = shot->requested_version;
      lock.unlock();
      const double started = NowSeconds();
      std::shared_ptr<const Image> frame = FrameForWork(shot);
      const bool saved = frame != nullptr && SaveToDisk(*shot, frame, region);
      Trace("save: %s in %.1f ms", saved ? "ok" : "FAILED", (NowSeconds() - started) * 1000.0);
      frame.reset();
      lock.lock();
      shot->saved_version = version;
      // A failed write still releases waiters so that the UI cannot hang on a full disk.
      saved_.notify_all();
      continue;
    }

    if (stopping_) break;
  }

  saved_.notify_all();
  lock.unlock();
  codec_factory_.Reset();
  CoUninitialize();
}

}  // namespace sstk
