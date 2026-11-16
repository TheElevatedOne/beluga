// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: progress of running file operations, shown as a tool bar button with
// a popover (KDE showed these as Plasma notifications).
#pragma once

#include <gtk/gtk.h>

namespace beluga {

GtkWidget *jobs_indicator_new();

} // namespace beluga
