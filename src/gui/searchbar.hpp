// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the filter bar (Ctrl+I) and the search bar (Ctrl+F) of a view.
#pragma once

#include "search.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace beluga {

class FilterBar {
public:
    FilterBar();
    ~FilterBar();
    GtkWidget *widget() const { return revealer_; }
    void setVisible(bool on);
    bool visible() const;
    void focus();
    void clear();
    std::string text() const;
    bool locked() const;
    std::function<void(const std::string &)> onChanged;
    std::function<void()> onClose;
    std::function<void()> onFocusView;

private:
    GtkWidget *revealer_, *entry_, *lock_;
};

class SearchBar {
public:
    SearchBar();
    ~SearchBar();
    GtkWidget *widget() const { return revealer_; }
    void setVisible(bool on);
    bool visible() const;
    void focus();
    void setFolder(const std::string &uri);     // "this folder"
    void showQuery(const SearchQuery &q);
    std::function<void(const std::string &searchUri)> onSearch;
    std::function<void()> onClose;
    std::function<void()> onFocusView;

private:
    void schedule();
    void run();
    GtkWidget *revealer_, *entry_, *where_, *what_, *type_, *when_, *hidden_;
    std::string folder_;
    guint timer_ = 0;
    bool updating_ = false;
};

} // namespace beluga
