#include "longimage.h"

#include <compressapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sstk {

namespace {

constexpr int kStripRows = 128;      // Rows packed together: more pack tighter, fewer unpack faster.
constexpr int kResampleRows = 32;    // Rows of a reduction made from one read.

}  // namespace

LongImage::~LongImage() {
  if (packer_ != nullptr) CloseCompressor(static_cast<COMPRESSOR_HANDLE>(packer_));
  if (unpacker_ != nullptr) CloseDecompressor(static_cast<DECOMPRESSOR_HANDLE>(unpacker_));
}

void LongImage::Add(int first, int count, const uint32_t* pixels) {
  if (packer_ == nullptr) {
    COMPRESSOR_HANDLE handle = nullptr;
    if (CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle)) packer_ = handle;
  }
  const size_t row_bytes = static_cast<size_t>(width_) * 3;
  for (int done = 0; done < count; done += kStripRows) {
    const int rows = std::min(kStripRows, count - done);
    const size_t size = row_bytes * rows;
    scratch_.resize(size * 2);
    uint8_t* plain = scratch_.data();
    // Most rows of a screen are much like the row above, so each is kept as what it differs by.
    for (int y = 0; y < rows; ++y) {
      const uint32_t* source = pixels + static_cast<size_t>(done + y) * width_;
      const uint32_t* above = y > 0 ? source - width_ : nullptr;
      uint8_t* out = plain + row_bytes * y;
      for (int x = 0; x < width_; ++x, out += 3) {
        const uint32_t pixel = source[x];
        const uint32_t over = above != nullptr ? above[x] : 0;
        out[0] = static_cast<uint8_t>(pixel - over);
        out[1] = static_cast<uint8_t>((pixel >> 8) - (over >> 8));
        out[2] = static_cast<uint8_t>((pixel >> 16) - (over >> 16));
      }
    }
    Strip strip;
    strip.first = first + done;
    strip.count = rows;
    SIZE_T written = 0;
    if (packer_ != nullptr && Compress(static_cast<COMPRESSOR_HANDLE>(packer_), plain, size, plain + size, size, &written) && written < size) {
      strip.packed = true;
      strip.bytes.assign(plain + size, plain + size + written);
    } else {
      strip.bytes.assign(plain, plain + size);
    }
    strips_.push_back(std::move(strip));
  }
}

void LongImage::Seal(int top, int bottom, int left, int right) {
  left_ = std::clamp(left, 0, width_ - 1);
  right_ = std::clamp(right, left_ + 1, width_);
  top_ = top;
  owner_.assign(static_cast<size_t>(std::max(bottom - top, 0)), -1);
  // Later strips lie over earlier ones.
  for (size_t i = 0; i < strips_.size(); ++i) {
    const Strip& strip = strips_[i];
    for (int row = std::max(strip.first, top); row < std::min(strip.first + strip.count, bottom); ++row) owner_[static_cast<size_t>(row - top)] = static_cast<int>(i);
  }
  std::vector<uint8_t> used(strips_.size(), 0);
  for (const int index : owner_) {
    if (index >= 0) used[static_cast<size_t>(index)] = 1;
  }
  for (size_t i = 0; i < strips_.size(); ++i) {
    if (used[i] == 0) strips_[i].bytes = std::vector<uint8_t>{};
  }
  scratch_ = std::vector<uint8_t>{};
  if (packer_ != nullptr) CloseCompressor(static_cast<COMPRESSOR_HANDLE>(packer_));
  packer_ = nullptr;
}

size_t LongImage::packed_bytes() const {
  size_t bytes = 0;
  for (const Strip& strip : strips_) bytes += strip.bytes.size();
  return bytes;
}

// Leaves a strip's rows in `unpacked_` as plain BGR. The caller holds the lock.
bool LongImage::Unpack(int index) const {
  if (index == unpacked_index_) return true;
  const Strip& strip = strips_[static_cast<size_t>(index)];
  const size_t row_bytes = static_cast<size_t>(width_) * 3;
  const size_t size = row_bytes * strip.count;
  unpacked_.resize(size);
  unpacked_index_ = -1;
  if (strip.packed) {
    if (unpacker_ == nullptr) {
      DECOMPRESSOR_HANDLE handle = nullptr;
      if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle)) return false;
      unpacker_ = handle;
    }
    SIZE_T written = 0;
    if (!Decompress(static_cast<DECOMPRESSOR_HANDLE>(unpacker_), strip.bytes.data(), strip.bytes.size(), unpacked_.data(), size, &written) || written != size) return false;
  } else {
    if (strip.bytes.size() != size) return false;
    std::memcpy(unpacked_.data(), strip.bytes.data(), size);
  }
  for (int y = 1; y < strip.count; ++y) {
    uint8_t* row = unpacked_.data() + row_bytes * y;
    const uint8_t* above = row - row_bytes;
    for (size_t i = 0; i < row_bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + above[i]);
  }
  unpacked_index_ = index;
  return true;
}

void LongImage::Read(int row, int count, uint8_t* out, ptrdiff_t stride, bool bgr24) const {
  std::lock_guard lock(mutex_);
  const size_t row_bytes = static_cast<size_t>(width_) * 3;
  const int columns = width();
  for (int i = 0; i < count; ++i, out += stride) {
    const int at = row + i;
    const int index = at >= 0 && at < height() ? owner_[static_cast<size_t>(at)] : -1;
    if (index < 0 || !Unpack(index)) {
      std::memset(out, 0, static_cast<size_t>(columns) * (bgr24 ? 3 : 4));
      if (!bgr24) {
        for (int x = 0; x < columns; ++x) out[x * 4 + 3] = 0xFF;
      }
      continue;
    }
    const uint8_t* source = unpacked_.data() + row_bytes * (at + top_ - strips_[static_cast<size_t>(index)].first) + static_cast<size_t>(left_) * 3;
    if (bgr24) {
      std::memcpy(out, source, static_cast<size_t>(columns) * 3);
      continue;
    }
    uint32_t* pixels = reinterpret_cast<uint32_t*>(out);
    for (int x = 0; x < columns; ++x, source += 3) pixels[x] = 0xFF000000u | source[0] | static_cast<uint32_t>(source[1]) << 8 | static_cast<uint32_t>(source[2]) << 16;
  }
}

Image LongImage::Resample(double top, double bottom, int width, int height) const {
  Image result = Image::Allocate(width, height);
  if (!result || height <= 0 || this->height() <= 0) return result;
  top = std::clamp(top, 0.0, static_cast<double>(this->height()));
  bottom = std::clamp(bottom, top, static_cast<double>(this->height()));
  const double step = (bottom - top) / height;
  const int columns = this->width();
  std::vector<uint8_t> rows;
  for (int done = 0; done < height; done += kResampleRows) {
    const int part = std::min(kResampleRows, height - done);
    const double from = top + done * step;
    const double to = top + (done + part) * step;
    const int first = std::min(static_cast<int>(std::floor(from)), this->height() - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(to)), first + 1, this->height());
    rows.resize(static_cast<size_t>(last - first) * columns * 4);
    Read(first, last - first, rows.data(), static_cast<ptrdiff_t>(columns) * 4, false);
    const Image reduced = ResampleArea(PixelView{rows.data(), columns, last - first, columns * 4}, 0.0, from - first, columns, std::min(to - first, static_cast<double>(last - first)), width, part);
    if (!reduced) return Image();
    for (int y = 0; y < part; ++y) {
      std::memcpy(result.data() + static_cast<ptrdiff_t>(done + y) * result.stride(), reduced.data() + static_cast<ptrdiff_t>(y) * reduced.stride(), static_cast<size_t>(width) * 4);
    }
  }
  return result;
}

}  // namespace sstk
