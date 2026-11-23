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
// Plane a region switch passes over with no picture there, beside a window or scrolling screenshot reaching past the
// screen, shows the pictures around it carried over and blurred: this many cells along its longer side, blurred
// across this many.
inline constexpr int kBackfillCells = 64;
inline constexpr int kBackfillBlur = 2;

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

// The editing panel's controls along the top, and the space the picture is fitted into.
inline constexpr float kEditorMargin = 16.0f;         // From the screen's edges to the controls.
inline constexpr float kEditorButton = 40.0f;         // A round button, and each cell of a cluster.
inline constexpr float kEditorIcon = 20.0f;
inline constexpr float kEditorControlGap = 10.0f;
inline constexpr float kEditorPictureTop = 22.0f;     // From the controls to the picture,
inline constexpr float kEditorPictureSide = 40.0f;    // from the screen's sides,
inline constexpr float kEditorPictureBottom = 22.0f;  // and from the toolbar.
inline constexpr float kEditorCornerArm = 22.0f;      // The crop frame's handles.
inline constexpr float kEditorEdgeArm = 30.0f;
inline constexpr float kEditorHandleWidth = 4.0f;
inline constexpr float kEditorFrameLine = 1.0f;
inline constexpr float kEditorCornerReach = 26.0f;    // How near the pointer must be to take a handle.
inline constexpr float kEditorEdgeReach = 16.0f;
inline constexpr float kEditorMinCrop = 28.0f;        // The smallest crop, on screen.
inline constexpr float kEditorPressSlop = 4.0f;
inline constexpr float kEditorBrushCursorMin = 6.0f;

// The markup toolbar along the bottom, and its popovers.
inline constexpr float kToolbarHeight = 98.0f;
inline constexpr float kToolbarRadius = 34.0f;
inline constexpr float kToolbarPadding = 20.0f;
inline constexpr float kToolCell = 44.0f;
inline constexpr float kToolArtWidth = 30.0f;
inline constexpr float kToolArtHeight = 106.0f;
inline constexpr float kToolRest = 26.0f;    // From the toolbar's top to a resting tool's tip.
inline constexpr float kToolRaise = 15.0f;   // The chosen tool stands this much higher,
inline constexpr float kToolHover = 3.5f;    // and one under the pointer this much.
inline constexpr float kSwatch = 29.0f;
inline constexpr float kSwatchGap = 10.0f;
inline constexpr float kSwatchRing = 2.2f;
inline constexpr float kToolbarSection = 22.0f;  // Between the tools, the colors, and the button that folds the toolbar away.
inline constexpr float kToolbarFold = 34.0f;
inline constexpr float kMarkupButton = 50.0f;
inline constexpr float kPopoverGap = 10.0f;
inline constexpr float kPopoverRadius = 20.0f;
inline constexpr float kPopoverPadding = 12.0f;
inline constexpr float kWidthCell = 46.0f;
inline constexpr float kOpacityTrack = 28.0f;
inline constexpr float kOpacityThumb = 26.0f;
inline constexpr float kModeHeight = 32.0f;
inline constexpr float kGridCell = 20.0f;
inline constexpr int kGridColumns = 12;
inline constexpr int kGridRows = 10;
inline constexpr float kRulerWidth = 80.0f;
inline constexpr float kRulerSnap = 26.0f;  // A stroke starting this near the ruler's edge runs along it.
inline constexpr float kEditorFontSize = 13.0f;
inline constexpr float kRulerFontSize = 15.0f;
inline constexpr float kToolLabelSize = 8.5f;
inline constexpr wchar_t kLabelCustom[] = L"Custom";
inline constexpr wchar_t kLabelPixelEraser[] = L"Pixel Eraser";
inline constexpr wchar_t kLabelObjectEraser[] = L"Object Eraser";

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
inline constexpr wchar_t kSetupQuit[] = L"Quit";
inline constexpr wchar_t kSetupCheckUpdates[] = L"Check for Updates";
inline constexpr wchar_t kSetupChecking[] = L"Checking for updates";
inline constexpr wchar_t kSetupDownloading[] = L"Downloading";  // The build's name follows, as on the next three.
inline constexpr wchar_t kSetupUpdating[] = L"Installing";
inline constexpr wchar_t kSetupUpdatedTo[] = L"Updated to";
inline constexpr wchar_t kSetupUpToDate[] = L"You have the newest build";
inline constexpr wchar_t kSetupUpdateFailed[] = L"Couldn't update";
inline constexpr wchar_t kSetupNoService[] = L"The Screenshot Toolkit service isn't running.";
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
