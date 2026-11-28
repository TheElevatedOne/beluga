// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: lists one location into a GListStore of BelugaItem and keeps it up to
// date with file monitors. Locations: folders (any GIO URI), trash:///, recent:///
// and search:?... result sets.
#pragma once

#include "item.hpp"
#include "search.hpp"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace beluga {

class DirLister {
public:
    DirLister();
    ~DirLister();
    DirLister(const DirLister &) = delete;
    DirLister &operator=(const DirLister &) = delete;

    GListModel *model() const { return G_LIST_MODEL(store_); }
    void open(const std::string &uri);
    void reload();
    void stop();
    const std::string &uri() const { return uri_; }
    bool loading() const { return loading_; }
    BelugaItem *find(const std::string &uri) const;
    // Token for async helpers (previews, folder sizes): expires on reload/destroy.
    std::weak_ptr<void> token() const { return token_; }

    std::function<void()> onStarted;
    std::function<void()> onFinished;
    std::function<void(const std::string &)> onError;
    std::function<void()> onLocationGone;
    std::function<GMountOperation *()> mountOperation;   // for remote locations

    struct Guard {
        DirLister *self;
    };

    // Used by the GIO callbacks.
    void startFolder(bool mountTried);
    void addItems(std::vector<BelugaItem *> &items);

private:
    void clear();
    void startTrash();
    void startArchive();
    void startRecent();
    void addNetworkPlaces();
    void startSearch();
    void setLoading(bool on);
    void monitorEvent(GFile *file, GFile *other, GFileMonitorEvent ev);
    void scheduleFlush();
    void flush();
    void applyQueried(std::vector<std::pair<std::string, Ref<GFileInfo>>> &results);
    void watch(GFile *dir);

    friend struct ListOp;
    GListStore *store_;
    std::string uri_;
    gint64 loadStart_ = 0;
    Ref<GFile> dir_;
    GCancellable *cancellable_ = nullptr;
    std::vector<GFileMonitor *> monitors_;
    std::unordered_map<std::string, BelugaItem *> byUri_;
    std::set<std::string> pendingAdd_, pendingRemove_;
    guint flushSource_ = 0;
    gint64 firstPending_ = 0;
    bool loading_ = false;
    unsigned generation_ = 0;
    std::shared_ptr<Guard> guard_;
    std::shared_ptr<int> token_;
    SearchJob::Ptr search_;
    gulong recentHandler_ = 0;
};

} // namespace beluga
