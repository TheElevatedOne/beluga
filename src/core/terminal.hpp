// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: opens the user's terminal emulator in a folder (F4). The command is
// a Beluga setting; when empty, $TERMINAL or the first installed known terminal is used.
#pragma once

#include <gio/gio.h>

#include <string>
#include <vector>

namespace beluga::terminal {

// Installed terminals from the built-in list, for the preferences dialog.
std::vector<std::string> installed();
std::string detect();
std::string effective_command();
// The terminal command for a folder: effective_command() with "%d" replaced by the quoted folder.
std::string command_for(const std::string &folder);
// "%d" in the command is replaced by the folder; else the folder is the working dir.
bool open(const std::string &folder, GError **err);

} // namespace beluga::terminal
