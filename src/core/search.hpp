// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: file search without an indexer. Walks the folder tree in a worker
// thread and matches names (and optionally text content), file type and age.
#pragma once

#include "util.hpp"

#include <sys/stat.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace beluga {

// Attributes queried for every listed item (folder listings and search results).
extern const char *kItemAttributes;

struct SearchQuery {
    std::string text;
    std::string rootUri;          // folder to search in
    bool content = false;
    bool includeHidden = false;
    std::string type;             // "", folders, documents, images, audio, videos, archives
    int modifiedWithinDays = 0;   // 0: any time
    guint64 minSize = 0;          // bytes, 0: any

    std::string toUri() const;
    static bool fromUri(const std::string &uri, SearchQuery &out);
    std::string title() const;
};

std::string search_uri_for_type(const std::string &type);
bool type_matches(const std::string &type, const std::string &contentType, bool isDir);
const char *type_title(const std::string &type);

class SearchJob : public std::enable_shared_from_this<SearchJob> {
public:
    using Ptr = std::shared_ptr<SearchJob>;
    using ResultsFn = std::function<void(std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&)>;
    static Ptr start(const SearchQuery &q, ResultsFn onResults, std::function<void()> onDone);
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
    int scannedFolders() const { return folders_; }

private:
    void run();
    void runLocal(const std::string &rootPath, bool finish = true);   // the parallel Walker, for local folders
    void runIndexed(const std::string &rootPath); // the name index (fileindex), for plain name/type searches
    void runGio();                                 // one GIO walk, for remote locations
    bool matches(GFile *f, GFileInfo *info);
    bool matchesLocal(const std::string &path, const char *name, const struct stat &st);
    bool contentMatches(GFile *f);
    SearchQuery q_;
    std::string needle_;
    ResultsFn onResults_;
    std::function<void()> onDone_;
    std::atomic<bool> cancelled_{false};
    std::atomic<int> folders_{0};
};

} // namespace beluga
