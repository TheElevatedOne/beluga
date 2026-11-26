// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the Disk Usage window, a dust-like tree of what takes up the space in a folder.
#pragma once

#include <gtk/gtk.h>

#include <string>

namespace beluga {

class MainWindow;

// Opens a Disk Usage window for a local folder. Activating a row opens it in `w`.
GtkWidget *show_disk_usage(MainWindow *w, const std::string &path);
// The Disk Usage pane of a main window (the toolbar's Disk Usage button), and the folder it shows (a URI; folders
// that aren't local show a note).
GtkWidget *disk_usage_pane_new(MainWindow *w);
void disk_usage_pane_show(GtkWidget *pane, const std::string &uri);
// Whether a scan of the window or pane is running (for the self-test).
bool disk_usage_busy(GtkWidget *window);
// The number of rows shown in the window or pane (for the self-test).
guint disk_usage_rows(GtkWidget *window);

} // namespace beluga
