# Changelog

Built programs are kept per version in the local `release/` folder, created by `scripts\release.cmd`. Each version is also tagged in git as `vMajor.Minor.Patch`.

## 1.0.0

The first production release.

### Added

- An editing panel, over the whole monitor and above everything else, with the other monitors dimmed and nothing under it reachable while it is open. It follows the Windows light or dark app theme.
- Clicking a thumbnail opens it in the panel, growing out of the thumbnail. **Done** settles the result back into the deck, where it is saved and copied; the close button pulls the shot into the rubbish bin.
- Along the top: close, then reset, undo, and redo; the region selector, with **Custom** first; turn right, turn left, and **Done**. Choosing **Scrolling Screenshot** closes the panel into its thumbnail and starts recording.
- Cropping by the frame's corners and edges. Dragged past the picture's edge, the picture shrinks to bring more of it within reach, in step with the handle, and the crop settles to fill the space a moment after it is let go. Changing the crop by hand makes the region **Custom**.
- A markup toolbar with an ink pen, a monoline pen, a highlighter, a pencil, a crayon, and an ink brush, each with five widths, an opacity, five colors, and a grid of more; a pixel and an object eraser; a lasso that moves and recolors marks; and a ruler that strokes run along. A pen's pressure and tilt shape the marks; with a mouse, the ink pen and the brush thin with speed. Pencil and crayon take a paper grain fixed to the picture, and the ink brush bleeds when full, dries darker at its edges, and breaks into dry streaks as it runs out.
- The toolbar folds into a round button in the nearer bottom corner, and can be dragged to rest along the bottom or the top, at the left, middle, or right, or standing along either side. The picture is fitted clear of it, and the place is kept for next time.
- With the toolbar open, the wheel and Ctrl+Plus or Ctrl+Minus zoom the picture, up to eight times, without changing the crop.

### Changed

- Win+Shift+S opens the editing panel: the screen flashes, shrinks into the panel, and a region is chosen by dragging, by clicking a window, or with the selector. PrintScreen captures straight to a thumbnail as before.
- Builds are named `vX.Y.Z[-dev]-<commit>`, without `-beta`. Each version is published as a full release, the newest marked as the latest; dev builds of main stay pre-releases.

## 0.3.1

### Changed

- A window region, and the scrolling option before it starts, hold the window alone: it is read by itself a moment after the shot, through Windows Graphics Capture without its border, or where Windows would draw one, by the window rendering itself. A notification or the taskbar over it stays out, and a part past the edge of the screen is included. Rounded corners show what the screen showed there. The picture is packed as a long image is, and saved and copied as it is.
- A scrolling screenshot reads the window by itself too, so a notification passing over it stays out, and an area reaching past the edge of the screen is captured whole. Where the window can't be read, the screen is, as before.
- The scrolling option stands only while the app still shows what the shot did. Once a click, a scroll, or a key press has reached anything but the thumbnail or its region bar, the app is compared with the shot four times a second, on a thread of its own: as it was, scrolled within reach, or changed in parts, as by typing or a playing video, it keeps the option. Replaced across nearly all of what scrolls, as by another page, it loses it, and so it does by moving or changing size.
- Esc or a right-click while swiping or pressing a thumbnail calls it off, as during a drag, and the thumbnail goes back.
- Switching to or from a region that reaches past the screen, a window read by itself or a scrolling screenshot, shows no white space and fades nothing in from nowhere. The switch stays one straight move on one spring, with no zooming back and forth and an even pace. A copy of the picture beneath the screen's own shows what lies past the edge at full strength as the move slides over it, and a corner no picture covers shows the colors around it carried across and blurred.

### Fixed

- A video played fullscreen in a browser was offered a scrolling screenshot, because the page under it scrolls. In a window covering its monitor, something inside the area as large as all of it now counts as covering the content.

## 0.3.0

### Added

- An icon: three viewfinder corners on a graphite tile, the fourth a screenshot landing there. It is drawn in code and fitted to whole pixels at each size from 16 to 256, then baked into the program at build time.
- A setup window in the style of an About box: the icon, "Screenshot *Toolkit*" in Sitka, the version, the build's time in UTC, and one wide **Quick Install** button. It follows the Windows light or dark app theme, title bar included, and the button folds into a progress bar that reports each step, then unfolds into **Done** with the version installed. Each step stays up for at least 0.65 seconds, so a fast install still reads as one.
- Installing, after one administrator prompt, copies the program to Program Files, writes `%ProgramData%\Screenshot Toolkit\settings.ini` if there is none, adds an entry in Settings > Apps and a Start menu shortcut for everyone without a desktop icon, and registers the Screenshot Toolkit service. Earlier versions' logon tasks and their folder go.
- The service starts the program with UI Access in every signed-in session. The program comes back within a second if it is ended or crashes, more slowly if it keeps failing at once, and Windows restarts the service should it fail.
- Uninstall in Settings > Apps opens the same window with a deep red **Uninstall** button. It removes the service, the program, its settings, its entries, and what earlier versions left, and the copy that runs it deletes itself afterward. Screenshots stay.
- Opened from Start, the program shows its About window, with **Check for Updates** and **Quit**, which reads **Quick Restart** while the program is not running. A quit lasts until Quick Restart or the next sign-in. The About and uninstall windows run from a copy in `%TEMP%` that deletes itself, so they never hold the installed file.
- Automatic updates from the GitHub releases, dev builds included. The service looks at each start and every 12 hours, an hour after a failed check, or at once from Check for Updates. A release is newer when its build number, the count of commits on main, is higher. The download runs in the background through the user's proxy, and is kept only once its size and SHA-256 match. It goes in, without an administrator prompt, when nothing is on screen and the keyboard and mouse have been still for a minute: the new copy starts before the old one lets go, the installed file is replaced in one rename, and the download is deleted. The About window shows an update under way and hides its second button meanwhile. `AutoUpdate=0` in settings.ini turns the automatic checks off.
- `settings.ini` changes the gallery folder, the dwell time, whether shots show the thumbnails, and whether deleting goes to the Recycle Bin, without a rebuild.
- Every build is named `vX.Y.Z[-dev]-<commit>-beta`, with `dev` unless it was built from the commit tagged for its version.
- GitHub Actions builds, signs, and publishes releases, all as pre-releases, keeping compiled objects between runs with ccache. Each push to main publishes its newest commit as the one dev build, and each version tag is published with its section of this changelog and a link comparing it with the version before. Builds are signed with a self-signed certificate and timestamped.
- The project is licensed under the Apache License 2.0.

### Changed

- With no arguments the program opens its window. The program itself now runs with `--run`.
- `--install` and `--uninstall` open the window, and take `--quiet` to run without it.

### Fixed

- With Microsoft Defender's Controlled folder access turned on, screenshots couldn't be saved to Pictures, and Defender reported the program as blocked. Installing and every update now add the installed program to the apps allowed through it, and uninstalling takes it off the list.

## 0.2.0

### Added

- Scrolling screenshots. When the top app has content out of view, nested areas included, the selector offers a scrolling option. It is found in the background without touching any window, from what the app itself reports in UI Automation's raw view, where plain boxes in browsers count too: scrollers that show only part of their content, content stacked past the edge of the box that clips it, lists longer than what they render, and text documents such as terminals with text out of view. Each must be what the screen really shows at that place, so a page under a dialog, an overlay, or a fullscreen video does not count. A window that reports none of this, such as a game, gets no scrolling option, and neither does a short page or a dialog that fits. Scroll bars play no part.
- Its icon is the area it scrolls, drawn like the other options, with a chevron pointing down cut out of it. An area too small to hold the chevron grows around its middle until it does. Selected, it shows "Scrolling Screenshot" with the Win and Enter keycaps after it, quieter, as a shortcut beside a menu item. Pressing it again or pressing Win+Enter starts recording. Shift may still be held from the selector chord.
- A press on the growing thumbnail finishes the capture and acts on the thumbnail at once: it drags, swipes away, or opens without waiting for the image. A drop or the viewer gets the scrolling screenshot as soon as it is saved, and a thumbnail swiped away keeps its scrolling screenshot. Released over the rubbish bin before the file is ready, it goes in at once.
- While recording, the selector's pill gathers into a breathing dot beside "Scroll to capture", which turns into "Recording" once the first scroll picks the area. What really moves decides which area is scrolled, with the pointer settling it when several could be. An area the app reports is kept whole, as wide as its layout makes it however its content is arranged, less a scroll bar at its edge. Otherwise the display's own divisions bound it, around everything seen to move.
- The image is built in layers, pixel by pixel. Content enters it where readings agree it followed the scroll, and where it was only seen under something, from wherever it was seen clear of it, as under a see-through bar. Fixed headers, floating buttons of any shape, composers, and bars that only stick partway appear once, at the end of the image nearer to where they sit, just as the screen showed them there. Something reaching down the whole view is split between the two ends. Scrolling above the start adds content there too.
- A background that scrolls with the content is content. A plain one keeps its color. A fixed one is rebuilt from wherever it showed between and beside the content, and repeats down the image a view at a time, without a seam where it is a pattern. When it changes as a whole, the repeats from the next one on show it as it then was. Nothing in the image is left black.
- Hover effects stay out of the image, whatever their size: content near the pointer is taken from where it was seen clear of it.
- Readings are lined up by where brightness steps, cell by cell and against the rows already held, so see-through windows such as Settings stitch as well as any other, frames blurred in motion are followed and retaken at rest, and errors cannot add up.
- The thumbnail grows with the capture at an unchanged width and scale, up to about half the screen, and then follows the part on screen.
- PrintScreen, Win+Shift+S, Win+Enter, a click on the selector, or 5 seconds without scrolling finish it. Moving or closing the window, turning the wheel outside the window, or content that stops matching ends it early. A video playing in the page does not.
- The outcome fills the selector for 1.5 seconds: green when saved, dark green when it ended early after growing by at least 10%, and red when nothing was kept. The selector then returns, with the scrolling screenshot as an option of its own. Each screenshot takes at most one.

### Changed

- The selected option shows its name beside its icon, laid out by what is visibly drawn, and the selector springs to its new width as the selection moves.
- Options run from the whole screen, to the screen without the taskbar, to windows, with the scrolling option last.
- The selector's shadow is cut into caps and a stretching middle, so it follows any width without being stretched.
- The selector's labels are drawn after the first frame of a shot, which they never delay.
- Text on the selector is centered by the true height of its capitals, read off the letter's outline, and the option names on the glyph beside them as drawn. Segoe UI Variable reports its capitals as half the font size, well short of their real height, which set the labels a few pixels high.
- Clicking a thumbnail opens the image without holding anything up: the thumbnail goes at once, and the viewer opens once the file is saved.
- The source tree is split into modules: headers in `include/sstk/<module>/`, sources in `src/<module>/`, and the build scripts in `scripts/`. The default build's settings moved with it to `include/sstk/config/`.

### Performance

- A scrolling screenshot takes about a third less memory for the same area, and hardly grows as the capture gets longer. Rows held until the capture finishes are kept packed instead of as plain pixels, the screens at the two ends share the readings they were taken from instead of copying them, the first screen is kept packed, and less is kept for each pixel of the view. The image comes out exactly as before.
- The work of each reading is shared with worker threads that exist only while recording, instead of resting on one core: comparing readings, lining them up, telling the view apart pixel by pixel, making rows, drawing the thumbnail, and packing the image. Rows held near the ends are drawn for the thumbnail again only when something they are drawn from has changed. Sampling keeps its pace while the content moves.

## 0.1.2

### Added

- Thumbnails are now really dragged. A thumbnail shrinks into the pointer's hand, follows it directly with a slight lean, and can be dropped anywhere the system's drag and drop can.
- A blue plus appears on the thumbnail's top-left corner while the window under the pointer accepts the drop. On a drop the thumbnail shrinks and fades away. Anywhere else it flies back to its place in the stack.
- The right mouse button or Escape cancels a drag at once, and the thumbnail flies back. Neither press reaches the application under the pointer.
- Win+Shift+Left and Win+Shift+Right step through the region selector, as do Win+Shift with side buttons 4 and 5. The steps wrap around. With nothing to switch, the keys keep their Windows meaning.
- A rubbish bin appears at the bottom center of the screen during a drag, above the taskbar: a deep red disc that opens into a capsule with its label as the pointer nears. Releasing over it deletes the screenshot from the clipboard and sends its file to the Recycle Bin. `kDeleteToRecycleBin` in `src/config/core.h` makes the deletion permanent instead.
- The thumbnail is drawn into the bin from wherever it is. Each point runs straight at the bin, nearer ones first, and the card stays on the pointer, so the shape follows the pointer the whole way. The card keeps most of its shape instead of drawing out into a long funnel. It starts as the pointer arrives, and leaving plays the same motion backward. Coming close, the near edge already reaches toward the bin.
- The bin's lid opens toward the thumbnail, as seen from the capsule's center, and follows it around: it pivots on its far end from the side, and lifts off level from above. It starts to open as the pointer comes close. The can squats as things go in, and the lid drops shut on a deletion.

### Changed

- The capsule stays put. It no longer leans toward the pointer, grows, or squashes, and its text stays sharp as a result.
- The capsule is laid out by what is visibly drawn. The icon is concentric with the left cap, the text ends as far from the right edge as the icon's body starts from the left, and both are centered on their ink. The label uses Segoe UI Variable.
- The capture flash lasts about four times as long at a lower peak. It eases in instead of appearing at full strength.
- The default build's settings now live in headers under `src/config/`: animation, colors, core behavior, hook triggers, and layout. They replace `src/theme.h` and `src/config.h`.
- The region selector sinks away while a thumbnail is held. It returns for the new front card after a drop or deletion, and for the original card if the drop fails.
- The cards behind a held thumbnail move up to take its place. If the drop fails, they step back, and shots taken in the meantime stay on top.
- Every thumbnail's timer stands still during a drag. A drop or deletion then gives the remaining thumbnails 2 more seconds, up to the 3-second dwell.
- The system's own drag image and drop cursors are gone.

### Performance

- The drag loop runs on its own thread, so a slow application under the pointer can no longer stall the thumbnail. New screenshots and timers keep working during a drag.

## 0.1.1

### Fixed

- Switching regions now affects only the latest screenshot. Cards behind it keep their shape, place, and timer.
- The taskbar no longer drops behind other windows during a capture. The overlay was large enough to count as a fullscreen app, which also let the top window's shadow fall across the taskbar.
- Opening Start, Task View, or another taskbar panel no longer makes the thumbnail lose track of the taskbar and overlap it.
- Thumbnails now appear above Start, Task View, and other shell surfaces instead of being hidden until they close.
- Thumbnail shadows no longer fall on the taskbar.

### Changed

- Region switches crossfade between thumbnails in exact registration over a properly filtered base image, so the picture stays clean throughout the move.
- The shadow now follows every region switch and flight continuously, without its blur or corners being stretched.
- The selector icon for the screen without the taskbar leaves a clear gap along the taskbar's edge, so it can no longer be mistaken for the whole screen.
- Thumbnails have a thin white border, 2 DIP wide.
- Installation leaves one background process instead of two. The application now holds the UI Access right, set by a logon task that exits at once, so the elevated hook process is gone.
- A swiped or expiring thumbnail keeps nearly all of its opacity as it leaves.
- The capture flash is much softer.

### Added

- `kCaptureIncludesThumbnails` in `src/config.h` chooses at build time whether screenshots show the thumbnails. Off by default.

### Performance

- One screenshot on screen uses about 15 MB of private memory instead of about 40 MB, and a burst of five uses about 8 MB. Raw pixels are released as soon as the file is saved, and later crops decode the in-memory archive.
- Region switches no longer resample anything on click, because every region's thumbnail is prepared with the shot.
- The application returns idle memory every five minutes.

## 0.1.0

First version: instant capture with PrintScreen and Win+Shift+S, animated thumbnail deck, region selector for Snap layouts, automatic save and clipboard copy, and logon tasks.
