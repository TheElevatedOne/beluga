// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: all user settings in one key file, ~/.config/beluga/beluga.conf.
// Settings that used to live in KDE's System Settings (click behaviour, terminal,
// trash limits, places) are kept here too.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace beluga {

enum class StatusBarMode { Small = 0, FullWidth = 1, Hidden = 2 };
enum class DirSizeMode { Count = 0, Size = 1, None = 2 };
enum class ExecAction { Ask = 0, Open = 1, Run = 2 };
enum class TrashLimitAction { Warn = 0, DeleteOldest = 1, DeleteLargest = 2 };

struct ModeSettings {
    int iconSize;          // without previews
    int previewSize;       // with previews
    bool useSystemFont = true;
    std::string font;      // Pango font description
};

struct Settings {
    // ---- Startup / behaviour
    std::string homeUrl;                 // start location (URI)
    bool rememberOpenedTabs = true;
    bool splitView = false;
    bool filterBar = false;
    bool editableUrl = false;
    bool showFullPath = false;
    bool showFullPathInTitlebar = false;
    bool openExternallyCalledFolderInNewTab = false;
    bool browseArchives = true;   // open zip, tar, 7z, rar and ISO files as folders
    bool alwaysShowTabBar = false;
    bool showCloseButtonOnTabs = true;
    bool openNewTabAfterLastTab = false;
    int closeSplitViewChoice = 0;        // 0 active, 1 inactive, 2 right
    bool useTabForSwitchingSplitView = false;
    bool globalViewProps = true;
    // Disk Usage window
    bool duApparentSize = false;
    bool duOneFileSystem = true;
    bool searchIndex = true;   // keep an index of the home folder's names for instant search
    bool renameInline = true;
    bool showSelectionToggle = true;
    bool showPasteBarAfterCopying = true;
    bool showToolTips = false;
    bool autoExpandFolders = false;      // open folders while dragging over them
    bool singleClick = false;            // was KDE "Clicking files or folders"
    bool showMenuBar = false;
    bool showToolBar = true;
    StatusBarMode statusBar = StatusBarMode::Small;
    bool showZoomSlider = false;
    bool showSpaceInfo = true;
    bool enlargeSmallPreviews = true;
    int sortingChoice = 0;               // 0 natural, 1 case-insensitive, 2 case-sensitive
    bool hideBackupFiles = false;        // application/x-trash and *~ files
    int doubleClickViewAction = 0;       // 0 nothing, 1 select all, 2 go up, 3 new tab, 4 toggle hidden

    // ---- Confirmations
    bool confirmTrash = false;
    bool confirmDelete = true;
    bool confirmEmptyTrash = true;
    bool confirmClosingMultipleTabs = true;
    bool confirmOpenManyFolders = true;
    bool confirmOpenManyTerminals = true;
    bool confirmRenameFileType = true;
    ExecAction executableAction = ExecAction::Ask;

    // ---- Context menu entries
    bool ctxCopyMoveMenu = false;
    bool ctxAddToPlaces = true;
    bool ctxSortBy = true;
    bool ctxViewMode = true;
    bool ctxOpenInNewTab = true;
    bool ctxOpenInNewWindow = true;
    bool ctxOpenInSplitView = true;
    bool ctxCopyLocation = true;
    bool ctxDuplicateHere = true;
    bool ctxOpenTerminal = true;
    bool ctxCopyToOtherView = true;
    bool ctxMoveToOtherView = true;
    bool ctxDeletePermanently = true;

    // ---- Terminal (was KDE "Default Applications")
    std::string terminalCommand;         // empty: detect

    // ---- Content display
    DirSizeMode dirSizeMode = DirSizeMode::Count;
    int recursiveDirSizeLimit = 10;
    bool useShortRelativeDates = true;
    int permissionsFormat = 0;
    int elidingMode = 1;                 // 0 right, 1 middle

    // ---- View modes
    ModeSettings icons{32, 64};
    int iconsTextWidth = 1;              // 0 small .. 3 huge
    int iconsMaxLines = 3;               // 0 unlimited
    bool iconsCompactSpacing = false;
    ModeSettings compact{16, 48};
    int compactMaxTextWidth = 0;         // 0 unlimited, 1 small, 2 medium, 3 large
    ModeSettings details{16, 32};
    bool detailsExpandableFolders = true;
    bool detailsHighlightEntireRow = true;
    bool detailsDiskUsage = false;   // a Disk Usage column in every Details view

    // ---- Previews
    bool previewsDefault = true;
    std::vector<std::string> disabledPreviewMime;   // e.g. "video/*"
    int maxLocalPreviewMiB = 0;          // 0 = no limit
    int maxRemotePreviewMiB = 0;         // 0 = never for remote files

    // ---- Trash (was the KDE trash KCM)
    bool trashUseTimeLimit = false;
    int trashDays = 7;
    bool trashUseSizeLimit = false;
    double trashPercent = 10.0;
    TrashLimitAction trashLimitAction = TrashLimitAction::Warn;

    // ---- Panels
    bool placesPanel = true;
    bool foldersPanel = false;
    bool infoPanel = false;
    int placesIconSize = 22;
    std::vector<std::string> hiddenPlaceSections;
    bool showHiddenPlaces = false;
    bool foldersHiddenShown = false;
    bool foldersLimitToHome = true;
    bool foldersAutoScroll = true;
    bool infoPreviews = true;
    bool infoAutoPlay = false;
    bool infoShowHovered = true;
    bool infoLongDates = true;
    int leftPanelWidth = 200;
    int infoPanelWidth = 260;
    int foldersPanelHeight = 260;

    // ---- Search
    bool searchEverywhere = false;
    bool searchContent = false;
    bool searchHidden = false;

    // ---- Window
    int windowWidth = 1100;
    int windowHeight = 700;
    bool windowMaximized = false;

    // ---- Keyboard shortcuts: action name -> accelerators (overrides defaults)
    std::map<std::string, std::vector<std::string>> shortcuts;

    ModeSettings &mode(int viewMode);
    void load();
    void save() const;
};

Settings &settings();

// Called after the preferences dialog applied changes.
using SettingsListener = std::function<void()>;
int add_settings_listener(SettingsListener l);
void remove_settings_listener(int id);
void notify_settings_changed();

// Zoom levels: 0..16 -> icon size, as in Dolphin.
int icon_size_for_zoom(int level);
int zoom_for_icon_size(int size);
constexpr int kMinZoom = 0;
constexpr int kMaxZoom = 16;

} // namespace beluga
