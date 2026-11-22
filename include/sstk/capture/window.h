// Reads a window's own pixels, wherever it reaches and whatever lies over it. Windows Graphics Capture does it where
// Windows can leave out the border it would otherwise draw around the window; elsewhere a still picture comes from the
// window's own rendering instead.
#pragma once

#include "sstk/base/platform.h"
#include "sstk/capture/screen.h"
#include "sstk/graphics/image.h"
#include "sstk/scroll/longimage.h"

#include <memory>
#include <vector>

namespace sstk {

// A Direct3D device for Windows Graphics Capture. The composition device is a software one, and is no use for it.
class CaptureDevice {
 public:
  CaptureDevice();
  ~CaptureDevice();
  CaptureDevice(const CaptureDevice&) = delete;
  CaptureDevice& operator=(const CaptureDevice&) = delete;

  bool Open();
  void Close();

 private:
  friend class WindowStream;
  struct State;
  std::unique_ptr<State> state_;
};

// One window's pictures as it changes. Its thread is in the multithreaded apartment, and the device outlives it.
class WindowStream {
 public:
  WindowStream();
  ~WindowStream();
  WindowStream(const WindowStream&) = delete;
  WindowStream& operator=(const WindowStream&) = delete;

  // With `borderless`, fails where Windows would draw a border around the window while it is read.
  bool Start(CaptureDevice& device, HWND window, bool borderless);

  // Waits for the first picture.
  bool Wait(DWORD ms);

  // The window's visible frame when it started, in virtual screen coordinates.
  const RECT& frame() const;

  // Copies `rect` of the newest picture, relative to the frame's top-left corner, into rows `stride` bytes apart. What
  // lies beyond the picture comes out black. Alpha is the window's own, premultiplied.
  bool Read(const RECT& rect, uint32_t* out, ptrdiff_t stride);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

struct WindowShot {
  HWND window = nullptr;
  RECT rect{};  // Its whole visible frame, in monitor pixels.
  std::shared_ptr<const LongImage> image;
  Image thumbnail;  // Sized for a card on that monitor.
};

// The windows' own pictures, taken just after `screen` was read from `monitor`, in virtual screen coordinates. Rounded
// corners and see-through parts lie over what the screen showed there. A window that cannot be read, or that has moved
// or changed size since its frame was noted, is left out. The thread is in the multithreaded apartment.
std::vector<WindowShot> GrabWindows(const std::vector<WindowFrame>& windows, const Image& screen, const RECT& monitor, float scale,
                                    CaptureDevice& device);

}  // namespace sstk
