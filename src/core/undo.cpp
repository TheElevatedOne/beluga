// SPDX-License-Identifier: GPL-2.0-or-later
#include "undo.hpp"

namespace beluga {

UndoManager &UndoManager::instance()
{
    static UndoManager m;
    return m;
}

void UndoManager::push(UndoCommand c)
{
    if (c.pairs.empty() && c.trashed.empty()) return;
    undo_.push_back(std::move(c));
    while (undo_.size() > 50) undo_.pop_front();
    redo_.clear();
    notify();
}

void UndoManager::pushFromJob(const FileJob &job)
{
    if (!job.undoable || job.done().empty()) return;
    UndoCommand c;
    switch (job.kind()) {
    case OpKind::Copy: c.type = UndoCommand::Copy; break;
    case OpKind::Move: c.type = UndoCommand::Move; break;
    case OpKind::Link: c.type = UndoCommand::Link; break;
    case OpKind::Trash: c.type = UndoCommand::Trash; break;
    default: return;
    }
    for (auto &it : job.done())
        c.pairs.emplace_back(file_uri(it.source.get()), it.dest ? file_uri(it.dest.get()) : std::string());
    c.trashed = job.trashed();
    push(std::move(c));
}

std::string UndoManager::describe(const UndoCommand &c)
{
    switch (c.type) {
    case UndoCommand::Copy: return _("Copy");
    case UndoCommand::Move: return _("Move");
    case UndoCommand::Link: return _("Link");
    case UndoCommand::Rename: return _("Rename");
    case UndoCommand::Trash: return _("Move to Trash");
    case UndoCommand::Mkdir: return _("Create Folder");
    case UndoCommand::CreateFile: return _("Create File");
    }
    return {};
}

std::string UndoManager::undoText() const
{
    return undo_.empty() ? std::string(_("Undo")) : printf_str(_("Undo: %s"), describe(undo_.back()).c_str());
}

std::string UndoManager::redoText() const
{
    return redo_.empty() ? std::string(_("Redo")) : printf_str(_("Redo: %s"), describe(redo_.back()).c_str());
}

void UndoManager::undo()
{
    if (!canUndo()) return;
    UndoCommand c = undo_.back();
    undo_.pop_back();
    run(std::move(c), true);
}

void UndoManager::redo()
{
    if (!canRedo()) return;
    UndoCommand c = redo_.back();
    redo_.pop_back();
    run(std::move(c), false);
}

static Ref<GFile> F(const std::string &uri) { return Ref<GFile>(g_file_new_for_uri(uri.c_str())); }

void UndoManager::run(UndoCommand c, bool isUndo)
{
    auto finish = [this, isUndo](UndoCommand done) {
        busy_ = false;
        (isUndo ? redo_ : undo_).push_back(std::move(done));
        notify();
    };
    auto report = [this](const std::string &e) { if (onError) onError(e); };

    FileJob::Ptr job;
    switch (c.type) {
    case UndoCommand::Copy:
    case UndoCommand::Link:
    case UndoCommand::Mkdir:
    case UndoCommand::CreateFile:
        if (isUndo) {
            std::vector<Ref<GFile>> files;
            for (auto &[src, dst] : c.pairs) files.push_back(F(dst));
            job = c.type == UndoCommand::Link ? FileJob::remove(files) : FileJob::trash(files);
        } else if (c.type == UndoCommand::Copy) {
            std::vector<OpItem> items;
            for (auto &[src, dst] : c.pairs) items.push_back({F(src), F(dst)});
            job = FileJob::copyPairs(items);
        } else if (c.type == UndoCommand::Link) {
            for (auto &[src, dst] : c.pairs) {
                GError *err = nullptr;
                Ref<GFile> d = F(dst), s = F(src);
                std::string target = display_location(s.get());
                if (!g_file_make_symbolic_link(d.get(), target.c_str(), nullptr, &err)) {
                    report(err->message);
                    g_error_free(err);
                }
            }
            finish(c);
            return;
        } else {
            for (auto &[src, dst] : c.pairs) {
                GError *err = nullptr;
                Ref<GFile> d = F(dst);
                bool ok = c.type == UndoCommand::Mkdir ? g_file_make_directory(d.get(), nullptr, &err) : [&] {
                    GFileOutputStream *s = g_file_create(d.get(), G_FILE_CREATE_NONE, nullptr, &err);
                    if (s) g_object_unref(s);
                    return s != nullptr;
                }();
                if (!ok) {
                    report(err->message);
                    g_error_free(err);
                }
            }
            finish(c);
            return;
        }
        break;
    case UndoCommand::Move: {
        std::vector<OpItem> items;
        for (auto &[src, dst] : c.pairs)
            items.push_back(isUndo ? OpItem{F(dst), F(src)} : OpItem{F(src), F(dst)});
        job = FileJob::movePairs(items);
        break;
    }
    case UndoCommand::Rename:
        for (auto &[from, to] : c.pairs) {
            Ref<GFile> f = F(isUndo ? to : from);
            Ref<GFile> target = F(isUndo ? from : to);
            Ref<GFile> renamed;
            GError *err = nullptr;
            char *base = g_file_get_basename(target.get());
            if (!rename_file(f.get(), take(base), renamed, &err)) {
                report(err->message);
                g_error_free(err);
            }
        }
        finish(c);
        return;
    case UndoCommand::Trash:
        if (isUndo) {
            job = FileJob::restore(c.trashed);
        } else {
            std::vector<Ref<GFile>> files;
            for (auto &e : c.trashed) files.push_back(Ref<GFile>(g_file_new_for_path(e.originalPath.c_str())));
            job = FileJob::trash(files);
        }
        break;
    }
    if (!job) return;
    job->undoable = false;
    busy_ = true;
    notify();
    job->onFinished = [finish, c, isUndo](FileJob &j) mutable {
        if (c.type == UndoCommand::Trash && !isUndo) {
            c.trashed = j.trashed();
            c.pairs.clear();
            for (auto &it : j.done())
                c.pairs.emplace_back(file_uri(it.source.get()), it.dest ? file_uri(it.dest.get()) : std::string());
        }
        finish(c);
    };
    job->start();
}

int UndoManager::addListener(std::function<void()> l)
{
    static int next = 1;
    listeners_.emplace_back(next, std::move(l));
    return next++;
}

void UndoManager::removeListener(int id)
{
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) {
            listeners_.erase(it);
            return;
        }
}

void UndoManager::notify()
{
    auto copy = listeners_;
    for (auto &[id, l] : copy) l();
}

} // namespace beluga
