// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: what takes up the space in a folder, like dust (https://github.com/bootandy/dust): a tree
// of the folder's contents with their sizes, biggest first, read with the parallel Walker.
#pragma once

#include "walker.hpp"

#include <glib.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace beluga::du {

struct Node {
    std::string name;                 // the full path for the top folder; "" for a "smaller files" row
    Node *parent = nullptr;
    bool dir = false;
    bool otherFileSystem = false;     // a folder on another file system that wasn't entered
    bool unreadable = false;          // a folder whose contents couldn't be read
    guint64 size = 0;                 // including everything inside
    guint64 files = 0;                // files inside (1 for a file; the count for a "smaller files" row)
    guint64 folders = 0;              // folders inside, at any depth
    gint64 mtimeSec = 0, mtimeNsec = 0;   // a folder's own time when it was read (for the cache)
    std::vector<std::unique_ptr<Node>> children;   // biggest first

    bool rest() const { return !dir && name.empty(); }
    std::string path() const;         // "" for a "smaller files" row

    // While scanning: the biggest files of the folder (a min-heap) and what the others add up to.
    std::vector<std::unique_ptr<Node>> topFiles;
    guint64 restSize = 0, restFiles = 0;
};

struct Options {
    bool apparentSize = false;        // file sizes instead of the disk space they use (dust -s)
    bool oneFileSystem = true;        // don't count other drives mounted inside (dust -x)
    int filesPerFolder = 20;          // biggest files listed per folder; the rest is one row
    int threads = 0;                  // 0: one per core
};

class Scanner {
public:
    // Progress while run() works, readable from any thread.
    std::atomic<guint64> files{0}, folders{0}, bytes{0}, errors{0};

    void cancel() { walker_.cancel(); }
    bool cancelled() const { return walker_.cancelled; }

    // Blocks until the scan is done. nullptr when path isn't a folder or the scan was cancelled.
    std::unique_ptr<Node> run(const std::string &path, const Options &options);

private:
    Walker walker_;
};

// The size of a folder's contents without building a tree (folder sizes, Properties).
struct Totals {
    guint64 size = 0, files = 0, folders = 0;
};
// maxDepth < 0: no limit. stop is polled now and then and ends the walk early when it returns true.
Totals totals(const std::string &path, bool apparentSize, int threads, int maxDepth = -1,
              std::function<bool()> stop = nullptr, bool oneFileSystem = false);

// ---- cache --------------------------------------------------------------------------------------
// Scanning is cheap when the disk cache is warm, but not free, and not after a reboot. So the last results
// are remembered: folder totals (space on disk) in ~/.cache/beluga/usage-v1, and the Disk Usage window's
// latest trees in memory. A remembered total counts as current while the folder's own time is unchanged and
// it's younger than kFreshSeconds; older ones are shown at once and read again in the background.

constexpr gint64 kFreshSeconds = 600;

struct CachedTotal {
    guint64 size = 0, files = 0, folders = 0;
    gint64 when = 0;                      // unix time of the scan
    gint64 mtimeSec = 0, mtimeNsec = 0;   // the folder's time then
};
bool cached_total(const std::string &path, bool oneFileSystem, CachedTotal &out);
void remember_total(const std::string &path, bool oneFileSystem, const CachedTotal &t);
// Whether a remembered total can be used without reading the folder again.
bool is_fresh(const std::string &path, const CachedTotal &t);

struct CachedTree {
    std::shared_ptr<Node> tree;   // the whole scan
    Node *node = nullptr;         // the folder asked for, inside it
    gint64 when = 0;
};
// The newest remembered scan with the same options whose top is path or one of its parents.
bool cached_tree(const std::string &path, const Options &options, CachedTree &out);
// Remembers a scan (the last few) and the totals of its folders near the top.
void remember_tree(const std::shared_ptr<Node> &tree, const Options &options, gint64 when);
// Writes the remembered totals if they changed.
void save_cache();

} // namespace beluga::du
