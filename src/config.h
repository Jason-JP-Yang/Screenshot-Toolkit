// Core behavior switches, chosen at build time.
#pragma once

namespace sstk::config {

// Whether screenshots show Screenshot Toolkit's own thumbnails and selector.
//   false: consecutive shots contain only what is really on the desktop.
//   true:  a shot taken while thumbnails are on screen shows them, in the bottom-left corner.
// The capture flash is never included either way.
inline constexpr bool kCaptureIncludesThumbnails = false;

// Where a screenshot dropped on the rubbish bin goes.
//   true:  the Windows Recycle Bin, so an accidental drop can be undone.
//   false: deleted for good straight away.
inline constexpr bool kDeleteToRecycleBin = true;

}  // namespace sstk::config
