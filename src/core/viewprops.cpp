// SPDX-License-Identifier: GPL-2.0-or-later
#include "viewprops.hpp"

#include "settings.hpp"
#include "util.hpp"

#include <glib.h>

#include <algorithm>

namespace beluga {

const std::vector<RoleInfo> &all_roles()
{
    static const std::vector<RoleInfo> roles = {
        {"text", N_("Name"), false, 260},
        {"size", N_("Size"), true, 90},
        {"diskusage", N_("Disk Usage"), true, 160},
        {"modificationtime", N_("Modified"), false, 150},
        {"creationtime", N_("Created"), false, 150},
        {"accesstime", N_("Accessed"), false, 150},
        {"type", N_("Type"), false, 150},
        {"extension", N_("Extension"), false, 80},
        {"permissions", N_("Permissions"), false, 110},
        {"owner", N_("Owner"), false, 90},
        {"group", N_("User Group"), false, 90},
        {"path", N_("Path"), false, 220},
        {"destination", N_("Link Destination"), false, 180},
        {"deletiontime", N_("Deletion Time"), false, 150},
        {"originalpath", N_("Original Location"), false, 220},
        {"dimensions", N_("Dimensions"), false, 100},
    };
    return roles;
}

const RoleInfo *role_info(const std::string &id)
{
    for (auto &r : all_roles())
        if (id == r.id) return &r;
    return nullptr;
}

std::string role_title(const std::string &id)
{
    const RoleInfo *r = role_info(id);
    return r ? _(r->title) : id;
}

static const char *mode_prefix(int mode)
{
    switch (mode) {
    case ModeCompact: return "Compact_";
    case ModeDetails: return "Details_";
    case ModeColumns: return "Columns_";
    default: return "Icons_";
    }
}

std::vector<std::string> ViewProps::rolesForMode(int mode) const
{
    std::string prefix = mode_prefix(mode);
    std::vector<std::string> out;
    for (auto &r : visibleRoles)
        if (starts_with(r, prefix)) {
            std::string id = r.substr(prefix.size());
            if (id != "text" && role_info(id)) out.push_back(id);
        }
    return out;
}

void ViewProps::setRolesForMode(int mode, const std::vector<std::string> &roles)
{
    std::string prefix = mode_prefix(mode);
    std::vector<std::string> keep;
    for (auto &r : visibleRoles)
        if (!starts_with(r, prefix)) keep.push_back(r);
    for (auto &r : roles) keep.push_back(prefix + r);
    visibleRoles = keep;
}

bool ViewProps::operator==(const ViewProps &o) const
{
    return viewMode == o.viewMode && zoomLevel == o.zoomLevel && previewsShown == o.previewsShown
        && hiddenFilesShown == o.hiddenFilesShown && groupedSorting == o.groupedSorting
        && sortRole == o.sortRole && sortDescending == o.sortDescending
        && sortFoldersFirst == o.sortFoldersFirst && sortHiddenLast == o.sortHiddenLast
        && visibleRoles == o.visibleRoles && columnWidths == o.columnWidths;
}

namespace {

std::string store_path() { return data_dir() + "/viewprops.conf"; }

GKeyFile *open_store()
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, store_path().c_str(), G_KEY_FILE_NONE, nullptr);
    return kf;
}

// The group holding a folder's own settings. Trash, search results and recent files always have their
// own. GKeyFile group names can't contain brackets, which remote URIs such as sftp://[::1]/ have.
std::string own_group(const std::string &uri)
{
    if (is_trash_uri(uri)) return "trash";
    if (is_search_uri(uri)) return "search";
    if (is_recent_uri(uri)) return "recent";
    return replace_all(replace_all(uri, "[", "%5B"), "]", "%5D");
}

void write_group(GKeyFile *kf, const std::string &g, const ViewProps &p);

// A folder's own view lives in this group of its .directory file, so it moves and is copied with the
// folder. Dolphin keeps its own [Dolphin] group in the same file; the two don't touch each other's.
const char *kDirGroup = "Beluga";

// The local folder whose .directory file can hold the view; empty for remote folders, archives and the
// trash, search and recent views, which keep theirs in viewprops.conf.
std::string dir_path(const std::string &uri)
{
    if (is_virtual_uri(uri)) return {};
    GFile *f = g_file_new_for_uri(uri.c_str());
    std::string path = g_file_is_native(f) ? take(g_file_get_path(f)) : std::string();
    g_object_unref(f);
    return path;
}

// "Apply to all folders" can't reach every .directory file, so it records when it ran: a folder's own
// view saved before then no longer counts (as Dolphin does with its Timestamp key).
gint64 reset_time(GKeyFile *store) { return g_key_file_get_int64(store, "global", "ResetTime", nullptr); }

bool read_group(GKeyFile *kf, const std::string &g, ViewProps &p);

// Reads the folder's own view from its .directory file. Returns false when it has none that counts.
bool read_dir_file(const std::string &path, gint64 resetTime, ViewProps &p)
{
    std::string file = path + "/.directory";
    if (!g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR)) return false;
    GKeyFile *kf = g_key_file_new();
    bool ok = g_key_file_load_from_file(kf, file.c_str(), G_KEY_FILE_NONE, nullptr)
        && g_key_file_has_group(kf, kDirGroup)
        && g_key_file_get_int64(kf, kDirGroup, "Timestamp", nullptr) > resetTime
        && read_group(kf, kDirGroup, p);
    g_key_file_free(kf);
    return ok;
}

bool write_dir_file(const std::string &path, const ViewProps &p)
{
    return edit_directory_file(path, [&](GKeyFile *kf) {
        g_key_file_remove_group(kf, kDirGroup, nullptr);
        write_group(kf, kDirGroup, p);
        g_key_file_set_int64(kf, kDirGroup, "Timestamp", g_get_real_time());
    }, nullptr);
}

// Saves a folder's own view: into its .directory file when it can be written there (then any copy in
// viewprops.conf is dropped), else into viewprops.conf (read-only folders, remote folders, archives).
void save_own_into(GKeyFile *store, const std::string &uri, const ViewProps &p)
{
    std::string path = dir_path(uri);
    if (!path.empty() && write_dir_file(path, p)) {
        g_key_file_remove_group(store, own_group(uri).c_str(), nullptr);
        return;
    }
    write_group(store, own_group(uri), p);
}

bool read_group(GKeyFile *kf, const std::string &g, ViewProps &p)
{
    const char *G = g.c_str();
    if (!g_key_file_has_group(kf, G)) return false;
    GError *e = nullptr;
    auto gi = [&](const char *k, int &v) {
        int r = g_key_file_get_integer(kf, G, k, &e);
        if (!e) v = r; else g_clear_error(&e);
    };
    auto gb = [&](const char *k, bool &v) {
        gboolean r = g_key_file_get_boolean(kf, G, k, &e);
        if (!e) v = r; else g_clear_error(&e);
    };
    gi("ViewMode", p.viewMode);
    if (p.viewMode < 0 || p.viewMode > 3) p.viewMode = ModeIcons;
    gi("ZoomLevel", p.zoomLevel);
    p.zoomLevel = std::clamp(p.zoomLevel, 0, kMaxZoom);
    gb("PreviewsShown", p.previewsShown);
    gb("HiddenFilesShown", p.hiddenFilesShown);
    gb("GroupedSorting", p.groupedSorting);
    char *s = g_key_file_get_string(kf, G, "SortRole", nullptr);
    if (s) p.sortRole = take(s);
    if (!role_info(p.sortRole)) p.sortRole = "text";
    gb("SortDescending", p.sortDescending);
    gb("SortFoldersFirst", p.sortFoldersFirst);
    gb("SortHiddenLast", p.sortHiddenLast);
    gsize n = 0;
    char **l = g_key_file_get_string_list(kf, G, "VisibleRoles", &n, nullptr);
    if (l) {
        p.visibleRoles.clear();
        for (gsize i = 0; i < n; ++i) p.visibleRoles.emplace_back(l[i]);
        g_strfreev(l);
    }
    l = g_key_file_get_string_list(kf, G, "ColumnWidths", &n, nullptr);
    if (l) {
        p.columnWidths.clear();
        for (gsize i = 0; i < n; ++i) {
            auto parts = split(l[i], ':');
            if (parts.size() == 2) p.columnWidths[parts[0]] = std::atoi(parts[1].c_str());
        }
        g_strfreev(l);
    }
    return true;
}

} // namespace

ViewProps default_view_props(const std::string &uri)
{
    ViewProps p;
    p.previewsShown = settings().previewsDefault;
    if (is_trash_uri(uri)) {
        p.viewMode = ModeDetails;
        p.sortRole = "deletiontime";
        p.sortDescending = true;
        p.visibleRoles = {"Details_originalpath", "Details_deletiontime", "Details_size"};
    } else if (is_search_uri(uri)) {
        p.viewMode = ModeDetails;
        p.visibleRoles = {"Details_path", "Details_size", "Details_modificationtime"};
    } else if (is_recent_uri(uri)) {
        p.viewMode = ModeDetails;
        p.sortRole = "accesstime";
        p.sortDescending = true;
        p.sortFoldersFirst = false;
        p.visibleRoles = {"Details_path", "Details_accesstime"};
    } else {
        GKeyFile *kf = open_store();
        read_group(kf, "global", p);
        g_key_file_free(kf);
    }
    return p;
}

ViewProps load_view_props(const std::string &uri)
{
    // A folder's own settings win; every other folder uses the default ("global") settings.
    ViewProps p = default_view_props(uri);
    GKeyFile *kf = open_store();
    std::string path = dir_path(uri);
    if (path.empty() || !read_dir_file(path, reset_time(kf), p)) {
        if (read_group(kf, own_group(uri), p) && !path.empty()) {
            // Remembered before views moved into .directory files: move it there now.
            save_own_into(kf, uri, p);
            g_key_file_save_to_file(kf, store_path().c_str(), nullptr);
        }
    }
    g_key_file_free(kf);
    return p;
}

bool has_own_view_props(const std::string &uri)
{
    GKeyFile *kf = open_store();
    std::string path = dir_path(uri);
    ViewProps dummy;
    bool has = (!path.empty() && read_dir_file(path, reset_time(kf), dummy)) || g_key_file_has_group(kf, own_group(uri).c_str());
    g_key_file_free(kf);
    return has;
}

void save_view_props(const std::string &uri, const ViewProps &p)
{
    // A change in a folder that has its own settings stays there. Otherwise it changes the default
    // for all folders, unless each folder remembers its own view (the "same style" option is off).
    if (is_virtual_uri(uri) || !settings().globalViewProps || has_own_view_props(uri)) save_own_view_props(uri, p);
    else save_default_view_props(p);
}

void save_own_view_props(const std::string &uri, const ViewProps &p)
{
    save_own_view_props(std::vector<std::string>{uri}, p);
}

void save_own_view_props(const std::vector<std::string> &uris, const ViewProps &p)
{
    GKeyFile *kf = open_store();
    for (auto &u : uris) save_own_into(kf, u, p);
    g_key_file_save_to_file(kf, store_path().c_str(), nullptr);
    g_key_file_free(kf);
}

void save_default_view_props(const ViewProps &p)
{
    GKeyFile *kf = open_store();
    write_group(kf, "global", p);
    g_key_file_save_to_file(kf, store_path().c_str(), nullptr);
    g_key_file_free(kf);
}

void remove_own_view_props(const std::string &uri)
{
    if (is_virtual_uri(uri)) return;
    std::string path = dir_path(uri);
    if (!path.empty() && g_file_test((path + "/.directory").c_str(), G_FILE_TEST_IS_REGULAR))
        edit_directory_file(path, [](GKeyFile *kf) { g_key_file_remove_group(kf, kDirGroup, nullptr); }, nullptr);
    GKeyFile *kf = open_store();
    if (g_key_file_remove_group(kf, own_group(uri).c_str(), nullptr)) g_key_file_save_to_file(kf, store_path().c_str(), nullptr);
    g_key_file_free(kf);
}

namespace {
void write_group(GKeyFile *kf, const std::string &g, const ViewProps &p)
{
    const char *G = g.c_str();
    g_key_file_set_integer(kf, G, "ViewMode", p.viewMode);
    g_key_file_set_integer(kf, G, "ZoomLevel", p.zoomLevel);
    g_key_file_set_boolean(kf, G, "PreviewsShown", p.previewsShown);
    g_key_file_set_boolean(kf, G, "HiddenFilesShown", p.hiddenFilesShown);
    g_key_file_set_boolean(kf, G, "GroupedSorting", p.groupedSorting);
    g_key_file_set_string(kf, G, "SortRole", p.sortRole.c_str());
    g_key_file_set_boolean(kf, G, "SortDescending", p.sortDescending);
    g_key_file_set_boolean(kf, G, "SortFoldersFirst", p.sortFoldersFirst);
    g_key_file_set_boolean(kf, G, "SortHiddenLast", p.sortHiddenLast);
    std::vector<const char *> roles;
    for (auto &r : p.visibleRoles) roles.push_back(r.c_str());
    g_key_file_set_string_list(kf, G, "VisibleRoles", roles.data(), roles.size());
    std::vector<std::string> widths;
    for (auto &[k, v] : p.columnWidths) widths.push_back(k + ":" + std::to_string(v));
    std::vector<const char *> wp;
    for (auto &w : widths) wp.push_back(w.c_str());
    g_key_file_set_string_list(kf, G, "ColumnWidths", wp.data(), wp.size());
}
} // namespace

void reset_all_view_props()
{
    GKeyFile *kf = open_store();
    gsize n = 0;
    char **groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; ++i) g_key_file_remove_group(kf, groups[i], nullptr);
    g_strfreev(groups);
    // Views saved in .directory files before now stop counting.
    g_key_file_set_int64(kf, "global", "ResetTime", g_get_real_time());
    g_key_file_save_to_file(kf, store_path().c_str(), nullptr);
    g_key_file_free(kf);
}

} // namespace beluga
