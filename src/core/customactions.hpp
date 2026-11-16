// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: user-defined context menu actions, configured in Beluga itself.
// They replace KDE's service menus. Stored in ~/.config/beluga/actions.conf.
//
// Command placeholders: %f first file, %F all files, %u / %U URIs, %n / %N file names,
// %d current folder, %% a percent sign. Values are shell-quoted; the command runs
// through /bin/sh in the current folder.
#pragma once

#include <gio/gio.h>

#include <string>
#include <vector>

namespace beluga {

struct CustomAction {
    std::string name;
    std::string icon;
    std::string command;
    std::string patterns;        // "*.zip;*.tar.*", empty: any name
    std::string mimeTypes;       // "image/*;application/pdf", empty: any type
    bool forFiles = true;
    bool forFolders = false;
    bool multiple = true;        // also when several items are selected
    bool runInTerminal = false;
};

struct ActionTarget {
    std::string path;            // local path ("" for remote)
    std::string uri;
    std::string name;
    std::string mime;
    bool isDir = false;
};

std::vector<CustomAction> load_custom_actions();
void save_custom_actions(const std::vector<CustomAction> &actions);
std::vector<CustomAction> default_custom_actions();
bool action_applies(const CustomAction &a, const std::vector<ActionTarget> &targets);
std::string expand_action_command(const CustomAction &a, const std::vector<ActionTarget> &targets, const std::string &folder);
bool run_custom_action(const CustomAction &a, const std::vector<ActionTarget> &targets, const std::string &folder, GError **err);

} // namespace beluga
