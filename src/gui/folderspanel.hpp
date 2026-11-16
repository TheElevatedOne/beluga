// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the Folders panel, a folder tree that follows the current location.
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace beluga {

class FoldersPanel {
public:
    FoldersPanel();
    ~FoldersPanel();
    GtkWidget *widget() const { return root_; }
    void setUri(const std::string &uri);
    std::function<void(const std::string &uri)> onNavigate;
    std::function<void(const std::string &uri)> onOpenInNewTab;
    std::function<void(GFile *dest, GdkFileList *files, GdkModifierType mods, GtkWidget *w, double x, double y)> onDrop;

private:
    void setRoot(const std::string &rootUri);
    void expandTo(const std::string &uri);
    static GListModel *childModel(gpointer item, gpointer self);
    GtkWidget *root_;
    GtkWidget *list_;
    GtkTreeListModel *tree_ = nullptr;
    GtkSingleSelection *sel_ = nullptr;
    std::string rootUri_, uri_;
    bool syncing_ = false;
    guint expandSource_ = 0;
    int settingsListener_ = 0;
};

} // namespace beluga
