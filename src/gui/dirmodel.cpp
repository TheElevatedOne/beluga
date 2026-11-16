// SPDX-License-Identifier: GPL-2.0-or-later
#include "dirmodel.hpp"

#include "fileops.hpp"
#include "trash.hpp"

#include <algorithm>
#include <thread>

namespace beluga {

struct ListOp {
    std::shared_ptr<DirLister::Guard> guard;
    unsigned gen;
    bool mountTried;
    DirLister *live() const
    {
        DirLister *s = guard->self;
        return s && s->generation_ == gen ? s : nullptr;
    }
};

DirLister::DirLister()
{
    store_ = g_list_store_new(BELUGA_TYPE_ITEM);
    guard_ = std::make_shared<Guard>(Guard{this});
    token_ = std::make_shared<int>(0);
}

DirLister::~DirLister()
{
    guard_->self = nullptr;
    onStarted = nullptr;
    onFinished = nullptr;
    onError = nullptr;
    onLocationGone = nullptr;
    stop();
    clear();
    if (recentHandler_) g_signal_handler_disconnect(gtk_recent_manager_get_default(), recentHandler_);
    g_object_unref(store_);
}

void DirLister::stop()
{
    if (cancellable_) {
        g_cancellable_cancel(cancellable_);
        g_object_unref(cancellable_);
        cancellable_ = nullptr;
    }
    if (search_) {
        search_->cancel();
        search_.reset();
    }
    setLoading(false);
}

void DirLister::clear()
{
    for (GFileMonitor *m : monitors_) {
        g_signal_handlers_disconnect_by_data(m, this);
        g_file_monitor_cancel(m);
        g_object_unref(m);
    }
    monitors_.clear();
    if (flushSource_) g_source_remove(flushSource_);
    flushSource_ = 0;
    pendingAdd_.clear();
    pendingRemove_.clear();
    byUri_.clear();
    g_list_store_remove_all(store_);
    token_ = std::make_shared<int>(0);
}

void DirLister::setLoading(bool on)
{
    if (loading_ == on) return;
    loading_ = on;
    if (on && onStarted) onStarted();
    if (!on && onFinished) onFinished();
}

BelugaItem *DirLister::find(const std::string &uri) const
{
    auto it = byUri_.find(uri);
    return it == byUri_.end() ? nullptr : it->second;
}

void DirLister::open(const std::string &uri)
{
    stop();
    clear();
    generation_++;
    uri_ = uri;
    cancellable_ = g_cancellable_new();
    setLoading(true);
    if (is_trash_uri(uri)) startTrash();
    else if (is_recent_uri(uri)) startRecent();
    else if (is_search_uri(uri)) startSearch();
    else {
        dir_ = Ref<GFile>(g_file_new_for_uri(uri.c_str()));
        startFolder(false);
    }
}

void DirLister::reload() { open(uri_); }

void DirLister::addItems(std::vector<BelugaItem *> &items)
{
    std::vector<gpointer> add;
    for (BelugaItem *it : items) {
        const std::string &u = item_data(it).uri;
        if (byUri_.count(u)) {
            g_object_unref(it);
            continue;
        }
        byUri_[u] = it;
        add.push_back(it);
    }
    if (!add.empty()) g_list_store_splice(store_, g_list_model_get_n_items(G_LIST_MODEL(store_)), 0, add.data(), add.size());
    for (gpointer p : add) g_object_unref(p);
    items.clear();
}

// ---- folders ---------------------------------------------------------------------

static void next_files_cb(GObject *src, GAsyncResult *res, gpointer data);

static void enumerate_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    auto *op = static_cast<ListOp *>(data);
    GError *err = nullptr;
    GFileEnumerator *en = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    DirLister *self = op->live();
    if (!self) {
        if (en) g_object_unref(en);
        g_clear_error(&err);
        delete op;
        return;
    }
    if (!en) {
        self->stop();
        if (self->onError) self->onError(err->message);
        g_error_free(err);
        delete op;
        return;
    }
    g_file_enumerator_next_files_async(en, 256, G_PRIORITY_DEFAULT, nullptr, next_files_cb, op);
}

static void next_files_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    auto *op = static_cast<ListOp *>(data);
    GFileEnumerator *en = G_FILE_ENUMERATOR(src);
    GError *err = nullptr;
    GList *infos = g_file_enumerator_next_files_finish(en, res, &err);
    DirLister *self = op->live();
    if (!self || err) {
        g_list_free_full(infos, g_object_unref);
        if (self && err && self->onError) self->onError(err->message);
        if (self) self->stop();
        g_clear_error(&err);
        g_object_unref(en);
        delete op;
        return;
    }
    if (!infos) {
        g_object_unref(en);
        delete op;
        self->stop();
        return;
    }
    std::vector<BelugaItem *> items;
    for (GList *l = infos; l; l = l->next) {
        auto *info = G_FILE_INFO(l->data);
        GFile *child = g_file_enumerator_get_child(en, info);
        items.push_back(item_new(child, info));
        g_object_unref(child);
    }
    g_list_free_full(infos, g_object_unref);
    self->addItems(items);
    g_file_enumerator_next_files_async(en, 512, G_PRIORITY_DEFAULT, nullptr, next_files_cb, op);
}

static void mount_cb(GObject *src, GAsyncResult *res, gpointer data);

void DirLister::startFolder(bool mountTried)
{
    if (!mountTried) {
        // Check reachability first so that remote locations can be mounted.
        auto *op = new ListOp{guard_, generation_, false};
        g_file_query_info_async(dir_.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                cancellable_, [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *op = static_cast<ListOp *>(data);
            GError *err = nullptr;
            GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, &err);
            DirLister *self = op->live();
            if (!self) {
                if (info) g_object_unref(info);
                g_clear_error(&err);
                delete op;
                return;
            }
            if (!info && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && self->mountOperation) {
                g_error_free(err);
                GMountOperation *mo = self->mountOperation();
                op->mountTried = true;
                g_file_mount_enclosing_volume(G_FILE(src), G_MOUNT_MOUNT_NONE, mo, self->cancellable_, mount_cb, op);
                if (mo) g_object_unref(mo);
                return;
            }
            if (!info) {
                self->stop();
                if (self->onError) self->onError(err->message);
                g_error_free(err);
                delete op;
                return;
            }
            bool isDir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY
                      || g_file_info_get_file_type(info) == G_FILE_TYPE_MOUNTABLE;
            g_object_unref(info);
            delete op;
            if (!isDir) {
                self->stop();
                if (self->onError) self->onError(_("This location is not a folder."));
                return;
            }
            self->startFolder(true);
        }, op);
        return;
    }
    watch(dir_.get());
    auto *op = new ListOp{guard_, generation_, true};
    g_file_enumerate_children_async(dir_.get(), kItemAttributes, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                    cancellable_, enumerate_cb, op);
}

static void mount_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    auto *op = static_cast<ListOp *>(data);
    GError *err = nullptr;
    bool ok = g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err);
    DirLister *self = op->live();
    delete op;
    if (!self) {
        g_clear_error(&err);
        return;
    }
    if (!ok && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
        self->stop();
        if (self->onError && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) self->onError(err->message);
        g_clear_error(&err);
        return;
    }
    g_clear_error(&err);
    self->startFolder(true);
}

void DirLister::watch(GFile *dir)
{
    GFileMonitor *m = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (!m) return;
    g_file_monitor_set_rate_limit(m, 300);
    g_signal_connect(m, "changed", G_CALLBACK(+[](GFileMonitor *, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer self) {
        static_cast<DirLister *>(self)->monitorEvent(file, other, ev);
    }), this);
    monitors_.push_back(m);
}

void DirLister::monitorEvent(GFile *file, GFile *other, GFileMonitorEvent ev)
{
    if (is_trash_uri(uri_) || is_recent_uri(uri_)) {
        pendingAdd_.insert("*");   // re-list everything
        scheduleFlush();
        return;
    }
    if (dir_ && g_file_equal(file, dir_.get())) {
        if (ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_UNMOUNTED || ev == G_FILE_MONITOR_EVENT_MOVED_OUT) {
            if (onLocationGone) onLocationGone();
        }
        return;
    }
    std::string u = file_uri(file);
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGED:
        pendingRemove_.erase(u);
        pendingAdd_.insert(u);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        pendingAdd_.erase(u);
        pendingRemove_.insert(u);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        pendingAdd_.erase(u);
        pendingRemove_.insert(u);
        if (other) {
            std::string o = file_uri(other);
            pendingRemove_.erase(o);
            pendingAdd_.insert(o);
        }
        break;
    default:
        break;
    }
    scheduleFlush();
}

void DirLister::scheduleFlush()
{
    gint64 now = g_get_monotonic_time();
    if (!flushSource_) firstPending_ = now;
    else if (now - firstPending_ > 400000) return;   // keep the pending timer, flush soon anyway
    else g_source_remove(flushSource_);
    flushSource_ = g_timeout_add(120, [](gpointer d) -> gboolean {
        auto *self = static_cast<DirLister *>(d);
        self->flushSource_ = 0;
        self->flush();
        return G_SOURCE_REMOVE;
    }, this);
}

namespace {
struct Listed {
    std::string uri;
    Ref<GFile> file;
    Ref<GFileInfo> info;
    std::string originalPath;
    gint64 deletionTime = 0;
    std::string parentPath;
    gint64 atime = 0;
};

std::vector<Listed> list_trash()
{
    std::vector<Listed> out;
    for (auto &e : trash::list()) {
        Listed l;
        l.file = Ref<GFile>(g_file_new_for_path(e.trashedPath.c_str()));
        l.uri = file_uri(l.file.get());
        l.info = Ref<GFileInfo>(g_file_query_info(l.file.get(), kItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr));
        if (!l.info) continue;
        l.originalPath = e.originalPath;
        l.deletionTime = e.deletionTime;
        out.push_back(l);
    }
    return out;
}

struct RecentEntry {
    std::string uri;
    gint64 visited;
};

std::vector<Listed> list_recent(const std::vector<RecentEntry> &entries)
{
    std::vector<Listed> out;
    for (auto &r : entries) {
        Listed l;
        l.file = Ref<GFile>(g_file_new_for_uri(r.uri.c_str()));
        l.uri = r.uri;
        l.info = Ref<GFileInfo>(g_file_query_info(l.file.get(), kItemAttributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr));
        if (!l.info) continue;
        Ref<GFile> parent(g_file_get_parent(l.file.get()));
        if (parent) l.parentPath = display_location(parent.get());
        l.atime = r.visited;
        out.push_back(l);
    }
    return out;
}

std::vector<RecentEntry> recent_entries()
{
    std::vector<RecentEntry> v;
    GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
    for (GList *l = items; l; l = l->next) {
        auto *ri = static_cast<GtkRecentInfo *>(l->data);
        const char *uri = gtk_recent_info_get_uri(ri);
        if (uri && g_str_has_prefix(uri, "file://")) {
            GDateTime *dt = gtk_recent_info_get_visited(ri);
            if (!dt) dt = gtk_recent_info_get_modified(ri);
            v.push_back({uri, dt ? g_date_time_to_unix(dt) : 0});
        }
        gtk_recent_info_unref(ri);
    }
    g_list_free(items);
    return v;
}

BelugaItem *make_item(const Listed &l)
{
    BelugaItem *it = item_new(l.file.get(), l.info.get());
    ItemData &d = item_data(it);
    d.originalPath = l.originalPath;
    d.deletionTime = l.deletionTime;
    d.parentPath = l.parentPath;
    if (!l.originalPath.empty()) {
        d.name = basename_of(l.originalPath);
        item_refresh_collation(it);
    }
    if (l.atime) d.atime = l.atime;
    return it;
}
} // namespace

// Replaces the whole listing, keeping items that are still present.
static void apply_full_list(DirLister *self, GListStore *store, std::unordered_map<std::string, BelugaItem *> &byUri,
                            std::vector<Listed> &listed, std::vector<BelugaItem *> &added)
{
    std::set<std::string> present;
    for (auto &l : listed) present.insert(l.uri);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    for (guint i = n; i-- > 0;) {
        BelugaItem *it = item_at(G_LIST_MODEL(store), i);
        if (!present.count(item_data(it).uri)) {
            byUri.erase(item_data(it).uri);
            g_list_store_remove(store, i);
        }
    }
    for (auto &l : listed)
        if (!byUri.count(l.uri)) added.push_back(make_item(l));
}

void DirLister::startTrash()
{
    for (auto &dir : trash::trash_dirs()) {
        Ref<GFile> info(g_file_new_for_path((dir + "/info").c_str()));
        watch(info.get());
    }
    if (trash::trash_dirs().empty()) {
        g_mkdir_with_parents((trash::home_trash_dir() + "/info").c_str(), 0700);
        g_mkdir_with_parents((trash::home_trash_dir() + "/files").c_str(), 0700);
        Ref<GFile> info(g_file_new_for_path((trash::home_trash_dir() + "/info").c_str()));
        watch(info.get());
    }
    auto guard = guard_;
    unsigned gen = generation_;
    std::thread([guard, gen] {
        auto listed = std::make_shared<std::vector<Listed>>(list_trash());
        run_on_main([guard, gen, listed] {
            ListOp op{guard, gen, false};
            DirLister *self = op.live();
            if (!self) return;
            std::vector<BelugaItem *> added;
            apply_full_list(self, self->store_, self->byUri_, *listed, added);
            self->addItems(added);
            self->stop();
        });
    }).detach();
}

void DirLister::startRecent()
{
    GtkRecentManager *rm = gtk_recent_manager_get_default();
    if (!recentHandler_)
        recentHandler_ = g_signal_connect_swapped(rm, "changed", G_CALLBACK(+[](gpointer self) {
            auto *s = static_cast<DirLister *>(self);
            if (is_recent_uri(s->uri_)) {
                s->pendingAdd_.insert("*");
                s->scheduleFlush();
            }
        }), this);
    auto entries = recent_entries();
    auto guard = guard_;
    unsigned gen = generation_;
    std::thread([guard, gen, entries] {
        auto listed = std::make_shared<std::vector<Listed>>(list_recent(entries));
        run_on_main([guard, gen, listed] {
            ListOp op{guard, gen, false};
            DirLister *self = op.live();
            if (!self) return;
            std::vector<BelugaItem *> added;
            apply_full_list(self, self->store_, self->byUri_, *listed, added);
            self->addItems(added);
            self->stop();
        });
    }).detach();
}

void DirLister::startSearch()
{
    SearchQuery q;
    SearchQuery::fromUri(uri_, q);
    auto guard = guard_;
    unsigned gen = generation_;
    search_ = SearchJob::start(
        q,
        [guard, gen](std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&results) {
            ListOp op{guard, gen, false};
            DirLister *self = op.live();
            if (!self) return;
            std::vector<BelugaItem *> items;
            for (auto &[f, info] : results) {
                BelugaItem *it = item_new(f.get(), info.get());
                Ref<GFile> parent(g_file_get_parent(f.get()));
                if (parent) item_data(it).parentPath = display_location(parent.get());
                items.push_back(it);
            }
            self->addItems(items);
        },
        [guard, gen] {
            ListOp op{guard, gen, false};
            if (DirLister *self = op.live()) {
                self->search_.reset();
                self->setLoading(false);
            }
        });
}

void DirLister::flush()
{
    if (pendingAdd_.count("*")) {
        pendingAdd_.clear();
        pendingRemove_.clear();
        auto guard = guard_;
        unsigned gen = generation_;
        bool trashMode = is_trash_uri(uri_);
        std::vector<RecentEntry> entries;
        if (!trashMode) entries = recent_entries();
        std::thread([guard, gen, trashMode, entries] {
            auto listed = std::make_shared<std::vector<Listed>>(trashMode ? list_trash() : list_recent(entries));
            run_on_main([guard, gen, listed] {
                ListOp op{guard, gen, false};
                DirLister *self = op.live();
                if (!self) return;
                std::vector<BelugaItem *> added;
                apply_full_list(self, self->store_, self->byUri_, *listed, added);
                self->addItems(added);
            });
        }).detach();
        return;
    }
    if (!pendingRemove_.empty()) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(store_));
        for (guint i = n; i-- > 0 && !pendingRemove_.empty();) {
            BelugaItem *it = item_at(G_LIST_MODEL(store_), i);
            auto f = pendingRemove_.find(item_data(it).uri);
            if (f != pendingRemove_.end()) {
                byUri_.erase(item_data(it).uri);
                pendingRemove_.erase(f);
                g_list_store_remove(store_, i);
            }
        }
        pendingRemove_.clear();
    }
    if (pendingAdd_.empty()) return;
    std::vector<std::string> uris(pendingAdd_.begin(), pendingAdd_.end());
    pendingAdd_.clear();
    auto guard = guard_;
    unsigned gen = generation_;
    std::thread([guard, gen, uris] {
        auto results = std::make_shared<std::vector<std::pair<std::string, Ref<GFileInfo>>>>();
        for (auto &u : uris) {
            Ref<GFile> f(g_file_new_for_uri(u.c_str()));
            results->emplace_back(u, Ref<GFileInfo>(g_file_query_info(f.get(), kItemAttributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr)));
        }
        run_on_main([guard, gen, results] {
            ListOp op{guard, gen, false};
            if (DirLister *self = op.live()) self->applyQueried(*results);
        });
    }).detach();
}

void DirLister::applyQueried(std::vector<std::pair<std::string, Ref<GFileInfo>>> &results)
{
    std::vector<BelugaItem *> added;
    std::unordered_map<BelugaItem *, int> positions;
    bool needPositions = false;
    for (auto &[u, info] : results)
        if (byUri_.count(u)) needPositions = true;
    if (needPositions) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(store_));
        for (guint i = 0; i < n; ++i) positions[item_at(G_LIST_MODEL(store_), i)] = (int)i;
    }
    std::vector<guint> removeAt;
    for (auto &[u, info] : results) {
        auto it = byUri_.find(u);
        if (it == byUri_.end()) {
            if (!info) continue;
            Ref<GFile> f(g_file_new_for_uri(u.c_str()));
            added.push_back(item_new(f.get(), info.get()));
            continue;
        }
        BelugaItem *item = it->second;
        auto p = positions.find(item);
        if (p == positions.end()) continue;
        if (!info) {
            removeAt.push_back(p->second);
            byUri_.erase(it);
            continue;
        }
        ItemData &d = item_data(item);
        gint64 oldMtime = d.mtime;
        guint64 oldSize = d.size;
        item_set_info(item, info.get());
        if (d.mtime != oldMtime || d.size != oldSize) {
            d.thumb.reset();
            d.thumbPixels = 0;
            d.thumbFailed = false;
            d.dirCount = -2;
        }
        gpointer obj = g_object_ref(item);
        g_list_store_splice(store_, p->second, 1, &obj, 1);   // lets sorting see the change
        item_emit_changed(item);
        g_object_unref(obj);
    }
    std::sort(removeAt.rbegin(), removeAt.rend());
    for (guint pos : removeAt) g_list_store_remove(store_, pos);
    addItems(added);
}

} // namespace beluga
