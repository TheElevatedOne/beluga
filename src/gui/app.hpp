// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: application state shared by all windows: windows, clipboard,
// file operation questions, session and the org.freedesktop.FileManager1 service.
#pragma once

#include "dialogs.hpp"
#include "util.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace beluga {

class MainWindow;

class App {
public:
    static App &instance();
    void init(GtkApplication *app);
    void shutdown();
    GtkApplication *gtk() const { return app_; }

    MainWindow *newWindow(const std::vector<std::string> &uris, bool split);
    MainWindow *activeWindow() const;
    const std::vector<MainWindow *> &windows() const { return windows_; }
    void windowClosed(MainWindow *w);
    void quit();
    // Reloads every view showing this folder.
    void reloadLocation(const std::string &uri);
    // The folder shown in a main window (empty for other windows).
    std::string currentUriOf(GtkWindow *window) const;
    // Opens folders (in tabs of the active window or a new window) and selects files.
    void openLocations(const std::vector<std::string> &uris, bool select, bool split, bool newWindow);

    // Clipboard.
    void setClipboard(const std::vector<Ref<GFile>> &files, bool cut);
    void readClipboard(std::function<void(std::vector<Ref<GFile>> files, bool cut)> cb);
    bool canPaste() const { return canPaste_; }
    const std::set<std::string> &cutUris() const { return cutUris_; }
    void clearClipboardAfterMove();

    void saveSession();
    bool restoreSession();
    void ownFileManagerName();
    void applySettings();
    void updateAccels();
    // Whether a key event is one of the window shortcuts (as configured).
    bool isShortcut(GdkEvent *event) const;

    bool selftest = false;

private:
    void clipboardChanged();
    GtkApplication *app_ = nullptr;
    std::vector<MainWindow *> windows_;
    std::set<std::string> cutUris_;
    std::vector<Ref<GFile>> clipFiles_;
    bool clipCut_ = false;
    bool canPaste_ = false;
    GtkJobUi jobUi_;
    guint busOwner_ = 0;
    std::vector<GtkShortcutTrigger *> triggers_;
    guint busRegistration_ = 0;
    int settingsListener_ = 0;
};

// Accelerators of all actions, with the user's overrides applied.
struct ShortcutInfo {
    const char *action;      // "win.copy" or "win.view-mode('icons')"
    const char *title;
    std::vector<std::string> defaults;
};
const std::vector<ShortcutInfo> &shortcut_table();

} // namespace beluga
