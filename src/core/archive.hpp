// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: browsing archives (zip, tar.*, 7z, rar, ISO 9660, cpio) like folders, with libarchive. Read only:
// files come out by extracting them.
#pragma once

#include <glib.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace beluga::archive {

// Locations inside an archive: beluga-archive://<archive path, escaped>/<path inside>. A scheme of our own, so
// GIO (and gvfs, which owns archive://) never handles these.
extern const char *const kScheme;

bool is_archive_uri(const std::string &uri);
// inner: the path inside the archive without a leading slash, "" for its top.
std::string make_uri(const std::string &archivePath, const std::string &inner);
bool parse_uri(const std::string &uri, std::string &archivePath, std::string &inner);
// "/home/me/x.zip/folder", for the location bar and Copy Location.
std::string display_path(const std::string &uri);
// One level up; for the top of an archive, the folder that holds the archive file.
std::string parent_uri(const std::string &uri);
// A local path that runs into an archive ("/home/me/x.zip/folder") as an archive URI, else "".
std::string resolve_path(const std::string &path);
// Whether a file of this type can be opened as a folder.
bool can_browse(const std::string &contentType, const std::string &name);

struct Entry {
    std::string name;
    bool dir = false;
    bool link = false;
    bool encrypted = false;
    guint64 size = 0;
    gint64 mtime = 0;
    guint32 mode = 0;
    std::string linkTarget;
};

struct Index {
    // Folder path inside the archive ("" for the top) → its entries.
    std::map<std::string, std::vector<Entry>> folders;
    bool encrypted = false;   // some entries can't be read without a password
    const std::vector<Entry> *children(const std::string &folder) const;
};

// The archive's entries, read once and cached while the file's size and time stay the same. Thread safe.
std::shared_ptr<const Index> load(const std::string &archivePath, std::string *error);

// Extracts the entries at the given paths (files, or folders with everything in them) into destDir, as
// destDir/<name>. destDir should be a new, empty folder: nothing is written outside it, symbolic links are
// made last, and entries with ".." or absolute paths are skipped.
bool extract(const std::string &archivePath, const std::vector<std::string> &inner, const std::string &destDir,
             const std::atomic<bool> *cancel, std::string *error);

// Extracts one file into Beluga's cache folder (to open it with an application) and returns its path.
std::string extract_to_cache(const std::string &uri, std::string *error);

} // namespace beluga::archive
