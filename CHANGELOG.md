# Changelog

Built programs are kept per version in the local `release/` folder, created by `release.cmd`. Each version is also tagged in git as `vMajor.Minor.Patch`.

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
