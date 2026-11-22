// Installing and removing Screenshot Toolkit. The steps run in an elevated copy of this program, which reports each
// one to the setup window; messages may go from a higher integrity level to a lower one.
//
// Installing copies the program to Program Files, writes %ProgramData%\Screenshot Toolkit\settings.ini if there is
// none, adds the entry in Settings > Apps and a Start menu shortcut for everyone, and registers and starts the service.
// Uninstalling removes all of that, and anything earlier versions left, apart from the screenshots themselves.
#pragma once

#include "sstk/base/platform.h"

#include <string>

namespace sstk {

enum class SetupAction { kInstall, kUninstall };

inline constexpr UINT kMsgSetupStep = WM_APP + 64;  // wParam: the step starting, lParam: how many steps there are.

inline constexpr int kSetupDone = 0;
inline constexpr int kSetupCancelled = ERROR_CANCELLED;  // The user declined the administrator prompt.

int SetupStepCount(SetupAction action);
const wchar_t* SetupStepText(SetupAction action, int step);
std::wstring SetupFailureText(int code);

bool IsElevated();

// Runs every step in this process, which must be elevated. `window`, when not null, hears each step.
int RunSetupSteps(SetupAction action, HWND window);

// Starts this program elevated to run the steps for `window`. Null if it could not start, or if the user declined.
HANDLE StartElevatedSetup(SetupAction action, HWND window, bool* cancelled);

// For --quiet: runs the steps without a window, through the administrator prompt if needed.
int RunQuietSetup(SetupAction action);

// The uninstall window must not run from the folder it removes. When it would, a copy in %TEMP% takes over and this
// returns true.
bool HandOffToTemporaryCopy();

// A copy started by HandOffToTemporaryCopy removes itself once it exits.
void RemoveTemporaryCopyAtExit();

}  // namespace sstk
