#include "store.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cwchar>

namespace sstk {

namespace {

std::wstring GalleryFolder() {
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
    clipboard_sequence_.fetch_add(1, std::memory_order_relaxed);
  }
  wake_.notify_all();
}

std::wstring Store::WaitUntilSaved(const std::shared_ptr<Shot>& shot) {
  std::unique_lock lock(mutex_);
  saved_.wait(lock, [&] { return shot->saved_version >= shot->requested_version || stopping_; });
  return shot->path;
}

bool Store::IsIdle() {
  std::lock_guard lock(mutex_);
  return !busy_ && pending_saves_.empty() && pending_clipboard_ == nullptr;
}

void Store::PrepareClipboard(const std::shared_ptr<Shot>& shot, const RECT& region, uint64_t sequence) {
  HGLOBAL dib = BuildDib(*shot->frame, region);
  if (dib == nullptr) return;
  if (!PostMessageW(controller_, kMsgClipboardReady, static_cast<WPARAM>(sequence), reinterpret_cast<LPARAM>(dib))) {
    GlobalFree(dib);
  }
}

bool Store::SaveToDisk(Shot& shot, const RECT& region) {
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

  const Image& frame = *shot.frame;
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
  return true;
}

void Store::WorkerMain() {
  (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  std::unique_lock lock(mutex_);
  for (;;) {
    busy_ = false;
    wake_.wait(lock, [&] {
      return stopping_ || warmup_requested_ || pending_clipboard_ != nullptr || !pending_saves_.empty();
    });
    busy_ = true;

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

    // The clipboard is served first because it is what the next keystroke may depend on.
    if (pending_clipboard_ != nullptr) {
      std::shared_ptr<Shot> shot = std::move(pending_clipboard_);
      pending_clipboard_ = nullptr;
      const RECT region = pending_clipboard_region_;
      const uint64_t sequence = clipboard_sequence_.load(std::memory_order_relaxed);
      lock.unlock();
      PrepareClipboard(shot, region, sequence);
      shot.reset();
      lock.lock();
      continue;
    }

    if (!pending_saves_.empty()) {
      std::shared_ptr<Shot> shot = pending_saves_.front();
      pending_saves_.erase(pending_saves_.begin());
      const RECT region = shot->region;
      const uint64_t version = shot->requested_version;
      lock.unlock();
      const double started = NowSeconds();
      const bool saved = SaveToDisk(*shot, region);
      Trace("save: %s in %.1f ms", saved ? "ok" : "FAILED", (NowSeconds() - started) * 1000.0);
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
