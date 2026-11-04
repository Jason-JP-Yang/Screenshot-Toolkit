# Screenshot Toolkit

An always-running, invisible replacement for the Windows Snipping Tool. Press **PrintScreen** or **Win+Shift+S** and the whole screen is captured instantly, without freezing it. The screenshot flies into the bottom-left corner on a physically modeled spring, is saved to your Gallery, and is on your clipboard before you can move your hand.

There is no window, tray icon, editor, or settings page, by design.

## Using it

- **Capture:** press PrintScreen or Win+Shift+S. Press as fast as you like: each shot stacks into a deck and leaves on its own 3-second timer.
- **Pick a region:** the capsule above the thumbnail offers the whole screen, the top app, each window of a Snap layout, and the screen without the taskbar. The selected option shows its name beside its icon, and the capsule springs to its new width as the selection moves. A selection updates the thumbnail, the saved file, and the clipboard at once, and restarts the timer.
- **Take a scrolling screenshot:** when the top app has anything that scrolls vertically, the capsule also offers a page icon. Select it, then press it again or press Win+Enter. The capsule turns into a breathing recording dot, and you scroll any scrollable area of that app as usual: the first scroll picks the area, and the thumbnail grows downward as new content is stitched on, keeping its width. See [Scrolling screenshots](#scrolling-screenshots).
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

Each screenshot asks UI Automation, in the background, which parts of the top app window scroll vertically, nested ones included, and also checks classic scroll bars. Nothing is scrolled or clicked to find out, so no window on screen reacts. If anything scrolls, the page icon appears at the end of the capsule. It can take a moment in a browser, which only starts to build what it can tell once it is asked. Browsers and the apps built on them keep most of what scrolls to themselves, so any of their pages gets the icon, even one that turns out not to scroll.

Selected, the icon shows how to start: press it again, or press Win+Enter. While recording:

- Scroll any way you like: the wheel, the touchpad, the scroll bar, or the keyboard. What moves decides which area is captured, so this works in panes the app says nothing about. Columns that stay put beside it, such as a sidebar, are left out.
- Fixed headers and footers inside the area appear once, a scroll bar at its right edge is left out, and a bar that sticks to the top on the way down is not stamped over the content.
- Scrolling up past the start adds content above it as well.
- The thumbnail follows along, growing up to about half the screen and then showing the part you are on.

It finishes normally on PrintScreen, Win+Shift+S, Win+Enter, a click on the capsule, or 5 seconds without scrolling. It ends early when the window moves or closes, when a wheel turns outside the captured area, or when the content stops matching for a moment, for example after a click opens another page. Something merely playing in the page, such as a video, does not end it.

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
build.cmd Release
```

The result is `build\Release\ScreenshotToolkit.exe`.

## Configuration

There is no settings page. The default build's settings live in headers under `src/config/`. Change a value and rebuild.

| File | What it sets |
|---|---|
| `src/config/animation.h` | Springs, timing, the stretch into the rubbish bin, and the recording dot's breathing |
| `src/config/colors.h` | Colors, opacities, shadows, and the outcome colors of a scrolling screenshot |
| `src/config/core.h` | Dwell time, what a shot contains, how deletion works, which windows become options, and how scrolling screenshots sample, match, and end |
| `src/config/hooks.h` | The keys and mouse buttons that capture, step the selector, start and finish a scrolling screenshot, and cancel a drag |
| `src/config/layout.h` | Sizes, spacing, label text, and fonts |

## Releases

The version is set in `CMakeLists.txt`, and `CHANGELOG.md` records what changed. To publish, run:

```bash
release.cmd
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

**Scrolling screenshots.** Finding scrollable areas runs on its own thread after the shot is on screen, so a slow app never delays it. While recording, a thread reads three columns four pixels wide from the area about every 90 ms, and every 40 ms while things move. Only when they change is the whole area read, its rows fingerprinted, and the scroll distance found by letting rows vote for the shift that lines them up. A fingerprint records where a row's color steps and not the colors themselves, because a window's see-through material changes every pixel slightly as the content moves over it. New rows go into address space reserved up front, so the capture never has to be copied as it grows. Only the thumbnail rows that changed are resampled and drawn into a sparse surface, so the thumbnail grows without uploading what is already shown. Screen reads skip CAPTUREBLT, so the pointer never flickers.

**Consecutive shots.** All overlay windows use `WDA_EXCLUDEFROMCAPTURE`, so thumbnails and the flash never appear in later screenshots. To include thumbnails instead, set `kCaptureIncludesThumbnails` in `src/config/core.h` and rebuild.

**Leaving the desktop alone.** The overlay stops one pixel short of covering the monitor. A window that covers it counts as a fullscreen app, and Windows answers by dropping the taskbar below every other window.

**Above everything.** Thumbnails stay visible over Start, Task View, and the other shell surfaces, because the overlay sits in the UI Access window band. See below.

## Privilege model

The application runs as the logged-on user without administrator rights, so drag and drop, opening files, and the clipboard behave normally with every other program. It carries the UI Access right, as Magnifier and the on-screen keyboard do. Nothing else puts its topmost windows above Start and Task View, or lets its keyboard hook see keys typed into administrator windows.

Setting that right takes SYSTEM, so a logon task running as SYSTEM starts the application with it and exits at once. Only one process stays running, and it is not elevated. Because the task runs without a prompt at each logon, its executable has to live where only administrators can write. A user-writable path would let any program replace the file and silently gain those rights, which is why installation copies to Program Files.

Run directly without installing, the program works the same except that it stays below Start and Task View and does not see keys typed into administrator windows.

Keys are swallowed only while the application is alive. If it is not running, Windows keeps its native behavior.

## Source map

| File | Responsibility |
|---|---|
| `src/main.cpp` | Modes, message loop, clipboard installation, memory trimming |
| `src/hook.cpp` | Low-level keyboard and mouse hooks, Start menu suppression |
| `src/capture.cpp` | Capture thread |
| `src/screen.cpp` | Z-order walk, taskbar state, Snap layout detection, region list |
| `src/image.cpp` | Resampling, frosted glass, shadow and glyph rasterization |
| `src/store.cpp` | PNG encoding, atomic saves, clipboard preparation, frame archiving |
| `src/motion.cpp` | Damped springs emitted as DirectComposition cubic animations |
| `src/compositor.cpp` | WARP device and composition helpers |
| `src/card.cpp` | One screenshot: the image plane, its crossfades, its poses, and its shadow |
| `src/capsule.cpp` | The region selector, its sliding pill and labels, and a scrolling screenshot's recording and outcome |
| `src/scrollable.cpp` | Finding the areas of the top app that scroll vertically |
| `src/recorder.cpp` | Recording a scrolling screenshot: sampling, matching, stitching, and its thumbnail |
| `src/overlay.cpp` | Per-monitor deck, layout, timers, and input |
| `src/sprite.cpp` | The thumbnail in the air, and the mesh that draws it into the bin |
| `src/bin.cpp` | The rubbish bin capsule |
| `src/dragdrop.cpp` | The system drag loop, on its own thread |
| `src/install.cpp` | Logon task registration and starting in the user's session |
| `src/config/` | The default build's settings. See [Configuration](#configuration) |

## Debugging

| Variable | Effect |
|---|---|
| `SSTK_TRACE` | Appends timing to `%TEMP%\ScreenshotToolkit.log` |
| `SSTK_DEBUG_VISIBLE` | Makes the overlay capturable, so tooling can sample animation frames. Screenshots then contain thumbnails |
| `SSTK_GALLERY_DIR` | Saves screenshots to another folder |
