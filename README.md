# Screenshot Toolkit

An always-running, invisible replacement for the Windows Snipping Tool. Press **PrintScreen** or **Win+Shift+S** and the whole screen is captured instantly, without freezing it. The screenshot flies into the bottom-left corner on a physically modeled spring, is saved to your Gallery, and is on your clipboard before you can move your hand.

There is no window, tray icon, editor, or settings page, by design.

## Using it

- **Capture:** press PrintScreen or Win+Shift+S. Press as fast as you like: each shot stacks into a deck and leaves on its own 3-second timer.
- **Pick a region:** the capsule above the thumbnail offers the whole screen, the screen without the taskbar, the top app, and each window of a Snap layout, in that order. The selected option shows its name beside its icon, and the capsule springs to its new width as the selection moves. A selection updates the thumbnail, the saved file, and the clipboard at once, and restarts the timer.
- **Take a scrolling screenshot:** when the top app has content out of view, the capsule also offers, last, the area that scrolls with a chevron pointing down cut out of it. Select it, then press it again or press Win+Enter. The capsule turns into a breathing recording dot, and you scroll any scrollable area of that app as usual: the first scroll picks the area, and the thumbnail grows downward as new content is stitched on, keeping its width. See [Scrolling screenshots](#scrolling-screenshots).
- **Switch regions without the mouse:** while a thumbnail with options is on screen, Win+Shift+Left and Win+Shift+Right step through them, as do Win+Shift with side buttons 4 and 5. The steps wrap around. With nothing to switch, those keys keep their Windows meaning.
- **Hover** over the thumbnail to keep it on screen.
- **Click** the thumbnail to open the image.
- **Drag** it into another app to drop the file there. A blue plus shows when the drop will land. Anywhere else it flies back. Timers stand still during a drag.
- **Cancel a drag** with the right mouse button or Escape. The thumbnail flies back, and neither press reaches the app under the pointer.
- **Drag it onto the rubbish bin** at the bottom center of the screen and release to delete it from the clipboard and send the file to the Recycle Bin. Drag it back out to change your mind.
- **Swipe left** to dismiss it early. The file stays in the Gallery.

Screenshots are lossless PNGs in `Pictures\Screenshots`. Alt+PrintScreen and Win+PrintScreen keep their native Windows behavior.

With several monitors, the one under the cursor is captured. Options covering an identical area are merged, and the capsule is hidden when only one option is left, as with a true fullscreen app.

## Scrolling screenshots

Each screenshot looks in the background for content in the top app window that really lies out of view. Nothing is scrolled or clicked to find out, so no window on screen reacts. Only what the app itself reports counts, read from UI Automation's raw view, and the scrolling option appears at the end of the capsule only if one of these holds:

- Something reports that it shows only part of its content. This covers native lists and documents, and in browsers and the apps built on them every plain box that scrolls.
- Items stacked in a container run on past the edge it clips them at, or a list reports more items than it has rendered, as lists that draw only the visible part do.
- A text document that says which part of it shows, as terminals and editors do, has text out of view.

Each of these must also be what the screen really shows at its place: a page under a dialog or an overlay, behind a fullscreen video, or covered by another window does not count. A window that reports none of this, such as a game or a video, gets no scrolling option, and neither does a short page, a dialog that fits, or text cut off by a box that does not scroll. Scroll bars play no part in this.

Its icon is the area it scrolls, drawn like the other options, with a chevron pointing down cut out of it. Selected, it shows its name with the Win and Enter keycaps beside it: press it again, or press Win+Enter. Win+Shift+Enter works as well, so Enter can follow Win+Shift+Left or Right without letting go of Shift. While recording:

- Scroll any way you like: the wheel, the touchpad, the scroll bar, or the keyboard. What moves decides which area is captured, with the pointer settling it when several things could be. An area the app reports is kept whole, as wide as the app lays it out however its content is arranged, less a scroll bar at its edge. Otherwise the display's own divisions bound it, such as a sidebar beside it.
- The image is built pixel by pixel. Content enters it where readings agree it followed the scroll. What was only seen under something, such as a see-through bar, is taken from wherever it was seen clear of it.
- Fixed headers, floating buttons of any shape, composers, and bars that only stick partway appear once, at the end of the image nearer to where they sit, just as the screen showed them. Something reaching down the whole view is split between the two ends.
- A background that scrolls with the content is part of it, and a plain one keeps its color. A fixed one is rebuilt from wherever it showed between and beside the content, and repeats down the image a view at a time, without a seam where it is a pattern. When it changes as a whole, the repeats from the next one on show it as it then was.
- The effects of hovering stay out: content near the pointer is taken from where it was seen clear of it.
- Fast scrolling, frames blurred in motion, and see-through windows are all followed. What was first seen blurred is retaken when the content rests.
- Scrolling up past the start adds content above it as well. Each end of the image shows what the screen showed there.
- The thumbnail follows along, growing up to about half the screen and then showing the part you are on.

A press on the thumbnail finishes it too, and the thumbnail answers at once: drag it into an app, swipe it away, or click to open it, while the image is finished and saved in the background. A drop or the viewer gets the scrolling screenshot as soon as it is on disk, and a swiped thumbnail's screenshot is saved all the same.

It finishes normally on PrintScreen, Win+Shift+S, Win+Enter, a click on the capsule, or 5 seconds without scrolling. It ends early when the window moves or closes, when a wheel turns outside the window, or when the content stops matching for a moment, for example after a click opens another page. Something merely playing in the page, such as a video, does not end it.

The capsule then shows the outcome for 1.5 seconds before turning back into the selector:

| Color | Outcome |
|---|---|
| Green | Finished normally. The scrolling screenshot is saved and copied |
| Dark green | Ended early, but it had already grown by at least 10%. That part is kept |
| Red | Nothing worth keeping. The thumbnail goes back to the option chosen before |

A kept scrolling screenshot stays as an option of its own, so switching away and back still works. Each screenshot takes at most one scrolling screenshot.

## Install

```bash
ScreenshotToolkit.exe --install
```

This prompts through UAC once, copies the program to `Program Files\Screenshot Toolkit`, registers a logon task, and starts the program.

| Command | Effect |
|---|---|
| `--install` | Install or update, and start now |
| `--uninstall` | Remove the task and the program. Screenshots are kept |
| `--quit` | Stop the running application until the next logon |

## Build

Requires the Visual Studio C++ build tools, a Windows 10 SDK, CMake, and Ninja. There are no other dependencies.

```bash
scripts\build.cmd Release
```

The result is `build\Release\ScreenshotToolkit.exe`.

## Configuration

There is no settings page. The default build's settings live in headers under `include/sstk/config/`. Change a value and rebuild.

| File | What it sets |
|---|---|
| `include/sstk/config/animation.h` | Springs, timing, the stretch into the rubbish bin, and the recording dot's breathing |
| `include/sstk/config/colors.h` | Colors, opacities, shadows, and the outcome colors of a scrolling screenshot |
| `include/sstk/config/core.h` | Dwell time, what a shot contains, how deletion works, which windows become options, and how scrolling screenshots sample, match, and end |
| `include/sstk/config/hooks.h` | The keys and mouse buttons that capture, step the selector, start and finish a scrolling screenshot, and cancel a drag |
| `include/sstk/config/layout.h` | Sizes, spacing, label text, and fonts |

## Releases

The version is set in `CMakeLists.txt`, and `CHANGELOG.md` records what changed. To publish, run:

```bash
scripts\release.cmd
```

This builds Release and copies the program to `release\<version>\`, a local folder git does not track. Each version is tagged `vX.Y.Z`.

## How it stays fast and light

**Idle cost.** After each session the process returns its working set to the system, leaving about 0.2 MB of private working set. Pages stay cached in RAM, so the next press only takes soft page faults. No timers run on the UI thread while idle. The only recurring wake-up is a keyboard hook refresh every five minutes, which also hands back stray pages.

**Animation cost.** Spring curves have a closed-form solution, so each value is uploaded once as a chain of cubic segments and DWM evaluates it at the display's refresh rate on its own. The process sleeps during animation and the motion is immune to CPU load.

**No GPU driver in the process.** Rendering goes through a WARP device that only hands pixels to DirectComposition, while DWM does all drawing on the GPU. Measured on an RTX 4060 laptop, this makes device creation about ten times faster than using the vendor driver (8-18 ms versus 100-200 ms) and avoids roughly 80 MB of committed memory.

**Latency.** The flash is committed about 4 ms after the key press. BitBlt takes 25-40 ms at 2560x1600, and the image is on screen about 50 ms after the press. Thumbnails, file saving, and the clipboard happen after that first frame and never delay it.

**Active memory.** A thumbnail is prepared for every region when the shot is taken, and the raw frame is released once the PNG is saved, leaving about 15 MB per session instead of a raw copy per shot. Switching regions uploads nothing on click; the new crop and clipboard data come from the PNG kept in memory. Cards deep in the deck keep only their current thumbnail, and a burst of five stays near 8 MB.

**Dragging.** The system drag loop calls into whichever application is under the pointer and can block there, so it runs on its own thread. The UI thread wakes once per composed frame only while a thumbnail is in the air, reads the pointer, and moves a finished bitmap. Nothing is drawn or uploaded after the lift.

**Mouse hook.** The side buttons and the right-click cancel need a low-level mouse hook, which wakes the process on every pointer move. It therefore exists only while a thumbnail with options or a drag is on screen, and an idle program has only the keyboard hook.

**Scrolling screenshots.** Looking for content out of view runs on its own thread after the shot is on screen, so a slow app never delays it. While recording, a thread reads the window about every 90 ms, and every 25 ms while things move, and reduces each reading to an edge map, which records where brightness steps and not the shades, because a window's see-through material changes every pixel slightly as the content moves over it. Strips of columns each propose the shifts that line their rows up, so that whatever is fixed in one strip costs only that strip, and the proposals are checked cell by cell in 16 by 16 pixel cells. A cell that matches at the shift and not in place moves with the scroll. That tells the moving area from what surrounds it, with no help from the app, though a rectangle it reports can settle the plain margins. Each reading is placed against the rows already held, so errors cannot add up. A row of 16 pixel slices then enters the image according to how far it can be believed: seen from under something fixed or near the pointer it only fills an empty place, seen once it stands for now, and seen moving it stays unless later readings keep disagreeing with it. Reduced to edges, one reading costs a few milliseconds on a full window. New rows go into address space reserved up front, so the capture never has to be copied as it grows. Only the thumbnail rows that changed are resampled and drawn into a sparse surface, so the thumbnail grows without uploading what is already shown. Screen reads skip CAPTUREBLT, so the pointer never flickers.

**Consecutive shots.** All overlay windows use `WDA_EXCLUDEFROMCAPTURE`, so thumbnails and the flash never appear in later screenshots. To include thumbnails instead, set `kCaptureIncludesThumbnails` in `include/sstk/config/core.h` and rebuild.

**Leaving the desktop alone.** The overlay stops one pixel short of covering the monitor. A window that covers it counts as a fullscreen app, and Windows answers by dropping the taskbar below every other window.

**Above everything.** Thumbnails stay visible over Start, Task View, and the other shell surfaces, because the overlay sits in the UI Access window band. See below.

## Privilege model

The application runs as the logged-on user without administrator rights, so drag and drop, opening files, and the clipboard behave normally with every other program. It carries the UI Access right, as Magnifier and the on-screen keyboard do. Nothing else puts its topmost windows above Start and Task View, or lets its keyboard hook see keys typed into administrator windows.

Setting that right takes SYSTEM, so a logon task running as SYSTEM starts the application with it and exits at once. Only one process stays running, and it is not elevated. Because the task runs without a prompt at each logon, its executable has to live where only administrators can write. A user-writable path would let any program replace the file and silently gain those rights, which is why installation copies to Program Files.

Run directly without installing, the program works the same except that it stays below Start and Task View and does not see keys typed into administrator windows.

Keys are swallowed only while the application is alive. If it is not running, Windows keeps its native behavior.

## Source map

Each module keeps its headers in `include/sstk/<module>/` and its sources in `src/<module>/`, and code includes headers by module, as in `#include "sstk/overlay/capsule.h"`. A name without an extension below stands for both its `.h` and its `.cpp`.

| Module | File | Responsibility |
|---|---|---|
| `app` | `main.cpp` | Modes, message loop, clipboard installation, memory trimming |
| | `install` | Logon task registration and starting in the user's session |
| `base` | `platform` | Shared Win32 includes, process-wide names, timing, and tracing |
| | `workers` | The threads a scrolling screenshot shares its heavy loops with |
| `capture` | `hook` | Low-level keyboard and mouse hooks, Start menu suppression |
| | `capture` | Capture thread |
| | `screen` | Z-order walk, taskbar state, Snap layout detection, region list |
| `config` | `*.h` | The default build's settings. See [Configuration](#configuration) |
| `graphics` | `image` | Resampling, frosted glass, shadow and glyph rasterization |
| | `pixels.h` | Pixel helpers for the scrolling screenshot code |
| | `motion` | Damped springs emitted as DirectComposition cubic animations |
| | `compositor` | WARP device and composition helpers |
| `overlay` | `overlay` | Per-monitor deck, layout, timers, and input |
| | `card` | One screenshot: the image plane, its crossfades, its poses, and its shadow |
| | `capsule` | The region selector, its sliding pill and labels, and a scrolling screenshot's recording and outcome |
| | `sprite` | The thumbnail in the air, and the mesh that draws it into the bin |
| | `bin` | The rubbish bin capsule |
| | `dragdrop` | The system drag loop, on its own thread |
| `scroll` | `scrollable` | Finding whether the top app has content out of view |
| | `recorder` | Recording a scrolling screenshot: reading the screen, its timing, and its thumbnail |
| | `tracker` | How far content scrolled between two readings |
| | `stitcher` | Building the long image: what is content, what holds still, and what changed by itself |
| | `longimage` | The long image, kept packed in strips rather than as one bitmap |
| `storage` | `store` | PNG encoding, atomic saves, clipboard preparation, frame archiving |

Build scripts are in `scripts/`, and the manifest and version resource in `res/`.

## Debugging

| Variable | Effect |
|---|---|
| `SSTK_TRACE` | Appends timing to `%TEMP%\ScreenshotToolkit.log` |
| `SSTK_DEBUG_VISIBLE` | Makes the overlay capturable, so tooling can sample animation frames. Screenshots then contain thumbnails |
| `SSTK_GALLERY_DIR` | Saves screenshots to another folder |
