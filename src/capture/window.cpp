#include "sstk/capture/window.h"

#include "sstk/config/core.h"

#include <d3d11_4.h>
#include <dxgi.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sstk {

namespace {

namespace capture = winrt::Windows::Graphics::Capture;

// Windows leaves the border out only for apps it lets do so, which unpackaged ones are from Windows 11 on.
bool BorderlessCapture() {
  static const bool allowed = [] {
    try {
      using winrt::Windows::Foundation::Metadata::ApiInformation;
      if (!ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) return false;
      const auto status = capture::GraphicsCaptureAccess::RequestAccessAsync(capture::GraphicsCaptureAccessKind::Borderless).get();
      return status == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
    } catch (...) {
      return false;
    }
  }();
  return allowed;
}

struct Signal {
  Signal() : handle(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
  ~Signal() {
    if (handle != nullptr) CloseHandle(handle);
  }
  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;
  HANDLE handle;
};

// Fills rows lying at `at` in virtual screen coordinates with what the screen showed there, and black off it.
void Underlay(uint32_t* rows, int width, int height, const RECT& at, const Image& screen, const RECT& monitor) {
  for (int y = 0; y < height; ++y) {
    uint32_t* out = rows + static_cast<size_t>(y) * width;
    std::fill(out, out + width, 0xFF000000u);
    const int sy = at.top + y - monitor.top;
    if (sy < 0 || sy >= screen.height()) continue;
    const int first = std::max(static_cast<int>(monitor.left - at.left), 0);
    const int last = std::min(static_cast<int>(monitor.left - at.left) + screen.width(), width);
    if (first >= last) continue;
    const auto* line = reinterpret_cast<const uint32_t*>(screen.data() + static_cast<ptrdiff_t>(sy) * screen.stride());
    const int shift = static_cast<int>(at.left - monitor.left);
    std::copy(line + (first + shift), line + (last + shift), out + first);
  }
}

// What the window leaves see-through, as its rounded corners, shows what the screen showed beneath it. A pixel that
// cannot be premultiplied, as some apps leave theirs, is opaque.
void Flatten(Image& pixels, const RECT& frame, const Image& screen, const RECT& monitor) {
  for (int y = 0; y < pixels.height(); ++y) {
    auto* row = reinterpret_cast<uint32_t*>(pixels.data() + static_cast<ptrdiff_t>(y) * pixels.stride());
    const int sy = frame.top + y - monitor.top;
    const auto* under = sy >= 0 && sy < screen.height() ? reinterpret_cast<const uint32_t*>(screen.data() + static_cast<ptrdiff_t>(sy) * screen.stride()) : nullptr;
    for (int x = 0; x < pixels.width(); ++x) {
      const uint32_t pixel = row[x];
      const uint32_t alpha = pixel >> 24;
      if (alpha == 255) continue;
      if ((pixel & 0xFF) > alpha || ((pixel >> 8) & 0xFF) > alpha || ((pixel >> 16) & 0xFF) > alpha) {
        row[x] = pixel | 0xFF000000u;
        continue;
      }
      const int sx = frame.left + x - monitor.left;
      const uint32_t back = under != nullptr && sx >= 0 && sx < screen.width() ? under[sx] : 0u;
      const uint32_t keep = 255 - alpha;
      uint32_t mixed = 0xFF000000u;
      for (int shift = 0; shift < 24; shift += 8) {
        const uint32_t channel = ((pixel >> shift) & 0xFF) + (((back >> shift) & 0xFF) * keep + 127) / 255;
        mixed |= std::min(channel, 255u) << shift;
      }
      row[x] = mixed;
    }
  }
}

// The window drawn by itself, over what the screen showed, where no capture can leave the border out.
bool Print(HWND window, const RECT& frame, const Image& screen, const RECT& monitor, Image& pixels) {
  RECT bounds{};
  RECT inside{};
  if (IsHungAppWindow(window) || !GetWindowRect(window, &bounds) || !IntersectRect(&inside, &bounds, &frame) || !EqualRect(&inside, &frame)) return false;
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) return false;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool printed = false;
  if (dib != nullptr) {
    auto* rows = static_cast<uint32_t*>(bits);
    Underlay(rows, width, height, bounds, screen, monitor);
    const HGDIOBJ previous = SelectObject(dc, dib);
    printed = PrintWindow(window, dc, PW_RENDERFULLCONTENT) != FALSE;
    GdiFlush();
    SelectObject(dc, previous);
    if (printed) {
      for (int y = 0; y < pixels.height(); ++y) {
        const uint32_t* source = rows + static_cast<size_t>(frame.top - bounds.top + y) * width + (frame.left - bounds.left);
        auto* out = reinterpret_cast<uint32_t*>(pixels.data() + static_cast<ptrdiff_t>(y) * pixels.stride());
        for (int x = 0; x < pixels.width(); ++x) out[x] = source[x] | 0xFF000000u;
      }
    }
    DeleteObject(dib);
  }
  DeleteDC(dc);
  return printed;
}

Image Reduce(const Image& pixels, SIZE size) {
  if (pixels.width() / size.cx >= 2 && pixels.height() / size.cy >= 2) {
    const Image half = Halve(pixels);
    if (half) return ResampleArea(half, 0.0, 0.0, half.width(), half.height(), size.cx, size.cy);
  }
  return ResampleArea(pixels, 0.0, 0.0, pixels.width(), pixels.height(), size.cx, size.cy);
}

}  // namespace

struct CaptureDevice::State {
  ComPtr<ID3D11Device> d3d;
  ComPtr<ID3D11DeviceContext> context;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{nullptr};
};

CaptureDevice::CaptureDevice() : state_(std::make_unique<State>()) {}

CaptureDevice::~CaptureDevice() = default;

bool CaptureDevice::Open() {
  State& s = *state_;
  if (s.device) return true;
  for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &s.d3d, nullptr, &s.context))) break;
  }
  if (!s.d3d || !s.context) return false;
  // Capture reaches into the device from threads of its own.
  ComPtr<ID3D11Multithread> guard;
  if (SUCCEEDED(s.context.As(&guard))) guard->SetMultithreadProtected(TRUE);
  ComPtr<IDXGIDevice> dxgi;
  winrt::com_ptr<::IInspectable> inspectable;
  if (FAILED(s.d3d.As(&dxgi)) || FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) {
    Close();
    return false;
  }
  try {
    s.device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  } catch (...) {
    Close();
    return false;
  }
  return true;
}

void CaptureDevice::Close() { state_ = std::make_unique<State>(); }

struct WindowStream::State {
  State() = default;
  ~State() {
    texture.Reset();
    try {
      if (current) current.Close();
      if (session) session.Close();
      if (pool) {
        pool.FrameArrived(token);
        pool.Close();
      }
    } catch (...) {
    }
  }
  State(const State&) = delete;
  State& operator=(const State&) = delete;

  // Moves on to the newest picture, if one has come. Returns whether there is any.
  bool Take() {
    try {
      while (capture::Direct3D11CaptureFrame next = pool.TryGetNextFrame()) {
        ComPtr<ID3D11Texture2D> surface;
        const auto access = next.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&surface)))) {
          next.Close();
          continue;
        }
        texture.Reset();
        if (current) current.Close();
        current = next;
        texture = surface;
        const auto size = next.ContentSize();
        content = SIZE{size.Width, size.Height};
      }
    } catch (...) {
    }
    return texture != nullptr;
  }

  // Where the frame's top-left corner lies in the picture. Depending on the version of Windows, a picture holds the
  // visible frame alone or the whole window rectangle around it.
  POINT Origin() const {
    const int frame_w = frame.right - frame.left;
    const int frame_h = frame.bottom - frame.top;
    if (std::abs(content.cx - frame_w) <= 1 && std::abs(content.cy - frame_h) <= 1) return POINT{0, 0};
    if (std::abs(content.cx - (bounds.right - bounds.left)) <= 1 && std::abs(content.cy - (bounds.bottom - bounds.top)) <= 1) {
      return POINT{frame.left - bounds.left, frame.top - bounds.top};
    }
    return POINT{std::max((content.cx - frame_w) / 2, 0L), std::max((content.cy - frame_h) / 2, 0L)};
  }

  bool EnsureStaging() {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (staging) {
      D3D11_TEXTURE2D_DESC have{};
      staging->GetDesc(&have);
      if (have.Width >= desc.Width && have.Height >= desc.Height && have.Format == desc.Format) return true;
      staging.Reset();
    }
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    return SUCCEEDED(d3d->CreateTexture2D(&desc, nullptr, &staging));
  }

  ID3D11Device* d3d = nullptr;
  ID3D11DeviceContext* context = nullptr;
  RECT frame{};
  RECT bounds{};
  std::shared_ptr<Signal> arrived = std::make_shared<Signal>();
  capture::Direct3D11CaptureFramePool pool{nullptr};
  capture::GraphicsCaptureSession session{nullptr};
  winrt::event_token token{};
  capture::Direct3D11CaptureFrame current{nullptr};
  ComPtr<ID3D11Texture2D> texture;  // The newest picture, held until another takes its place.
  ComPtr<ID3D11Texture2D> staging;
  SIZE content{};
};

WindowStream::WindowStream() = default;

WindowStream::~WindowStream() = default;

bool WindowStream::Start(CaptureDevice& device, HWND window, bool borderless) {
  state_.reset();
  if (!device.Open()) return false;
  auto state = std::make_unique<State>();
  if (state->arrived->handle == nullptr || !WholeFrame(window, &state->frame) || !GetWindowRect(window, &state->bounds)) return false;
  state->d3d = device.state_->d3d.Get();
  state->context = device.state_->context.Get();
  try {
    if (!capture::GraphicsCaptureSession::IsSupported()) return false;
    const bool quiet = BorderlessCapture();
    if (borderless && !quiet) return false;
    const auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    capture::GraphicsCaptureItem item{nullptr};
    const winrt::guid& item_id = winrt::guid_of<capture::GraphicsCaptureItem>();
    winrt::check_hresult(interop->CreateForWindow(window, reinterpret_cast<const GUID&>(item_id), winrt::put_abi(item)));
    state->pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(device.state_->device, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    const std::shared_ptr<Signal> arrived = state->arrived;
    state->token = state->pool.FrameArrived([arrived](const capture::Direct3D11CaptureFramePool&, const winrt::Windows::Foundation::IInspectable&) { SetEvent(arrived->handle); });
    state->session = state->pool.CreateCaptureSession(item);
    try {
      state->session.IsCursorCaptureEnabled(false);
    } catch (...) {
    }
    if (quiet) {
      try {
        state->session.IsBorderRequired(false);
      } catch (...) {
        if (borderless) return false;
      }
    }
    state->session.StartCapture();
  } catch (...) {
    return false;
  }
  state_ = std::move(state);
  return true;
}

bool WindowStream::Wait(DWORD ms) {
  if (!state_) return false;
  const ULONGLONG deadline = GetTickCount64() + ms;
  while (!state_->Take()) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    WaitForSingleObject(state_->arrived->handle, static_cast<DWORD>(deadline - now));
  }
  return true;
}

const RECT& WindowStream::frame() const {
  static const RECT none{};
  return state_ ? state_->frame : none;
}

bool WindowStream::Read(const RECT& rect, uint32_t* out, ptrdiff_t stride) {
  if (!state_ || !state_->Take()) return false;
  State& s = *state_;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  const POINT origin = s.Origin();
  const RECT wanted{rect.left + origin.x, rect.top + origin.y, rect.right + origin.x, rect.bottom + origin.y};
  const RECT picture{0, 0, s.content.cx, s.content.cy};
  RECT part{};
  const bool any = IntersectRect(&part, &wanted, &picture) != FALSE;
  auto* rows = reinterpret_cast<uint8_t*>(out);
  if (!any || !EqualRect(&part, &wanted)) {
    for (int y = 0; y < height; ++y) {
      auto* row = reinterpret_cast<uint32_t*>(rows + y * stride);
      std::fill(row, row + width, 0xFF000000u);
    }
  }
  if (!any) return true;
  if (!s.EnsureStaging()) return false;
  const D3D11_BOX box{static_cast<UINT>(part.left), static_cast<UINT>(part.top), 0, static_cast<UINT>(part.right), static_cast<UINT>(part.bottom), 1};
  s.context->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, s.texture.Get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(s.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  const int dx = part.left - wanted.left;
  const int dy = part.top - wanted.top;
  const size_t bytes = static_cast<size_t>(part.right - part.left) * 4;
  for (int y = 0; y < part.bottom - part.top; ++y) {
    std::memcpy(rows + (y + dy) * stride + static_cast<ptrdiff_t>(dx) * 4, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, bytes);
  }
  s.context->Unmap(s.staging.Get(), 0);
  return true;
}

std::vector<WindowShot> GrabWindows(const std::vector<WindowFrame>& windows, const Image& screen, const RECT& monitor, float scale,
                                    CaptureDevice& device) {
  // Every capture starts at once, so that all wait for the same composed frame.
  std::vector<std::unique_ptr<WindowStream>> streams(windows.size());
  if (BorderlessCapture() && device.Open()) {
    for (size_t i = 0; i < windows.size(); ++i) {
      auto stream = std::make_unique<WindowStream>();
      if (stream->Start(device, windows[i].window, true)) streams[i] = std::move(stream);
    }
  }
  std::vector<WindowShot> shots;
  for (size_t i = 0; i < windows.size(); ++i) {
    const WindowFrame& entry = windows[i];
    RECT now{};
    if (!IsWindow(entry.window) || IsIconic(entry.window) || !WholeFrame(entry.window, &now) || !EqualRect(&now, &entry.frame)) continue;
    const int width = entry.frame.right - entry.frame.left;
    const int height = entry.frame.bottom - entry.frame.top;
    if (width <= 0 || height <= 0) continue;
    Image pixels = Image::Allocate(width, height);
    if (!pixels) continue;
    bool read = false;
    if (streams[i] && streams[i]->Wait(config::kWindowPictureWaitMs) && streams[i]->Read(RECT{0, 0, width, height}, reinterpret_cast<uint32_t*>(pixels.data()), pixels.stride())) {
      Flatten(pixels, entry.frame, screen, monitor);
      read = true;
    }
    streams[i].reset();
    if (!read) read = Print(entry.window, entry.frame, screen, monitor, pixels);
    if (!read) continue;
    WindowShot shot;
    shot.window = entry.window;
    shot.rect = RECT{entry.frame.left - monitor.left, entry.frame.top - monitor.top, entry.frame.right - monitor.left, entry.frame.bottom - monitor.top};
    auto image = std::make_shared<LongImage>(width);
    image->Add(0, height, reinterpret_cast<const uint32_t*>(pixels.data()));
    image->Seal(0, height, 0, width);
    shot.image = std::move(image);
    shot.thumbnail = Reduce(pixels, ThumbnailSize(shot.rect, scale));
    shots.push_back(std::move(shot));
  }
  return shots;
}

}  // namespace sstk
