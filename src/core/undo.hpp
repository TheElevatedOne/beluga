// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: undo and redo of file operations (copy, move, link, rename, trash,
// new folder, new file).
#pragma once

#include "fileops.hpp"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace beluga {

struct UndoCommand {
    enum Type { Copy, Move, Link, Rename, Trash, Mkdir, CreateFile };
    Type type;
    std::vector<std::pair<std::string, std::string>> pairs;   // source URI -> result URI
    std::vector<trash::Entry> trashed;
};

class UndoManager {
public:
    static UndoManager &instance();
    void push(UndoCommand c);
    void pushFromJob(const FileJob &job);
    bool canUndo() const { return !undo_.empty() && !busy_; }
    bool canRedo() const { return !redo_.empty() && !busy_; }
    std::string undoText() const;
    std::string redoText() const;
    void undo();
    void redo();
    int addListener(std::function<void()> l);
    void removeListener(int id);
    // Set by the GUI: shows errors of synchronous undo steps.
    std::function<void(const std::string &)> onError;

private:
    void run(UndoCommand c, bool isUndo);
    void notify();
    static std::string describe(const UndoCommand &c);
    std::deque<UndoCommand> undo_, redo_;
    bool busy_ = false;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
};

} // namespace beluga
