// SPDX-License-Identifier: GPL-2.0-or-later
#include "fileindex.hpp"

#include "settings.hpp"
#include "util.hpp"

#include <dirent.h>
#include <malloc.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>

namespace beluga::fileindex {

namespace {

struct Item {
    guint32 off;       // the name's offset in names; the top bit marks a folder
    guint32 foldOff;   // the folded name's offset in folded (when a folder has one)
    guint32 nameOff() const { return off & 0x7fffffffu; }
    bool dir() const { return (off >> 31) != 0; }
};

// One folder: its time when it was read, and the names in it (NUL-terminated, back to back).
struct Dir {
    gint64 sec = 0, nsec = 0;
    guint64 ino = 0;
    std::string names, folded;   // folded is empty when every name folds to itself (lower case ASCII)
    std::vector<Item> items;
};

std::shared_mutex g_lock;
std::map<std::string, Dir> g_dirs;   // full path → folder
std::string g_root;                   // the indexed folder (home)
std::string g_cacheDir;               // ~/.cache: regenerable data, not indexed
std::set<std::string> g_skipped;      // folders inside the root that aren't indexed: other drives and the cache
std::atomic<bool> g_ready{false};     // loaded from disk, or read in full once
std::atomic<bool> g_dirty{false};
std::mutex g_saveMutex;

const char kMagic[] = "BELUGA-INDEX-2\n";   // 2: hidden entries, packed items

std::string cache_file() { return std::string(g_get_user_cache_dir()) + "/beluga/index-v2"; }

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
    for (auto it = g_skipped.lower_bound(prefix); it != g_skipped.end() && starts_with(*it, prefix);) it = g_skipped.erase(it);
}

bool read_dir(const std::string &path, Dir &d, std::vector<std::string> &subdirs)
{
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dp = fd >= 0 ? fdopendir(fd) : nullptr;
    if (!dp) {
        if (fd >= 0) close(fd);
        return false;
    }
    bool sameFold = true;
    while (struct dirent *de = readdir(dp)) {
        const char *n = de->d_name;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;   // "." and ".."
        bool isDir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            isDir = fstatat(dirfd(dp), n, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
        }
        d.items.push_back({(guint32)d.names.size() | (isDir ? 0x80000000u : 0u), (guint32)d.folded.size()});
        std::string f = fold(n);
        if (f != n) sameFold = false;
        d.names.append(n).push_back('\0');
        d.folded.append(f).push_back('\0');
        if (isDir) subdirs.push_back(n);
    }
    closedir(dp);
    // Keep only what is needed: most folders (caches, sources) have lower case names only.
    if (sameFold) std::string().swap(d.folded);
    else d.folded.shrink_to_fit();
    d.names.shrink_to_fit();
    d.items.shrink_to_fit();
    return true;
}

void collect(const std::string &key, const Dir &d, const Matcher &match, bool hidden, std::vector<Hit> &out)
{
    for (const Item &it : d.items) {
        const char *name = d.names.data() + it.nameOff();
        if (!hidden && name[0] == '.') continue;
        const char *folded = d.folded.empty() ? name : d.folded.data() + it.foldOff;
        if (match(name, folded, it.dir())) out.push_back({key + "/" + name, it.dir()});
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
            if (!r.get(it.off) || !r.get(it.foldOff) || it.nameOff() >= d.names.size() || (!d.folded.empty() && it.foldOff >= d.folded.size()))
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
                put(b, it.off);
                put(b, it.foldOff);
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
    g_cacheDir = g_get_user_cache_dir();
    g_unlink((std::string(g_get_user_cache_dir()) + "/beluga/index-v1").c_str());   // without hidden entries
    std::thread([] {
        if (load()) g_ready = true;
        refresh(g_root);
        g_ready = true;   // now complete, also when there was nothing saved
        save();
#ifdef __GLIBC__
        malloc_trim(0);   // the file's buffer and the reading threads' memory go back to the system
#endif
    }).detach();
}

bool covers(const std::string &folder)
{
    if (!g_ready || g_root.empty() || !settings().searchIndex || !inside(folder, g_root)) return false;
    std::shared_lock<std::shared_mutex> l(g_lock);
    return g_dirs.count(folder) > 0;
}

std::vector<std::string> skipped(const std::string &folder)
{
    std::vector<std::string> out;
    std::shared_lock<std::shared_mutex> l(g_lock);
    std::string prefix = folder == "/" ? folder : folder + "/";
    for (auto it = g_skipped.lower_bound(prefix); it != g_skipped.end() && starts_with(*it, prefix); ++it) {
        // Only the outermost ones (a drive inside the cache is walked with it).
        bool nested = false;
        for (auto &o : out)
            if (starts_with(*it, o + "/")) nested = true;
        if (!nested) out.push_back(*it);
    }
    return out;
}

// Whether a folder below `folder` lies inside a hidden folder (counting from `folder` on).
bool hidden_below(const std::string &key, const std::string &folder)
{
    size_t from = folder == "/" ? 0 : folder.size();
    return key.size() > from && key.find("/.", from) != std::string::npos;
}

std::vector<Hit> query(const std::string &folder, const Matcher &match, bool hidden)
{
    std::vector<Hit> out;
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto top = g_dirs.find(folder);
    if (top != g_dirs.end()) collect(top->first, top->second, match, hidden, out);
    std::string prefix = folder == "/" ? folder : folder + "/";
    for (auto it = g_dirs.lower_bound(prefix); it != g_dirs.end() && starts_with(it->first, prefix);) {
        if (!hidden && hidden_below(it->first, folder)) {
            // Jump past the hidden folder H: keys from H up to H + "0" are H's subtree ('/' sorts before '0') and
            // siblings like "H-x", whose names start with the same dot.
            size_t from = folder == "/" ? 0 : folder.size();
            size_t dot = it->first.find("/.", from);
            size_t end = it->first.find('/', dot + 1);
            it = g_dirs.lower_bound(it->first.substr(0, end) + "0");
            continue;
        }
        collect(it->first, it->second, match, hidden, out);
        ++it;
    }
    return out;
}

std::vector<Hit> queryFolders(const std::string &folder, const std::vector<std::string> &folders, const Matcher &match, bool hidden)
{
    std::vector<Hit> out;
    std::shared_lock<std::shared_mutex> l(g_lock);
    for (auto &f : folders) {
        if (!hidden && hidden_below(f, folder)) continue;
        auto it = g_dirs.find(f);
        if (it != g_dirs.end()) collect(it->first, it->second, match, hidden, out);
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
        bool isDir = lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        if (!isDir || st.st_dev != dev || p == g_cacheDir) {
            // Gone, or another drive (searches read those from the disk), or the cache.
            std::unique_lock<std::shared_mutex> l(g_lock);
            if (g_dirs.count(p)) g_dirty = true;
            erase_subtree_locked(p);
            if (isDir) g_skipped.insert(p);
            else g_skipped.erase(p);
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
                    if (item.dir()) subdirs.push_back(it->second.names.data() + item.nameOff());
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
                    if (item.dir()) {
                        std::string name = old->second.names.data() + item.nameOff();
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
