// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: what the main tool bar can hold, and Settings › Configure Toolbars…, as in Dolphin (KEditToolBar): the
// actions, the location bar and separators, in the order shown. Items before the location bar sit left of it,
// the others right of it.
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace beluga {

enum class ToolKind {
    Button,      // runs an action
    Toggle,      // shows an action's on/off state
    ViewMenu,    // the current view mode's icon; modes, sorting and columns in its menu
    SortMenu,    // Sort By
    Split,       // Split / Close, with its name
    Menu,        // the hamburger menu
    Location,    // the location bar(s)
    Separator,
};

struct ToolItemInfo {
    const char *id;
    const char *title;          // N_() string
    const char *const *icons;   // themed icon names, first found wins
    ToolKind kind;
    const char *action;         // detailed action name ("win.…"), when there is one
};

const std::vector<ToolItemInfo> &tool_items();
const ToolItemInfo *tool_item(const std::string &id);
std::vector<std::string> default_toolbar();

// Settings › Configure Toolbars…
void show_toolbar_editor(GtkWidget *parent);

} // namespace beluga
