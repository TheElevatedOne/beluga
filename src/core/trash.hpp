// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: freedesktop.org trash, implemented directly on the file system so
// it works without gvfs. Also applies the trash limits that used to be set in KDE's
// trash settings module (maximum age, maximum size).
#pragma once

#include <gio/gio.h>

#include <string>
#include <vector>

namespace beluga::trash {

struct Entry {
    std::string trashedPath;     // .../Trash/files/NAME
    std::string infoPath;        // .../Trash/info/NAME.trashinfo
    std::string originalPath;    // where it came from
    gint64 deletionTime = 0;     // unix time
};

std::string home_trash_dir();            // ~/.local/share/Trash
std::vector<std::string> trash_dirs();   // existing trash directories
std::vector<Entry> list();
bool is_empty();
// Entry for a path inside a trash "files" directory (top-level item only).
bool entry_for_trashed_path(const std::string &trashedPath, Entry &out);
bool is_in_trash(const std::string &path);

// Moves a local file into the trash. Fills out for undo.
bool trash_file(GFile *file, Entry *out, GError **err);
// Puts the item back; dest overrides the original location when not empty.
bool restore(const Entry &e, const std::string &dest, GError **err);
bool erase(const Entry &e, GError **err);
bool empty_all(GError **err);

guint64 size_of_path(const std::string &path, GCancellable *c = nullptr);
guint64 home_trash_size();

// Applies the configured age and size limits. Returns a warning text when the size
// limit is exceeded and the configured action is "warn", else "".
std::string apply_limits();

// Removes a file or folder tree permanently.
bool delete_recursive(GFile *f, GCancellable *c, GError **err);

} // namespace beluga::trash
