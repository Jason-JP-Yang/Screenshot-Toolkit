// Registers and removes the two logon tasks that keep the application running.
#pragma once

namespace sstk {

// Both relaunch themselves through UAC when needed and report the outcome in a message box.
int InstallApplication();
int UninstallApplication();

// Signals the running instance, if any, to exit.
void RequestQuit();

}  // namespace sstk
