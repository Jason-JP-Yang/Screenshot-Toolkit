#include "compositor.h"

#include <dwmapi.h>
#include <dxgi1_3.h>

#include <algorithm>

namespace sstk {

bool Compositor::Initialize() {
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED, &level, 1,
                                 D3D11_SDK_VERSION, &d3d_device_, nullptr, &d3d_context_);
  if (FAILED(hr)) {
    Trace("D3D11CreateDevice failed: %08lx", hr);
    return false;
  }
  ComPtr<IDXGIDevice> dxgi;
  d3d_device_.As(&dxgi);
  hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&device_));
  if (FAILED(hr)) {
    Trace("DCompositionCreateDevice failed: %08lx", hr);
    return false;
  }
  return true;
}

void Compositor::Shutdown() {
  solid_colors_.clear();
  device_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
}

bool Compositor::IsHealthy() {
  BOOL valid = FALSE;
  return device_ && SUCCEEDED(device_->CheckDeviceState(&valid)) && valid;
}

ComPtr<IDCompositionVisual> Compositor::CreateVisual() {
  ComPtr<IDCompositionVisual> visual;
  device_->CreateVisual(&visual);
  return visual;
}

ComPtr<IDCompositionEffectGroup> Compositor::CreateEffectGroup() {
  ComPtr<IDCompositionEffectGroup> group;
  device_->CreateEffectGroup(&group);
  return group;
}

ComPtr<IDCompositionRectangleClip> Compositor::CreateClip() {
  ComPtr<IDCompositionRectangleClip> clip;
  device_->CreateRectangleClip(&clip);
  return clip;
}

ComPtr<IDCompositionScaleTransform> Compositor::CreateScale() {
  ComPtr<IDCompositionScaleTransform> scale;
  device_->CreateScaleTransform(&scale);
  return scale;
}

ComPtr<IDCompositionSurface> Compositor::CreateSurface(const Image& image, bool opaque) {
  ComPtr<IDCompositionSurface> surface;
  if (!image) return surface;
  if (FAILED(device_->CreateSurface(image.width(), image.height(), DXGI_FORMAT_B8G8R8A8_UNORM,
                                    opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) {
    return nullptr;
  }
  if (!Upload(surface.Get(), POINT{0, 0}, image)) return nullptr;
  return surface;
}

ComPtr<IDCompositionVirtualSurface> Compositor::CreateVirtualSurface(int width, int height, bool opaque) {
  ComPtr<IDCompositionVirtualSurface> surface;
  if (FAILED(device_->CreateVirtualSurface(static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM,
                                           opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) {
    return nullptr;
  }
  return surface;
}

bool Compositor::Upload(IDCompositionSurface* surface, POINT at, const Image& image) {
  if (surface == nullptr || !image) return false;
  const RECT area{at.x, at.y, at.x + image.width(), at.y + image.height()};
  ComPtr<ID3D11Texture2D> texture;
  POINT offset{};
  if (FAILED(surface->BeginDraw(&area, IID_PPV_ARGS(&texture), &offset))) return false;
  const D3D11_BOX box{static_cast<UINT>(offset.x),
                      static_cast<UINT>(offset.y),
                      0,
                      static_cast<UINT>(offset.x + image.width()),
                      static_cast<UINT>(offset.y + image.height()),
                      1};
  d3d_context_->UpdateSubresource(texture.Get(), 0, &box, image.data(), image.stride(), 0);
  texture.Reset();
  surface->EndDraw();
  return true;
}

IDCompositionSurface* Compositor::SolidColor(uint32_t argb) {
  auto found = solid_colors_.find(argb);
  if (found != solid_colors_.end()) return found->second.Get();

  const float alpha = static_cast<float>(argb >> 24) / 255.0f;
  Image pixel = Image::Allocate(1, 1);
  if (!pixel) return nullptr;
  pixel.data()[0] = static_cast<uint8_t>((argb & 0xFF) * alpha + 0.5f);
  pixel.data()[1] = static_cast<uint8_t>(((argb >> 8) & 0xFF) * alpha + 0.5f);
  pixel.data()[2] = static_cast<uint8_t>(((argb >> 16) & 0xFF) * alpha + 0.5f);
  pixel.data()[3] = static_cast<uint8_t>(argb >> 24);
  ComPtr<IDCompositionSurface> surface = CreateSurface(pixel, false);
  IDCompositionSurface* raw = surface.Get();
  solid_colors_.emplace(argb, std::move(surface));
  return raw;
}

ComPtr<IDCompositionVisual> Compositor::CreateSolidVisual(uint32_t argb, float width, float height) {
  ComPtr<IDCompositionVisual> visual = CreateVisual();
  visual->SetContent(SolidColor(argb));
  visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
  visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
  const D2D_MATRIX_3X2_F stretch{width, 0.0f, 0.0f, height, 0.0f, 0.0f};
  visual->SetTransform(stretch);
  return visual;
}

TimeBase Compositor::Now() {
  DCOMPOSITION_FRAME_STATISTICS statistics{};
  device_->GetFrameStatistics(&statistics);
  LARGE_INTEGER current;
  QueryPerformanceCounter(&current);
  TimeBase now;
  now.ticks.QuadPart = std::max(statistics.nextEstimatedFrameTime.QuadPart, current.QuadPart);
  now.seconds = static_cast<double>(now.ticks.QuadPart) / static_cast<double>(QpcFrequency());
  return now;
}

void Compositor::Commit() {
  HRESULT hr = device_->Commit();
  if (FAILED(hr)) Trace("Commit failed: %08lx", hr);
}

void Compositor::ReleaseUnusedMemory() {
  // D3D destroys resources lazily, so without this the last surfaces stay committed until the next shot.
  d3d_context_->ClearState();
  d3d_context_->Flush();
  ComPtr<IDXGIDevice3> dxgi;
  if (SUCCEEDED(d3d_device_.As(&dxgi))) dxgi->Trim();
}

void SetClipRadii(void* object, float value, IDCompositionAnimation* animation) {
  auto* clip = static_cast<IDCompositionRectangleClip*>(object);
  if (animation != nullptr) {
    clip->SetTopLeftRadiusX(animation);
    clip->SetTopLeftRadiusY(animation);
    clip->SetTopRightRadiusX(animation);
    clip->SetTopRightRadiusY(animation);
    clip->SetBottomLeftRadiusX(animation);
    clip->SetBottomLeftRadiusY(animation);
    clip->SetBottomRightRadiusX(animation);
    clip->SetBottomRightRadiusY(animation);
  } else {
    clip->SetTopLeftRadiusX(value);
    clip->SetTopLeftRadiusY(value);
    clip->SetTopRightRadiusX(value);
    clip->SetTopRightRadiusY(value);
    clip->SetBottomLeftRadiusX(value);
    clip->SetBottomLeftRadiusY(value);
    clip->SetBottomRightRadiusX(value);
    clip->SetBottomRightRadiusY(value);
  }
}

void FramePacer::Start(HWND target, UINT message) {
  if (running()) return;
  running_.store(true, std::memory_order_relaxed);
  posted_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this, target, message] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // The compositor clock arrived with Windows 11, and until then the next present is the closest thing.
    using WaitForClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    using BoostClock = HRESULT(WINAPI*)(BOOL);
    HMODULE dcomp = GetModuleHandleW(L"dcomp.dll");
    auto wait = dcomp != nullptr ? reinterpret_cast<WaitForClock>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock")) : nullptr;
    auto boost = dcomp != nullptr ? reinterpret_cast<BoostClock>(GetProcAddress(dcomp, "DCompositionBoostCompositorClock")) : nullptr;
    if (boost != nullptr) boost(TRUE);

    while (running_.load(std::memory_order_relaxed)) {
      const double before = NowSeconds();
      if (wait != nullptr) {
        wait(0, nullptr, 50);
      } else {
        DwmFlush();
      }
      // Both can return at once when nothing is being composed, which must not turn into a spin.
      if (NowSeconds() - before < 0.002) Sleep(2);
      if (!posted_.exchange(true, std::memory_order_relaxed)) PostMessageW(target, message, 0, 0);
    }
    if (boost != nullptr) boost(FALSE);
  });
}

void FramePacer::Stop() {
  running_.store(false, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
}

}  // namespace sstk
