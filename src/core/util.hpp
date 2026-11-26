// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: small helpers shared by the core and the GUI. Strings, sizes,
// dates, permissions, paths and RAII wrappers for GLib objects.
#pragma once

#include <gio/gio.h>
#include <glib/gi18n.h>

#include <string>
#include <utility>
#include <vector>

namespace beluga {

// ---- RAII for GObject / g_free ------------------------------------------------

template <typename T>
class Ref {
public:
    Ref() = default;
    explicit Ref(T *p) : p_(p) {}                       // adopts a reference
    Ref(const Ref &o) : p_(o.p_) { if (p_) g_object_ref(p_); }
    Ref(Ref &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ref() { if (p_) g_object_unref(p_); }
    Ref &operator=(const Ref &o) { Ref t(o); std::swap(p_, t.p_); return *this; }
    Ref &operator=(Ref &&o) noexcept { std::swap(p_, o.p_); return *this; }
    static Ref share(T *p) { if (p) g_object_ref(p); return Ref(p); }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T *release() { T *t = p_; p_ = nullptr; return t; }
    void reset(T *p = nullptr) { if (p_) g_object_unref(p_); p_ = p; }
private:
    T *p_ = nullptr;
};

// Takes ownership of a g_malloc'd string and converts it.
std::string take(char *s);
std::string str(const char *s);

// ---- strings -------------------------------------------------------------------

bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);
std::string trim(const std::string &s);
std::string casefold(const std::string &s);
std::vector<std::string> split(const std::string &s, char sep, bool skipEmpty = true);
std::string join(const std::vector<std::string> &v, const std::string &sep);
std::string replace_all(std::string s, const std::string &from, const std::string &to);
bool contains_ci(const std::string &haystack, const std::string &needle);
std::string shell_quote(const std::string &s);
std::string markup_escape(const std::string &s);
std::string printf_str(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

// Wildcard filter like Dolphin's filter bar: "*.txt", "foo", "a?c". Without
// wildcards the pattern matches as a case-insensitive substring.
bool filter_match(const std::string &pattern, const std::string &name);

// ---- formatting ----------------------------------------------------------------

std::string format_size(guint64 bytes);
std::string format_items(int count);                    // "3 items"
std::string format_files_folders(int files, int folders);
enum class DateStyle { Relative, Short, Long };
std::string format_date(gint64 unixTime, DateStyle style);
// Permissions format: 0 symbolic, 1 numeric, 2 combined.
std::string format_permissions(guint32 mode, int format);
std::string format_duration(double seconds);

// ---- paths and files ------------------------------------------------------------

std::string home_dir();
std::string user_dir(GUserDirectory which);            // "" when not set
std::string config_dir();                              // ~/.config/beluga
std::string data_dir();                                // ~/.local/share/beluga
std::string state_dir();                               // ~/.local/state/beluga
void ensure_dir(const std::string &path);
std::string basename_of(const std::string &path);
std::string dirname_of(const std::string &path);

// Location helpers. Virtual locations: trash:///, search:..., recent:///
bool is_trash_uri(const std::string &uri);
bool is_search_uri(const std::string &uri);
bool is_recent_uri(const std::string &uri);
bool is_virtual_uri(const std::string &uri);
std::string file_uri(GFile *f);
std::string display_location(GFile *f);               // path for local files, URI otherwise
std::string display_location(const std::string &uri);
GFile *file_for_input(const std::string &text, GFile *base); // ~, relative, URIs
// A folder's own icon from its .directory file (Dolphin's [Desktop Entry] Icon=): an icon name or an image path;
// empty when it has none. set_folder_icon writes it (empty removes it).
std::string folder_icon(const std::string &dirPath);
bool set_folder_icon(const std::string &dirPath, const std::string &icon, GError **err);
GIcon *icon_from_string(const std::string &icon);

// "name.tar.gz" -> {"name", ".tar.gz"}; folders have no extension.
std::pair<std::string, std::string> split_extension(const std::string &name, bool isDir = false);
// A free name in dir like "name (2).ext".
std::string unique_name(GFile *dir, const std::string &name);
// "Copy" naming used by Duplicate Here: "name copy.ext", "name copy 2.ext".
std::string duplicate_name(GFile *dir, const std::string &name, bool isDir);

// Natural, case-insensitive filename ordering key (g_utf8_collate_key_for_filename).
std::string collate_key(const std::string &name, int sortingChoice);

bool is_executable_file(GFileInfo *info);
bool is_desktop_file(GFileInfo *info);
std::string content_type_of(GFileInfo *info);

// Disk space of the filesystem holding f: returns false when unknown.
bool filesystem_space(GFile *f, guint64 &total, guint64 &free);

// Launches a command line (shell-style) in a working directory.
bool spawn_command(const std::string &cmdline, const std::string &workdir, GError **err);

} // namespace beluga
