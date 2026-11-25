// SPDX-License-Identifier: GPL-2.0-or-later
#include "fileops.hpp"

#include "undo.hpp"

#include <glib/gstdio.h>

#include <chrono>
#include <condition_variable>
#include <thread>

namespace beluga {

namespace {
JobUi *g_ui = nullptr;
int g_nextId = 1;
const char *kInfoAttrs = G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME ","
                         G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                         G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
                         G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_ID_FILESYSTEM ","
                         G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK;

std::string name_of(GFile *f)
{
    char *n = g_file_get_basename(f);
    return n ? take(n) : file_uri(f);
}

std::string parse_name(GFile *f) { return take(g_file_get_parse_name(f)); }

// The .trashinfo file of an item at the top of a trash "files" folder (the Trash view lists those).
// Deleting the item there or moving it out must remove its info file too.
std::string trash_info_of(GFile *f)
{
    std::string p = take(g_file_get_path(f));
    trash::Entry e;
    if (p.empty() || !trash::is_in_trash(p) || !trash::entry_for_trashed_path(p, e)) return {};
    return e.infoPath;
}

void drop_trash_info(const std::string &info, GFile *f)
{
    if (!info.empty() && !g_file_query_exists(f, nullptr)) g_unlink(info.c_str());
}

} // namespace

void set_job_ui(JobUi *ui) { g_ui = ui; }

void run_on_main(std::function<void()> fn)
{
    auto *p = new std::function<void()>(std::move(fn));
    g_main_context_invoke_full(
        nullptr, G_PRIORITY_DEFAULT,
        [](gpointer d) -> gboolean {
            (*static_cast<std::function<void()> *>(d))();
            return G_SOURCE_REMOVE;
        },
        p, [](gpointer d) { delete static_cast<std::function<void()> *>(d); });
}

FileJob::FileJob(OpKind k) : kind_(k), id_(g_nextId++)
{
    cancellable_ = g_cancellable_new();
}

static std::vector<OpItem> to_dir(const std::vector<Ref<GFile>> &sources, GFile *destDir)
{
    std::vector<OpItem> items;
    for (auto &s : sources) {
        char *base = g_file_get_basename(s.get());
        if (!base) continue;
        items.push_back({s, Ref<GFile>(g_file_get_child(destDir, base))});
        g_free(base);
    }
    return items;
}

FileJob::Ptr FileJob::copy(const std::vector<Ref<GFile>> &sources, GFile *destDir)
{
    Ptr j(new FileJob(OpKind::Copy));
    j->items_ = to_dir(sources, destDir);
    return j;
}

FileJob::Ptr FileJob::move(const std::vector<Ref<GFile>> &sources, GFile *destDir)
{
    Ptr j(new FileJob(OpKind::Move));
    j->items_ = to_dir(sources, destDir);
    return j;
}

FileJob::Ptr FileJob::link(const std::vector<Ref<GFile>> &sources, GFile *destDir)
{
    Ptr j(new FileJob(OpKind::Link));
    j->items_ = to_dir(sources, destDir);
    return j;
}

FileJob::Ptr FileJob::copyPairs(const std::vector<OpItem> &items)
{
    Ptr j(new FileJob(OpKind::Copy));
    j->items_ = items;
    return j;
}

FileJob::Ptr FileJob::movePairs(const std::vector<OpItem> &items)
{
    Ptr j(new FileJob(OpKind::Move));
    j->items_ = items;
    return j;
}

FileJob::Ptr FileJob::trash(const std::vector<Ref<GFile>> &files)
{
    Ptr j(new FileJob(OpKind::Trash));
    for (auto &f : files) j->items_.push_back({f, {}});
    return j;
}

FileJob::Ptr FileJob::remove(const std::vector<Ref<GFile>> &files)
{
    Ptr j(new FileJob(OpKind::Delete));
    for (auto &f : files) j->items_.push_back({f, {}});
    j->undoable = false;
    return j;
}

FileJob::Ptr FileJob::restore(const std::vector<trash::Entry> &entries)
{
    Ptr j(new FileJob(OpKind::Restore));
    j->restoreEntries_ = entries;
    return j;
}

FileJob::Ptr FileJob::emptyTrash()
{
    Ptr j(new FileJob(OpKind::EmptyTrash));
    j->undoable = false;
    return j;
}

void FileJob::start()
{
    if (running_ || finished_) return;
    running_ = true;
    self_ = shared_from_this();
    startTime_ = g_get_monotonic_time();
    JobManager::instance().add(self_);
    std::thread([self = self_] {
        self->run();
        run_on_main([self] {
            self->running_ = false;
            self->finished_ = true;
            if (self->onFinished) self->onFinished(*self);
            UndoManager::instance().pushFromJob(*self);
            JobManager::instance().finished(self.get());
            g_object_unref(self->cancellable_);
            self->cancellable_ = nullptr;
            self->self_.reset();
        });
    }).detach();
}

void FileJob::cancel()
{
    cancelled_ = true;
    if (cancellable_) g_cancellable_cancel(cancellable_);
}

std::string FileJob::title() const
{
    int n = kind_ == OpKind::Restore ? (int)restoreEntries_.size() : (int)items_.size();
    std::string target;
    if (!items_.empty() && items_[0].dest) {
        GFile *parent = g_file_get_parent(items_[0].dest.get());
        if (parent) {
            target = name_of(parent);
            g_object_unref(parent);
        }
    }
    std::string what = n == 1 && !items_.empty() ? "“" + name_of(items_[0].source.get()) + "”" : format_items(n);
    switch (kind_) {
    case OpKind::Copy: return printf_str(_("Copying %s to “%s”"), what.c_str(), target.c_str());
    case OpKind::Move: return printf_str(_("Moving %s to “%s”"), what.c_str(), target.c_str());
    case OpKind::Link: return printf_str(_("Linking %s in “%s”"), what.c_str(), target.c_str());
    case OpKind::Trash: return printf_str(_("Moving %s to the trash"), what.c_str());
    case OpKind::Delete: return printf_str(_("Deleting %s"), what.c_str());
    case OpKind::Restore: return printf_str(_("Restoring %s"), format_items(n).c_str());
    case OpKind::EmptyTrash: return _("Emptying the trash");
    }
    return {};
}

std::string FileJob::currentText() const
{
    std::lock_guard<std::mutex> l(mutex_);
    return current_;
}

void FileJob::setCurrent(const std::string &s)
{
    std::lock_guard<std::mutex> l(mutex_);
    current_ = s;
}

double FileJob::fraction() const
{
    if ((kind_ == OpKind::Copy || kind_ == OpKind::Move) && totalBytes_ > 0)
        return std::min(1.0, (double)doneBytes_ / (double)totalBytes_);
    if (totalFiles_ > 0) return std::min(1.0, (double)doneFiles_ / (double)totalFiles_);
    return 0.0;
}

double FileJob::bytesPerSecond() const
{
    double secs = (g_get_monotonic_time() - startTime_) / 1e6;
    return secs > 0.2 ? doneBytes_ / secs : 0.0;
}

guint64 FileJob::scanFile(GFile *f, int depth)
{
    if (cancelled_) return 0;
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, nullptr);
    if (!info) return 0;
    guint64 size = 0;
    totalFiles_++;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY && depth < 256) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, nullptr);
        if (en) {
            GFileInfo *ci;
            while ((ci = g_file_enumerator_next_file(en, cancellable_, nullptr))) {
                GFile *child = g_file_get_child(f, g_file_info_get_name(ci));
                size += scanFile(child, depth + 1);
                g_object_unref(child);
                g_object_unref(ci);
            }
            g_object_unref(en);
        }
    } else {
        size = g_file_info_get_size(info);
    }
    g_object_unref(info);
    return size;
}

void FileJob::scan()
{
    setCurrent(_("Counting files…"));
    switch (kind_) {
    case OpKind::Copy:
    case OpKind::Move:
        for (auto &it : items_) {
            if (kind_ == OpKind::Move) {
                // Same file system: a rename, no need to count bytes.
                GFile *parent = g_file_get_parent(it.dest.get());
                GFileInfo *a = g_file_query_info(it.source.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
                GFileInfo *b = parent ? g_file_query_info(parent, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, nullptr, nullptr) : nullptr;
                bool same = a && b && g_strcmp0(g_file_info_get_attribute_string(a, G_FILE_ATTRIBUTE_ID_FILESYSTEM),
                                                g_file_info_get_attribute_string(b, G_FILE_ATTRIBUTE_ID_FILESYSTEM)) == 0;
                if (a) g_object_unref(a);
                if (b) g_object_unref(b);
                if (parent) g_object_unref(parent);
                if (same) {
                    totalFiles_++;
                    continue;
                }
            }
            totalBytes_ += scanFile(it.source.get(), 0);
        }
        break;
    case OpKind::Delete:
        for (auto &it : items_) scanFile(it.source.get(), 0);
        break;
    case OpKind::Trash:
    case OpKind::Link:
        totalFiles_ = (int)items_.size();
        break;
    case OpKind::Restore:
        totalFiles_ = (int)restoreEntries_.size();
        break;
    case OpKind::EmptyTrash:
        restoreEntries_ = trash::list();
        totalFiles_ = (int)restoreEntries_.size();
        break;
    }
}

ConflictReply FileJob::askConflict(ConflictRequest req)
{
    ConflictReply skip;
    skip.choice = ConflictChoice::Skip;
    if (!g_ui) return skip;
    struct Wait {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        ConflictReply reply;
    };
    auto w = std::make_shared<Wait>();
    auto self = shared_from_this();
    run_on_main([self, req, w] {
        g_ui->conflict(*self, req, [w](ConflictReply r) {
            std::lock_guard<std::mutex> l(w->m);
            w->reply = r;
            w->done = true;
            w->cv.notify_all();
        });
    });
    std::unique_lock<std::mutex> l(w->m);
    while (!w->done) w->cv.wait_for(l, std::chrono::milliseconds(200));
    return w->reply;
}

ErrorChoice FileJob::askError(const std::string &msg, bool canSkip)
{
    if (!g_ui) return canSkip ? ErrorChoice::Skip : ErrorChoice::Cancel;
    struct Wait {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        ErrorChoice reply = ErrorChoice::Cancel;
    };
    auto w = std::make_shared<Wait>();
    auto self = shared_from_this();
    run_on_main([self, msg, canSkip, w] {
        g_ui->error(*self, msg, canSkip, [w](ErrorChoice r) {
            std::lock_guard<std::mutex> l(w->m);
            w->reply = r;
            w->done = true;
            w->cv.notify_all();
        });
    });
    std::unique_lock<std::mutex> l(w->m);
    while (!w->done) w->cv.wait_for(l, std::chrono::milliseconds(200));
    return w->reply;
}

ErrorChoice FileJob::handleError(GError *err, GFile *f)
{
    if (!err) return ErrorChoice::Skip;
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || cancelled_) {
        g_error_free(err);
        return ErrorChoice::Cancel;
    }
    std::string msg = f ? printf_str(_("“%s”: %s"), parse_name(f).c_str(), err->message) : std::string(err->message);
    g_error_free(err);
    {
        std::lock_guard<std::mutex> l(mutex_);
        errors_.push_back(msg);
    }
    if (skipAllErrors_) return ErrorChoice::Skip;
    ErrorChoice c = askError(msg, true);
    if (c == ErrorChoice::SkipAll) {
        skipAllErrors_ = true;
        return ErrorChoice::Skip;
    }
    if (c == ErrorChoice::Cancel) cancel();
    return c;
}

namespace {
struct ProgressData {
    std::atomic<guint64> *done;
    guint64 base;
};
void copy_progress(goffset current, goffset, gpointer data)
{
    auto *pd = static_cast<ProgressData *>(data);
    *pd->done = pd->base + (guint64)current;
}
} // namespace

bool FileJob::copyOne(GFile *srcIn, GFile *destIn, bool isTop, bool move, OpItem *result)
{
    Ref<GFile> src = Ref<GFile>::share(srcIn);
    Ref<GFile> dest = Ref<GFile>::share(destIn);
    bool merge = false;
    bool overwrite = false;
    for (;;) {
        if (cancelled_) return false;
        setCurrent(parse_name(src.get()));
        if (g_file_equal(src.get(), dest.get())) {
            if (move) {
                doneFiles_++;
                return false;
            }
            Ref<GFile> parent(g_file_get_parent(dest.get()));
            dest.reset(g_file_get_child(parent.get(), duplicate_name(parent.get(), name_of(src.get()), false).c_str()));
            continue;
        }
        if (g_file_has_prefix(dest.get(), src.get())) {
            GError *e = g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s",
                                    move ? _("A folder cannot be moved into itself.") : _("A folder cannot be copied into itself."));
            handleError(e, src.get());
            return false;
        }
        GError *err = nullptr;
        Ref<GFileInfo> srcInfo(g_file_query_info(src.get(), kInfoAttrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, &err));
        if (!srcInfo) {
            ErrorChoice c = handleError(err, src.get());
            if (c == ErrorChoice::Retry) continue;
            return false;
        }
        bool isDir = g_file_info_get_file_type(srcInfo.get()) == G_FILE_TYPE_DIRECTORY;

        if (move && !merge) {
            GFileCopyFlags mf = GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE
                                               | (overwrite ? G_FILE_COPY_OVERWRITE : 0));
            if (g_file_move(src.get(), dest.get(), mf, cancellable_, nullptr, nullptr, &err)) {
                doneFiles_++;
                if (result) *result = {src, dest};
                return true;
            }
            bool fallback = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
                         || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)
                         || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE);
            bool exists = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS) || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY);
            if (!fallback && !exists) {
                ErrorChoice c = handleError(err, src.get());
                if (c == ErrorChoice::Retry) continue;
                return false;
            }
            g_clear_error(&err);
        }

        Ref<GFileInfo> destInfo(g_file_query_info(dest.get(), kInfoAttrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, nullptr));
        if (destInfo && !merge && !overwrite) {
            bool destDir = g_file_info_get_file_type(destInfo.get()) == G_FILE_TYPE_DIRECTORY;
            ConflictRequest req;
            req.source = src;
            req.dest = dest;
            req.sourceInfo = srcInfo;
            req.destInfo = destInfo;
            req.folderConflict = isDir && destDir;
            req.multiple = items_.size() > 1 || !isTop;
            Ref<GFile> parent(g_file_get_parent(dest.get()));
            req.suggestedName = unique_name(parent.get(), name_of(dest.get()));
            ConflictReply rep;
            if (req.folderConflict && dirApplyAll_) rep = dirAll_;
            else if (!req.folderConflict && fileApplyAll_) rep = fileAll_;
            else {
                rep = askConflict(req);
                if (rep.applyToAll) {
                    ConflictReply stored = rep;
                    stored.newName.clear();   // later conflicts get their own free name
                    if (req.folderConflict) { dirApplyAll_ = true; dirAll_ = stored; }
                    else { fileApplyAll_ = true; fileAll_ = stored; }
                }
            }
            switch (rep.choice) {
            case ConflictChoice::Cancel:
                cancel();
                return false;
            case ConflictChoice::Skip:
                doneBytes_ += isDir ? 0 : g_file_info_get_size(srcInfo.get());
                fileBase_ = doneBytes_.load();
                doneFiles_++;
                return false;
            case ConflictChoice::Rename: {
                std::string nn = rep.newName.empty() ? req.suggestedName : rep.newName;
                dest.reset(g_file_get_child(parent.get(), nn.c_str()));
                continue;
            }
            case ConflictChoice::Merge:
                merge = true;
                continue;
            case ConflictChoice::Overwrite:
                if (isDir != destDir || (isDir && destDir)) {
                    if (!(isDir && destDir) && !trash::delete_recursive(dest.get(), cancellable_, &err)) {
                        ErrorChoice c = handleError(err, dest.get());
                        if (c == ErrorChoice::Retry) continue;
                        return false;
                    }
                    if (isDir && destDir) merge = true;
                }
                overwrite = true;
                continue;
            }
        }

        if (isDir) {
            if (!copyTree(src.get(), dest.get(), srcInfo.get(), move)) {
                if (cancelled_) return false;
            }
            if (result) *result = {src, dest};
            return !cancelled_;
        }

        GFileCopyFlags flags = GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA);
        if (overwrite) flags = GFileCopyFlags(flags | G_FILE_COPY_OVERWRITE);
        ProgressData pd{&doneBytes_, fileBase_.load()};
        bool ok = g_file_copy(src.get(), dest.get(), flags, cancellable_, copy_progress, &pd, &err);
        if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
            // Some file systems refuse metadata; retry without.
            g_clear_error(&err);
            flags = GFileCopyFlags(flags & ~G_FILE_COPY_ALL_METADATA);
            ok = g_file_copy(src.get(), dest.get(), flags, cancellable_, copy_progress, &pd, &err);
        }
        if (!ok) {
            if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                g_clear_error(&err);
                continue;
            }
            doneBytes_ = fileBase_.load();
            ErrorChoice c = handleError(err, src.get());
            if (c == ErrorChoice::Retry) continue;
            return false;
        }
        fileBase_ += g_file_info_get_size(srcInfo.get());
        doneBytes_ = fileBase_.load();
        doneFiles_++;
        if (move) {
            GError *de = nullptr;
            if (!g_file_delete(src.get(), cancellable_, &de)) handleError(de, src.get());
        }
        if (result) *result = {src, dest};
        return true;
    }
}

bool FileJob::copyTree(GFile *src, GFile *dest, GFileInfo *info, bool move)
{
    GError *err = nullptr;
    for (;;) {
        if (g_file_make_directory(dest, cancellable_, &err)) break;
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_clear_error(&err);
            break;   // merging into an existing folder
        }
        ErrorChoice c = handleError(err, dest);
        err = nullptr;
        if (c == ErrorChoice::Retry) continue;
        return false;
    }
    doneFiles_++;
    GFileEnumerator *en = nullptr;
    for (;;) {
        en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, &err);
        if (en) break;
        ErrorChoice c = handleError(err, src);
        err = nullptr;
        if (c == ErrorChoice::Retry) continue;
        return false;
    }
    GFileInfo *ci;
    while (!cancelled_ && (ci = g_file_enumerator_next_file(en, cancellable_, nullptr))) {
        GFile *child = g_file_get_child(src, g_file_info_get_name(ci));
        GFile *target = g_file_get_child(dest, g_file_info_get_name(ci));
        copyOne(child, target, false, move, nullptr);
        g_object_unref(child);
        g_object_unref(target);
        g_object_unref(ci);
    }
    g_object_unref(en);
    g_file_copy_attributes(src, dest, GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA), nullptr, nullptr);
    if (move && !cancelled_) g_file_delete(src, nullptr, nullptr);   // fails when items were skipped
    return !cancelled_;
}

bool FileJob::linkOne(GFile *src, GFile *dest, OpItem *result)
{
    Ref<GFile> d = Ref<GFile>::share(dest);
    for (;;) {
        if (cancelled_) return false;
        GError *err = nullptr;
        char *target = g_file_get_path(src);
        std::string t = target ? take(target) : file_uri(src);
        if (g_file_make_symbolic_link(d.get(), t.c_str(), cancellable_, &err)) {
            doneFiles_++;
            *result = {Ref<GFile>::share(src), d};
            return true;
        }
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_clear_error(&err);
            Ref<GFile> parent(g_file_get_parent(d.get()));
            d.reset(g_file_get_child(parent.get(), unique_name(parent.get(), name_of(d.get())).c_str()));
            continue;
        }
        ErrorChoice c = handleError(err, src);
        if (c == ErrorChoice::Retry) continue;
        return false;
    }
}

bool FileJob::deleteTree(GFile *f)
{
    if (cancelled_) return false;
    setCurrent(parse_name(f));
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, nullptr);
    bool ok = true;
    if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable_, nullptr);
        if (en) {
            GFileInfo *ci;
            while (!cancelled_ && (ci = g_file_enumerator_next_file(en, cancellable_, nullptr))) {
                GFile *child = g_file_get_child(f, g_file_info_get_name(ci));
                if (!deleteTree(child)) ok = false;
                g_object_unref(child);
                g_object_unref(ci);
            }
            g_object_unref(en);
        }
    }
    if (info) g_object_unref(info);
    if (cancelled_) return false;
    for (;;) {
        GError *err = nullptr;
        if (g_file_delete(f, cancellable_, &err)) break;
        if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) {
            g_error_free(err);
            return false;
        }
        ErrorChoice c = handleError(err, f);
        if (c == ErrorChoice::Retry) continue;
        return false;
    }
    doneFiles_++;
    return ok;
}

void FileJob::run()
{
    scan();
    switch (kind_) {
    case OpKind::Copy:
    case OpKind::Move:
        for (auto &it : items_) {
            if (cancelled_) break;
            OpItem r;
            std::string info = kind_ == OpKind::Move ? trash_info_of(it.source.get()) : std::string();
            if (copyOne(it.source.get(), it.dest.get(), true, kind_ == OpKind::Move, &r)) doneItems_.push_back(r);
            drop_trash_info(info, it.source.get());
        }
        break;
    case OpKind::Link:
        for (auto &it : items_) {
            if (cancelled_) break;
            OpItem r;
            if (linkOne(it.source.get(), it.dest.get(), &r)) doneItems_.push_back(r);
        }
        break;
    case OpKind::Trash:
        for (auto &it : items_) {
            if (cancelled_) break;
            setCurrent(parse_name(it.source.get()));
            for (;;) {
                GError *err = nullptr;
                trash::Entry e;
                if (trash::trash_file(it.source.get(), &e, &err)) {
                    trashed_.push_back(e);
                    doneItems_.push_back({it.source, Ref<GFile>(e.trashedPath.empty() ? nullptr : g_file_new_for_path(e.trashedPath.c_str()))});
                    break;
                }
                ErrorChoice c = handleError(err, it.source.get());
                if (c != ErrorChoice::Retry) break;
            }
            doneFiles_++;
        }
        break;
    case OpKind::Delete:
        for (auto &it : items_) {
            if (cancelled_) break;
            std::string info = trash_info_of(it.source.get());
            if (deleteTree(it.source.get())) doneItems_.push_back({it.source, {}});
            drop_trash_info(info, it.source.get());
        }
        break;
    case OpKind::Restore:
        for (auto &e : restoreEntries_) {
            if (cancelled_) break;
            setCurrent(e.originalPath);
            std::string dest;
            for (;;) {
                GError *err = nullptr;
                if (trash::restore(e, dest, &err)) {
                    std::string final = dest.empty() ? e.originalPath : dest;
                    doneItems_.push_back({Ref<GFile>(g_file_new_for_path(e.trashedPath.c_str())), Ref<GFile>(g_file_new_for_path(final.c_str()))});
                    break;
                }
                if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                    g_error_free(err);
                    ConflictRequest req;
                    req.source = Ref<GFile>(g_file_new_for_path(e.trashedPath.c_str()));
                    req.dest = Ref<GFile>(g_file_new_for_path((dest.empty() ? e.originalPath : dest).c_str()));
                    req.sourceInfo = Ref<GFileInfo>(g_file_query_info(req.source.get(), kInfoAttrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr));
                    req.destInfo = Ref<GFileInfo>(g_file_query_info(req.dest.get(), kInfoAttrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr));
                    req.sameFile = true;     // no overwrite when restoring
                    req.multiple = restoreEntries_.size() > 1;
                    Ref<GFile> parent(g_file_get_parent(req.dest.get()));
                    req.suggestedName = unique_name(parent.get(), name_of(req.dest.get()));
                    ConflictReply rep = askConflict(req);
                    if (rep.choice == ConflictChoice::Rename) {
                        dest = dirname_of(e.originalPath) + "/" + (rep.newName.empty() ? req.suggestedName : rep.newName);
                        continue;
                    }
                    if (rep.choice == ConflictChoice::Cancel) cancel();
                    break;
                }
                ErrorChoice c = handleError(err, nullptr);
                if (c != ErrorChoice::Retry) break;
            }
            doneFiles_++;
        }
        break;
    case OpKind::EmptyTrash:
        for (auto &e : restoreEntries_) {
            if (cancelled_) break;
            setCurrent(e.originalPath);
            GError *err = nullptr;
            if (!trash::erase(e, &err)) {
                if (handleError(err, nullptr) == ErrorChoice::Cancel) break;
            }
            doneFiles_++;
        }
        if (!cancelled_) trash::empty_all(nullptr);
        break;
    }
    setCurrent({});
}

// ---------------------------------------------------------------------------------

JobManager &JobManager::instance()
{
    static JobManager m;
    return m;
}

int JobManager::addListener(std::function<void()> l)
{
    static int next = 1;
    listeners_.emplace_back(next, std::move(l));
    return next++;
}

void JobManager::removeListener(int id)
{
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) {
            listeners_.erase(it);
            return;
        }
}

void JobManager::add(FileJob::Ptr j)
{
    jobs_.push_back(j);
    notify();
}

void JobManager::finished(FileJob *j)
{
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
        if (it->get() == j) {
            jobs_.erase(it);
            break;
        }
    notify();
}

void JobManager::notify()
{
    auto copy = listeners_;
    for (auto &[id, l] : copy) l();
}

// ---------------------------------------------------------------------------------

bool make_directory(GFile *dir, const std::string &name, Ref<GFile> &created, GError **err)
{
    Ref<GFile> f(g_file_get_child_for_display_name(dir, name.c_str(), err));
    if (!f) return false;
    if (!g_file_make_directory(f.get(), nullptr, err)) return false;
    created = f;
    return true;
}

bool create_empty_file(GFile *dir, const std::string &name, Ref<GFile> &created, GError **err)
{
    Ref<GFile> f(g_file_get_child_for_display_name(dir, name.c_str(), err));
    if (!f) return false;
    GFileOutputStream *s = g_file_create(f.get(), G_FILE_CREATE_NONE, nullptr, err);
    if (!s) return false;
    g_output_stream_close(G_OUTPUT_STREAM(s), nullptr, nullptr);
    g_object_unref(s);
    created = f;
    return true;
}

bool rename_file(GFile *f, const std::string &newName, Ref<GFile> &renamed, GError **err)
{
    GFile *r = g_file_set_display_name(f, newName.c_str(), nullptr, err);
    if (!r) return false;
    renamed.reset(r);
    return true;
}

} // namespace beluga
