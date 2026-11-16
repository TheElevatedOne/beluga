// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: opening files with their applications, executables and .desktop
// launchers, and the "Open With" dialog.
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace beluga {

struct OpenTarget {
    Ref<GFile> file;
    std::string mime;
    bool executable = false;
};

void open_files(GtkWidget *parent, const std::vector<OpenTarget> &targets);
bool launch_with(GtkWidget *parent, GAppInfo *app, const std::vector<Ref<GFile>> &files);
void show_open_with_dialog(GtkWidget *parent, const std::vector<Ref<GFile>> &files, const std::string &mime);
// Applications for a content type, default first.
std::vector<Ref<GAppInfo>> apps_for_type(const std::string &mime);

} // namespace beluga
