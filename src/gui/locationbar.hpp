// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the location bar. Breadcrumb buttons (with a subfolder menu between
// them) or an editable path entry with completion, like KDE's URL navigator.
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace beluga {

class LocationBar {
public:
    LocationBar();
    ~LocationBar();
    GtkWidget *widget() const { return root_; }

    void setUri(const std::string &uri);
    const std::string &uri() const { return uri_; }
    void setEditable(bool on);
    bool editing() const { return editing_; }
    void startEditing(bool selectAll);
    void setActive(bool active);
    void setLoading(bool loading);

    std::function<void(const std::string &uri)> onNavigate;
    std::function<void(const std::string &uri)> onOpenInNewTab;
    std::function<void(GFile *dest, GdkFileList *files, GdkModifierType mods, GtkWidget *w, double x, double y)> onDrop;
    std::function<void()> onActivated;
    std::function<void()> onEditingFinished;
    std::function<bool()> showHidden;

    struct Crumb {
        std::string label, uri, icon;
    };

private:
    void rebuild();
    void stopEditing();
    void navigateTo(const std::string &uri);
    void commit();
    void updateCompletion();
    void hideCompletion();
    bool completionKey(guint keyval);
    std::vector<Crumb> crumbs() const;
    GtkWidget *subfolderButton(const std::string &uri);

    GtkWidget *root_;
    GtkWidget *stack_;
    GtkWidget *crumbScroll_;
    GtkWidget *crumbBox_;
    GtkWidget *entry_;
    GtkWidget *spinner_;
    GtkWidget *completionPop_ = nullptr;
    GtkWidget *completionList_ = nullptr;
    std::string uri_;
    bool editing_ = false;
    bool alwaysEditable_ = false;
    std::map<std::string, std::vector<std::pair<std::string, bool>>> dirCache_;
    bool settingText_ = false;
};

} // namespace beluga
