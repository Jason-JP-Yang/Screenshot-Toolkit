// Reads the desktop arrangement at the moment of a capture and turns it into selectable regions.
#pragma once

#include "platform.h"

#include <vector>

namespace sstk {

// kScroll offers a scrolling screenshot of the top app, and kLong is the one taken.
enum class RegionKind { kFull, kApp, kZone, kNoTaskbar, kScroll, kLong };

struct Region {
  RegionKind kind;
  RECT rect;  // Physical pixels relative to the monitor origin. A kScroll option holds the app window.
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
};

ScreenInfo AnalyzeScreen(HMONITOR monitor);
TaskbarInsets QueryTaskbarInsets(HMONITOR monitor);

// Pixel size of the thumbnail showing a region.
SIZE ThumbnailSize(const RECT& region, float scale);

}  // namespace sstk
