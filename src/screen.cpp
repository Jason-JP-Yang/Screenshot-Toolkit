#include "screen.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>

namespace sstk {

namespace {

constexpr int kMinimumWindowExtent = 48;
constexpr int kTaskbarHiddenThickness = 4;
constexpr int kSameRectTolerance = 2;
constexpr double kMinimumVisibleFraction = 0.5;

struct WindowEntry {
  HWND handle;
  RECT rect;  // Visible frame clipped to the monitor, in virtual screen coordinates.
  bool arranged;
  double visible_fraction;
};

struct TaskbarState {
  bool found = false;
  bool covered = false;
  RECT rect{};
};

struct WalkState {
  HMONITOR monitor;
  RECT bounds;
  DWORD own_process;
  std::vector<WindowEntry> windows;
  TaskbarState taskbar;
  bool covered_so_far = false;
};

bool ClassNameIs(const wchar_t* name, const wchar_t* expected) { return wcscmp(name, expected) == 0; }

bool IsTaskbarClass(const wchar_t* name) {
  return ClassNameIs(name, L"Shell_TrayWnd") || ClassNameIs(name, L"Shell_SecondaryTrayWnd");
}

bool IsShellSurfaceClass(const wchar_t* name) {
  static constexpr const wchar_t* kIgnored[] = {
      L"Progman",   L"WorkerW",          L"#32768",  L"tooltips_class32",          L"SysShadow",
      L"NotifyIconOverflowWindow",       L"TaskListThumbnailWnd",                  L"XamlExplorerHostIslandWindow",
      L"Windows.UI.Core.CoreWindow",     L"Shell_InputSwitchTopLevelWindow",       L"ForegroundStaging",
      L"MultitaskingViewFrame",          L"Windows.Internal.Shell.TabProxyWindow",
  };
  for (const wchar_t* ignored : kIgnored) {
    if (ClassNameIs(name, ignored)) return true;
  }
  return false;
}

bool IsCloaked(HWND window) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

bool IsAppSurface(HWND window, LONG_PTR ex_style) {
  if ((ex_style & WS_EX_APPWINDOW) != 0) return true;
  if ((ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) return false;
  if ((ex_style & WS_EX_LAYERED) != 0) {
    if ((ex_style & WS_EX_TRANSPARENT) != 0) return false;
    BYTE alpha = 255;
    DWORD flags = 0;
    if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) != 0 && alpha == 0) {
      return false;
    }
  }
  return true;
}

BOOL CALLBACK CollectWindow(HWND window, LPARAM context) {
  auto& state = *reinterpret_cast<WalkState*>(context);
  if (!IsWindowVisible(window)) return TRUE;

  wchar_t class_name[64];
  if (GetClassNameW(window, class_name, ARRAYSIZE(class_name)) == 0) return TRUE;

  if (IsTaskbarClass(class_name)) {
    if (!state.taskbar.found && MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == state.monitor) {
      RECT rect;
      if (GetWindowRect(window, &rect)) {
        RECT clipped;
        IntersectRect(&clipped, &rect, &state.bounds);
        state.taskbar.found = true;
        state.taskbar.rect = clipped;
        state.taskbar.covered = state.covered_so_far;
      }
    }
    return TRUE;
  }

  if (IsIconic(window) || IsShellSurfaceClass(class_name) || IsCloaked(window)) return TRUE;

  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process == state.own_process) return TRUE;

  const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if (!IsAppSurface(window, ex_style)) return TRUE;

  RECT frame;
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
    if (!GetWindowRect(window, &frame)) return TRUE;
  }

  RECT clipped;
  if (!IntersectRect(&clipped, &frame, &state.bounds)) return TRUE;
  if (clipped.right - clipped.left < kMinimumWindowExtent || clipped.bottom - clipped.top < kMinimumWindowExtent) {
    return TRUE;
  }

  if (EqualRect(&clipped, &state.bounds)) state.covered_so_far = true;

  state.windows.push_back({window, clipped, IsWindowArranged(window) != FALSE, 0.0});
  return TRUE;
}

long Area(const RECT& rect) { return (rect.right - rect.left) * (rect.bottom - rect.top); }

long RegionArea(HRGN region) {
  DWORD size = GetRegionData(region, 0, nullptr);
  if (size == 0) return 0;
  std::vector<BYTE> buffer(size);
  auto* data = reinterpret_cast<RGNDATA*>(buffer.data());
  if (GetRegionData(region, size, data) == 0) return 0;
  const RECT* rects = reinterpret_cast<const RECT*>(data->Buffer);
  long area = 0;
  for (DWORD i = 0; i < data->rdh.nCount; ++i) area += Area(rects[i]);
  return area;
}

// Walks windows from the top down, recording how much of each one is left uncovered.
void ComputeVisibility(std::vector<WindowEntry>& windows, const RECT& bounds) {
  HRGN remaining = CreateRectRgnIndirect(&bounds);
  HRGN window_region = CreateRectRgn(0, 0, 0, 0);
  HRGN visible = CreateRectRgn(0, 0, 0, 0);
  for (WindowEntry& entry : windows) {
    SetRectRgn(window_region, entry.rect.left, entry.rect.top, entry.rect.right, entry.rect.bottom);
    int kind = CombineRgn(visible, window_region, remaining, RGN_AND);
    entry.visible_fraction = kind == NULLREGION ? 0.0 : static_cast<double>(RegionArea(visible)) / Area(entry.rect);
    CombineRgn(remaining, remaining, window_region, RGN_DIFF);
  }
  DeleteObject(visible);
  DeleteObject(window_region);
  DeleteObject(remaining);
}

const WindowEntry* FindTopApp(const std::vector<WindowEntry>& windows) {
  HWND foreground = GetForegroundWindow();
  if (foreground != nullptr) {
    HWND root = GetAncestor(foreground, GA_ROOT);
    for (const WindowEntry& entry : windows) {
      if (entry.handle == foreground || entry.handle == root) return &entry;
    }
  }
  // The desktop or another monitor has focus: fall back to the frontmost window that is actually seen.
  for (const WindowEntry& entry : windows) {
    if (entry.visible_fraction >= kMinimumVisibleFraction) return &entry;
  }
  return nullptr;
}

bool NearlyEqual(const RECT& a, const RECT& b) {
  return std::abs(a.left - b.left) <= kSameRectTolerance && std::abs(a.top - b.top) <= kSameRectTolerance &&
         std::abs(a.right - b.right) <= kSameRectTolerance && std::abs(a.bottom - b.bottom) <= kSameRectTolerance;
}

RECT ToMonitorSpace(const RECT& rect, const RECT& bounds) {
  return RECT{rect.left - bounds.left, rect.top - bounds.top, rect.right - bounds.left, rect.bottom - bounds.top};
}

bool TaskbarIsShown(const TaskbarState& taskbar) {
  if (!taskbar.found || taskbar.covered) return false;
  const int width = taskbar.rect.right - taskbar.rect.left;
  const int height = taskbar.rect.bottom - taskbar.rect.top;
  return std::min(width, height) > kTaskbarHiddenThickness;
}

TaskbarInsets InsetsFrom(const TaskbarState& taskbar, const RECT& bounds) {
  TaskbarInsets insets;
  if (!TaskbarIsShown(taskbar)) return insets;
  const RECT& bar = taskbar.rect;
  const int width = bar.right - bar.left;
  const int height = bar.bottom - bar.top;
  if (width >= height) {
    if (bar.top - bounds.top <= bounds.bottom - bar.bottom) insets.top = height;
    else insets.bottom = height;
  } else {
    if (bar.left - bounds.left <= bounds.right - bar.right) insets.left = width;
    else insets.right = width;
  }
  return insets;
}

WalkState WalkDesktop(HMONITOR monitor, const RECT& bounds) {
  WalkState state{monitor, bounds, GetCurrentProcessId()};
  state.windows.reserve(32);
  EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&state));
  return state;
}

}  // namespace

ScreenInfo AnalyzeScreen(HMONITOR monitor) {
  ScreenInfo info;
  info.monitor = monitor;
  MONITORINFO monitor_info{sizeof(monitor_info)};
  if (!GetMonitorInfoW(monitor, &monitor_info)) return info;
  info.bounds = monitor_info.rcMonitor;

  WalkState state = WalkDesktop(monitor, info.bounds);
  ComputeVisibility(state.windows, info.bounds);

  const RECT full = ToMonitorSpace(info.bounds, info.bounds);
  auto add_region = [&](RegionKind kind, const RECT& rect) {
    for (const Region& existing : info.regions) {
      if (NearlyEqual(existing.rect, rect)) return;
    }
    info.regions.push_back({kind, rect});
  };
  add_region(RegionKind::kFull, full);

  std::vector<const WindowEntry*> zones;
  for (const WindowEntry& entry : state.windows) {
    if (entry.arranged && entry.visible_fraction >= kMinimumVisibleFraction) zones.push_back(&entry);
  }

  const WindowEntry* app = FindTopApp(state.windows);
  if (zones.size() >= 2) {
    // A window floating above a snapped layout is still offered, ahead of the zones it sits on.
    if (app != nullptr && !app->arranged) add_region(RegionKind::kApp, ToMonitorSpace(app->rect, info.bounds));
    std::sort(zones.begin(), zones.end(), [](const WindowEntry* a, const WindowEntry* b) {
      if (std::abs(a->rect.top - b->rect.top) > kSameRectTolerance) return a->rect.top < b->rect.top;
      return a->rect.left < b->rect.left;
    });
    for (const WindowEntry* zone : zones) add_region(RegionKind::kZone, ToMonitorSpace(zone->rect, info.bounds));
  } else if (app != nullptr) {
    add_region(RegionKind::kApp, ToMonitorSpace(app->rect, info.bounds));
  }

  const TaskbarInsets insets = InsetsFrom(state.taskbar, info.bounds);
  if (insets != TaskbarInsets{}) {
    RECT without_taskbar = full;
    without_taskbar.left += insets.left;
    without_taskbar.top += insets.top;
    without_taskbar.right -= insets.right;
    without_taskbar.bottom -= insets.bottom;
    add_region(RegionKind::kNoTaskbar, without_taskbar);
  }
  return info;
}

TaskbarInsets QueryTaskbarInsets(HMONITOR monitor) {
  MONITORINFO monitor_info{sizeof(monitor_info)};
  if (!GetMonitorInfoW(monitor, &monitor_info)) return {};
  WalkState state = WalkDesktop(monitor, monitor_info.rcMonitor);
  return InsetsFrom(state.taskbar, monitor_info.rcMonitor);
}

}  // namespace sstk
