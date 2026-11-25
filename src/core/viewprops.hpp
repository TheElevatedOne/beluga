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

// A folder's own settings when it has them, else the default settings.
ViewProps load_view_props(const std::string &uri);
// Saves a change made in a folder: to the folder's own settings when it has them (or when every
// folder remembers its own view), else to the default settings for all folders.
void save_view_props(const std::string &uri, const ViewProps &props);
// "Remember view for this folder": a folder's own settings, which override the default.
bool has_own_view_props(const std::string &uri);
void save_own_view_props(const std::string &uri, const ViewProps &props);
void save_own_view_props(const std::vector<std::string> &uris, const ViewProps &props);   // one write
void remove_own_view_props(const std::string &uri);
void save_default_view_props(const ViewProps &props);
// Defaults of the "global" set, used for new folders.
ViewProps default_view_props(const std::string &uri);
void reset_all_view_props();

} // namespace beluga
