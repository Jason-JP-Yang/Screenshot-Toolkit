// Reads the desktop arrangement at the moment of a capture and turns it into selectable regions.
#pragma once

#include "sstk/base/platform.h"

#include <vector>

namespace sstk {

// kScroll offers a scrolling screenshot of the top app, and kLong is the one taken. kCustom is a part chosen by hand in
// the editing panel, which offers it first.
enum class RegionKind { kFull, kApp, kZone, kNoTaskbar, kScroll, kLong, kCustom };

struct Region {
  RegionKind kind;
  RECT rect;  // Physical pixels relative to the monitor origin. A kScroll option holds the app window.
  HWND window = nullptr;  // The window an option shows by itself. Once its own picture comes, `rect` is its whole frame.
};

struct WindowFrame {
  HWND window;
  RECT frame;  // Its visible frame, whole, in virtual screen coordinates.
};

// Space currently taken by a visible taskbar on each monitor edge, in physical pixels.
struct TaskbarInsets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool operator==(const TaskbarInsets&) const = default;
};

struct ScreenInfo {
  HMONITOR monitor = nullptr;
  RECT bounds{};  // Monitor rectangle in virtual screen coordinates.
  TaskbarInsets insets;
  std::vector<Region> regions;  // Always starts with the full monitor.
  HWND app = nullptr;           // The top app window, whose scrollable areas a scrolling screenshot can follow.
  RECT app_rect{};              // Its visible frame, in monitor pixels.
  RECT app_bounds{};            // Its window rectangle, to tell whether it has moved or changed size since.
  std::vector<WindowFrame> windows;  // Those the regions show by themselves.
};

ScreenInfo AnalyzeScreen(HMONITOR monitor);
TaskbarInsets QueryTaskbarInsets(HMONITOR monitor);

// A window's visible frame, whole, in virtual screen coordinates.
bool WholeFrame(HWND window, RECT* frame);

// The part of a window's frame that can be seen on a monitor, both in virtual screen coordinates.
bool VisibleFrame(HWND window, const RECT& monitor, RECT* visible);

// Pixel size of the thumbnail showing a region.
SIZE ThumbnailSize(const RECT& region, float scale);

}  // namespace sstk
