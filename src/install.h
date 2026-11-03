// Registers and removes the logon task that keeps the application running.
#pragma once

namespace sstk {

// Both relaunch themselves through UAC when needed and report the outcome in a message box.
int InstallApplication();
int UninstallApplication();

// Run as SYSTEM by the logon task: starts the application as the logged-on user, with UI Access.
int LaunchInUserSession();

// Signals the running instance, if any, to exit.
void RequestQuit();

}  // namespace sstk
