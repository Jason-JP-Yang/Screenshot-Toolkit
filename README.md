# Screenshot Toolkit

An always-running, invisible replacement for the Windows Snipping Tool. Press **PrintScreen** or **Win+Shift+S** and the whole screen is captured instantly, without freezing it. The screenshot flies into the bottom-left corner on a physically modeled spring, is saved to your Gallery, and is on your clipboard before you can move your hand.

There is no window, tray icon, editor, or settings page, by design.

## Using it

- **Capture:** press PrintScreen or Win+Shift+S. Press as fast as you like: each shot stacks into a deck and leaves on its own 3-second timer.
- **Pick a region:** the capsule above the thumbnail offers the whole screen, the top app, each window of a Snap layout, and the screen without the taskbar. A selection updates the thumbnail, the saved file, and the clipboard at once, and restarts the timer.
- **Hover** over the thumbnail to keep it on screen.
- **Click** the thumbnail to open the image.
- **Drag** it into another app to drop the file there.
- **Swipe left** to dismiss it early. The file stays in the Gallery.

Screenshots are lossless PNGs in `Pictures\Screenshots`. Alt+PrintScreen and Win+PrintScreen keep their native Windows behavior.

With several monitors, the one under the cursor is captured. Options covering an identical area are merged, and the capsule is hidden when only one option is left, as with a true fullscreen app.

## Install

```bash
ScreenshotToolkit.exe --install
```

This prompts through UAC once, copies the program to `Program Files\Screenshot Toolkit`, registers two logon tasks, and starts them.

| Command | Effect |
|---|---|
| `--install` | Install or update, and start now |
| `--uninstall` | Remove the tasks and the program. Screenshots are kept |
| `--quit` | Stop the running application until the next logon |

## Build

Requires the Visual Studio C++ build tools, a Windows 10 SDK, CMake, and Ninja. There are no other dependencies.

```bash
build.cmd Release
```

The result is `build\Release\ScreenshotToolkit.exe`.

## How it stays fast and light

**Idle cost.** After each session the process returns its working set to the system, leaving about 0.2 MB of private working set. Pages stay cached in RAM, so the next press only takes soft page faults. No timers run while idle; the only recurring wake-up is a keyboard hook refresh every five minutes.

**Animation cost.** Spring curves have a closed-form solution, so each value is uploaded once as a chain of cubic segments and DWM evaluates it at the display's refresh rate on its own. The process sleeps during animation and the motion is immune to CPU load.

**No GPU driver in the process.** Rendering goes through a WARP device that only hands pixels to DirectComposition, while DWM does all drawing on the GPU. Measured on an RTX 4060 laptop, this makes device creation about ten times faster than using the vendor driver (8-18 ms versus 100-200 ms) and avoids roughly 80 MB of committed memory.

**Latency.** The flash is committed about 4 ms after the key press. BitBlt takes 25-40 ms at 2560x1600, and the image is on screen about 50 ms after the press. Thumbnails, file saving, and the clipboard happen after that first frame and never delay it.

**Bursts.** Cards behind the front cannot be edited, so their raw pixels are dropped in favor of the PNG already encoded in memory, and restored if the card returns to the front. A burst of twelve shots peaks near 45 MB instead of 250 MB.

**Consecutive shots.** All overlay windows use `WDA_EXCLUDEFROMCAPTURE`, so thumbnails and the flash never appear in later screenshots.

## Privilege model

The application runs unelevated, so drag and drop, opening files, and the clipboard behave normally with every other program. Windows does not deliver keys typed into administrator windows to an unelevated hook, so a second elevated process holds only a keyboard hook and signals the application. It maps no rendering or codec libraries and parses no input.

That task runs elevated without a prompt at each logon, so its executable has to live where only administrators can write. A user-writable path would let any program replace the file and silently gain administrator rights, which is why installation copies to Program Files.

Keys are swallowed only while the application is alive. If it is not running, Windows keeps its native behavior.

## Source map

| File | Responsibility |
|---|---|
| `src/main.cpp` | Modes, message loop, clipboard installation, memory trimming |
| `src/hook.cpp` | Low-level keyboard hook, Start menu suppression |
| `src/capture.cpp` | Capture thread |
| `src/screen.cpp` | Z-order walk, taskbar state, Snap layout detection, region list |
| `src/image.cpp` | Resampling, frosted glass, shadow and glyph rasterization |
| `src/store.cpp` | PNG encoding, atomic saves, clipboard preparation, frame archiving |
| `src/motion.cpp` | Damped springs emitted as DirectComposition cubic animations |
| `src/compositor.cpp` | WARP device and composition helpers |
| `src/card.cpp` | One screenshot: the image plane, its crossfades, and its poses |
| `src/capsule.cpp` | The region selector and its sliding pill |
| `src/overlay.cpp` | Per-monitor deck, layout, timers, and input |
| `src/dragdrop.cpp` | File drag out of the overlay |
| `src/install.cpp` | Logon task registration |
| `src/theme.h` | Design tokens: "Graphite Glass" palette, metrics, and springs |

## Debugging

| Variable | Effect |
|---|---|
| `SSTK_TRACE` | Appends timing to `%TEMP%\ScreenshotToolkit.log` |
| `SSTK_DEBUG_VISIBLE` | Makes the overlay capturable, so tooling can sample animation frames |
| `SSTK_GALLERY_DIR` | Saves screenshots to another folder |
