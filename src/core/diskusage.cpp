// SPDX-License-Identifier: GPL-2.0-or-later
#include "diskusage.hpp"

#include "util.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace beluga::du {

namespace {

// Disk usage counts the blocks of everything, folders included. The apparent size adds up what the
// files hold; a folder's own size is file system bookkeeping and doesn't count.
guint64 size_of(const struct stat &st, bool apparent)
{
    if (apparent) return S_ISDIR(st.st_mode) ? 0 : (guint64)st.st_size;
    return (guint64)st.st_blocks * 512;
}

// Files with several hard links are counted once, like du and dust do.
class SeenInodes {
public:
    bool firstTime(dev_t dev, ino_t ino)
    {
        guint64 key = (guint64)ino ^ ((guint64)dev << 40) ^ ((guint64)dev >> 24);
        Shard &s = shards_[key % kShards];
        std::lock_guard<std::mutex> l(s.m);
        return s.keys.insert({(guint64)dev, (guint64)ino}).second;
    }

private:
    struct PairHash {
        size_t operator()(const std::pair<guint64, guint64> &p) const { return std::hash<guint64>()(p.first * 0x9e3779b97f4a7c15ull ^ p.second); }
    };
    struct Shard {
        std::mutex m;
        std::unordered_set<std::pair<guint64, guint64>, PairHash> keys;
    };
    static constexpr int kShards = 32;
    Shard shards_[kShards];
};

bool smaller(const std::unique_ptr<Node> &a, const std::unique_ptr<Node> &b) { return a->size > b->size; }   // min-heap

void finish(Node *n)
{
    for (auto &c : n->children) {
        if (c->dir) finish(c.get());
        n->size += c->size;
        n->files += c->files;
        n->folders += c->dir ? 1 + c->folders : 0;
    }
    std::sort(n->children.begin(), n->children.end(), [](const auto &a, const auto &b) {
        if (a->size != b->size) return a->size > b->size;
        if (a->rest() != b->rest()) return b->rest();   // the "smaller files" row last
        return a->name < b->name;
    });
}

} // namespace

std::string Node::path() const
{
    if (rest()) return {};
    if (!parent) return name;
    std::string p = parent->path();
    return (p == "/" ? std::string() : p) + "/" + name;
}

std::unique_ptr<Node> Scanner::run(const std::string &path, const Options &o)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return nullptr;
    auto root = std::make_unique<Node>();
    root->name = path;
    root->dir = true;
    root->size = size_of(st, o.apparentSize);
    root->mtimeSec = st.st_mtim.tv_sec;
    root->mtimeNsec = st.st_mtim.tv_nsec;
    SeenInodes seen;
    const size_t keep = (size_t)std::max(0, o.filesPerFolder);

    walker_.onEntry = [&](const WalkEntry &e, void **childData) {
        auto *parent = static_cast<Node *>(e.dirData);
        const bool dir = S_ISDIR(e.st.st_mode);
        if (!dir && e.st.st_nlink > 1 && !seen.firstTime(e.st.st_dev, e.st.st_ino)) return false;
        auto n = std::make_unique<Node>();
        n->name = e.name;
        n->parent = parent;
        n->dir = dir;
        n->size = size_of(e.st, o.apparentSize);
        if (dir) {
            n->otherFileSystem = e.otherFileSystem;
            n->mtimeSec = e.st.st_mtim.tv_sec;
            n->mtimeNsec = e.st.st_mtim.tv_nsec;
            *childData = n.get();
            parent->children.push_back(std::move(n));
            return !(e.otherFileSystem && o.oneFileSystem);
        }
        n->files = 1;
        // Keep the folder's biggest files; the others only add up.
        if (parent->topFiles.size() < keep) {
            parent->topFiles.push_back(std::move(n));
            std::push_heap(parent->topFiles.begin(), parent->topFiles.end(), smaller);
        } else if (keep && n->size > parent->topFiles.front()->size) {
            std::pop_heap(parent->topFiles.begin(), parent->topFiles.end(), smaller);
            parent->restSize += parent->topFiles.back()->size;
            parent->restFiles++;
            parent->topFiles.back() = std::move(n);
            std::push_heap(parent->topFiles.begin(), parent->topFiles.end(), smaller);
        } else {
            parent->restSize += n->size;
            parent->restFiles++;
        }
        return false;
    };
    walker_.onFolderDone = [&](void *data) {
        auto *n = static_cast<Node *>(data);
        guint64 fileCount = n->topFiles.size() + n->restFiles, fileBytes = n->restSize, dirs = 0;
        for (auto &f : n->topFiles) {
            fileBytes += f->size;
            n->children.push_back(std::move(f));
        }
        n->topFiles.clear();
        n->topFiles.shrink_to_fit();
        if (n->restFiles) {
            auto r = std::make_unique<Node>();
            r->parent = n;
            r->size = n->restSize;
            r->files = n->restFiles;
            n->children.push_back(std::move(r));
        }
        for (auto &c : n->children) {
            if (c->dir) {
                dirs++;
                fileBytes += c->size;   // the folder entry itself
            }
        }
        files += fileCount;
        folders += dirs;
        bytes += fileBytes;
    };
    walker_.onError = [&](const std::string &, int, void *data) {
        if (data) static_cast<Node *>(data)->unreadable = true;
        errors++;
    };
    Walker::Options wo;
    wo.threads = o.threads;
    wo.oneFileSystem = o.oneFileSystem;
    walker_.run(path, root.get(), wo);
    if (walker_.cancelled) return nullptr;
    finish(root.get());
    return root;
}

Totals totals(const std::string &path, bool apparentSize, int threads, int maxDepth, std::function<bool()> stop, bool oneFileSystem)
{
    std::atomic<guint64> size{0}, files{0}, folders{0}, seenEntries{0};
    SeenInodes seen;
    Walker w;
    w.onEntry = [&](const WalkEntry &e, void **childData) {
        if (stop && (++seenEntries & 1023) == 0 && stop()) w.cancel();
        const bool dir = S_ISDIR(e.st.st_mode);
        if (!dir && e.st.st_nlink > 1 && !seen.firstTime(e.st.st_dev, e.st.st_ino)) return false;
        size += size_of(e.st, apparentSize);
        if (!dir) {
            files++;
            return false;
        }
        folders++;
        intptr_t depth = reinterpret_cast<intptr_t>(e.dirData) + 1;
        *childData = reinterpret_cast<void *>(depth);
        return maxDepth < 0 || depth <= maxDepth;
    };
    Walker::Options wo;
    wo.threads = threads;
    wo.oneFileSystem = oneFileSystem;
    w.run(path, nullptr, wo);
    return {size, files, folders};
}

// ---- cache ----------------------------------------------------------------------------------------

namespace {

std::mutex g_cacheMutex;
std::unordered_map<std::string, CachedTotal> g_totals;   // "1" or "0" (one drive) + path
bool g_totalsLoaded = false, g_totalsDirty = false;
gint64 g_lastSave = 0;
struct TreeEntry {
    std::shared_ptr<Node> tree;
    bool apparent, oneFs;
    gint64 when;
};
std::vector<TreeEntry> g_trees;   // newest last
constexpr size_t kMaxTotals = 20000, kMaxTrees = 3;

std::string totals_file() { return std::string(g_get_user_cache_dir()) + "/beluga/usage-v1"; }
std::string key_of(const std::string &path, bool oneFs) { return (oneFs ? "1" : "0") + path; }

// Lines: one-drive size files folders when mtime-sec mtime-nsec path (tab-separated, the path last).
void load_totals_locked()
{
    if (g_totalsLoaded) return;
    g_totalsLoaded = true;
    char *data = nullptr;
    if (!g_file_get_contents(totals_file().c_str(), &data, nullptr, nullptr)) return;
    std::string text = take(data);
    for (auto &line : split(text, '\n')) {
        size_t pos = 0;
        std::vector<std::string> f;
        for (int i = 0; i < 7; ++i) {
            size_t t = line.find('\t', pos);
            if (t == std::string::npos) break;
            f.push_back(line.substr(pos, t - pos));
            pos = t + 1;
        }
        if (f.size() != 7 || pos >= line.size()) continue;
        CachedTotal c;
        c.size = g_ascii_strtoull(f[1].c_str(), nullptr, 10);
        c.files = g_ascii_strtoull(f[2].c_str(), nullptr, 10);
        c.folders = g_ascii_strtoull(f[3].c_str(), nullptr, 10);
        c.when = g_ascii_strtoll(f[4].c_str(), nullptr, 10);
        c.mtimeSec = g_ascii_strtoll(f[5].c_str(), nullptr, 10);
        c.mtimeNsec = g_ascii_strtoll(f[6].c_str(), nullptr, 10);
        g_totals[key_of(line.substr(pos), f[0] == "1")] = c;
    }
}

} // namespace

bool cached_total(const std::string &path, bool oneFileSystem, CachedTotal &out)
{
    std::lock_guard<std::mutex> l(g_cacheMutex);
    load_totals_locked();
    auto it = g_totals.find(key_of(path, oneFileSystem));
    if (it == g_totals.end()) return false;
    out = it->second;
    return true;
}

void remember_total(const std::string &path, bool oneFileSystem, const CachedTotal &t)
{
    bool saveNow = false;
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        load_totals_locked();
        g_totals[key_of(path, oneFileSystem)] = t;
        g_totalsDirty = true;
        if (g_totals.size() > kMaxTotals) {
            // Drop the oldest tenth.
            std::vector<std::pair<gint64, std::string>> ages;
            for (auto &[k, v] : g_totals) ages.push_back({v.when, k});
            std::nth_element(ages.begin(), ages.begin() + kMaxTotals / 10, ages.end());
            for (size_t i = 0; i < kMaxTotals / 10; ++i) g_totals.erase(ages[i].second);
        }
        gint64 now = g_get_monotonic_time();
        if (now - g_lastSave > 30 * G_USEC_PER_SEC) {
            g_lastSave = now;
            saveNow = true;
        }
    }
    if (saveNow) std::thread(save_cache).detach();
}

bool is_fresh(const std::string &path, const CachedTotal &t)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return false;
    gint64 age = g_get_real_time() / G_USEC_PER_SEC - t.when;
    return st.st_mtim.tv_sec == t.mtimeSec && st.st_mtim.tv_nsec == t.mtimeNsec && age >= 0 && age < kFreshSeconds;
}

void save_cache()
{
    std::string out;
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        if (!g_totalsDirty) return;
        g_totalsDirty = false;
        for (auto &[k, c] : g_totals)
            out += printf_str("%c\t%llu\t%llu\t%llu\t%lld\t%lld\t%lld\t", k[0], (unsigned long long)c.size, (unsigned long long)c.files,
                              (unsigned long long)c.folders, (long long)c.when, (long long)c.mtimeSec, (long long)c.mtimeNsec)
                 + k.substr(1) + "\n";
    }
    ensure_dir(dirname_of(totals_file()));
    g_file_set_contents_full(totals_file().c_str(), out.data(), (gssize)out.size(), G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr);
}

bool cached_tree(const std::string &path, const Options &o, CachedTree &out)
{
    std::lock_guard<std::mutex> l(g_cacheMutex);
    for (auto it = g_trees.rbegin(); it != g_trees.rend(); ++it) {
        if (it->apparent != o.apparentSize || it->oneFs != o.oneFileSystem) continue;
        const std::string &top = it->tree->name;
        Node *n = nullptr;
        if (path == top) {
            n = it->tree.get();
        } else if (starts_with(path, top == "/" ? top : top + "/")) {
            n = it->tree.get();
            for (auto &part : split(path.substr(top.size()), '/')) {
                Node *next = nullptr;
                for (auto &c : n->children)
                    if (c->dir && c->name == part) next = c.get();
                n = next;
                if (!n) break;
            }
        }
        if (n && (n == it->tree.get() || !n->children.empty() || n->folders == 0)) {
            out = {it->tree, n, it->when};
            return true;
        }
    }
    return false;
}

void remember_tree(const std::shared_ptr<Node> &tree, const Options &o, gint64 when)
{
    {
        std::lock_guard<std::mutex> l(g_cacheMutex);
        g_trees.erase(std::remove_if(g_trees.begin(), g_trees.end(), [&](const TreeEntry &e) {
                          return e.tree->name == tree->name && e.apparent == o.apparentSize && e.oneFs == o.oneFileSystem;
                      }),
                      g_trees.end());
        g_trees.push_back({tree, o.apparentSize, o.oneFileSystem, when});
        if (g_trees.size() > kMaxTrees) g_trees.erase(g_trees.begin());
    }
    if (o.apparentSize) return;   // the remembered totals are space on disk, like the Disk Usage column
    // The folders near the top, for the Disk Usage column.
    std::vector<std::pair<const Node *, int>> stack{{tree.get(), 0}};
    while (!stack.empty()) {
        auto [n, depth] = stack.back();
        stack.pop_back();
        if (!n->dir || (n->otherFileSystem && n->children.empty())) continue;
        CachedTotal c{n->size, n->files, n->folders, when, n->mtimeSec, n->mtimeNsec};
        remember_total(n->path(), o.oneFileSystem, c);
        if (depth < 3)
            for (auto &ch : n->children) stack.push_back({ch.get(), depth + 1});
    }
}

} // namespace beluga::du
