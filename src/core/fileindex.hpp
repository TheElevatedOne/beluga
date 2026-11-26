// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: an index of the names under the home folder, so that searching by name answers at once, like
// Everything does. Hidden files and folders are indexed too (queries leave them out unless asked); other drives
// mounted inside and the cache folder aren't (see skipped()).
//
// It stays correct without a daemon: adding, removing or renaming an entry changes its folder's time, so a
// refresh only lstat()s the folders and re-reads the ones whose time changed (the entry types come from
// readdir, no stat per file). Every hit is checked with lstat() before it's shown, so deleted files never
// appear. The index is saved in ~/.cache/beluga and loaded at start.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace beluga::fileindex {

// Loads the saved index of `root` (the home folder when empty) and refreshes it in the background (once,
// at start). Does nothing when the setting is off.
void start(const std::string &root = {});
// Whether the index is loaded or was read in full once.
bool ready();
// Whether a search of this folder can use the index.
bool covers(const std::string &folder);

// Folders below `folder` that the index leaves out and searches read from the disk: other drives mounted
// there and the cache folder (~/.cache). Known after the first refresh.
std::vector<std::string> skipped(const std::string &folder);

struct Hit {
    std::string path;
    bool dir;
};
// Entries under folder whose name matches, from the index as it is. match(name, casefoldedName, isDir).
// Without hidden, entries whose name or a folder between `folder` and them starts with a dot are left out.
using Matcher = std::function<bool(const char *name, const char *folded, bool dir)>;
std::vector<Hit> query(const std::string &folder, const Matcher &match, bool hidden = false);
// The same for the entries directly inside the given folders (below `folder`).
std::vector<Hit> queryFolders(const std::string &folder, const std::vector<std::string> &folders, const Matcher &match,
                              bool hidden = false);

// Brings the index under folder up to date and returns the folders that were (re-)read. Thread safe.
std::vector<std::string> refresh(const std::string &folder, const std::atomic<bool> *cancel = nullptr);

// Writes the index if it changed (from any thread).
void save();

struct Stats {
    size_t folders = 0, entries = 0, bytes = 0;
};
Stats stats();

} // namespace beluga::fileindex
