// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: Vim-like keyboard mode (off by default; Configure Beluga › Vim Mode). Normal mode moves with h j k l,
// visual modes select (v: each item the cursor passes, V: a range like Shift, Ctrl+V: a block of the grid), y d p
// copy, cut and paste, dd moves to the trash, i/a rename, the leader key (Space) starts more commands, and ':'
// opens a command line (:q, :q!, :e path, :tabnew, :mkdir …). Every sequence can be changed.
#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

namespace beluga {

class MainWindow;

struct VimAction {
    const char *id;
    const char *title;                    // N_() string
    std::vector<const char *> defaults;   // key sequences in Vim notation: "gg", "<C-v>", "<leader>t"
};

const std::vector<VimAction> &vim_actions();
// The sequences for an action: the user's (Settings::vimKeys) or the defaults.
std::vector<std::string> vim_keys(const std::string &id);
// A sequence written in Vim notation as tokens ("<leader>t" → {"<Space>", "t"}); empty when it can't be read.
std::vector<std::string> vim_parse(const std::string &seq);
// A key press as a token: "h", "G", "<C-v>", "<CR>", "<Left>".
std::string vim_token(guint keyval, GdkModifierType state);

class VimMode {
public:
    explicit VimMode(MainWindow *w);
    ~VimMode();
    VimMode(const VimMode &) = delete;
    VimMode &operator=(const VimMode &) = delete;

    GtkWidget *bar() const { return bar_; }   // mode line and command line, at the bottom of the window
    void applySettings();
    // Another folder or tab: the visual selection's positions no longer apply.
    void locationChanged();
    // A key press from the window (capture phase). True when Vim mode used it.
    bool key(guint keyval, GdkModifierType state);
    // Runs a command line (without the ':'); returns an error message, empty when it worked.
    std::string command(const std::string &line);
    // For the self-test.
    const char *modeName() const;
    void feed(const std::string &sequence);

    enum class Mode { Normal, Visual, VisualLine, VisualBlock };

private:
    struct Binding {
        std::vector<std::string> tokens;
        std::string action;
    };
    void compile();
    void run(const std::string &action, int count);
    void move(int dx, int dy, int count);
    void moveTo(int pos);
    void setMode(Mode m);
    void updateSelection();
    void ensureTarget();
    void updateBar();
    void showMessage(const std::string &text, bool error);
    void openCommandLine();
    void flushPending();

    MainWindow *win_;
    GtkWidget *bar_, *modeLabel_, *keysLabel_, *cmdBox_, *cmdEntry_;
    std::vector<Binding> bindings_;
    std::vector<std::string> pending_;
    int count_ = 0;
    guint timeout_ = 0;
    Mode mode_ = Mode::Normal;
    int anchor_ = -1;
    std::vector<int> visited_;   // visual (v): every item the cursor passed
    std::shared_ptr<void> life_ = std::make_shared<char>(0);
};

} // namespace beluga
