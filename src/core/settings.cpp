// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.hpp"

#include "util.hpp"

#include <glib.h>

namespace beluga {

namespace {

std::map<int, SettingsListener> &listeners()
{
    static std::map<int, SettingsListener> l;
    return l;
}

std::string conf_path() { return config_dir() + "/beluga.conf"; }

struct Reader {
    GKeyFile *kf;
    void b(const char *g, const char *k, bool &v) {
        GError *e = nullptr;
        gboolean r = g_key_file_get_boolean(kf, g, k, &e);
        if (!e) v = r; else g_error_free(e);
    }
    void i(const char *g, const char *k, int &v) {
        GError *e = nullptr;
        int r = g_key_file_get_integer(kf, g, k, &e);
        if (!e) v = r; else g_error_free(e);
    }
    void d(const char *g, const char *k, double &v) {
        GError *e = nullptr;
        double r = g_key_file_get_double(kf, g, k, &e);
        if (!e) v = r; else g_error_free(e);
    }
    void s(const char *g, const char *k, std::string &v) {
        char *r = g_key_file_get_string(kf, g, k, nullptr);
        if (r) v = take(r);
    }
    void l(const char *g, const char *k, std::vector<std::string> &v) {
        gsize n = 0;
        char **r = g_key_file_get_string_list(kf, g, k, &n, nullptr);
        if (!r) return;
        v.clear();
        for (gsize j = 0; j < n; ++j) v.emplace_back(r[j]);
        g_strfreev(r);
    }
    template <typename E> void e(const char *g, const char *k, E &v) {
        int x = (int)v;
        i(g, k, x);
        v = (E)x;
    }
    void mode(const char *g, ModeSettings &m) {
        i(g, "IconSize", m.iconSize);
        i(g, "PreviewSize", m.previewSize);
        b(g, "UseSystemFont", m.useSystemFont);
        s(g, "ViewFont", m.font);
    }
};

struct Writer {
    GKeyFile *kf;
    void b(const char *g, const char *k, bool v) { g_key_file_set_boolean(kf, g, k, v); }
    void i(const char *g, const char *k, int v) { g_key_file_set_integer(kf, g, k, v); }
    void d(const char *g, const char *k, double v) { g_key_file_set_double(kf, g, k, v); }
    void s(const char *g, const char *k, const std::string &v) { g_key_file_set_string(kf, g, k, v.c_str()); }
    void l(const char *g, const char *k, const std::vector<std::string> &v) {
        std::vector<const char *> p;
        for (auto &x : v) p.push_back(x.c_str());
        g_key_file_set_string_list(kf, g, k, p.data(), p.size());
    }
    template <typename E> void e(const char *g, const char *k, E v) { i(g, k, (int)v); }
    void mode(const char *g, const ModeSettings &m) {
        i(g, "IconSize", m.iconSize);
        i(g, "PreviewSize", m.previewSize);
        b(g, "UseSystemFont", m.useSystemFont);
        s(g, "ViewFont", m.font);
    }
};

// One list of fields used for both reading and writing.
template <typename IO, typename ST>
void transfer(IO &io, ST &st)
{
    const char *G = "General";
    io.s(G, "HomeUrl", st.homeUrl);
    io.b(G, "RememberOpenedTabs", st.rememberOpenedTabs);
    io.b(G, "SplitView", st.splitView);
    io.b(G, "FilterBar", st.filterBar);
    io.b(G, "EditableUrl", st.editableUrl);
    io.b(G, "ShowFullPath", st.showFullPath);
    io.b(G, "ShowFullPathInTitlebar", st.showFullPathInTitlebar);
    io.b(G, "OpenExternallyCalledFolderInNewTab", st.openExternallyCalledFolderInNewTab);
    io.b(G, "BrowseArchives", st.browseArchives);
    io.b(G, "AlwaysShowTabBar", st.alwaysShowTabBar);
    io.b(G, "ShowCloseButtonOnTabs", st.showCloseButtonOnTabs);
    io.b(G, "OpenNewTabAfterLastTab", st.openNewTabAfterLastTab);
    io.i(G, "TabStyle", st.tabStyle);
    io.i(G, "CloseSplitViewChoice", st.closeSplitViewChoice);
    io.b(G, "UseTabForSwitchingSplitView", st.useTabForSwitchingSplitView);
    io.b(G, "GlobalViewProps", st.globalViewProps);
    io.b(G, "RenameInline", st.renameInline);
    io.b(G, "ShowSelectionToggle", st.showSelectionToggle);
    io.b(G, "ShowPasteBarAfterCopying", st.showPasteBarAfterCopying);
    io.b(G, "ShowToolTips", st.showToolTips);
    io.b(G, "AutoExpandFolders", st.autoExpandFolders);
    io.b(G, "SingleClick", st.singleClick);
    io.b(G, "ShowMenuBar", st.showMenuBar);
    io.b(G, "ShowToolBar", st.showToolBar);
    io.l("Toolbar", "Items", st.toolbarItems);
    io.b("Vim", "Enabled", st.vimMode);
    io.s("Vim", "Leader", st.vimLeader);
    io.i("Vim", "Timeout", st.vimTimeout);
    io.e(G, "ShowStatusBar", st.statusBar);
    io.e(G, "ShownStatusBar", st.statusBarShown);
    io.b(G, "ShowZoomSlider", st.showZoomSlider);
    io.b(G, "ShowSpaceInfo", st.showSpaceInfo);
    io.b(G, "EnlargeSmallPreviews", st.enlargeSmallPreviews);
    io.i(G, "SortingChoice", st.sortingChoice);
    io.b(G, "HideBackupFiles", st.hideBackupFiles);
    io.i(G, "DoubleClickViewAction", st.doubleClickViewAction);

    const char *C = "Confirmations";
    io.b(C, "Trash", st.confirmTrash);
    io.b(C, "Delete", st.confirmDelete);
    io.b(C, "EmptyTrash", st.confirmEmptyTrash);
    io.b(C, "ClosingMultipleTabs", st.confirmClosingMultipleTabs);
    io.b(C, "OpenManyFolders", st.confirmOpenManyFolders);
    io.b(C, "OpenManyTerminals", st.confirmOpenManyTerminals);
    io.b(C, "RenameFileType", st.confirmRenameFileType);
    io.b(C, "ActAsAdmin", st.confirmActAsAdmin);
    io.e(C, "ExecutableAction", st.executableAction);

    const char *M = "ContextMenu";
    io.b(M, "ShowCopyMoveMenu", st.ctxCopyMoveMenu);
    io.b(M, "ShowAddToPlaces", st.ctxAddToPlaces);
    io.b(M, "ShowSortBy", st.ctxSortBy);
    io.b(M, "ShowViewMode", st.ctxViewMode);
    io.b(M, "ShowOpenInNewTab", st.ctxOpenInNewTab);
    io.b(M, "ShowOpenInNewWindow", st.ctxOpenInNewWindow);
    io.b(M, "ShowOpenInSplitView", st.ctxOpenInSplitView);
    io.b(M, "ShowCopyLocation", st.ctxCopyLocation);
    io.b(M, "ShowDuplicateHere", st.ctxDuplicateHere);
    io.b(M, "ShowOpenTerminal", st.ctxOpenTerminal);
    io.b(M, "ShowCopyToOtherSplitView", st.ctxCopyToOtherView);
    io.b(M, "ShowMoveToOtherSplitView", st.ctxMoveToOtherView);
    io.b(M, "ShowDeletePermanently", st.ctxDeletePermanently);

    io.s("Terminal", "Command", st.terminalCommand);

    const char *D = "ContentDisplay";
    io.e(D, "DirectorySizeMode", st.dirSizeMode);
    io.i(D, "RecursiveDirectorySizeLimit", st.recursiveDirSizeLimit);
    io.b(D, "UseShortRelativeDates", st.useShortRelativeDates);
    io.i(D, "PermissionsFormat", st.permissionsFormat);
    io.i(D, "ElidingMode", st.elidingMode);

    io.mode("IconsMode", st.icons);
    io.i("IconsMode", "TextWidthIndex", st.iconsTextWidth);
    io.i("IconsMode", "MaximumTextLines", st.iconsMaxLines);
    io.b("IconsMode", "CompactSpacing", st.iconsCompactSpacing);
    io.mode("CompactMode", st.compact);
    io.i("CompactMode", "MaximumTextWidthIndex", st.compactMaxTextWidth);
    io.mode("DetailsMode", st.details);
    io.b("DetailsMode", "ExpandableFolders", st.detailsExpandableFolders);
    io.b("DetailsMode", "HighlightEntireRow", st.detailsHighlightEntireRow);
    io.b("DetailsMode", "DiskUsageColumn", st.detailsDiskUsage);

    const char *P = "Previews";
    io.b(P, "ShowByDefault", st.previewsDefault);
    io.l(P, "DisabledTypes", st.disabledPreviewMime);
    io.i(P, "MaximumLocalSizeMiB", st.maxLocalPreviewMiB);
    io.i(P, "MaximumRemoteSizeMiB", st.maxRemotePreviewMiB);

    const char *T = "Trash";
    io.b(T, "UseTimeLimit", st.trashUseTimeLimit);
    io.b(T, "HomeOnly", st.trashHomeOnly);
    io.i(T, "Days", st.trashDays);
    io.b(T, "UseSizeLimit", st.trashUseSizeLimit);
    io.d(T, "Percent", st.trashPercent);
    io.e(T, "LimitReachedAction", st.trashLimitAction);

    const char *PN = "Panels";
    io.b(PN, "Places", st.placesPanel);
    io.b(PN, "Folders", st.foldersPanel);
    io.b(PN, "Information", st.infoPanel);
    io.i(PN, "PlacesIconSize", st.placesIconSize);
    io.l(PN, "HiddenPlaceSections", st.hiddenPlaceSections);
    io.l(PN, "SectionOrder", st.placeSectionOrder);
    io.b(PN, "ShowHiddenPlaces", st.showHiddenPlaces);
    io.b(PN, "FoldersHiddenShown", st.foldersHiddenShown);
    io.b(PN, "FoldersLimitToHome", st.foldersLimitToHome);
    io.b(PN, "FoldersAutoScroll", st.foldersAutoScroll);
    io.b(PN, "InfoPreviews", st.infoPreviews);
    io.b(PN, "InfoAutoPlay", st.infoAutoPlay);
    io.b(PN, "InfoShowHovered", st.infoShowHovered);
    io.b(PN, "InfoLongDates", st.infoLongDates);
    io.i(PN, "LeftWidth", st.leftPanelWidth);
    io.i(PN, "InfoWidth", st.infoPanelWidth);
    io.i(PN, "DiskUsageWidth", st.duPaneWidth);
    io.i(PN, "FoldersHeight", st.foldersPanelHeight);

    const char *S = "Search";
    io.b(S, "Everywhere", st.searchEverywhere);
    io.b(S, "Content", st.searchContent);
    io.b(S, "IncludeHidden", st.searchHidden);
    io.b(S, "Index", st.searchIndex);

    const char *DU = "DiskUsage";
    io.b(DU, "ApparentSize", st.duApparentSize);
    io.b(DU, "OneFileSystem", st.duOneFileSystem);

    const char *W = "Window";
    io.i(W, "Width", st.windowWidth);
    io.i(W, "Height", st.windowHeight);
    io.b(W, "Maximized", st.windowMaximized);
}

} // namespace

Settings &settings()
{
    static Settings s;
    return s;
}

ModeSettings &Settings::mode(int viewMode)
{
    switch (viewMode) {
    case 1: return compact;
    case 2:
    case 3: return details;
    default: return icons;
    }
}

void Settings::load()
{
    homeUrl = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, conf_path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        Reader r{kf};
        transfer(r, *this);
        gsize n = 0;
        char **keys = g_key_file_get_keys(kf, "Shortcuts", &n, nullptr);
        for (gsize i = 0; keys && i < n; ++i) {
            std::vector<std::string> accels;
            r.l("Shortcuts", keys[i], accels);
            shortcuts[keys[i]] = accels;
        }
        g_strfreev(keys);
        keys = g_key_file_get_keys(kf, "VimKeys", &n, nullptr);
        for (gsize i = 0; keys && i < n; ++i) {
            std::vector<std::string> seqs;
            r.l("VimKeys", keys[i], seqs);
            vimKeys[keys[i]] = seqs;
        }
        g_strfreev(keys);
    }
    g_key_file_free(kf);
    if (homeUrl.empty()) homeUrl = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
}

void Settings::save() const
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, conf_path().c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    Writer w{kf};
    transfer(w, const_cast<Settings &>(*this));
    g_key_file_remove_group(kf, "Shortcuts", nullptr);
    for (auto &[k, v] : shortcuts) w.l("Shortcuts", k.c_str(), v);
    g_key_file_remove_group(kf, "VimKeys", nullptr);
    for (auto &[k, v] : vimKeys) w.l("VimKeys", k.c_str(), v);
    GError *err = nullptr;
    if (!g_key_file_save_to_file(kf, conf_path().c_str(), &err)) {
        g_warning("Could not save settings: %s", err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
}

int add_settings_listener(SettingsListener l)
{
    static int next = 1;
    listeners()[next] = std::move(l);
    return next++;
}

void remove_settings_listener(int id) { listeners().erase(id); }

void notify_settings_changed()
{
    auto copy = listeners();
    for (auto &[id, l] : copy) l();
}

int icon_size_for_zoom(int level)
{
    switch (level) {
    case 0: return 16;
    case 1: return 22;
    case 2: return 32;
    case 3: return 48;
    case 4: return 64;
    default: return 64 + ((level - 4) << 4);
    }
}

int zoom_for_icon_size(int size)
{
    if (size <= 16) return 0;
    if (size <= 22) return 1;
    if (size <= 32) return 2;
    if (size <= 48) return 3;
    if (size <= 64) return 4;
    int l = 4 + ((size - 64) >> 4);
    return l > kMaxZoom ? kMaxZoom : l;
}

} // namespace beluga
