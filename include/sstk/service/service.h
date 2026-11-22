// The Windows service that keeps the application running in every signed-in session.
//
// The application needs the UI Access right: only that puts its topmost windows above Start and Task View and lets
// its keyboard hook see keys typed into administrator windows. Setting the right takes SYSTEM, so this service, running
// as LocalSystem from Program Files where only administrators can write, starts the application in each session with
// it. The application itself runs as the signed-in user without administrator rights.
//
// An application that is ended or crashes comes back within a second, more slowly if it keeps failing at once. One that
// exits cleanly, as it does when asked to quit or when the user signs out, stays down until the next sign-in or until
// its About window asks for it again.
#pragma once

namespace sstk {

// Runs as the service process (--service). Returns once the service stops.
int RunService();

// Asks the service to start the application in this session. False when no service answers.
bool AskServiceToStart();

}  // namespace sstk
