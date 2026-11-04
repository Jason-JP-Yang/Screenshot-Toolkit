// Default build settings: the keys and buttons the low-level hooks act on. Keys are virtual key codes.
// Modifiers must match exactly, so a chord with extra modifiers held keeps its Windows meaning.
#pragma once

#include "../platform.h"

namespace sstk::config {

struct Modifiers {
  bool win = false;
  bool shift = false;
  bool control = false;
  bool alt = false;
};

struct Chord {
  DWORD key;
  Modifiers modifiers;
};

enum class MouseButton { kRight, kMiddle, kBack, kForward };  // Back and forward are side buttons 4 and 5.

// Take a screenshot. Always active.
inline constexpr Chord kCaptureChords[] = {
    {VK_SNAPSHOT, {}},
    {'S', {.win = true, .shift = true}},
};

// Step the region selector. Active while a thumbnail with options is on screen.
inline constexpr Modifiers kSelectorModifiers{.win = true, .shift = true};
inline constexpr DWORD kSelectorPreviousKey = VK_LEFT;
inline constexpr DWORD kSelectorNextKey = VK_RIGHT;
inline constexpr MouseButton kSelectorPreviousButton = MouseButton::kBack;
inline constexpr MouseButton kSelectorNextButton = MouseButton::kForward;

// Start a scrolling screenshot while its option is selected, and finish one. While one runs, the capture chords
// finish it as well instead of taking a screenshot.
inline constexpr Chord kScrollChord{VK_RETURN, {.win = true}};

// Call off a drag. Active while a thumbnail is held, and takes no modifiers.
inline constexpr DWORD kDragCancelKey = VK_ESCAPE;
inline constexpr MouseButton kDragCancelButton = MouseButton::kRight;

// Windows silently removes hooks that once ran too slowly, so the keyboard hook is refreshed this often.
inline constexpr DWORD kHookRefreshMs = 5 * 60 * 1000;

}  // namespace sstk::config
