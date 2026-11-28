// SPDX-License-Identifier: GPL-2.0-or-later
#include "util.hpp"

#include "archive.hpp"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <ctime>

namespace beluga {

std::string take(char *s)
{
    if (!s) return {};
    std::string r(s);
    g_free(s);
    return r;
}

std::string str(const char *s) { return s ? std::string(s) : std::string(); }

bool starts_with(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string casefold(const std::string &s)
{
    return take(g_utf8_casefold(s.c_str(), -1));
}

std::vector<std::string> split(const std::string &s, char sep, bool skipEmpty)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) pos = s.size();
        std::string part = s.substr(start, pos - start);
        if (!skipEmpty || !part.empty()) out.push_back(part);
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string> &v, const std::string &sep)
{
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += sep;
        r += v[i];
    }
    return r;
}

std::string replace_all(std::string s, const std::string &from, const std::string &to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool contains_ci(const std::string &haystack, const std::string &needle)
{
    if (needle.empty()) return true;
    return casefold(haystack).find(casefold(needle)) != std::string::npos;
}

std::string shell_quote(const std::string &s) { return take(g_shell_quote(s.c_str())); }

std::string markup_escape(const std::string &s) { return take(g_markup_escape_text(s.c_str(), -1)); }

std::string printf_str(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return take(s);
}

bool filter_match(const std::string &pattern, const std::string &name)
{
    if (pattern.empty()) return true;
    if (pattern.find_first_of("*?[") == std::string::npos) return contains_ci(name, pattern);
    std::string p = casefold(pattern);
    std::string n = casefold(name);
    GPatternSpec *spec = g_pattern_spec_new(p.c_str());
    bool ok = g_pattern_spec_match_string(spec, n.c_str());
    g_pattern_spec_free(spec);
    return ok;
}

std::string format_size(guint64 bytes)
{
    return take(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
}

std::string format_items(int count)
{
    return printf_str(g_dngettext(GETTEXT_PACKAGE, "%d item", "%d items", count), count);
}

std::string format_files_folders(int files, int folders)
{
    std::string f = printf_str(g_dngettext(GETTEXT_PACKAGE, "%d file", "%d files", files), files);
    std::string d = printf_str(g_dngettext(GETTEXT_PACKAGE, "%d folder", "%d folders", folders), folders);
    if (folders == 0) return f;
    if (files == 0) return d;
    return printf_str(_("%s, %s"), d.c_str(), f.c_str());
}

std::string format_date(gint64 unixTime, DateStyle style)
{
    if (unixTime <= 0) return {};
    GDateTime *dt = g_date_time_new_from_unix_local(unixTime);
    if (!dt) return {};
    std::string out;
    if (style == DateStyle::Long) {
        out = take(g_date_time_format(dt, "%A, %-e %B %Y %H:%M:%S"));   // %-e: no space before 1-9
    } else if (style == DateStyle::Short) {
        out = take(g_date_time_format(dt, "%x %H:%M"));
    } else {
        GDateTime *now = g_date_time_new_now_local();
        int y1, m1, d1, y2, m2, d2;
        g_date_time_get_ymd(dt, &y1, &m1, &d1);
        g_date_time_get_ymd(now, &y2, &m2, &d2);
        GDate *a = g_date_new_dmy(d1, (GDateMonth)m1, y1);
        GDate *b = g_date_new_dmy(d2, (GDateMonth)m2, y2);
        int days = g_date_days_between(a, b);
        g_date_free(a);
        g_date_free(b);
        std::string time = take(g_date_time_format(dt, "%H:%M"));
        if (days == 0) out = printf_str(_("Today at %s"), time.c_str());
        else if (days == 1) out = printf_str(_("Yesterday at %s"), time.c_str());
        else if (days > 1 && days < 7) out = take(g_date_time_format(dt, _("%A at %H:%M")));
        else out = take(g_date_time_format(dt, "%x %H:%M"));
        g_date_time_unref(now);
    }
    g_date_time_unref(dt);
    return out;
}

std::string format_permissions(guint32 mode, int format)
{
    char sym[11];
    sym[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b'
           : S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) sym[i + 1] = (mode & (1u << (8 - i))) ? rwx[i] : '-';
    if (mode & S_ISUID) sym[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) sym[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) sym[9] = (mode & S_IXOTH) ? 't' : 'T';
    sym[10] = 0;
    std::string num = printf_str("%04o", mode & 07777);
    switch (format) {
    case 1: return num;
    case 2: return std::string(sym) + " (" + num + ")";
    default: return sym;
    }
}

std::string format_duration(double seconds)
{
    if (seconds < 0) return {};
    long s = (long)(seconds + 0.5);
    if (s < 60) return printf_str(_("%ld s"), s);
    if (s < 3600) return printf_str(_("%ld min %ld s"), s / 60, s % 60);
    return printf_str(_("%ld h %ld min"), s / 3600, (s % 3600) / 60);
}

std::string home_dir() { return str(g_get_home_dir()); }

std::string user_dir(GUserDirectory which)
{
    const char *d = g_get_user_special_dir(which);
    if (!d) return {};
    std::string s(d);
    if (s == home_dir()) return {};
    return s;
}

static std::string app_subdir(const char *base)
{
    std::string d = std::string(base) + "/beluga";
    ensure_dir(d);
    return d;
}

std::string config_dir() { return app_subdir(g_get_user_config_dir()); }
std::string data_dir() { return app_subdir(g_get_user_data_dir()); }
std::string state_dir() { return app_subdir(g_get_user_state_dir()); }

void ensure_dir(const std::string &path) { g_mkdir_with_parents(path.c_str(), 0700); }

std::string basename_of(const std::string &path) { return take(g_path_get_basename(path.c_str())); }
std::string dirname_of(const std::string &path) { return take(g_path_get_dirname(path.c_str())); }

bool is_trash_uri(const std::string &uri) { return starts_with(uri, "trash:"); }
bool is_search_uri(const std::string &uri) { return starts_with(uri, "search:"); }
bool is_recent_uri(const std::string &uri) { return starts_with(uri, "recent:"); }
bool is_virtual_uri(const std::string &uri)
{
    return is_trash_uri(uri) || is_search_uri(uri) || is_recent_uri(uri) || archive::is_archive_uri(uri);
}

std::string file_uri(GFile *f) { return f ? take(g_file_get_uri(f)) : std::string(); }

std::string display_location(GFile *f)
{
    if (!f) return {};
    if (g_file_has_uri_scheme(f, archive::kScheme)) return archive::display_path(file_uri(f));
    // Remote files have a path too (gvfs's FUSE folder, /run/user/…/gvfs/ftp:host=…); show their address.
    if (g_file_is_native(f)) {
        char *p = g_file_get_path(f);
        if (p) return take(p);
    }
    return take(g_file_get_parse_name(f));
}

std::string display_location(const std::string &uri)
{
    if (archive::is_archive_uri(uri)) return archive::display_path(uri);
    if (is_virtual_uri(uri)) return uri;
    GFile *f = g_file_new_for_uri(uri.c_str());
    std::string s = display_location(f);
    g_object_unref(f);
    return s;
}

GFile *file_for_input(const std::string &input, GFile *base)
{
    std::string text = trim(input);
    if (text.empty()) return nullptr;
    if (text == "~") return g_file_new_for_path(g_get_home_dir());
    if (starts_with(text, "~/")) return g_file_new_for_path((home_dir() + text.substr(1)).c_str());
    if (text[0] == '/') return g_file_new_for_path(text.c_str());
    char *scheme = g_uri_parse_scheme(text.c_str());
    if (scheme) {
        g_free(scheme);
        return g_file_new_for_uri(text.c_str());
    }
    if (base) return g_file_resolve_relative_path(base, text.c_str());
    return g_file_new_for_path(text.c_str());
}

std::pair<std::string, std::string> split_extension(const std::string &name, bool isDir)
{
    if (isDir) return {name, {}};
    static const char *doubleExt[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.Z", ".tar.lzma", nullptr};
    for (int i = 0; doubleExt[i]; ++i) {
        std::string e = doubleExt[i];
        if (name.size() > e.size() && ends_with(name, e)) return {name.substr(0, name.size() - e.size()), e};
    }
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {name, {}};
    return {name.substr(0, dot), name.substr(dot)};
}

static bool child_exists(GFile *dir, const std::string &name)
{
    GFile *c = g_file_get_child(dir, name.c_str());
    bool e = g_file_query_exists(c, nullptr);
    g_object_unref(c);
    return e;
}

std::string unique_name(GFile *dir, const std::string &name)
{
    if (!child_exists(dir, name)) return name;
    auto [stem, ext] = split_extension(name);
    // Continue an existing "(N)" counter.
    int start = 1;
    size_t open = stem.rfind(" (");
    if (open != std::string::npos && ends_with(stem, ")")) {
        std::string num = stem.substr(open + 2, stem.size() - open - 3);
        if (!num.empty() && std::all_of(num.begin(), num.end(), ::isdigit)) {
            start = std::atoi(num.c_str()) + 1;
            stem = stem.substr(0, open);
        }
    }
    for (int i = start; i < 100000; ++i) {
        std::string cand = printf_str("%s (%d)%s", stem.c_str(), i, ext.c_str());
        if (!child_exists(dir, cand)) return cand;
    }
    return name;
}

std::string duplicate_name(GFile *dir, const std::string &name, bool isDir)
{
    auto [stem, ext] = split_extension(name, isDir);
    std::string cand = printf_str(_("%s copy"), stem.c_str()) + ext;
    for (int i = 2; child_exists(dir, cand) && i < 100000; ++i)
        cand = printf_str(_("%s copy %d"), stem.c_str(), i) + ext;
    return cand;
}

std::string collate_key(const std::string &name, int sortingChoice)
{
    switch (sortingChoice) {
    case 2: return name; // case sensitive, byte order
    case 1: return take(g_utf8_collate_key(casefold(name).c_str(), -1));
    default: return take(g_utf8_collate_key_for_filename(casefold(name).c_str(), -1));
    }
}

goffset info_size(GFileInfo *info)
{
    return info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ? g_file_info_get_size(info) : 0;
}

bool info_is_symlink(GFileInfo *info)
{
    return info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) && g_file_info_get_is_symlink(info);
}

std::string content_type_of(GFileInfo *info)
{
    if (!info) return {};
    const char *ct = nullptr;
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
        ct = g_file_info_get_content_type(info);
    if (!ct && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE))
        ct = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    return str(ct);
}

bool is_executable_file(GFileInfo *info)
{
    if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) return false;
    if (!g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)) return false;
    std::string ct = content_type_of(info);
    return g_content_type_can_be_executable(ct.c_str());
}

bool is_desktop_file(GFileInfo *info)
{
    return content_type_of(info) == "application/x-desktop";
}

bool filesystem_space(GFile *f, guint64 &total, guint64 &free)
{
    GFileInfo *info = g_file_query_filesystem_info(f, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE, nullptr, nullptr);
    if (!info) return false;
    bool ok = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    free = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    g_object_unref(info);
    return ok && total > 0;
}

bool spawn_command(const std::string &cmdline, const std::string &workdir, GError **err)
{
    int argc = 0;
    char **argv = nullptr;
    if (!g_shell_parse_argv(cmdline.c_str(), &argc, &argv, err)) return false;
    bool ok = g_spawn_async(workdir.empty() ? nullptr : workdir.c_str(), argv, nullptr,
                            GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL), nullptr, nullptr, nullptr, err);
    g_strfreev(argv);
    return ok;
}

std::string folder_icon(const std::string &dirPath)
{
    std::string file = dirPath + "/.directory";
    if (!g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR)) return {};
    GKeyFile *kf = g_key_file_new();
    std::string icon;
    if (g_key_file_load_from_file(kf, file.c_str(), G_KEY_FILE_NONE, nullptr))
        icon = take(g_key_file_get_string(kf, "Desktop Entry", "Icon", nullptr));
    g_key_file_free(kf);
    // A relative image path is relative to the folder.
    if (!icon.empty() && icon[0] != '/' && icon.find('/') != std::string::npos) icon = dirPath + "/" + icon;
    return icon;
}

bool edit_directory_file(const std::string &dirPath, const std::function<void(GKeyFile *)> &fn, GError **err)
{
    std::string file = dirPath + "/.directory";
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, file.c_str(), GKeyFileFlags(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS), nullptr);
    fn(kf);
    // Drop groups that ended up empty.
    gsize groups = 0;
    char **g = g_key_file_get_groups(kf, &groups);
    for (gsize i = 0; i < groups; ++i) {
        gsize n = 0;
        char **keys = g_key_file_get_keys(kf, g[i], &n, nullptr);
        g_strfreev(keys);
        if (n == 0) g_key_file_remove_group(kf, g[i], nullptr);
    }
    g_strfreev(g);
    g = g_key_file_get_groups(kf, &groups);
    g_strfreev(g);
    bool ok;
    if (groups == 0) ok = g_unlink(file.c_str()) == 0 || errno == ENOENT;   // nothing left: no file
    else ok = g_key_file_save_to_file(kf, file.c_str(), err);
    g_key_file_free(kf);
    return ok;
}

bool set_folder_icon(const std::string &dirPath, const std::string &icon, GError **err)
{
    return edit_directory_file(dirPath, [&](GKeyFile *kf) {
        if (icon.empty()) g_key_file_remove_key(kf, "Desktop Entry", "Icon", nullptr);
        else g_key_file_set_string(kf, "Desktop Entry", "Icon", icon.c_str());
    }, err);
}

GIcon *icon_from_string(const std::string &icon)
{
    if (icon.empty()) return nullptr;
    if (icon[0] == '/') {
        GFile *f = g_file_new_for_path(icon.c_str());
        GIcon *i = g_file_icon_new(f);
        g_object_unref(f);
        return i;
    }
    return g_themed_icon_new_with_default_fallbacks(icon.c_str());
}

} // namespace beluga
