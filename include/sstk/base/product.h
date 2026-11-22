// Names and places the program, its service, and its setup share, and the running application in this session.
#pragma once

#include "sstk/base/platform.h"

#include <string>

namespace sstk {

inline constexpr wchar_t kProductName[] = L"Screenshot Toolkit";
inline constexpr wchar_t kPublisher[] = L"Jason-JP-Yang";
inline constexpr wchar_t kExecutableName[] = L"ScreenshotToolkit.exe";
inline constexpr wchar_t kServiceName[] = L"ScreenshotToolkit";
inline constexpr wchar_t kAppUserModelId[] = L"JasonJPYang.ScreenshotToolkit";
inline constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ScreenshotToolkit";

// When this program was built, as "YYYY-MM-DD hh:mm:ss UTC". Written by scripts/version.cmake.
extern const wchar_t kBuildTime[];

std::wstring ExecutablePath();
std::wstring InstallDirectory();  // Program Files\Screenshot Toolkit
std::wstring InstalledExecutablePath();
std::wstring DataDirectory();     // ProgramData\Screenshot Toolkit
bool SamePath(const std::wstring& a, const std::wstring& b);
bool IsInstalledCopy();           // This process runs from InstalledExecutablePath().

bool IsApplicationRunning();
void RequestQuit();
bool WaitForApplicationExit(DWORD timeout_ms);

}  // namespace sstk
