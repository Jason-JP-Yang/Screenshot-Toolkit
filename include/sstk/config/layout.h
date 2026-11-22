// Default build settings: sizes, spacing, and type. Lengths are device independent pixels.
#pragma once

namespace sstk::config {

// Thumbnail deck.
inline constexpr float kCardMaxHeight = 160.0f;
inline constexpr float kCardMaxWidth = 340.0f;
inline constexpr float kEdgeMargin = 16.0f;
inline constexpr float kCardRadius = 12.0f;
inline constexpr float kCardBorder = 2.0f;
inline constexpr float kDeckPeek = 9.0f;
inline constexpr float kDeckShrinkPerLevel = 0.055f;
inline constexpr int kDeckVisibleBehind = 2;

// Region selector.
inline constexpr float kCapsuleGap = 10.0f;
inline constexpr float kSegmentWidth = 38.0f;
inline constexpr float kSegmentHeight = 28.0f;
inline constexpr float kCapsulePadding = 3.0f;
inline constexpr int kFrostDownscale = 8;
inline constexpr float kFrostWidth = 640.0f;  // Wide enough for the widest selector.

// The selected option's label, and the states of a scrolling screenshot. Gaps measure between what is visibly drawn.
inline constexpr float kCapsuleFontSize = 13.0f;
inline constexpr float kLabelGap = 6.0f;
inline constexpr float kKeycapHeight = 15.0f;
inline constexpr float kKeycapGap = 2.5f;
inline constexpr float kKeycapTextGap = 5.0f;
inline constexpr float kShortcutGap = 8.0f;  // Between an option's name and the keycaps that start it.
inline constexpr float kRecordingDot = 8.0f;
inline constexpr float kStatusIconSize = 14.0f;
inline constexpr float kStatusGap = 8.0f;
inline constexpr float kFinishHintGap = 14.0f;
inline constexpr wchar_t kLabelFullscreen[] = L"Fullscreen";
inline constexpr wchar_t kLabelWindow[] = L"Window";
inline constexpr wchar_t kLabelNoTaskbar[] = L"No Taskbar";
inline constexpr wchar_t kLabelScrolling[] = L"Scrolling Screenshot";  // Offered, the keycaps of the scroll chord follow.
inline constexpr wchar_t kRecordingWaiting[] = L"Scroll to capture";
inline constexpr wchar_t kRecordingActive[] = L"Recording";
inline constexpr wchar_t kRecordingFinish[] = L"to finish";
inline constexpr wchar_t kResultSucceeded[] = L"Scrolling Screenshot Saved";
inline constexpr wchar_t kResultPartial[] = L"Stopped Early, Saved";
inline constexpr wchar_t kResultFailed[] = L"Scrolling Screenshot Failed";

// A thumbnail growing with a scrolling screenshot keeps its width, and stops growing at the lower of these.
inline constexpr float kScrollCardMaxHeight = 460.0f;
inline constexpr float kScrollCardMaxShare = 0.5f;  // Of the screen height left free by the taskbar.

// The badge on a thumbnail in the air.
inline constexpr float kDropBadgeSize = 22.0f;

// Rubbish bin. The insets measure to what is visibly drawn, not to the bitmaps carrying it.
inline constexpr float kBinHeight = 38.0f;
inline constexpr float kBinMargin = 18.0f;
inline constexpr float kBinIconSize = 20.0f;
inline constexpr float kBinFontSize = 14.0f;
inline constexpr float kBinTextGap = 7.5f;
inline constexpr float kBinTextInset = 13.5f;
inline constexpr wchar_t kBinLabel[] = L"Rubbish Bin";

// Type, tried in order. Windows 11's display cut is tighter and cleaner, and older systems have only the static face.
inline constexpr const wchar_t* kLabelFaces[] = {L"Segoe UI Variable Display Semib", L"Segoe UI Semibold"};
inline constexpr float kLabelWeight = 0.92f;  // Below 1 thickens light text on a saturated body.

// The setup and About window: the icon, the title, two lines under it, and wide buttons.
inline constexpr float kSetupWidth = 380.0f;
inline constexpr float kSetupLogoTop = 52.0f;
inline constexpr float kSetupLogoSize = 96.0f;
inline constexpr float kSetupTitleGap = 30.0f;      // From the icon to the top of the title's capitals.
inline constexpr float kSetupTitleSize = 31.0f;
inline constexpr float kSetupLineSize = 14.0f;
inline constexpr float kSetupFirstLineGap = 31.0f;  // Between the title's baseline and the first line's.
inline constexpr float kSetupLineGap = 21.0f;       // Between the two lines' baselines.
inline constexpr float kSetupButtonGap = 38.0f;     // From the last baseline to the first button.
inline constexpr float kSetupButtonWidth = 260.0f;
inline constexpr float kSetupButtonHeight = 44.0f;
inline constexpr float kSetupButtonSpacing = 12.0f;
inline constexpr float kSetupButtonFontSize = 15.0f;
inline constexpr float kSetupBottom = 46.0f;
inline constexpr float kSetupProgressHeight = 4.0f;  // A button folds into a bar this thick while it works.
inline constexpr float kSetupFocusGap = 3.0f;        // Between a button and the ring the keyboard puts around it.
inline constexpr float kSetupFocusStroke = 2.0f;
inline constexpr const wchar_t* kSetupTitleFaces[] = {L"Sitka Heading", L"Sitka Display", L"Sitka", L"Georgia"};
inline constexpr const wchar_t* kSetupTextFaces[] = {L"Segoe UI Variable Text", L"Segoe UI"};
inline constexpr wchar_t kSetupTitleUpright[] = L"Screenshot ";
inline constexpr wchar_t kSetupTitleItalic[] = L"Toolkit";
inline constexpr wchar_t kSetupCaptionInstall[] = L"Screenshot Toolkit Setup";
inline constexpr wchar_t kSetupCaptionUninstall[] = L"Uninstall Screenshot Toolkit";
inline constexpr wchar_t kSetupCaptionAbout[] = L"About Screenshot Toolkit";
inline constexpr wchar_t kSetupInstall[] = L"Quick Install";
inline constexpr wchar_t kSetupUninstall[] = L"Uninstall";
inline constexpr wchar_t kSetupRestart[] = L"Quick Restart";
inline constexpr wchar_t kSetupQuit[] = L"Complete Quit";
inline constexpr wchar_t kSetupDone[] = L"Done";
inline constexpr wchar_t kSetupClose[] = L"Close";
inline constexpr wchar_t kSetupInstalling[] = L"Installing";
inline constexpr wchar_t kSetupUninstalling[] = L"Uninstalling";
inline constexpr wchar_t kSetupInstalled[] = L"Installed";  // The version installed follows.
inline constexpr wchar_t kSetupInstalledHint[] = L"Press PrintScreen to take a screenshot";
inline constexpr wchar_t kSetupUninstalled[] = L"Uninstalled";
inline constexpr wchar_t kSetupUninstalledHint[] = L"Sorry to see you go. Hope to see you again.";
inline constexpr wchar_t kSetupInstallFailed[] = L"Not installed";
inline constexpr wchar_t kSetupUninstallFailed[] = L"Not uninstalled";
inline constexpr wchar_t kSetupRunning[] = L"Running";
inline constexpr wchar_t kSetupStopped[] = L"Not running";
inline constexpr wchar_t kSetupRestarting[] = L"Restarting";
inline constexpr wchar_t kSetupQuitting[] = L"Quitting";

}  // namespace sstk::config
