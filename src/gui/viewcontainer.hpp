// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: DolphinViewContainer. One view with its location bar, history,
// filter bar, search bar, selection mode bars, messages and status bar.
#pragma once

#include "locationbar.hpp"
#include "searchbar.hpp"
#include "statusbar.hpp"
#include "view.hpp"

#include <memory>
#include <string>
#include <vector>

namespace beluga {

class MainWindow;

class ViewContainer {
public:
    ViewContainer(MainWindow *win, const std::string &uri);
    ~ViewContainer();
    ViewContainer(const ViewContainer &) = delete;
    ViewContainer &operator=(const ViewContainer &) = delete;

    GtkWidget *widget() const { return root_; }
    View *view() const { return view_.get(); }
    LocationBar *locationBar() { return &loc_; }
    FilterBar *filterBar() { return &filter_; }
    SearchBar *searchBar() { return &search_; }
    MainWindow *window() const { return win_; }

    const std::string &uri() const { return view_->location(); }
    void navigate(const std::string &uri, bool addHistory = true);
    bool canGoBack() const { return index_ > 0; }
    bool canGoForward() const { return index_ + 1 < (int)history_.size(); }
    bool canGoUp() const;
    void goBack();
    void goForward();
    void goUp();
    void goHome();
    void goHistory(int index);
    // Most recent first.
    std::vector<std::pair<int, std::string>> backList() const;
    std::vector<std::pair<int, std::string>> forwardList() const;
    void reload();

    void setActive(bool on);
    bool active() const { return active_; }
    void setFilterBarVisible(bool on);
    bool filterBarVisible() const { return filter_.visible(); }
    void setSearchBarVisible(bool on);
    bool searchBarVisible() const { return search_.visible(); }
    void setSelectionMode(bool on);
    bool selectionMode() const { return view_->selectionMode(); }
    void showMessage(const std::string &text, bool error);
    std::string title() const;
    void updateStatus();
    void applySettings();

private:
    struct HistoryEntry {
        std::string uri;
        double scroll = 0;
        std::string current;
    };
    void saveHistoryState();
    void restoreHistoryState(const HistoryEntry &e);
    void open(const std::string &uri);
    void scheduleStatus();
    void handleLocationGone();
    void updateColumns();
    void columnNavigateLater(const std::string &uri);

    MainWindow *win_;
    GtkWidget *root_;
    GtkWidget *message_, *messageLabel_;
    GtkWidget *selTop_, *selBottom_, *selCount_;
    GtkWidget *viewSlot_;
    GtkWidget *colScroll_ = nullptr, *colBox_ = nullptr;
    std::vector<std::unique_ptr<View>> parents_;     // Columns view: the folders above the current one
    std::string columnsBase_;
    bool columnsActive_ = false, updatingColumns_ = false;
    guint columnNavSource_ = 0;
    std::string columnNavTarget_;
    std::unique_ptr<View> view_;
    LocationBar loc_;
    FilterBar filter_;
    SearchBar search_;
    StatusBar status_;
    std::vector<HistoryEntry> history_;
    int index_ = -1;
    bool active_ = false;
    guint statusSource_ = 0;
    guint messageSource_ = 0;
    bool searchNavigation_ = false;
    int settingsListener_ = 0;
};

} // namespace beluga
