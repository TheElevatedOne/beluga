// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: an xdg-desktop-portal backend for org.freedesktop.impl.portal.FileChooser, so programs that open and
// save files through the portal (Firefox, Chromium, Flatpaks, GTK and Qt programs set up for it) get Beluga's
// file chooser. `beluga --portal` is started by D-Bus; choosing it is a line in portals.conf (see README).
#pragma once

#include <gtk/gtk.h>

namespace beluga {

extern const char *const kPortalBusName;   // org.freedesktop.impl.portal.desktop.beluga

int run_portal(int argc, char **argv);

// Exports the FileChooser object (/org/freedesktop/portal/desktop) on a connection; the self-test uses it.
guint portal_register(GDBusConnection *bus, GtkApplication *app, GError **error);

} // namespace beluga
