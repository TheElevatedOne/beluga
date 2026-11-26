// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: one tab: a view container, or two side by side (split view).
#pragma once

#include "viewcontainer.hpp"

#include <memory>
#include <string>

namespace beluga {

class MainWindow;

class TabPage {
public:
    TabPage(MainWindow *win, const std::string &uri, const std::string &secondary);
    ~TabPage();
    GtkWidget *widget() const { return root_; }
    GtkWidget *label() const { return label_; }

    ViewContainer *primary() const { return primary_.get(); }
    ViewContainer *secondary() const { return secondary_.get(); }
    ViewContainer *active() const { return active_; }
    ViewContainer *other(ViewContainer *c) const;
    bool contains(ViewContainer *c) const { return c && (c == primary_.get() || c == secondary_.get()); }
    void setActive(ViewContainer *c);
    bool split() const { return secondary_ != nullptr; }
    void setSplit(bool on, const std::string &uri = {});
    // Closes one side of the split view and keeps the other.
    void closeView(ViewContainer *c);
    void updateLabel();
    // Dolphin's tab styles (Settings::tabStyle) for this tab in the window's notebook.
    void applyTabStyle(GtkWidget *notebook, int style);

private:
    void attach();
    MainWindow *win_;
    GtkWidget *root_;
    GtkWidget *paned_;
    GtkWidget *label_;
    GtkWidget *labelText_;
    GtkWidget *labelIcon_;
    std::unique_ptr<ViewContainer> primary_, secondary_;
    ViewContainer *active_ = nullptr;
};

} // namespace beluga
