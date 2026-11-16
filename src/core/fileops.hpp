// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: file operations (copy, move, link, trash, delete, restore) that run
// in a worker thread and report progress. Questions (name conflicts, errors) are sent
// to the GUI through JobUi and the worker waits for the answer.
#pragma once

#include "trash.hpp"
#include "util.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace beluga {

enum class OpKind { Copy, Move, Link, Trash, Delete, Restore, EmptyTrash };

enum class ConflictChoice { Cancel, Skip, Overwrite, Rename, Merge };
struct ConflictRequest {
    Ref<GFile> source;
    Ref<GFile> dest;
    Ref<GFileInfo> sourceInfo;
    Ref<GFileInfo> destInfo;
    bool folderConflict = false;   // both are folders: offer "Write Into"
    bool sameFile = false;         // source and dest are the same: no overwrite
    bool multiple = false;         // more items follow: offer "apply to all"
    std::string suggestedName;
};
struct ConflictReply {
    ConflictChoice choice = ConflictChoice::Cancel;
    bool applyToAll = false;
    std::string newName;
};

enum class ErrorChoice { Cancel, Skip, SkipAll, Retry };

class FileJob;

// Implemented by the GUI. Both calls arrive on the main thread; call reply exactly once.
struct JobUi {
    virtual ~JobUi() = default;
    virtual void conflict(FileJob &job, const ConflictRequest &req, std::function<void(ConflictReply)> reply) = 0;
    virtual void error(FileJob &job, const std::string &message, bool canSkip, std::function<void(ErrorChoice)> reply) = 0;
};
void set_job_ui(JobUi *ui);

struct OpItem {
    Ref<GFile> source;
    Ref<GFile> dest;              // full target path (not the folder)
};

class FileJob : public std::enable_shared_from_this<FileJob> {
public:
    using Ptr = std::shared_ptr<FileJob>;

    // Builders. destDir: the folder receiving the items.
    static Ptr copy(const std::vector<Ref<GFile>> &sources, GFile *destDir);
    static Ptr move(const std::vector<Ref<GFile>> &sources, GFile *destDir);
    static Ptr link(const std::vector<Ref<GFile>> &sources, GFile *destDir);
    static Ptr copyPairs(const std::vector<OpItem> &items);
    static Ptr movePairs(const std::vector<OpItem> &items);
    static Ptr trash(const std::vector<Ref<GFile>> &files);
    static Ptr remove(const std::vector<Ref<GFile>> &files);
    static Ptr restore(const std::vector<trash::Entry> &entries);
    static Ptr emptyTrash();

    void start();
    void cancel();
    bool cancelled() const { return cancelled_; }
    bool running() const { return running_; }
    bool finished() const { return finished_; }

    OpKind kind() const { return kind_; }
    std::string title() const;
    std::string currentText() const;
    double fraction() const;
    guint64 totalBytes() const { return totalBytes_; }
    guint64 doneBytes() const { return doneBytes_; }
    int totalFiles() const { return totalFiles_; }
    int doneFiles() const { return doneFiles_; }
    double bytesPerSecond() const;

    // Results (valid after finish, main thread).
    const std::vector<OpItem> &done() const { return doneItems_; }       // top-level items, final dest
    const std::vector<trash::Entry> &trashed() const { return trashed_; }
    const std::vector<std::string> &errors() const { return errors_; }
    bool undoable = true;          // register in the undo stack

    // Main-thread callbacks.
    std::function<void(FileJob &)> onFinished;

    // Free ids so the GUI can track jobs.
    int id() const { return id_; }

private:
    FileJob(OpKind k);
    void run();
    void scan();
    guint64 scanFile(GFile *f, int depth);
    bool copyOne(GFile *src, GFile *dest, bool isTop, bool move, OpItem *result);
    bool copyTree(GFile *src, GFile *dest, GFileInfo *info, bool move);
    bool linkOne(GFile *src, GFile *dest, OpItem *result);
    bool deleteTree(GFile *f);
    ConflictReply askConflict(ConflictRequest req);
    ErrorChoice askError(const std::string &msg, bool canSkip);
    ErrorChoice handleError(GError *err, GFile *f);
    void setCurrent(const std::string &s);

    OpKind kind_;
    std::vector<OpItem> items_;
    std::vector<trash::Entry> restoreEntries_;
    GCancellable *cancellable_ = nullptr;
    std::atomic<bool> cancelled_{false}, running_{false}, finished_{false};
    std::atomic<guint64> totalBytes_{0}, doneBytes_{0}, fileBase_{0};
    std::atomic<int> totalFiles_{0}, doneFiles_{0};
    gint64 startTime_ = 0;
    mutable std::mutex mutex_;
    std::string current_;
    std::vector<OpItem> doneItems_;
    std::vector<trash::Entry> trashed_;
    std::vector<std::string> errors_;
    bool fileApplyAll_ = false, dirApplyAll_ = false, skipAllErrors_ = false;
    ConflictReply fileAll_, dirAll_;
    int id_;
    Ptr self_;   // keeps the job alive while the thread runs
    friend class JobManager;
};

// Running jobs, for the progress display in the status bar.
class JobManager {
public:
    static JobManager &instance();
    const std::vector<FileJob::Ptr> &jobs() const { return jobs_; }
    int addListener(std::function<void()> l);
    void removeListener(int id);
    void add(FileJob::Ptr j);
    void finished(FileJob *j);
private:
    void notify();
    std::vector<FileJob::Ptr> jobs_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
};

// Runs fn on the main thread.
void run_on_main(std::function<void()> fn);

// Small synchronous helpers used by the GUI.
bool make_directory(GFile *dir, const std::string &name, Ref<GFile> &created, GError **err);
bool create_empty_file(GFile *dir, const std::string &name, Ref<GFile> &created, GError **err);
bool rename_file(GFile *f, const std::string &newName, Ref<GFile> &renamed, GError **err);

} // namespace beluga
