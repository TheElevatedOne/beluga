// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: a fast parallel directory walker for local folders (disk usage, folder sizes, search).
// It works like dust's: one task per folder on a pool of threads, lstat on every entry, nothing
// followed through symbolic links. Plain POSIX calls instead of GIO, which costs several times more
// per file.
#pragma once

#include <sys/stat.h>

#include <atomic>
#include <functional>
#include <string>

namespace beluga {

struct WalkEntry {
    const std::string &dir;    // the folder holding the entry, without a trailing slash
    const char *name;
    const struct stat &st;     // lstat of the entry
    void *dirData;             // what onEntry returned for the folder (rootData for the top folder)
    bool otherFileSystem;      // a folder on another file system than the top folder
};

class Walker {
public:
    struct Options {
        int threads = 0;              // 0: one per CPU core (at most 16); 1: in the calling thread
        bool oneFileSystem = false;   // don't descend into folders on other file systems
        bool skipHidden = false;      // skip entries whose name starts with a dot
    };

    // Called for every entry of every folder, from the worker threads. All entries of one folder
    // come from the same thread, one after another. For a folder, return true to walk into it and
    // set *childData to what its own entries receive as dirData.
    std::function<bool(const WalkEntry &e, void **childData)> onEntry;
    // Called after the last entry of a folder, from the same thread, with that folder's data.
    std::function<void(void *dirData)> onFolderDone;
    // A folder that couldn't be read (errno) and its data, from the worker threads. onFolderDone
    // follows for it as for any other folder.
    std::function<void(const std::string &path, int err, void *dirData)> onError;

    std::atomic<bool> cancelled{false};
    void cancel() { cancelled = true; }

    // Walks the folder at path and returns when every folder was read or the walk was cancelled.
    // /proc, /sys and /dev are never entered.
    void run(const std::string &path, void *rootData, const Options &options);
};

} // namespace beluga
