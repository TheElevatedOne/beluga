// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: status bar (item summary, zoom slider, free space) and the small
// variant that floats over the bottom-left corner of the view.
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace beluga {

class StatusBar {
public:
    StatusBar();
    ~StatusBar();
    GtkWidget *widget() const { return bar_; }          // full width bar
    GtkWidget *smallLabel() const { return small_; }    // overlay for the small mode
    void setText(const std::string &text);
    void setZoom(int level);
    void setLocation(const std::string &uri);
    void applySettings();
    std::function<void(int)> onZoom;

private:
    void updateSpace();
    GtkWidget *bar_;
    GtkWidget *label_;
    GtkWidget *zoom_;
    GtkWidget *zoomLabel_;
    GtkWidget *spaceDetail_;   // the free space popover's text
    GtkWidget *spaceBox_;
    GtkWidget *spaceBar_;
    GtkWidget *spaceLabel_;
    GtkWidget *small_;
    std::string uri_;
    guint timer_ = 0;
    bool settingZoom_ = false;
    GCancellable *cancel_ = nullptr;
};

} // namespace beluga
