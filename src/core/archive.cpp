// SPDX-License-Identifier: GPL-2.0-or-later
#include "archive.hpp"

#include "util.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace beluga::archive {

const char *const kScheme = "beluga-archive";

namespace {

const std::string kPrefix = std::string("beluga-archive") + "://";

// "./a//b/" → "a/b"; empty with ok = false for ".." anywhere.
std::string normalize(const char *path, bool &ok)
{
    ok = true;
    std::string out;
    for (auto &part : split(path ? path : "", '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            ok = false;
            return {};
        }
        if (!out.empty()) out += '/';
        out += part;
    }
    return out;
}

std::string parent_of(const std::string &p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string name_of(const std::string &p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

struct archive *open_archive(const std::string &path, std::string *error)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        if (error) *error = archive_error_string(a) ? archive_error_string(a) : _("The archive could not be opened.");
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

const char *entry_path(struct archive_entry *e)
{
    const char *p = archive_entry_pathname_utf8(e);
    return p ? p : archive_entry_pathname(e);
}

struct Cached {
    std::shared_ptr<const Index> index;
    gint64 mtime = 0;
    gint64 size = 0;
    gint64 used = 0;
};
std::mutex g_cacheMutex;
std::map<std::string, Cached> g_cache;

const char *const kTypes[] = {
    "application/zip", "application/x-zip-compressed", "application/x-tar", "application/x-compressed-tar",
    "application/x-bzip-compressed-tar", "application/x-bzip2-compressed-tar", "application/x-xz-compressed-tar",
    "application/x-lzma-compressed-tar", "application/x-zstd-compressed-tar", "application/x-lzip-compressed-tar",
    "application/x-lz4-compressed-tar", "application/x-tarz", "application/x-7z-compressed", "application/vnd.rar",
    "application/x-rar", "application/x-rar-compressed", "application/x-cd-image", "application/x-iso9660-image",
    "application/x-cpio", nullptr,
};
const char *const kExtensions[] = {".zip", ".tar", ".tgz", ".tar.gz", ".tbz", ".tbz2", ".tar.bz2", ".txz", ".tar.xz",
                                   ".tar.zst", ".tzst", ".tar.lz", ".tar.lzma", ".tar.lz4", ".7z", ".rar", ".iso",
                                   ".cpio", nullptr};

} // namespace

bool is_archive_uri(const std::string &uri) { return starts_with(uri, kPrefix); }

std::string make_uri(const std::string &archivePath, const std::string &inner)
{
    return kPrefix + take(g_uri_escape_string(archivePath.c_str(), nullptr, FALSE)) + "/"
         + take(g_uri_escape_string(inner.c_str(), "/", FALSE));
}

bool parse_uri(const std::string &uri, std::string &archivePath, std::string &inner)
{
    if (!is_archive_uri(uri)) return false;
    std::string rest = uri.substr(kPrefix.size());
    size_t slash = rest.find('/');
    archivePath = take(g_uri_unescape_string(rest.substr(0, slash).c_str(), nullptr));
    inner = slash == std::string::npos ? std::string() : take(g_uri_unescape_string(rest.substr(slash + 1).c_str(), nullptr));
    while (!inner.empty() && inner.back() == '/') inner.pop_back();
    return !archivePath.empty() && archivePath[0] == '/';
}

std::string display_path(const std::string &uri)
{
    std::string a, in;
    if (!parse_uri(uri, a, in)) return uri;
    return in.empty() ? a : a + "/" + in;
}

std::string parent_uri(const std::string &uri)
{
    std::string a, in;
    if (!parse_uri(uri, a, in)) return {};
    if (in.empty()) return take(g_filename_to_uri(dirname_of(a).c_str(), nullptr, nullptr));
    return make_uri(a, parent_of(in));
}

bool can_browse(const std::string &contentType, const std::string &name)
{
    for (int i = 0; kTypes[i]; ++i)
        if (contentType == kTypes[i]) return true;
    std::string lower = casefold(name);
    for (int i = 0; kExtensions[i]; ++i)
        if (ends_with(lower, kExtensions[i]) && lower.size() > strlen(kExtensions[i])) return true;
    return false;
}

std::string resolve_path(const std::string &path)
{
    std::string p = path;
    while (!p.empty() && p != "/") {
        struct stat st;
        if (lstat(p.c_str(), &st) == 0) {
            if (!S_ISREG(st.st_mode) || p.size() >= path.size()) return {};
            std::string name = basename_of(p);
            std::string ct = take(g_content_type_guess(name.c_str(), nullptr, 0, nullptr));
            if (!can_browse(ct, name)) return {};
            return make_uri(p, path.substr(p.size() + 1));
        }
        p = dirname_of(p);
    }
    return {};
}

const std::vector<Entry> *Index::children(const std::string &folder) const
{
    auto it = folders.find(folder);
    return it == folders.end() ? nullptr : &it->second;
}

std::shared_ptr<const Index> load(const std::string &archivePath, std::string *error)
{
    struct stat st;
    if (stat(archivePath.c_str(), &st) != 0) {
        if (error) *error = g_strerror(errno);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        auto it = g_cache.find(archivePath);
        if (it != g_cache.end() && it->second.mtime == (gint64)st.st_mtime && it->second.size == (gint64)st.st_size) {
            it->second.used = g_get_monotonic_time();
            return it->second.index;
        }
    }
    struct archive *a = open_archive(archivePath, error);
    if (!a) return nullptr;

    // Folder → name → entry while reading, so that folders listed after their contents (or not at all) merge.
    std::map<std::string, std::map<std::string, Entry>> tmp;
    tmp[""];
    auto ensureFolder = [&](const std::string &folder) {
        std::string cur;
        for (auto &part : split(folder, '/')) {
            std::string next = cur.empty() ? part : cur + "/" + part;
            auto &slot = tmp[cur][part];
            if (!slot.dir) {
                slot = Entry{};
                slot.name = part;
                slot.dir = true;
            }
            tmp[next];
            cur = next;
        }
    };
    auto index = std::make_shared<Index>();
    struct archive_entry *e;
    int r;
    int entries = 0;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        bool ok = true;
        std::string path = normalize(entry_path(e), ok);
        if (ok && !path.empty()) {
            entries++;
            Entry en;
            en.name = name_of(path);
            mode_t type = archive_entry_filetype(e);
            en.dir = type == AE_IFDIR;
            en.link = type == AE_IFLNK;
            en.size = archive_entry_size_is_set(e) ? (guint64)std::max<la_int64_t>(0, archive_entry_size(e)) : 0;
            en.mtime = archive_entry_mtime_is_set(e) ? (gint64)archive_entry_mtime(e) : 0;
            en.mode = (guint32)archive_entry_perm(e);
            en.encrypted = archive_entry_is_encrypted(e);
            if (en.encrypted) index->encrypted = true;
            if (en.link) {
                const char *t = archive_entry_symlink_utf8(e);
                en.linkTarget = t ? t : str(archive_entry_symlink(e));
            }
            std::string parent = parent_of(path);
            ensureFolder(parent);
            if (en.dir) tmp[path];
            tmp[parent][en.name] = en;
        }
        archive_read_data_skip(a);
    }
    if (r != ARCHIVE_EOF && entries == 0) {
        if (error) *error = archive_error_string(a) ? archive_error_string(a) : _("The archive could not be read.");
        archive_read_free(a);
        return nullptr;
    }
    archive_read_free(a);
    for (auto &[folder, names] : tmp) {
        auto &v = index->folders[folder];
        v.reserve(names.size());
        for (auto &[name, en] : names) v.push_back(en);
    }
    std::lock_guard<std::mutex> l(g_cacheMutex);
    if (g_cache.size() >= 8) {
        auto oldest = std::min_element(g_cache.begin(), g_cache.end(), [](auto &x, auto &y) { return x.second.used < y.second.used; });
        g_cache.erase(oldest);
    }
    g_cache[archivePath] = {index, (gint64)st.st_mtime, (gint64)st.st_size, g_get_monotonic_time()};
    return index;
}

bool extract(const std::string &archivePath, const std::vector<std::string> &inner, const std::string &destDir,
             const std::atomic<bool> *cancel, std::string *error)
{
    std::vector<std::string> sels;
    for (auto &i : inner) {
        bool ok = true;
        sels.push_back(normalize(i.c_str(), ok));
        if (!ok) {
            if (error) *error = _("The archive could not be read.");
            return false;
        }
    }
    struct archive *a = open_archive(archivePath, error);
    if (!a) return false;
    std::vector<std::pair<std::string, std::string>> links;   // made last, so nothing is written through them
    std::map<std::string, std::string> written;               // entry → extracted file, for hard links
    struct archive_entry *e;
    int r;
    bool failed = false;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (cancel && *cancel) {
            if (error) *error = _("Cancelled.");
            failed = true;
            break;
        }
        bool ok = true;
        std::string path = normalize(entry_path(e), ok);
        const std::string *sel = nullptr;
        if (ok && !path.empty())
            for (auto &s : sels)
                if (s.empty() || path == s || starts_with(path, s + "/")) sel = &s;
        if (!sel) {
            archive_read_data_skip(a);
            continue;
        }
        std::string base = parent_of(*sel);
        std::string rel = base.empty() ? path : path.substr(base.size() + 1);
        std::string target = destDir + "/" + rel;
        g_mkdir_with_parents(dirname_of(target).c_str(), 0755);
        mode_t type = archive_entry_filetype(e);
        if (type == AE_IFDIR) {
            g_mkdir_with_parents(target.c_str(), 0755);
        } else if (type == AE_IFLNK) {
            const char *t = archive_entry_symlink_utf8(e);
            links.push_back({target, t ? t : str(archive_entry_symlink(e))});
        } else if (type == AE_IFREG) {
            const char *hard = archive_entry_hardlink_utf8(e);
            if (!hard) hard = archive_entry_hardlink(e);
            if (hard) {
                bool hok = true;
                auto src = written.find(normalize(hard, hok));
                if (src == written.end() || link(src->second.c_str(), target.c_str()) != 0) {
                    int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
                    if (fd >= 0) close(fd);
                }
                archive_read_data_skip(a);
                continue;
            }
            int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0) {
                if (error) *error = printf_str("“%s”: %s", rel.c_str(), g_strerror(errno));
                failed = true;
                break;
            }
            const void *buf;
            size_t len;
            la_int64_t off;
            int dr;
            while ((dr = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK || dr == ARCHIVE_WARN) {
                if (cancel && *cancel) break;
                if (lseek(fd, off, SEEK_SET) < 0) break;
                const char *p = static_cast<const char *>(buf);
                while (len > 0) {
                    ssize_t w = write(fd, p, len);
                    if (w <= 0) break;
                    p += w;
                    len -= (size_t)w;
                }
                if (len > 0) break;
            }
            close(fd);
            if (dr != ARCHIVE_EOF) {
                if (error) {
                    const char *msg = archive_error_string(a);
                    *error = archive_entry_is_encrypted(e) ? std::string(_("This file is encrypted with a password."))
                           : cancel && *cancel           ? std::string(_("Cancelled."))
                           : msg                         ? printf_str("“%s”: %s", rel.c_str(), msg)
                                                         : printf_str(_("“%s” could not be read."), rel.c_str());
                }
                g_unlink(target.c_str());
                failed = true;
                break;
            }
            guint32 perm = (guint32)archive_entry_perm(e) & 0777;
            g_chmod(target.c_str(), (perm ? perm : 0644) | 0600);
            if (archive_entry_mtime_is_set(e)) {
                struct timeval tv[2] = {{(time_t)archive_entry_mtime(e), 0}, {(time_t)archive_entry_mtime(e), 0}};
                utimes(target.c_str(), tv);
            }
            written[path] = target;
        } else {
            archive_read_data_skip(a);   // devices, pipes: not extracted
        }
    }
    if (!failed && r != ARCHIVE_EOF && r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        if (error) *error = archive_error_string(a) ? archive_error_string(a) : _("The archive could not be read.");
        failed = true;
    }
    archive_read_free(a);
    if (failed) return false;
    for (auto &[target, to] : links) {
        if (symlink(to.c_str(), target.c_str()) != 0) { /* an existing name: keep the file */ }
    }
    return true;
}

std::string extract_to_cache(const std::string &uri, std::string *error)
{
    std::string archivePath, inner;
    if (!parse_uri(uri, archivePath, inner) || inner.empty()) {
        if (error) *error = _("This is not a file in an archive.");
        return {};
    }
    struct stat st;
    if (stat(archivePath.c_str(), &st) != 0) {
        if (error) *error = g_strerror(errno);
        return {};
    }
    std::string key = take(g_compute_checksum_for_string(G_CHECKSUM_SHA1,
                                                         printf_str("%s\n%lld\n%lld", archivePath.c_str(), (long long)st.st_mtime,
                                                                    (long long)st.st_size).c_str(), -1));
    std::string dir = std::string(g_get_user_cache_dir()) + "/beluga/archives/" + key;
    std::string target = dir + "/" + inner;
    if (g_file_test(target.c_str(), G_FILE_TEST_IS_REGULAR)) return target;
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::string tmpl = dir + "/.extract-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!g_mkdtemp(buf.data())) {
        if (error) *error = g_strerror(errno);
        return {};
    }
    std::string tmp = buf.data();
    bool ok = extract(archivePath, {inner}, tmp, nullptr, error);
    std::string got = tmp + "/" + basename_of(inner);
    if (ok) {
        g_mkdir_with_parents(dirname_of(target).c_str(), 0700);
        if (g_rename(got.c_str(), target.c_str()) != 0) {
            if (error) *error = g_strerror(errno);
            ok = false;
        } else {
            // A copy to look at: changes are not written back into the archive.
            struct stat ts;
            if (stat(target.c_str(), &ts) == 0) g_chmod(target.c_str(), ts.st_mode & 0555);
        }
    }
    g_unlink(got.c_str());
    g_rmdir(tmp.c_str());
    return ok ? target : std::string();
}

} // namespace beluga::archive
