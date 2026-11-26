// SPDX-License-Identifier: GPL-2.0-or-later
#include "trash.hpp"

#include "settings.hpp"
#include "util.hpp"

#include <gio/gunixmounts.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace beluga::trash {

std::string home_trash_dir() { return std::string(g_get_user_data_dir()) + "/Trash"; }

namespace {

bool is_dir(const std::string &p)
{
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string uid_str() { return std::to_string(getuid()); }

// Mount point holding path, by walking up until the device changes.
std::string topdir_of(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return {};
    dev_t dev = st.st_dev;
    std::string cur = path;
    while (cur != "/") {
        std::string parent = dirname_of(cur);
        struct stat ps;
        if (stat(parent.c_str(), &ps) != 0 || ps.st_dev != dev) return cur;
        cur = parent;
    }
    return "/";
}

// Trash directory to use for files on the mount at topdir (created on demand).
std::string topdir_trash(const std::string &topdir, bool create)
{
    std::string base = topdir == "/" ? "" : topdir;
    std::string admin = base + "/.Trash";
    struct stat st;
    if (lstat(admin.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX)) {
        std::string d = admin + "/" + uid_str();
        if (is_dir(d)) return d;
        if (create && g_mkdir(d.c_str(), 0700) == 0) return d;
    }
    std::string d = base + "/.Trash-" + uid_str();
    if (is_dir(d)) return d;
    if (create && g_mkdir(d.c_str(), 0700) == 0) return d;
    return {};
}

bool parse_info(const std::string &trashDir, const std::string &infoFile, Entry &e)
{
    GKeyFile *kf = g_key_file_new();
    bool ok = g_key_file_load_from_file(kf, infoFile.c_str(), G_KEY_FILE_NONE, nullptr);
    if (ok) {
        std::string path = take(g_key_file_get_string(kf, "Trash Info", "Path", nullptr));
        std::string date = take(g_key_file_get_string(kf, "Trash Info", "DeletionDate", nullptr));
        std::string unesc = take(g_uri_unescape_string(path.c_str(), nullptr));
        if (unesc.empty()) unesc = path;
        if (!unesc.empty() && unesc[0] != '/') {
            // Relative to the top directory that holds this trash.
            std::string top = trashDir;
            if (ends_with(top, "/.Trash-" + uid_str())) top = dirname_of(top);
            else top = dirname_of(dirname_of(top));
            unesc = (top == "/" ? "" : top) + "/" + unesc;
        }
        e.originalPath = unesc;
        GTimeZone *tz = g_time_zone_new_local();
        GDateTime *dt = g_date_time_new_from_iso8601(date.c_str(), tz);
        g_time_zone_unref(tz);
        if (dt) {
            e.deletionTime = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
        }
        std::string name = basename_of(infoFile);
        name = name.substr(0, name.size() - strlen(".trashinfo"));
        e.infoPath = infoFile;
        e.trashedPath = trashDir + "/files/" + name;
        struct stat st;
        ok = lstat(e.trashedPath.c_str(), &st) == 0;
    }
    g_key_file_free(kf);
    return ok;
}

} // namespace

std::vector<std::string> trash_dirs()
{
    std::vector<std::string> dirs;
    if (is_dir(home_trash_dir())) dirs.push_back(home_trash_dir());
    GList *mounts = g_unix_mount_entries_get(nullptr);
    for (GList *l = mounts; l; l = l->next) {
        auto *m = static_cast<GUnixMountEntry *>(l->data);
        if (!g_unix_mount_entry_is_system_internal(m) || strcmp(g_unix_mount_entry_get_mount_path(m), "/") == 0) {
            std::string mp = g_unix_mount_entry_get_mount_path(m);
            std::string t = topdir_trash(mp, false);
            if (!t.empty() && std::find(dirs.begin(), dirs.end(), t) == dirs.end() && t != home_trash_dir()) dirs.push_back(t);
        }
        g_unix_mount_entry_free(m);
    }
    g_list_free(mounts);
    return dirs;
}

std::vector<Entry> list()
{
    std::vector<Entry> out;
    for (auto &dir : trash_dirs()) {
        std::string infoDir = dir + "/info";
        GDir *d = g_dir_open(infoDir.c_str(), 0, nullptr);
        if (!d) continue;
        while (const char *name = g_dir_read_name(d)) {
            if (!g_str_has_suffix(name, ".trashinfo")) continue;
            Entry e;
            if (parse_info(dir, infoDir + "/" + name, e)) out.push_back(e);
        }
        g_dir_close(d);
    }
    return out;
}

bool is_empty()
{
    for (auto &dir : trash_dirs()) {
        GDir *d = g_dir_open((dir + "/files").c_str(), 0, nullptr);
        if (!d) continue;
        bool any = g_dir_read_name(d) != nullptr;
        g_dir_close(d);
        if (any) return false;
    }
    return true;
}

bool entry_for_trashed_path(const std::string &trashedPath, Entry &out)
{
    std::string filesDir = dirname_of(trashedPath);
    if (basename_of(filesDir) != "files") return false;
    std::string dir = dirname_of(filesDir);
    return parse_info(dir, dir + "/info/" + basename_of(trashedPath) + ".trashinfo", out);
}

bool is_in_trash(const std::string &path)
{
    for (auto &dir : trash_dirs())
        if (starts_with(path, dir + "/files/")) return true;
    return false;
}

// Copies a file, a link or a whole folder (with permissions and times).
static bool copy_tree(GFile *src, GFile *dst, GError **err)
{
    GFileInfo *info = g_file_query_info(src, "standard::type,standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err);
    if (!info) return false;
    bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(info);
    if (!dir) return g_file_copy(src, dst, GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA), nullptr, nullptr, nullptr, err);
    if (!g_file_make_directory(dst, nullptr, err)) return false;
    GFileEnumerator *en = g_file_enumerate_children(src, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err);
    if (!en) return false;
    bool ok = true;
    while (GFileInfo *ci = g_file_enumerator_next_file(en, nullptr, err)) {
        GFile *a = g_file_get_child(src, g_file_info_get_name(ci)), *b = g_file_get_child(dst, g_file_info_get_name(ci));
        ok = copy_tree(a, b, err);
        g_object_unref(a);
        g_object_unref(b);
        g_object_unref(ci);
        if (!ok) break;
    }
    g_object_unref(en);
    if (ok) g_file_copy_attributes(src, dst, G_FILE_COPY_ALL_METADATA, nullptr, nullptr);
    return ok && !(err && *err);
}

bool trash_file(GFile *file, Entry *out, GError **err)
{
    char *p = g_file_get_path(file);
    if (!p) {
        // Remote files: let GIO (gvfs) handle it when it can.
        return g_file_trash(file, nullptr, err);
    }
    std::string path = take(p);
    struct stat st, ht;
    if (lstat(path.c_str(), &st) != 0) {
        g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
        return false;
    }
    std::string trashDir = home_trash_dir();
    g_mkdir_with_parents((trashDir + "/files").c_str(), 0700);
    g_mkdir_with_parents((trashDir + "/info").c_str(), 0700);
    bool relative = false;
    std::string topdir;
    // Another drive: its own trash folder (.Trash-UID at its top), unless the trash is to stay on the home drive.
    bool otherDrive = stat(trashDir.c_str(), &ht) != 0 || ht.st_dev != st.st_dev;
    bool copyHome = otherDrive && settings().trashHomeOnly;
    if (otherDrive && !copyHome) {
        topdir = topdir_of(path);
        trashDir = topdir_trash(topdir, true);
        relative = true;
        if (trashDir.empty()) {
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("This file system has no trash."));
            return false;
        }
        g_mkdir_with_parents((trashDir + "/files").c_str(), 0700);
        g_mkdir_with_parents((trashDir + "/info").c_str(), 0700);
    }

    std::string base = basename_of(path);
    auto [stem, ext] = split_extension(base, S_ISDIR(st.st_mode));
    std::string name = base, infoFile;
    int fd = -1;
    for (int i = 1; i < 10000; ++i) {
        infoFile = trashDir + "/info/" + name + ".trashinfo";
        struct stat tmp;
        if (lstat((trashDir + "/files/" + name).c_str(), &tmp) != 0) {
            fd = g_open(infoFile.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd >= 0) break;
        }
        name = stem + "." + std::to_string(i + 1) + ext;
    }
    if (fd < 0) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, _("Could not create the trash information file."));
        return false;
    }
    std::string stored = path;
    if (relative && topdir != "/") stored = path.substr(topdir.size() + 1);
    else if (relative) stored = path.substr(1);
    std::string escaped = take(g_uri_escape_string(stored.c_str(), "/", FALSE));
    GDateTime *now = g_date_time_new_now_local();
    std::string date = take(g_date_time_format(now, "%Y-%m-%dT%H:%M:%S"));
    g_date_time_unref(now);
    std::string content = "[Trash Info]\nPath=" + escaped + "\nDeletionDate=" + date + "\n";
    bool wrote = write(fd, content.data(), content.size()) == (ssize_t)content.size();
    close(fd);
    std::string dest = trashDir + "/files/" + name;
    if (wrote && copyHome) {
        // Across drives: copy into the home trash, then remove the original.
        GError *e = nullptr;
        Ref<GFile> src(g_file_new_for_path(path.c_str())), dst(g_file_new_for_path(dest.c_str()));
        if (!copy_tree(src.get(), dst.get(), &e) || !delete_recursive(src.get(), nullptr, &e)) {
            g_unlink(infoFile.c_str());
            if (!g_file_query_exists(src.get(), nullptr)) {   // copied, original gone: keep the trashed copy
                g_clear_error(&e);
            } else {
                delete_recursive(dst.get(), nullptr, nullptr);
                g_propagate_error(err, e);
                return false;
            }
        }
    } else if (!wrote || g_rename(path.c_str(), dest.c_str()) != 0) {
        int e = errno;
        g_unlink(infoFile.c_str());
        g_set_error(err, G_IO_ERROR, g_io_error_from_errno(e), "%s", g_strerror(e));
        return false;
    }
    if (out) {
        out->trashedPath = dest;
        out->infoPath = infoFile;
        out->originalPath = path;
        out->deletionTime = g_get_real_time() / G_USEC_PER_SEC;
    }
    return true;
}

bool restore(const Entry &e, const std::string &destOverride, GError **err)
{
    std::string dest = destOverride.empty() ? e.originalPath : destOverride;
    g_mkdir_with_parents(dirname_of(dest).c_str(), 0755);
    struct stat st;
    if (lstat(dest.c_str(), &st) == 0) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_EXISTS, _("“%s” already exists."), dest.c_str());
        return false;
    }
    if (g_rename(e.trashedPath.c_str(), dest.c_str()) != 0) {
        if (errno != EXDEV) {
            g_set_error(err, G_IO_ERROR, g_io_error_from_errno(errno), "%s", g_strerror(errno));
            return false;
        }
        GFile *src = g_file_new_for_path(e.trashedPath.c_str());
        GFile *dst = g_file_new_for_path(dest.c_str());
        bool ok = g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, err);
        g_object_unref(src);
        g_object_unref(dst);
        if (!ok) return false;
    }
    g_unlink(e.infoPath.c_str());
    return true;
}

bool delete_recursive(GFile *f, GCancellable *c, GError **err)
{
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c, nullptr);
    if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c, err);
        if (!en) {
            g_object_unref(info);
            return false;
        }
        GFileInfo *ci;
        bool ok = true;
        while (ok && (ci = g_file_enumerator_next_file(en, c, err))) {
            GFile *child = g_file_get_child(f, g_file_info_get_name(ci));
            ok = delete_recursive(child, c, err);
            g_object_unref(child);
            g_object_unref(ci);
        }
        g_object_unref(en);
        if (!ok || (err && *err)) {
            g_object_unref(info);
            return false;
        }
    }
    if (info) g_object_unref(info);
    return g_file_delete(f, c, err);
}

bool erase(const Entry &e, GError **err)
{
    GFile *f = g_file_new_for_path(e.trashedPath.c_str());
    bool ok = delete_recursive(f, nullptr, err);
    g_object_unref(f);
    if (ok) g_unlink(e.infoPath.c_str());
    return ok;
}

bool empty_all(GError **err)
{
    bool ok = true;
    for (auto &e : list())
        if (!erase(e, ok ? err : nullptr)) ok = false;
    // Orphans without info files.
    for (auto &dir : trash_dirs()) {
        GDir *d = g_dir_open((dir + "/files").c_str(), 0, nullptr);
        if (!d) continue;
        std::vector<std::string> names;
        while (const char *n = g_dir_read_name(d)) names.push_back(n);
        g_dir_close(d);
        for (auto &n : names) {
            GFile *f = g_file_new_for_path((dir + "/files/" + n).c_str());
            delete_recursive(f, nullptr, nullptr);
            g_object_unref(f);
        }
        g_unlink((dir + "/directorysizes").c_str());
    }
    return ok;
}

guint64 size_of_path(const std::string &path, GCancellable *c)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (guint64)st.st_size;
    guint64 total = 0;
    GDir *d = g_dir_open(path.c_str(), 0, nullptr);
    if (!d) return 0;
    while (const char *n = g_dir_read_name(d)) {
        if (c && g_cancellable_is_cancelled(c)) break;
        total += size_of_path(path + "/" + n, c);
    }
    g_dir_close(d);
    return total;
}

guint64 home_trash_size() { return size_of_path(home_trash_dir() + "/files"); }

std::string apply_limits()
{
    const Settings &s = settings();
    if (s.trashUseTimeLimit && s.trashDays > 0) {
        gint64 limit = g_get_real_time() / G_USEC_PER_SEC - (gint64)s.trashDays * 86400;
        for (auto &e : list())
            if (e.deletionTime > 0 && e.deletionTime < limit) erase(e, nullptr);
    }
    if (!s.trashUseSizeLimit) return {};
    guint64 total = 0, free = 0;
    GFile *f = g_file_new_for_path(home_trash_dir().c_str());
    bool known = g_file_query_exists(f, nullptr) && filesystem_space(f, total, free);
    g_object_unref(f);
    if (!known) return {};
    guint64 limit = (guint64)(total * (s.trashPercent / 100.0));
    guint64 size = home_trash_size();
    if (size <= limit) return {};
    if (s.trashLimitAction == TrashLimitAction::Warn)
        return printf_str(_("The trash is larger than its limit of %s. Consider emptying it."), format_size(limit).c_str());
    std::vector<std::pair<Entry, guint64>> entries;
    for (auto &e : list())
        if (starts_with(e.trashedPath, home_trash_dir())) entries.push_back({e, size_of_path(e.trashedPath)});
    if (s.trashLimitAction == TrashLimitAction::DeleteOldest)
        std::sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.first.deletionTime < b.first.deletionTime; });
    else
        std::sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &[e, sz] : entries) {
        if (size <= limit) break;
        if (erase(e, nullptr)) size -= std::min(size, sz);
    }
    return {};
}

} // namespace beluga::trash
