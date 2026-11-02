// Owns the render device and wraps the handful of composition objects the overlay needs.
//
// The device is a WARP software device on purpose. Nothing here ever runs a shader: it only hands
// pixels to DirectComposition, and DWM does all drawing on the GPU. Skipping the vendor driver
// makes device creation about ten times faster and removes roughly 80 MB of committed memory.
#pragma once

#include "image.h"
#include "motion.h"
#include "platform.h"

#include <d3d11.h>
#include <dcomp.h>

#include <unordered_map>

namespace sstk {

class Compositor {
 public:
  bool Initialize();
  void Shutdown();
  bool IsHealthy();

  IDCompositionDevice* device() const { return device_.Get(); }

  ComPtr<IDCompositionVisual> CreateVisual();
  ComPtr<IDCompositionEffectGroup> CreateEffectGroup();
  ComPtr<IDCompositionRectangleClip> CreateClip();
  ComPtr<IDCompositionScaleTransform> CreateScale();

  // Uploads pixels into a new composition surface. Returns null on failure.
  ComPtr<IDCompositionSurface> CreateSurface(const Image& image, bool opaque);

  // A shared 1x1 surface of a color that is stretched by the visual showing it. Colors are 0xAARRGGBB, straight alpha.
  IDCompositionSurface* SolidColor(uint32_t argb);

  // Creates a visual showing a solid color across width x height, sampled without any edge softening.
  ComPtr<IDCompositionVisual> CreateSolidVisual(uint32_t argb, float width, float height);

  TimeBase Now();
  void Commit();

  // Hands memory held for already released surfaces back to the system.
  void ReleaseUnusedMemory();

 private:
  ComPtr<ID3D11Device> d3d_device_;
  ComPtr<ID3D11DeviceContext> d3d_context_;
  ComPtr<IDCompositionDevice> device_;
  std::unordered_map<uint32_t, ComPtr<IDCompositionSurface>> solid_colors_;
};

}  // namespace sstk
