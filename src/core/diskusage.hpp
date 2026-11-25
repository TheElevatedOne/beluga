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
              std::function<bool()> stop = nullptr);

} // namespace beluga::du
