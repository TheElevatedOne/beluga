// SPDX-License-Identifier: GPL-2.0-or-later
#include "diskusage.hpp"

#include <algorithm>
#include <mutex>
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

} // namespace beluga::du
