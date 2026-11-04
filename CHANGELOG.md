# Changelog

Built programs are kept per version in the local `release/` folder, created by `release.cmd`. Each version is also tagged in git as `vMajor.Minor.Patch`.

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
