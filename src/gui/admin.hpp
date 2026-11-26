// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: running as root. View › Act as Administrator opens a second Beluga as root through pkexec (polkit's
// password prompt), and `sudo beluga` works too: a root Beluga gets a display, a runtime folder and a D-Bus
// session of its own, and the theme of the user who started it. Its windows say that they act as administrator.
#pragma once

#include <gtk/gtk.h>

#include <string>

namespace beluga {

bool running_as_root();

// Before GTK starts. As root: takes the display handed over by Act as Administrator (--root-env), finds the
// user's Wayland display under sudo, makes a runtime folder, and restarts itself under dbus-run-session when
// there's no session bus of root's own. Removes --root-env from argv. Does nothing for other users.
void prepare_root_environment(int &argc, char **&argv);

// After GTK starts, before the style is loaded: the GTK theme, icons and style sheet of the user who started
// this root Beluga (PKEXEC_UID or SUDO_UID).
void use_invoking_users_theme();

// Asks (unless turned off), then opens a Beluga window as administrator at uri.
void act_as_admin(GtkWidget *parent, const std::string &uri);

} // namespace beluga
