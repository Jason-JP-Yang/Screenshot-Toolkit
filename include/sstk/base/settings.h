// Runtime settings from %ProgramData%\Screenshot Toolkit\settings.ini, read once when the program starts. The headers
// in include/sstk/config/ hold the defaults, and the file overrides these few.
#pragma once

#include <string>

namespace sstk {

struct Settings {
  std::wstring gallery_folder;  // Empty for Pictures\Screenshots.
  double dwell_seconds;
  bool capture_includes_thumbnails;
  bool delete_to_recycle_bin;
  bool auto_update;
};

// As read when the program started.
const Settings& CurrentSettings();

// Read now, for the service, which runs for days.
Settings ReadSettings();

std::wstring SettingsPath();

// What setup writes when there is no file yet: every setting at its default, each with a note.
std::wstring DefaultSettingsText();

}  // namespace sstk
