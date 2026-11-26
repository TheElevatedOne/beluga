// SPDX-License-Identifier: GPL-2.0-or-later
#include "fileindex.hpp"

#include "settings.hpp"
#include "util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace beluga::fileindex {

namespace {

struct Item {
    guint32 nameOff;
    guint32 foldOff;
    guint8 dir;
};

// One folder: its time when it was read, and the names in it (NUL-terminated, back to back).
struct Dir {
    gint64 sec = 0, nsec = 0;
    guint64 ino = 0;
    std::string names, folded;
    std::vector<Item> items;
};

std::shared_mutex g_lock;
std::map<std::string, Dir> g_dirs;   // full path → folder; hidden folders are left out
std::string g_root;                   // the indexed folder (home)
std::atomic<bool> g_ready{false};     // loaded from disk, or read in full once
std::atomic<bool> g_dirty{false};
std::mutex g_saveMutex;

const char kMagic[] = "BELUGA-INDEX-1\n";

std::string cache_file() { return std::string(g_get_user_cache_dir()) + "/beluga/index-v1"; }

// The same folding as filter_match() (casefold), with a fast path for ASCII names.
std::string fold(const char *s)
{
    bool ascii = true;
    for (const char *p = s; *p; ++p)
        if ((unsigned char)*p & 0x80) {
            ascii = false;
            break;
        }
    if (ascii || !g_utf8_validate(s, -1, nullptr)) {
        std::string r(s);
        for (auto &c : r) c = g_ascii_tolower(c);
        return r;
    }
    return take(g_utf8_casefold(s, -1));
}

bool inside(const std::string &path, const std::string &root) { return path == root || starts_with(path, root + "/"); }

// Erases a folder and everything indexed below it. Keys like "P-x" sort between "P" and "P/…", so the range
// below starts at "P/".
void erase_subtree_locked(const std::string &p)
{
    g_dirs.erase(p);
    std::string prefix = p + "/";
    for (auto it = g_dirs.lower_bound(prefix); it != g_dirs.end() && starts_with(it->first, prefix);) it = g_dirs.erase(it);
}

bool read_dir(const std::string &path, Dir &d, std::vector<std::string> &subdirs)
{
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dp = fd >= 0 ? fdopendir(fd) : nullptr;
    if (!dp) {
        if (fd >= 0) close(fd);
        return false;
    }
    while (struct dirent *de = readdir(dp)) {
        const char *n = de->d_name;
        if (n[0] == '.') continue;   // ".", ".." and hidden entries
        bool isDir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            isDir = fstatat(dirfd(dp), n, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
        }
        d.items.push_back({(guint32)d.names.size(), (guint32)d.folded.size(), (guint8)isDir});
        d.names.append(n).push_back('\0');
        d.folded.append(fold(n)).push_back('\0');
        if (isDir) subdirs.push_back(n);
    }
    closedir(dp);
    return true;
}

void collect(const std::string &key, const Dir &d, const Matcher &match, std::vector<Hit> &out)
{
    for (const Item &it : d.items) {
        const char *name = d.names.data() + it.nameOff;
        if (match(name, d.folded.data() + it.foldOff, it.dir)) out.push_back({key + "/" + name, it.dir != 0});
    }
}

// ---- saving ------------------------------------------------------------------------------------

template <typename T> void put(std::string &b, T v) { b.append(reinterpret_cast<const char *>(&v), sizeof v); }
void put_str(std::string &b, const std::string &s)
{
    put<guint32>(b, (guint32)s.size());
    b.append(s);
}

struct Reader {
    const char *p, *end;
    template <typename T> bool get(T &v)
    {
        if (end - p < (ptrdiff_t)sizeof v) return false;
        memcpy(&v, p, sizeof v);
        p += sizeof v;
        return true;
    }
    bool get_str(std::string &s)
    {
        guint32 n;
        if (!get(n) || end - p < (ptrdiff_t)n) return false;
        s.assign(p, n);
        p += n;
        return true;
    }
};

bool load()
{
    char *data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(cache_file().c_str(), &data, &len, nullptr)) return false;
    std::unique_ptr<char, decltype(&g_free)> keep(data, g_free);
    if (len < sizeof kMagic - 1 || memcmp(data, kMagic, sizeof kMagic - 1) != 0) return false;
    Reader r{data + sizeof kMagic - 1, data + len};
    std::string root;
    guint64 count = 0;
    if (!r.get_str(root) || root != g_root || !r.get(count)) return false;
    std::map<std::string, Dir> dirs;
    for (guint64 i = 0; i < count; ++i) {
        std::string path;
        Dir d;
        guint32 n = 0;
        if (!r.get_str(path) || !r.get(d.sec) || !r.get(d.nsec) || !r.get(d.ino) || !r.get_str(d.names) || !r.get_str(d.folded) || !r.get(n))
            return false;
        d.items.resize(n);
        for (auto &it : d.items)
            if (!r.get(it.nameOff) || !r.get(it.foldOff) || !r.get(it.dir) || it.nameOff >= d.names.size() || it.foldOff >= d.folded.size())
                return false;
        dirs.emplace(std::move(path), std::move(d));
    }
    std::unique_lock<std::shared_mutex> l(g_lock);
    g_dirs = std::move(dirs);
    return true;
}

} // namespace

void save()
{
    std::lock_guard<std::mutex> sl(g_saveMutex);
    if (!g_dirty.exchange(false)) return;
    std::string b(kMagic, sizeof kMagic - 1);
    {
        std::shared_lock<std::shared_mutex> l(g_lock);
        put_str(b, g_root);
        put<guint64>(b, g_dirs.size());
        for (auto &[path, d] : g_dirs) {
            put_str(b, path);
            put(b, d.sec);
            put(b, d.nsec);
            put(b, d.ino);
            put_str(b, d.names);
            put_str(b, d.folded);
            put<guint32>(b, (guint32)d.items.size());
            for (auto &it : d.items) {
                put(b, it.nameOff);
                put(b, it.foldOff);
                put(b, it.dir);
            }
        }
    }
    ensure_dir(dirname_of(cache_file()));
    g_file_set_contents_full(cache_file().c_str(), b.data(), (gssize)b.size(), G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr);
}

bool ready() { return g_ready; }

void start(const std::string &root)
{
    static bool started = false;
    if (started || !settings().searchIndex) return;
    started = true;
    g_root = root.empty() ? home_dir() : root;
    std::thread([] {
        if (load()) g_ready = true;
        refresh(g_root);
        g_ready = true;   // now complete, also when there was nothing saved
        save();
    }).detach();
}

bool covers(const std::string &folder)
{
    if (!g_ready || g_root.empty() || !settings().searchIndex || !inside(folder, g_root)) return false;
    // Hidden folders aren't indexed.
    for (auto &part : split(folder.substr(g_root.size()), '/'))
        if (part[0] == '.') return false;
    std::shared_lock<std::shared_mutex> l(g_lock);
    return g_dirs.count(folder) > 0;
}

std::vector<Hit> query(const std::string &folder, const Matcher &match)
{
    std::vector<Hit> out;
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto top = g_dirs.find(folder);
    if (top != g_dirs.end()) collect(top->first, top->second, match, out);
    std::string prefix = folder == "/" ? folder : folder + "/";
    for (auto it = g_dirs.lower_bound(prefix); it != g_dirs.end() && starts_with(it->first, prefix); ++it)
        collect(it->first, it->second, match, out);
    return out;
}

std::vector<Hit> queryFolders(const std::vector<std::string> &folders, const Matcher &match)
{
    std::vector<Hit> out;
    std::shared_lock<std::shared_mutex> l(g_lock);
    for (auto &f : folders) {
        auto it = g_dirs.find(f);
        if (it != g_dirs.end()) collect(it->first, it->second, match, out);
    }
    return out;
}

std::vector<std::string> refresh(const std::string &folder, const std::atomic<bool> *cancel)
{
    if (g_root.empty() || !inside(folder, g_root)) return {};
    struct stat rs;
    if (lstat(g_root.c_str(), &rs) != 0) return {};
    const dev_t dev = rs.st_dev;
    std::vector<std::string> changed;
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> tasks{folder};
    int busy = 0;
    bool done = false;

    // Checks one folder: unchanged ones only give their subfolders; changed ones are read again.
    auto process = [&](const std::string &p, std::vector<std::string> &next) {
        struct stat st;
        if (lstat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != dev) {
            std::unique_lock<std::shared_mutex> l(g_lock);
            if (g_dirs.count(p)) g_dirty = true;
            erase_subtree_locked(p);
            return;
        }
        std::vector<std::string> subdirs;
        bool same = false;
        {
            std::shared_lock<std::shared_mutex> l(g_lock);
            auto it = g_dirs.find(p);
            if (it != g_dirs.end() && it->second.sec == (gint64)st.st_mtim.tv_sec && it->second.nsec == (gint64)st.st_mtim.tv_nsec
                && it->second.ino == (guint64)st.st_ino) {
                same = true;
                for (auto &item : it->second.items)
                    if (item.dir) subdirs.push_back(it->second.names.data() + item.nameOff);
            }
        }
        if (!same) {
            Dir d;
            d.sec = st.st_mtim.tv_sec;
            d.nsec = st.st_mtim.tv_nsec;
            d.ino = st.st_ino;
            if (!read_dir(p, d, subdirs)) {
                std::unique_lock<std::shared_mutex> l(g_lock);
                erase_subtree_locked(p);
                return;
            }
            std::unique_lock<std::shared_mutex> l(g_lock);
            auto old = g_dirs.find(p);
            if (old != g_dirs.end())
                for (auto &item : old->second.items)
                    if (item.dir) {
                        std::string name = old->second.names.data() + item.nameOff;
                        if (std::find(subdirs.begin(), subdirs.end(), name) == subdirs.end()) erase_subtree_locked(p + "/" + name);
                    }
            g_dirs[p] = std::move(d);
            g_dirty = true;
            std::lock_guard<std::mutex> cl(m);
            changed.push_back(p);
        }
        for (auto &s : subdirs) next.push_back(p + "/" + s);
    };
    auto worker = [&] {
        std::vector<std::string> next;
        std::unique_lock<std::mutex> l(m);
        for (;;) {
            cv.wait(l, [&] { return done || !tasks.empty(); });
            if (done) return;
            std::string p = std::move(tasks.back());
            tasks.pop_back();
            ++busy;
            l.unlock();
            next.clear();
            if (!(cancel && *cancel)) process(p, next);
            l.lock();
            --busy;
            if (cancel && *cancel) tasks.clear();
            else
                for (auto &n : next) tasks.push_back(std::move(n));
            if (tasks.empty() && busy == 0) {
                done = true;
                cv.notify_all();
                return;
            }
            if (!next.empty()) cv.notify_all();
        }
    };
    int threads = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (auto &t : pool) t.join();
    return changed;
}

Stats stats()
{
    Stats s;
    std::shared_lock<std::shared_mutex> l(g_lock);
    s.folders = g_dirs.size();
    for (auto &[path, d] : g_dirs) {
        s.entries += d.items.size();
        s.bytes += path.size() + d.names.size() + d.folded.size() + d.items.size() * sizeof(Item) + sizeof(Dir) + 64;
    }
    return s;
}

} // namespace beluga::fileindex
