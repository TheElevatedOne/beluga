// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the Places panel: places, remote locations, recent files, saved
// searches and devices (mount, unmount, safely remove).
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace beluga {

class PlacesPanel {
public:
    PlacesPanel();
    ~PlacesPanel();
    GtkWidget *widget() const { return root_; }
    void setCurrentUri(const std::string &uri);
    void focus();

    std::function<void(const std::string &uri)> onNavigate;
    std::function<void(const std::string &uri)> onOpenInNewTab;
    std::function<void(const std::string &uri)> onOpenInNewWindow;
    std::function<void(GFile *dest, GdkFileList *files, GdkModifierType mods, GtkWidget *w, double x, double y)> onDrop;
    std::function<void(const std::string &uri)> onProperties;
    std::function<void(const std::string &message)> onError;

private:
    void rebuild();
    void activateRow(GtkListBoxRow *row, bool newTab);
    void showMenu(GtkListBoxRow *row, double x, double y);
    void runRowAction(const std::string &action);
    GtkWidget *root_;
    GtkWidget *list_;
    std::string current_;
    int placesListener_ = 0;
    int devicesListener_ = 0;
    int settingsListener_ = 0;
    std::string menuKey_;   // row the context menu belongs to
    bool rebuilding_ = false;
};

} // namespace beluga
