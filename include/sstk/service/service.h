// The Windows service that keeps the application running in every signed-in session, and keeps it up to date.
//
// The application needs the UI Access right: only that puts its topmost windows above Start and Task View and lets
// its keyboard hook see keys typed into administrator windows. Setting the right takes SYSTEM, so this service, running
// as LocalSystem from Program Files where only administrators can write, starts the application in each session with
// it. The application itself runs as the signed-in user without administrator rights.
//
// An application that is ended or crashes comes back within a second, more slowly if it keeps failing at once. One that
// exits cleanly, as it does when asked to quit or when the user signs out, stays down until the next sign-in or until
// its About window asks for it again. Updates are described in update/updater.h.
#pragma once

#include "sstk/base/platform.h"

#include <string>
#include <vector>

namespace sstk {

// Runs as the service process (--service). Returns once the service stops.
int RunService();

// Requests from a signed-in user, which the service's security allows. False when no service answers.
bool AskServiceToStart();        // Start the application in this session.
bool AskServiceToCheck();        // Check for an update now, and install one found at once.
bool AskServiceToApplyUpdate();  // The application is idle: an update waiting may go in now.

// Applications running `image` with UI Access, as the service starts them. The caller closes the handles.
struct RunningCopy {
  HANDLE process;
  DWORD session;
};
std::vector<RunningCopy> FindApplications(const std::wstring& image);

// Starts `image` as the application in a session, as its user, with UI Access. With `takeover`, it replaces the copy
// already running there once it is ready. Returns the process, or null. Needs SYSTEM.
HANDLE LaunchApplication(DWORD session, const std::wstring& image, bool takeover);

}  // namespace sstk
