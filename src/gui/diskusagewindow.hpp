// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the Disk Usage window, a dust-like tree of what takes up the space in a folder.
#pragma once

#include <gtk/gtk.h>

#include <string>

namespace beluga {

class MainWindow;

// Opens a Disk Usage window for a local folder. Activating a row opens it in `w`.
GtkWidget *show_disk_usage(MainWindow *w, const std::string &path);
// Whether a scan of the window is running (for the self-test).
bool disk_usage_busy(GtkWidget *window);
// The number of rows shown (for the self-test).
guint disk_usage_rows(GtkWidget *window);

} // namespace beluga
