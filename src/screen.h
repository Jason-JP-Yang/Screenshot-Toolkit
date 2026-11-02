// Reads the desktop arrangement at the moment of a capture and turns it into selectable regions.
#pragma once

#include "platform.h"

#include <vector>

namespace sstk {

enum class RegionKind { kFull, kApp, kZone, kNoTaskbar };

struct Region {
  RegionKind kind;
  RECT rect;  // Physical pixels relative to the monitor origin.
};

struct ScreenInfo {
  HMONITOR monitor = nullptr;
  RECT bounds{};  // Monitor rectangle in virtual screen coordinates.
  std::vector<Region> regions;  // Always starts with the full monitor.
};

// Space currently taken by a visible taskbar on each monitor edge, in physical pixels.
struct TaskbarInsets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool operator==(const TaskbarInsets&) const = default;
};

ScreenInfo AnalyzeScreen(HMONITOR monitor);
TaskbarInsets QueryTaskbarInsets(HMONITOR monitor);

}  // namespace sstk
