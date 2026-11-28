// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: DolphinMainWindow. Tool bar, optional menu bar, tabs (each with one
// or two views), the Places, Folders and Information panels, and all window actions.
#pragma once

#include "item.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

namespace beluga {
struct NetLocation;

class TabPage;
class ViewContainer;
class View;
class PlacesPanel;
class FoldersPanel;
class InfoPanel;

class MainWindow {
public:
    explicit MainWindow(GtkApplication *app);
    ~MainWindow();
    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;

    GtkWidget *widget() const { return win_; }
    GtkWindow *window() const { return GTK_WINDOW(win_); }
    static MainWindow *from(GtkWindow *w);

    // Tabs.
    TabPage *openTab(const std::string &uri, const std::string &secondary = {}, bool activate = true);
    void openInNewTab(const std::string &uri, bool activate = false);
    void openInNewWindow(const std::string &uri);
    void closeTab(int index, bool remember = true);
    void detachTab(int index);
    int tabCount() const { return (int)tabs_.size(); }
    int tabIndex(TabPage *t) const;
    TabPage *tab(int index) const { return index >= 0 && index < (int)tabs_.size() ? tabs_[index].get() : nullptr; }
    TabPage *activeTab() const;
    ViewContainer *activeContainer() const;
    PlacesPanel *placesPanel() const { return places_.get(); }
    // Expires when the window closes. Callbacks that may run later (file jobs, dialogs, clipboard
    // reads) hold this instead of trusting a raw MainWindow pointer.
    std::weak_ptr<void> lifeToken() const { return life_; }
    View *activeView() const;
    void activateTab(int index);
    void present();
    void closeWindow();

    // Called by view containers.
    void containerActivated(ViewContainer *c);
    void containerUriChanged(ViewContainer *c);
    void selectionChanged(ViewContainer *c);
    void hoverChanged(ViewContainer *c);
    void openItems(ViewContainer *c, const std::vector<BelugaItem *> &items, GdkModifierType mods);
    void showContextMenu(ViewContainer *c, int pos, double x, double y);
    // dest nullptr: move to the trash.
    void handleDrop(GFile *dest, GdkFileList *files, GdkModifierType mods, GtkWidget *w, double x, double y);
    void dropFiles(GFile *dest, const std::vector<Ref<GFile>> &files, GdkDragAction action);

    void openTerminal(const std::string &folder);
    void showProperties(const std::vector<Ref<GFile>> &files);
    void updateActions();
    void updateTitle();
    void applySettings();
    void syncPanels();
    void updateTabBar();
    void updateCutState();
    void focusPlaces();
    void toggleDiskUsagePane();
    // Closes the window without asking about its tabs (Vim's :q!).
    void closeWithoutAsking() { closing_ = true; gtk_window_close(GTK_WINDOW(win_)); }
    class VimMode *vim() const { return vim_.get(); }
    GtkWidget *toolbarWidget() const { return toolbar_; }
    GtkWidget *menubarWidget() const { return menubar_; }
    bool diskUsagePaneOpen() const { return duPaneOpen_; }
    GtkWidget *diskUsagePane() const { return duPane_; }

    // Session.
    void saveSession(GKeyFile *kf, const std::string &group) const;
    bool restoreSession(GKeyFile *kf, const std::string &group);

    // mainwindow_actions.cpp
    void setupActions();
    GMenuModel *menuModel() const { return menu_; }
    void newFolder();
    void newFile(const std::string &templatePath, const std::string &defaultName);
    void newLink();
    void trashOrDelete(bool del);
    void paste(GFile *destDir);
    void copySelection(bool cut);
    void copyOrMoveTo(const std::string &destUri, bool move);
    void addToPlaces(const std::string &uri);
    void addNetworkLocation(const NetLocation &n, const std::string &name);
    void runCustomAction(int index);
    std::vector<Ref<GFile>> selectedFiles() const;
    std::vector<BelugaItem *> selectedItems() const;

    struct ClosedTab {
        std::string primary, secondary;
    };
    std::vector<ClosedTab> closedTabs_;

private:
    friend struct WindowActions;
    void buildToolbar();
    void populateToolbar();
    void attachToolbarMenus();
    void buildMenus();
    void onTabSwitched(int index);
    void rebuildClosedTabsMenu();
    void layoutRight();
    void syncLocationBars();
    void fitTabs();
    void fitTabsLater();

    GtkWidget *win_;
    GtkWidget *menubar_;
    GtkWidget *toolbar_;
    GtkWidget *notebook_;
    GtkWidget *outerPaned_, *leftPaned_, *innerPaned_;
    GtkWidget *rightStack_ = nullptr;   // the Information panel or the Disk Usage pane
    GtkWidget *duPane_ = nullptr;
    bool duPaneOpen_ = false;
    GtkWidget *backButton_, *forwardButton_;
    GtkWidget *viewModeIcon_ = nullptr, *splitLabel_ = nullptr;   // weak: cleared with their widgets
    std::vector<std::string> toolbarItems_;   // what the tool bar was built from
    GMenuModel *mainMenuModel_ = nullptr, *viewMenuModel_ = nullptr, *sortMenuModel_ = nullptr;
    GtkWidget *locSlot_ = nullptr;   // the active tab's location bars, in the tool bar
    GtkWidget *plusPage_ = nullptr;  // the last notebook page: its tab is New Tab
    bool removingTab_ = false;
public:
    int tabsWidth_ = 0;              // the notebook's width, from the probe over it
private:
    guint fitSource_ = 0;
    GMenuModel *menu_ = nullptr;
    GMenu *closedTabsMenu_ = nullptr;
    std::vector<std::unique_ptr<TabPage>> tabs_;
    std::unique_ptr<PlacesPanel> places_;
    std::unique_ptr<FoldersPanel> folders_;
    std::unique_ptr<InfoPanel> info_;
public:
    std::unique_ptr<class VimMode> vim_;
private:
    int settingsListener_ = 0;
    int undoListener_ = 0;
    bool closing_ = false;
    bool updatingActions_ = false;
    std::shared_ptr<void> life_ = std::make_shared<char>(0);
};

} // namespace beluga
