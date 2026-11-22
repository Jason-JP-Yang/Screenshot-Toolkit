// Build tool: bakes the program icon from RenderLogo into an .ico file, so that the icon has one source, in code.
// Usage: sstk_iconbake <out.ico> [folder for a PNG of each size]
#include "sstk/graphics/image.h"

#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 256};

// Icons and PNG files keep straight alpha.
std::vector<uint8_t> Unpremultiply(const sstk::Image& image) {
  std::vector<uint8_t> pixels(image.data(), image.data() + static_cast<size_t>(image.stride()) * image.height());
  for (size_t i = 0; i < pixels.size(); i += 4) {
    const int alpha = pixels[i + 3];
    for (int c = 0; c < 3; ++c) pixels[i + c] = alpha == 0 ? 0 : static_cast<uint8_t>(std::min(255, (pixels[i + c] * 255 + alpha / 2) / alpha));
  }
  return pixels;
}

std::vector<uint8_t> EncodePng(IWICImagingFactory* factory, const std::vector<uint8_t>& pixels, int size) {
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  const bool encoded = SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
                       SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                       SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                       SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                       SUCCEEDED(frame->SetSize(size, size)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
                       IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
                       SUCCEEDED(frame->WritePixels(size, size * 4, static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()))) &&
                       SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  if (!encoded) return {};
  STATSTG stat{};
  HGLOBAL memory = nullptr;
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return {};
  const auto* bytes = static_cast<const uint8_t*>(GlobalLock(memory));
  std::vector<uint8_t> png(bytes, bytes + stat.cbSize.QuadPart);
  GlobalUnlock(memory);
  return png;
}

// A 32-bit DIB with its mask, rows from the bottom up, as icons keep their smaller sizes.
std::vector<uint8_t> EncodeDib(const std::vector<uint8_t>& pixels, int size) {
  BITMAPINFOHEADER header{};
  header.biSize = sizeof(header);
  header.biWidth = size;
  header.biHeight = size * 2;
  header.biPlanes = 1;
  header.biBitCount = 32;
  header.biCompression = BI_RGB;
  const int mask_stride = (size + 31) / 32 * 4;
  std::vector<uint8_t> dib(reinterpret_cast<const uint8_t*>(&header), reinterpret_cast<const uint8_t*>(&header) + sizeof(header));
  for (int y = size - 1; y >= 0; --y) {
    const uint8_t* row = pixels.data() + static_cast<size_t>(y) * size * 4;
    dib.insert(dib.end(), row, row + size * 4);
  }
  for (int y = size - 1; y >= 0; --y) {
    std::vector<uint8_t> mask(mask_stride, 0);
    for (int x = 0; x < size; ++x) {
      if (pixels[(static_cast<size_t>(y) * size + x) * 4 + 3] == 0) mask[x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
    }
    dib.insert(dib.end(), mask.begin(), mask.end());
  }
  return dib;
}

bool WriteAll(const wchar_t* path, const std::vector<uint8_t>& bytes) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path, L"wb") != 0 || file == nullptr) return false;
  const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  return fclose(file) == 0 && written;
}

void Append16(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void Append32(std::vector<uint8_t>& out, uint32_t value) {
  Append16(out, value & 0xFFFF);
  Append16(out, value >> 16);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    fwprintf(stderr, L"usage: sstk_iconbake <out.ico> [preview folder]\n");
    return 2;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
  int result = 1;
  {
    ComPtr<IWICImagingFactory> factory;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      std::vector<std::vector<uint8_t>> images;
      bool ok = true;
      for (const int size : kSizes) {
        const std::vector<uint8_t> pixels = Unpremultiply(sstk::RenderLogo(size));
        std::vector<uint8_t> png = EncodePng(factory.Get(), pixels, size);
        ok = ok && !png.empty();
        if (argc > 2) ok = ok && WriteAll((std::wstring(argv[2]) + L"\\logo-" + std::to_wstring(size) + L".png").c_str(), png);
        images.push_back(size >= 256 ? std::move(png) : EncodeDib(pixels, size));
      }

      std::vector<uint8_t> icon;
      Append16(icon, 0);
      Append16(icon, 1);
      Append16(icon, ARRAYSIZE(kSizes));
      uint32_t offset = 6 + 16 * ARRAYSIZE(kSizes);
      for (size_t i = 0; i < images.size(); ++i) {
        const int size = kSizes[i];
        icon.push_back(static_cast<uint8_t>(size >= 256 ? 0 : size));
        icon.push_back(static_cast<uint8_t>(size >= 256 ? 0 : size));
        icon.push_back(0);
        icon.push_back(0);
        Append16(icon, 1);
        Append16(icon, 32);
        Append32(icon, static_cast<uint32_t>(images[i].size()));
        Append32(icon, offset);
        offset += static_cast<uint32_t>(images[i].size());
      }
      for (const std::vector<uint8_t>& image : images) icon.insert(icon.end(), image.begin(), image.end());
      if (ok && WriteAll(argv[1], icon)) result = 0;
    }
  }
  CoUninitialize();
  if (result != 0) fwprintf(stderr, L"sstk_iconbake: could not write %ls\n", argv[1]);
  return result;
}
