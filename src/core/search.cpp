// SPDX-License-Identifier: GPL-2.0-or-later
#include "search.hpp"
#include "log.hpp"

#include "fileindex.hpp"
#include "fileops.hpp"
#include "walker.hpp"

#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace beluga {

const char *kItemAttributes =
    "standard::type,standard::name,standard::display-name,standard::size,standard::allocated-size,"
    "standard::icon,standard::symbolic-icon,standard::content-type,standard::fast-content-type,"
    "standard::is-hidden,standard::is-backup,standard::is-symlink,standard::symlink-target,"
    "standard::target-uri,standard::sort-order,"
    "time::modified,time::created,time::access,"
    "unix::mode,unix::uid,unix::gid,unix::nlink,owner::user,owner::group,"
    "access::can-read,access::can-write,access::can-execute,access::can-delete,access::can-trash,access::can-rename,"
    "id::file,id::filesystem,filesystem::readonly,mountable::*,thumbnail::path,thumbnail::failed,thumbnail::is-valid";

static std::string esc(const std::string &s) { return take(g_uri_escape_string(s.c_str(), nullptr, TRUE)); }

std::string SearchQuery::toUri() const
{
    std::string u = "search:?q=" + esc(text) + "&root=" + esc(rootUri);
    if (content) u += "&content=1";
    if (includeHidden) u += "&hidden=1";
    if (!type.empty()) u += "&type=" + esc(type);
    if (modifiedWithinDays > 0) u += "&days=" + std::to_string(modifiedWithinDays);
    if (minSize > 0) u += "&minsize=" + std::to_string(minSize);
    return u;
}

bool SearchQuery::fromUri(const std::string &uri, SearchQuery &out)
{
    if (!is_search_uri(uri)) return false;
    size_t q = uri.find('?');
    out = SearchQuery{};
    if (q == std::string::npos) return true;
    GHashTable *params = g_uri_parse_params(uri.c_str() + q + 1, -1, "&", G_URI_PARAMS_NONE, nullptr);
    if (!params) return false;
    auto get = [&](const char *k) { return str(static_cast<const char *>(g_hash_table_lookup(params, k))); };
    out.text = get("q");
    out.rootUri = get("root");
    out.content = get("content") == "1";
    out.includeHidden = get("hidden") == "1";
    out.type = get("type");
    out.modifiedWithinDays = std::atoi(get("days").c_str());
    out.minSize = g_ascii_strtoull(get("minsize").c_str(), nullptr, 10);
    g_hash_table_unref(params);
    if (out.rootUri.empty()) out.rootUri = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
    return true;
}

const char *type_title(const std::string &type)
{
    if (type == "folders") return _("Folders");
    if (type == "documents") return _("Documents");
    if (type == "images") return _("Images");
    if (type == "audio") return _("Audio");
    if (type == "videos") return _("Videos");
    if (type == "archives") return _("Archives");
    return _("All Files");
}

std::string SearchQuery::title() const
{
    if (text.empty() && !type.empty()) return type_title(type);
    return printf_str(_("Search for “%s”"), text.c_str());
}

std::string search_uri_for_type(const std::string &type)
{
    SearchQuery q;
    q.type = type;
    q.rootUri = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
    return q.toUri();
}

bool type_matches(const std::string &type, const std::string &ct, bool isDir)
{
    if (type.empty()) return true;
    if (type == "folders") return isDir;
    if (isDir) return false;
    if (type == "images") return starts_with(ct, "image/");
    if (type == "audio") return starts_with(ct, "audio/");
    if (type == "videos") return starts_with(ct, "video/");
    if (type == "archives") {
        static const char *a[] = {"application/zip", "application/x-tar", "application/x-compressed-tar",
                                  "application/x-bzip-compressed-tar", "application/x-xz-compressed-tar",
                                  "application/x-zstd-compressed-tar", "application/x-7z-compressed",
                                  "application/vnd.rar", "application/x-rar", "application/gzip", "application/x-xz",
                                  "application/zstd", "application/x-bzip2", "application/x-cpio",
                                  "application/vnd.debian.binary-package", "application/x-rpm", nullptr};
        for (int i = 0; a[i]; ++i)
            if (ct == a[i]) return true;
        return false;
    }
    if (type == "documents") {
        if (starts_with(ct, "text/")) return true;
        static const char *d[] = {"application/pdf", "application/rtf", "application/msword", "application/epub+zip",
                                  "application/vnd.ms-excel", "application/vnd.ms-powerpoint",
                                  "application/x-abiword", "application/postscript", "application/json",
                                  "application/xml", "application/x-mobipocket-ebook", "application/vnd.comicbook+zip",
                                  nullptr};
        for (int i = 0; d[i]; ++i)
            if (ct == d[i]) return true;
        return starts_with(ct, "application/vnd.oasis.opendocument.")
            || starts_with(ct, "application/vnd.openxmlformats-officedocument.");
    }
    return true;
}

SearchJob::Ptr SearchJob::start(const SearchQuery &q, ResultsFn onResults, std::function<void()> onDone)
{
    Ptr j(new SearchJob);
    j->q_ = q;
    j->needle_ = casefold(q.text);
    j->onResults_ = std::move(onResults);
    j->onDone_ = std::move(onDone);
    std::thread([j] { j->run(); }).detach();
    return j;
}

bool SearchJob::contentMatches(GFile *f)
{
    if (q_.text.empty()) return false;
    GFileInputStream *in = g_file_read(f, nullptr, nullptr);
    if (!in) return false;
    std::string needle = q_.text;
    for (auto &c : needle) c = g_ascii_tolower(c);
    const size_t chunk = 1 << 16;
    std::string buf, carry;
    bool found = false, first = true;
    guint64 total = 0;
    std::vector<char> data(chunk);
    while (!found && !cancelled_ && total < (64ull << 20)) {
        gssize n = g_input_stream_read(G_INPUT_STREAM(in), data.data(), chunk, nullptr, nullptr);
        if (n <= 0) break;
        if (first && memchr(data.data(), 0, std::min<size_t>(n, 4096))) break;   // binary
        first = false;
        total += n;
        buf = carry + std::string(data.data(), n);
        for (auto &c : buf) c = g_ascii_tolower(c);
        if (buf.find(needle) != std::string::npos) found = true;
        size_t keep = std::min(buf.size(), needle.size());
        carry = buf.substr(buf.size() - keep);
    }
    g_object_unref(in);
    return found;
}

bool SearchJob::matches(GFile *f, GFileInfo *info)
{
    bool isDir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    std::string ct = content_type_of(info);
    if (!type_matches(q_.type, ct, isDir)) return false;
    if (q_.modifiedWithinDays > 0) {
        gint64 limit = g_get_real_time() / G_USEC_PER_SEC - (gint64)q_.modifiedWithinDays * 86400;
        GDateTime *dt = g_file_info_get_modification_date_time(info);
        gint64 t = dt ? g_date_time_to_unix(dt) : 0;
        if (dt) g_date_time_unref(dt);
        if (t < limit) return false;
    }
    if (q_.minSize > 0 && (isDir || (guint64)info_size(info) < q_.minSize)) return false;
    if (q_.text.empty()) return true;
    const char *name = g_file_info_get_display_name(info);
    if (filter_match(q_.text, name ? name : "")) return true;
    if (q_.content && !isDir && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) return contentMatches(f);
    return false;
}

bool SearchJob::matchesLocal(const std::string &path, const char *name, const struct stat &st)
{
    const bool isDir = S_ISDIR(st.st_mode);
    if (q_.modifiedWithinDays > 0) {
        gint64 limit = g_get_real_time() / G_USEC_PER_SEC - (gint64)q_.modifiedWithinDays * 86400;
        if ((gint64)st.st_mtime < limit) return false;
    }
    if (q_.minSize > 0 && (isDir || (guint64)st.st_size < q_.minSize)) return false;
    if (!q_.type.empty() && !isDir) {
        // The type from the name; the file's first bytes only when the name doesn't tell.
        gboolean uncertain = FALSE;
        std::string ct = take(g_content_type_guess(name, nullptr, 0, &uncertain));
        if (uncertain && S_ISREG(st.st_mode)) {
            Ref<GFile> f(g_file_new_for_path(path.c_str()));
            Ref<GFileInfo> info(g_file_query_info(f.get(), G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr));
            if (info) ct = content_type_of(info.get());
        }
        if (!type_matches(q_.type, ct, false)) return false;
    } else if (!type_matches(q_.type, {}, isDir)) {
        return false;
    }
    if (q_.text.empty()) return true;
    std::string display = g_utf8_validate(name, -1, nullptr) ? std::string(name) : take(g_filename_display_name(name));
    if (filter_match(q_.text, display)) return true;
    if (q_.content && S_ISREG(st.st_mode)) {
        Ref<GFile> f(g_file_new_for_path(path.c_str()));
        return contentMatches(f.get());
    }
    return false;
}

void SearchJob::run()
{
    BLOG_INFO("search", "\"%s\" in %s%s", q_.text.c_str(), q_.rootUri.c_str(), q_.content ? " (contents)" : "");
    Ref<GFile> root(g_file_new_for_uri(q_.rootUri.c_str()));
    std::string path = take(g_file_get_path(root.get()));
    if (path.empty() || !g_file_is_native(root.get())) {
        runGio();
        return;
    }
    // The index knows names and folders only: dates, sizes and contents need the disk.
    if (!q_.content && q_.modifiedWithinDays <= 0 && q_.minSize == 0 && fileindex::covers(path)) runIndexed(path);
    else runLocal(path);
}

void SearchJob::runIndexed(const std::string &rootPath)
{
    auto self = shared_from_this();
    std::string needle = casefold(q_.text);
    bool wildcard = q_.text.find_first_of("*?[") != std::string::npos;
    GPatternSpec *spec = wildcard ? g_pattern_spec_new(needle.c_str()) : nullptr;
    const std::string type = q_.type;
    // The same test as matchesLocal() on the name, and the type when the name tells it.
    fileindex::Matcher match = [&](const char *name, const char *folded, bool dir) {
        if (type == "folders" && !dir) return false;
        if (!type.empty() && type != "folders") {
            if (dir) return false;
            gboolean uncertain = FALSE;
            std::string ct = take(g_content_type_guess(name, nullptr, 0, &uncertain));
            if (!uncertain && !type_matches(type, ct, false)) return false;
        }
        if (needle.empty()) return true;
        return spec ? (bool)g_pattern_spec_match_string(spec, folded) : strstr(folded, needle.c_str()) != nullptr;
    };
    std::set<std::string> sent;
    std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> batch;
    gint64 lastFlush = g_get_monotonic_time();
    auto flush = [&] {
        if (batch.empty()) return;
        auto moved = std::make_shared<std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>>>(std::move(batch));
        batch.clear();
        run_on_main([self, moved] {
            if (!self->cancelled_) self->onResults_(std::move(*moved));
        });
        lastFlush = g_get_monotonic_time();
    };
    // Every hit is checked on the disk, so files deleted since the index was made never show.
    auto emit = [&](std::vector<fileindex::Hit> hits) {
        for (auto &h : hits) {
            if (cancelled_) return;
            if (!sent.insert(h.path).second) continue;
            struct stat st;
            if (lstat(h.path.c_str(), &st) != 0) continue;
            if (!matchesLocal(h.path, basename_of(h.path).c_str(), st)) continue;
            Ref<GFile> f(g_file_new_for_path(h.path.c_str()));
            Ref<GFileInfo> info(g_file_query_info(f.get(), kItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr));
            if (!info) continue;
            batch.emplace_back(f, info);
            if (batch.size() >= 200 || g_get_monotonic_time() - lastFlush > 100000) flush();
        }
        flush();
    };
    emit(fileindex::query(rootPath, match, q_.includeHidden));
    // Then whatever changed since: only folders whose time changed are read again.
    std::vector<std::string> changed = fileindex::refresh(rootPath, &cancelled_);
    folders_ = (int)fileindex::stats().folders;
    if (!changed.empty() && !cancelled_) emit(fileindex::queryFolders(rootPath, changed, match, q_.includeHidden));
    if (spec) g_pattern_spec_free(spec);
    // What the index leaves out (other drives mounted inside, the cache folder) is read from the disk.
    for (auto &p : fileindex::skipped(rootPath)) {
        if (cancelled_) break;
        if (!q_.includeHidden && p.find("/.", rootPath == "/" ? 0 : rootPath.size()) != std::string::npos) continue;
        runLocal(p, false);
    }
    std::thread([] { fileindex::save(); }).detach();
    run_on_main([self] {
        BLOG_DEBUG("search", "done");
        if (self->onDone_) self->onDone_();
    });
}

void SearchJob::runLocal(const std::string &rootPath, bool finish)
{
    auto self = shared_from_this();
    std::mutex m;
    std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> batch;
    gint64 lastFlush = g_get_monotonic_time();
    auto flushLocked = [&] {
        if (batch.empty()) return;
        auto moved = std::make_shared<std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>>>(std::move(batch));
        batch.clear();
        run_on_main([self, moved] {
            if (!self->cancelled_) self->onResults_(std::move(*moved));
        });
        lastFlush = g_get_monotonic_time();
    };
    Walker w;
    w.onEntry = [&](const WalkEntry &e, void **) {
        if (cancelled_) {
            w.cancel();
            return false;
        }
        std::string path = e.dir + "/" + e.name;
        const bool isDir = S_ISDIR(e.st.st_mode);
        if (isDir) folders_++;
        if (matchesLocal(path, e.name, e.st)) {
            Ref<GFile> f(g_file_new_for_path(path.c_str()));
            Ref<GFileInfo> info(g_file_query_info(f.get(), kItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr));
            if (info) {
                std::lock_guard<std::mutex> l(m);
                batch.emplace_back(f, info);
                if (batch.size() >= 200 || g_get_monotonic_time() - lastFlush > 150000) flushLocked();
            }
        }
        // Not into the FUSE mounts of remote locations (gvfs) under /run/user.
        return isDir && path != "/run/user";
    };
    Walker::Options wo;
    wo.skipHidden = !q_.includeHidden;
    w.run(rootPath, nullptr, wo);
    {
        std::lock_guard<std::mutex> l(m);
        flushLocked();
    }
    if (!finish) return;
    run_on_main([self] {
        BLOG_DEBUG("search", "done");
        if (self->onDone_) self->onDone_();
    });
}

void SearchJob::runGio()
{
    std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> batch;
    gint64 lastFlush = g_get_monotonic_time();
    auto self = shared_from_this();
    auto flush = [&] {
        if (batch.empty()) return;
        auto moved = std::make_shared<std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>>>(std::move(batch));
        batch.clear();
        run_on_main([self, moved] {
            if (!self->cancelled_) self->onResults_(std::move(*moved));
        });
        lastFlush = g_get_monotonic_time();
    };

    std::deque<Ref<GFile>> stack;   // breadth first: nearby results come first
    stack.push_back(Ref<GFile>(g_file_new_for_uri(q_.rootUri.c_str())));
    static const char *skipRoots[] = {"/proc", "/sys", "/dev", "/run/user", nullptr};
    while (!stack.empty() && !cancelled_) {
        Ref<GFile> dir = stack.front();
        stack.pop_front();
        folders_++;
        GFileEnumerator *en = g_file_enumerate_children(dir.get(), kItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        if (!en) continue;
        GFileInfo *info;
        while (!cancelled_ && (info = g_file_enumerator_next_file(en, nullptr, nullptr))) {
            Ref<GFileInfo> ri(info);
            if (!q_.includeHidden && g_file_info_get_is_hidden(info)) continue;
            Ref<GFile> child(g_file_get_child(dir.get(), g_file_info_get_name(info)));
            if (matches(child.get(), info)) batch.emplace_back(child, ri);
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
                std::string p = display_location(child.get());
                bool skip = false;
                for (int i = 0; skipRoots[i]; ++i)
                    if (p == skipRoots[i]) skip = true;
                if (!skip) stack.push_back(child);
            }
            if (batch.size() >= 200 || g_get_monotonic_time() - lastFlush > 150000) flush();
        }
        g_object_unref(en);
    }
    flush();
    run_on_main([self] {
        BLOG_DEBUG("search", "done");
        if (self->onDone_) self->onDone_();
    });
}

} // namespace beluga
