// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: view properties (mode, sorting, columns...). Either one global
// set or one set per folder, stored in ~/.local/share/beluga/viewprops.conf
// instead of .directory files inside the folders.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace beluga {

enum ViewMode { ModeIcons = 0, ModeCompact = 1, ModeDetails = 2, ModeColumns = 3 };

// Item roles shown as columns / extra lines and used for sorting and grouping.
struct RoleInfo {
    const char *id;
    const char *title;
    bool rightAligned;
    int defaultWidth;
};
const std::vector<RoleInfo> &all_roles();
const RoleInfo *role_info(const std::string &id);
std::string role_title(const std::string &id);

struct ViewProps {
    int viewMode = ModeIcons;
    int zoomLevel = -1;                 // -1: the mode's default size
    bool previewsShown = true;
    bool hiddenFilesShown = false;
    bool groupedSorting = false;
    std::string sortRole = "text";
    bool sortDescending = false;
    bool sortFoldersFirst = true;
    bool sortHiddenLast = false;
    std::vector<std::string> visibleRoles = {"Details_size", "Details_modificationtime"};
    std::map<std::string, int> columnWidths;

    std::vector<std::string> rolesForMode(int mode) const;
    void setRolesForMode(int mode, const std::vector<std::string> &roles);
    bool operator==(const ViewProps &o) const;
};

ViewProps load_view_props(const std::string &uri);
void save_view_props(const std::string &uri, const ViewProps &props);
// Defaults of the "global" set, used for new folders.
ViewProps default_view_props(const std::string &uri);
void reset_all_view_props();

} // namespace beluga
