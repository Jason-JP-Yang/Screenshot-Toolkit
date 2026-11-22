// The one window Screenshot Toolkit has: the icon, the name, two lines of status, and wide buttons. It follows the
// Windows app theme, and its buttons fold into a progress bar while setup works.
//   kInstall    Quick Install. Shown when a copy other than the installed one is opened.
//   kUninstall  Uninstall. Opened from Settings > Apps.
//   kAbout      Quick Restart and Complete Quit. Shown when the installed copy is opened, as from Start.
#pragma once

namespace sstk {

enum class SetupWindowMode { kInstall, kUninstall, kAbout };

// Shows the window until it closes, and returns the process exit code.
int RunSetupWindow(SetupWindowMode mode);

}  // namespace sstk
