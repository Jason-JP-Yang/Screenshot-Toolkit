// A long image that is never held as one bitmap. Rows come in blocks, in any order, and are kept packed in strips, so
// that a scrolling screenshot of any length takes the memory of a small part of its pixels. Whoever needs pixels reads
// a few rows at a time.
#pragma once

#include "image.h"

#include <mutex>
#include <vector>

namespace sstk {

class LongImage {
 public:
  explicit LongImage(int width) : width_(width), right_(width) {}
  ~LongImage();
  LongImage(const LongImage&) = delete;
  LongImage& operator=(const LongImage&) = delete;

  // Takes rows [first, first + count) of 32-bit pixels, numbered as the writer likes. Rows that come again replace
  // the ones before. One thread writes, and nobody reads until the image is sealed.
  void Add(int first, int count, const uint32_t* pixels);

  // Ends the writing. The image is rows [top, bottom) and columns [left, right) of what was added, numbered from 0
  // from here on.
  void Seal(int top, int bottom, int left, int right);

  int width() const { return right_ - left_; }
  int height() const { return static_cast<int>(owner_.size()); }
  size_t packed_bytes() const;

  // Copies rows [row, row + count) out, as 32-bit BGRA or as packed 24-bit BGR. Rows never added come out black.
  void Read(int row, int count, uint8_t* out, ptrdiff_t stride, bool bgr24) const;

  // Area-weighted reduction of rows [top, bottom) across the whole width, as ResampleArea does for a bitmap.
  Image Resample(double top, double bottom, int width, int height) const;

 private:
  struct Strip {
    int first = 0;
    int count = 0;
    bool packed = false;
    std::vector<uint8_t> bytes;  // Rows of BGR, each less the row above it, then compressed.
  };

  bool Unpack(int index) const;

  int width_ = 0;  // Of the rows added.
  int left_ = 0;
  int right_ = 0;
  int top_ = 0;
  std::vector<Strip> strips_;
  std::vector<int> owner_;  // Per row of the sealed image: the strip holding it, or -1.
  std::vector<uint8_t> scratch_;
  void* packer_ = nullptr;

  mutable std::mutex mutex_;
  mutable std::vector<uint8_t> unpacked_;  // The strip read last, as plain BGR rows.
  mutable int unpacked_index_ = -1;
  mutable void* unpacker_ = nullptr;
};

}  // namespace sstk
