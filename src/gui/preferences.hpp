// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: "Configure Beluga". Every setting lives here, including those that
// KDE kept in System Settings (click behaviour, terminal, trash, shortcuts).
#pragma once

#include <gtk/gtk.h>

#include <string>

namespace beluga {

void show_preferences(GtkWidget *parent);
// Opens it on a page: "interface", "view", "vim", "contextmenu", "trash", "shortcuts".
void show_preferences(GtkWidget *parent, const std::string &page);

} // namespace beluga
