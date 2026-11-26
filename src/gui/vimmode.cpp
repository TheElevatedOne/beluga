// SPDX-License-Identifier: GPL-2.0-or-later
#include "vimmode.hpp"

#include "app.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "itemview.hpp"
#include "mainwindow.hpp"
#include "preferences.hpp"
#include "settings.hpp"
#include "tabpage.hpp"
#include "util.hpp"
#include "view.hpp"
#include "viewcontainer.hpp"

#include <algorithm>
#include <cstring>

namespace beluga {

const std::vector<VimAction> &vim_actions()
{
    static const std::vector<VimAction> t = {
        {"left", N_("Move left"), {"h", "<Left>"}},
        {"down", N_("Move down"), {"j", "<Down>"}},
        {"up", N_("Move up"), {"k", "<Up>"}},
        {"right", N_("Move right"), {"l", "<Right>"}},
        {"first", N_("First item"), {"gg", "<Home>"}},
        {"last", N_("Last item"), {"G", "<End>"}},
        {"page-down", N_("Page down"), {"<C-d>", "<PageDown>"}},
        {"page-up", N_("Page up"), {"<C-u>", "<PageUp>"}},
        {"open", N_("Open"), {"<CR>"}},
        {"parent", N_("Parent folder"), {"-", "<BS>"}},
        {"back", N_("Back"), {"H"}},
        {"forward", N_("Forward"), {"L"}},
        {"visual", N_("Select the items the cursor passes (visual)"), {"v"}},
        {"visual-line", N_("Select a range, like Shift (visual line)"), {"V"}},
        {"visual-block", N_("Select a block of the grid (visual block)"), {"<C-v>"}},
        {"escape", N_("Back to normal mode"), {"<Esc>"}},
        {"copy", N_("Copy"), {"y"}},
        {"copy-item", N_("Copy the item under the cursor"), {"yy"}},
        {"cut", N_("Cut"), {"d"}},
        {"trash", N_("Move to Trash"), {"dd"}},
        {"delete", N_("Delete"), {"D"}},
        {"paste", N_("Paste"), {"p"}},
        {"paste-into", N_("Paste into Folder"), {"P"}},
        {"rename-start", N_("Rename, cursor at the start (insert)"), {"i", "I"}},
        {"rename-end", N_("Rename, cursor after the name (append)"), {"a", "A"}},
        {"rename", N_("Rename, name selected (change)"), {"cw", "<F2>"}},
        {"new-folder", N_("Create Folder"), {"o"}},
        {"new-file", N_("Create File"), {"O"}},
        {"undo", N_("Undo"), {"u"}},
        {"redo", N_("Redo"), {"<C-r>"}},
        {"filter", N_("Filter"), {"/"}},
        {"search", N_("Search"), {"<leader>/", "<leader>f"}},
        {"command", N_("Command line"), {":"}},
        {"next-tab", N_("Next Tab"), {"gt"}},
        {"prev-tab", N_("Previous Tab"), {"gT"}},
        {"new-tab", N_("New Tab"), {"<leader>t"}},
        {"close-tab", N_("Close Tab"), {"qq", "<leader>w"}},
        {"close-window", N_("Close Window"), {"ZZ"}},
        {"split", N_("Split View"), {"<leader>s"}},
        {"other-view", N_("Focus Other View"), {"<C-w><C-w>", "<C-w>w"}},
        {"hidden", N_("Show Hidden Files"), {"zh", "<leader>h"}},
        {"reload", N_("Reload"), {"<C-l>"}},
        {"properties", N_("Properties"), {"K"}},
        {"terminal", N_("Open Terminal"), {"<leader>c"}},
        {"disk-usage", N_("Disk Usage Panel"), {"<leader>d"}},
        {"information", N_("Information Panel"), {"<leader>i"}},
        {"places", N_("Places Panel"), {"<leader>p"}},
        {"location", N_("Edit the location"), {"<leader>e"}},
        {"home", N_("Home"), {"gh"}},
        {"preferences", N_("Configure Beluga"), {"<leader>,"}},
    };
    return t;
}

std::vector<std::string> vim_keys(const std::string &id)
{
    auto it = settings().vimKeys.find(id);
    if (it != settings().vimKeys.end()) return it->second;
    std::vector<std::string> out;
    for (auto &a : vim_actions())
        if (id == a.id)
            for (const char *k : a.defaults) out.push_back(k);
    return out;
}

namespace {

struct KeyName {
    const char *name;
    guint keyval;
};
const KeyName kNames[] = {
    {"CR", GDK_KEY_Return}, {"Esc", GDK_KEY_Escape}, {"BS", GDK_KEY_BackSpace}, {"Tab", GDK_KEY_Tab},
    {"Space", GDK_KEY_space}, {"Left", GDK_KEY_Left}, {"Right", GDK_KEY_Right}, {"Up", GDK_KEY_Up},
    {"Down", GDK_KEY_Down}, {"Home", GDK_KEY_Home}, {"End", GDK_KEY_End}, {"PageUp", GDK_KEY_Page_Up},
    {"PageDown", GDK_KEY_Page_Down}, {"Del", GDK_KEY_Delete}, {"Insert", GDK_KEY_Insert}, {"lt", GDK_KEY_less},
    {"F1", GDK_KEY_F1}, {"F2", GDK_KEY_F2}, {"F3", GDK_KEY_F3}, {"F4", GDK_KEY_F4}, {"F5", GDK_KEY_F5},
    {"F6", GDK_KEY_F6}, {"F7", GDK_KEY_F7}, {"F8", GDK_KEY_F8}, {"F9", GDK_KEY_F9}, {"F10", GDK_KEY_F10},
    {"F11", GDK_KEY_F11}, {"F12", GDK_KEY_F12},
};
// Other spellings Vim accepts.
const std::pair<const char *, const char *> kAliases[] = {
    {"enter", "CR"}, {"return", "CR"}, {"escape", "Esc"}, {"backspace", "BS"}, {"delete", "Del"},
    {"pagedown", "PageDown"}, {"pageup", "PageUp"}, {"space", "Space"}, {"leader", "leader"},
};

std::string canonical_name(const std::string &raw)
{
    std::string lower = raw;
    for (auto &c : lower) c = g_ascii_tolower(c);
    for (auto &[a, b] : kAliases)
        if (lower == a) return b;
    for (auto &k : kNames) {
        std::string n = k.name;
        for (auto &c : n) c = g_ascii_tolower(c);
        if (n == lower) return k.name;
    }
    return {};
}

// "<c-V>" → "<C-v>"; "<S-tab>" → "<S-Tab>". Empty when it isn't a key.
std::string normalize_bracket(const std::string &inner)
{
    std::string mods, key = inner;
    for (;;) {
        if (key.size() > 2 && key[1] == '-' && strchr("cCaAmMsS", key[0])) {
            char m = (char)g_ascii_toupper(key[0]);
            if (m == 'M') m = 'A';
            if (mods.find(m) == std::string::npos) mods += m;
            key = key.substr(2);
        } else {
            break;
        }
    }
    std::string k;
    if (g_utf8_strlen(key.c_str(), -1) == 1) {
        k = key;
        if (!mods.empty() && mods.find('S') == std::string::npos && (unsigned char)k[0] < 128) k[0] = (char)g_ascii_tolower(k[0]);
    } else {
        k = canonical_name(key);
        if (k.empty()) return {};
    }
    std::sort(mods.begin(), mods.end());   // A, C, S
    std::string out = "<";
    for (char m : mods) out += std::string(1, m) + "-";
    return out + k + ">";
}

} // namespace

std::vector<std::string> vim_parse(const std::string &seq)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < seq.size();) {
        if (seq[i] == '<') {
            size_t close = seq.find('>', i + 1);
            if (close != std::string::npos && close > i + 1) {
                std::string inner = seq.substr(i + 1, close - i - 1);
                if (canonical_name(inner) == "leader") {
                    auto leader = vim_parse(settings().vimLeader);
                    out.insert(out.end(), leader.begin(), leader.end());
                    i = close + 1;
                    continue;
                }
                std::string tok = normalize_bracket(inner);
                if (!tok.empty()) {
                    if (tok == "<lt>") tok = "<";
                    else if (tok == "<Space>") tok = "<Space>";
                    out.push_back(tok);
                    i = close + 1;
                    continue;
                }
            }
        }
        const char *p = seq.c_str() + i;
        const char *n = g_utf8_next_char(p);
        std::string ch(p, n - p);
        out.push_back(ch == " " ? "<Space>" : ch);
        i += n - p;
    }
    return out;
}

std::string vim_token(guint keyval, GdkModifierType state)
{
    bool ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_ALT_MASK, shift = state & GDK_SHIFT_MASK;
    for (auto &k : kNames)
        if (k.keyval == keyval || (keyval == GDK_KEY_KP_Enter && k.keyval == GDK_KEY_Return)
            || (keyval == GDK_KEY_ISO_Left_Tab && k.keyval == GDK_KEY_Tab)) {
            if (k.keyval == GDK_KEY_less && !ctrl && !alt) return "<";
            std::string mods;
            if (alt) mods += "A-";
            if (ctrl) mods += "C-";
            if (shift && k.keyval != GDK_KEY_less) mods += "S-";
            if (mods.empty() && k.keyval == GDK_KEY_space) return "<Space>";
            return "<" + mods + k.name + ">";
        }
    gunichar u = gdk_keyval_to_unicode(keyval);
    if (!u || g_unichar_iscntrl(u)) return {};
    if (ctrl || alt) {
        gunichar l = g_unichar_tolower(gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval)));
        char buf[8] = {};
        g_unichar_to_utf8(l ? l : u, buf);
        std::string mods;
        if (alt) mods += "A-";
        if (ctrl) mods += "C-";
        if (shift) mods += "S-";
        return "<" + mods + buf + ">";
    }
    char buf[8] = {};
    g_unichar_to_utf8(u, buf);
    return buf;
}

// ---- the mode ---------------------------------------------------------------------------------------

VimMode::VimMode(MainWindow *w) : win_(w)
{
    bar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar_, "beluga-vim-bar");
    modeLabel_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(modeLabel_, "beluga-vim-mode");
    gtk_box_append(GTK_BOX(bar_), modeLabel_);
    cmdBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(cmdBox_, TRUE);
    gtk_box_append(GTK_BOX(cmdBox_), gtk_label_new(":"));
    cmdEntry_ = gtk_entry_new();
    gtk_widget_set_hexpand(cmdEntry_, TRUE);
    gtk_widget_add_css_class(cmdEntry_, "beluga-vim-command");
    gtk_box_append(GTK_BOX(cmdBox_), cmdEntry_);
    gtk_widget_set_visible(cmdBox_, FALSE);
    gtk_box_append(GTK_BOX(bar_), cmdBox_);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    g_object_set_data(G_OBJECT(bar_), "beluga-spacer", spacer);
    gtk_box_append(GTK_BOX(bar_), spacer);
    keysLabel_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(keysLabel_, "dim-label");
    gtk_box_append(GTK_BOX(bar_), keysLabel_);

    g_signal_connect(cmdEntry_, "activate", CB([](GtkEntry *e, gpointer p) {
        auto *v = static_cast<VimMode *>(p);
        std::string line = gtk_editable_get_text(GTK_EDITABLE(e));
        gtk_widget_set_visible(v->cmdBox_, FALSE);
        gtk_widget_set_visible(GTK_WIDGET(g_object_get_data(G_OBJECT(v->bar_), "beluga-spacer")), TRUE);
        if (View *view = v->win_->activeView()) view->focus();
        std::string err = v->command(line);
        if (!err.empty()) v->showMessage(err, true);
        else v->updateBar();
    }), this);
    GtkEventController *ek = gtk_event_controller_key_new();
    g_signal_connect(ek, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer p) -> gboolean {
        if (keyval != GDK_KEY_Escape) return FALSE;
        auto *v = static_cast<VimMode *>(p);
        gtk_widget_set_visible(v->cmdBox_, FALSE);
        gtk_widget_set_visible(GTK_WIDGET(g_object_get_data(G_OBJECT(v->bar_), "beluga-spacer")), TRUE);
        if (View *view = v->win_->activeView()) view->focus();
        v->updateBar();
        return TRUE;
    }), this);
    gtk_widget_add_controller(cmdEntry_, ek);
    applySettings();
}

VimMode::~VimMode()
{
    if (timeout_) g_source_remove(timeout_);
}

void VimMode::applySettings()
{
    gtk_widget_set_visible(bar_, settings().vimMode);
    compile();
    if (!settings().vimMode) setMode(Mode::Normal);
    updateBar();
}

void VimMode::compile()
{
    bindings_.clear();
    for (auto &a : vim_actions())
        for (auto &seq : vim_keys(a.id)) {
            auto t = vim_parse(seq);
            if (!t.empty()) bindings_.push_back({t, a.id});
        }
}

const char *VimMode::modeName() const
{
    switch (mode_) {
    case Mode::Visual: return "VISUAL";
    case Mode::VisualLine: return "V-LINE";
    case Mode::VisualBlock: return "V-BLOCK";
    default: return "NORMAL";
    }
}

void VimMode::updateBar()
{
    std::string name = std::string("-- ") + modeName() + " --";
    gtk_label_set_text(GTK_LABEL(modeLabel_), name.c_str());
    if (mode_ == Mode::Normal) gtk_widget_remove_css_class(modeLabel_, "visual");
    else gtk_widget_add_css_class(modeLabel_, "visual");
    std::string keys = count_ ? std::to_string(count_) : std::string();
    for (auto &t : pending_) keys += t;
    gtk_label_set_text(GTK_LABEL(keysLabel_), keys.c_str());
    gtk_widget_remove_css_class(keysLabel_, "error");
}

void VimMode::showMessage(const std::string &text, bool error)
{
    gtk_label_set_text(GTK_LABEL(keysLabel_), text.c_str());
    if (error) gtk_widget_add_css_class(keysLabel_, "error");
}

void VimMode::setMode(Mode m)
{
    mode_ = m;
    ItemView *iv = win_->activeView() ? win_->activeView()->itemView() : nullptr;
    int cur = iv ? iv->currentPosition() : -1;
    anchor_ = cur;
    visited_.clear();
    if (m != Mode::Normal && cur >= 0) visited_.push_back(cur);
    if (m != Mode::Normal) updateSelection();
    updateBar();
}

// The selection of the visual modes, from the anchor (where the mode started) and the cursor.
void VimMode::updateSelection()
{
    View *v = win_->activeView();
    if (!v || mode_ == Mode::Normal) return;
    ItemView *iv = v->itemView();
    int cur = iv->currentPosition();
    if (cur < 0) return;
    if (anchor_ < 0) anchor_ = cur;
    std::vector<int> sel;
    if (mode_ == Mode::Visual) {
        sel = visited_;
    } else if (mode_ == Mode::VisualLine) {
        for (int p = std::min(anchor_, cur); p <= std::max(anchor_, cur); ++p) sel.push_back(p);
    } else {
        sel = iv->block(anchor_, cur);
    }
    iv->selectPositions(sel);
}

void VimMode::moveTo(int pos)
{
    View *v = win_->activeView();
    if (!v || pos < 0) return;
    ItemView *iv = v->itemView();
    // Normal mode: the cursor's item is selected, as with arrow keys. Visual modes keep their own selection.
    iv->setCurrent(pos, mode_ == Mode::Normal, true);
    if (mode_ == Mode::Visual && std::find(visited_.begin(), visited_.end(), pos) == visited_.end()) visited_.push_back(pos);
    updateSelection();
}

void VimMode::move(int dx, int dy, int count)
{
    View *v = win_->activeView();
    if (!v) return;
    ItemView *iv = v->itemView();
    int pos = iv->currentPosition();
    if (pos < 0) pos = 0;
    for (int i = 0; i < std::max(1, count); ++i) {
        int next = iv->step(pos, dx, dy);
        if (next < 0 || next == pos) break;
        pos = next;
        // v: every item passed on the way is part of the selection
        if (mode_ == Mode::Visual && std::find(visited_.begin(), visited_.end(), pos) == visited_.end()) visited_.push_back(pos);
    }
    moveTo(pos);
}

// Actions work on the selection, or on the item under the cursor when nothing is selected.
void VimMode::ensureTarget()
{
    View *v = win_->activeView();
    if (!v || !v->selectedItems().empty()) return;
    int cur = v->itemView()->currentPosition();
    if (cur >= 0) v->itemView()->selectPositions({cur});
}

void VimMode::run(const std::string &a, int count)
{
    View *v = win_->activeView();
    ViewContainer *c = win_->activeContainer();
    GActionGroup *ag = G_ACTION_GROUP(win_->window());
    auto act = [&](const char *name, GVariant *param = nullptr) { g_action_group_activate_action(ag, name, param); };
    auto leaveVisual = [&] {
        if (mode_ != Mode::Normal) {
            mode_ = Mode::Normal;
            visited_.clear();
            updateBar();
        }
    };
    if (a == "left") move(-1, 0, count);
    else if (a == "right") move(1, 0, count);
    else if (a == "up") move(0, -1, count);
    else if (a == "down") move(0, 1, count);
    else if (a == "first") moveTo(count > 0 ? count - 1 : 0);
    else if (a == "last") {
        if (v) moveTo(count > 0 ? count - 1 : v->itemView()->count() - 1);
    } else if (a == "page-down" || a == "page-up") {
        if (v) {
            int lines = std::max(1, v->itemView()->pageLines() / 2) * std::max(1, count);
            if (v->itemView()->mode() == 1) move(a == "page-down" ? 1 : -1, 0, lines);
            else move(0, a == "page-down" ? 1 : -1, lines);
        }
    } else if (a == "open") {
        ensureTarget();
        leaveVisual();
        act("open-selected");
    } else if (a == "parent") {
        if (c) c->goUp();
    } else if (a == "back") {
        if (c) c->goBack();
    } else if (a == "forward") {
        if (c) c->goForward();
    } else if (a == "home") {
        if (c) c->goHome();
    } else if (a == "visual") setMode(mode_ == Mode::Visual ? Mode::Normal : Mode::Visual);
    else if (a == "visual-line") setMode(mode_ == Mode::VisualLine ? Mode::Normal : Mode::VisualLine);
    else if (a == "visual-block") setMode(mode_ == Mode::VisualBlock ? Mode::Normal : Mode::VisualBlock);
    else if (a == "escape") {
        if (mode_ != Mode::Normal) {
            setMode(Mode::Normal);
            if (v) {
                int cur = v->itemView()->currentPosition();
                v->itemView()->selectPositions(cur >= 0 ? std::vector<int>{cur} : std::vector<int>{});
            }
        }
    } else if (a == "copy" || a == "cut") {
        ensureTarget();
        act(a.c_str());
        leaveVisual();
        showMessage(a == "copy" ? _("Copied.") : _("Cut."), false);
        return;
    } else if (a == "copy-item") {
        if (v) {
            int cur = v->itemView()->currentPosition();
            if (cur >= 0) v->itemView()->selectPositions({cur});
        }
        act("copy");
        leaveVisual();
        showMessage(_("Copied."), false);
        return;
    } else if (a == "trash") {
        ensureTarget();
        leaveVisual();
        act("move-to-trash");
    } else if (a == "delete") {
        ensureTarget();
        leaveVisual();
        act("delete");
    } else if (a == "paste") {
        leaveVisual();
        act("paste");
    } else if (a == "paste-into") {
        ensureTarget();
        leaveVisual();
        act("paste-into");
    } else if (a == "rename-start" || a == "rename-end" || a == "rename") {
        leaveVisual();
        if (v) {
            int cur = v->itemView()->currentPosition();
            if (cur >= 0) v->itemView()->selectPositions({cur});
            v->renameSelected(a == "rename-start" ? 0 : a == "rename-end" ? 1 : -1);
        }
    } else if (a == "new-folder") {
        leaveVisual();
        act("create-folder");
    } else if (a == "new-file") {
        leaveVisual();
        act("create-file");
    } else if (a == "undo") act("undo");
    else if (a == "redo") act("redo");
    else if (a == "filter") act("toggle-filter");
    else if (a == "search") act("toggle-search");
    else if (a == "command") openCommandLine();
    else if (a == "next-tab") {
        if (count > 0) win_->activateTab(count - 1);
        else act("next-tab");
    } else if (a == "prev-tab") act("prev-tab");
    else if (a == "new-tab") act("new-tab");
    else if (a == "close-tab") act("close-tab");
    else if (a == "close-window") gtk_window_close(GTK_WINDOW(win_->window()));
    else if (a == "split") act("split");
    else if (a == "other-view") act("focus-other-view");
    else if (a == "hidden") act("show-hidden");
    else if (a == "reload") act("reload");
    else if (a == "properties") {
        ensureTarget();
        act("properties");
    } else if (a == "terminal") act("open-terminal");
    else if (a == "disk-usage") act("show-disk-usage");
    else if (a == "information") act("show-info");
    else if (a == "places") act("show-places");
    else if (a == "location") act("replace-location");
    else if (a == "preferences") act("preferences");
    updateBar();
}

void VimMode::openCommandLine()
{
    gtk_editable_set_text(GTK_EDITABLE(cmdEntry_), "");
    gtk_widget_set_visible(GTK_WIDGET(g_object_get_data(G_OBJECT(bar_), "beluga-spacer")), FALSE);
    gtk_widget_set_visible(cmdBox_, TRUE);
    gtk_label_set_text(GTK_LABEL(keysLabel_), "");
    gtk_widget_grab_focus(cmdEntry_);
}

void VimMode::flushPending()
{
    // The time ran out while a sequence could still go on: run the one that's complete (d → cut, not dd).
    if (timeout_) g_source_remove(timeout_);
    timeout_ = 0;
    std::string action;
    for (auto &b : bindings_)
        if (b.tokens == pending_) action = b.action;
    int count = count_;
    pending_.clear();
    count_ = 0;
    if (!action.empty()) run(action, count);
    updateBar();
}

bool VimMode::key(guint keyval, GdkModifierType state)
{
    if (!settings().vimMode) return false;
    // Typing into a field (renaming, the location bar, dialogs) is insert mode: keys go to the field.
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win_->window()));
    if (focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT(focus) || GTK_IS_TEXT_VIEW(focus))) return false;
    // Only in the view (or with nothing focused); the panels keep their own keys.
    View *v = win_->activeView();
    if (focus && v && !gtk_widget_is_ancestor(focus, v->widget()) && focus != v->widget()) return false;
    if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R || keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R
        || keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R || keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R
        || keyval == GDK_KEY_ISO_Level3_Shift || keyval == GDK_KEY_Caps_Lock)
        return false;
    GdkModifierType mods = GdkModifierType(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK));
    std::string tok = vim_token(keyval, mods);
    if (tok.empty()) return false;
    // A count before a motion: 5j.
    gunichar u = gdk_keyval_to_unicode(keyval);
    if (pending_.empty() && !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK)) && g_unichar_isdigit(u) && (u != '0' || count_ > 0)) {
        bool bound = false;
        for (auto &b : bindings_)
            if (b.tokens.size() == 1 && b.tokens[0] == tok) bound = true;
        if (!bound) {
            count_ = std::min(count_ * 10 + (int)(u - '0'), 9999);
            updateBar();
            return true;
        }
    }
    if (timeout_) {
        g_source_remove(timeout_);
        timeout_ = 0;
    }
    pending_.push_back(tok);
    std::string exact;
    bool longer = false;
    for (auto &b : bindings_) {
        if (b.tokens.size() < pending_.size() || !std::equal(pending_.begin(), pending_.end(), b.tokens.begin())) continue;
        if (b.tokens.size() == pending_.size()) exact = b.action;
        else longer = true;
    }
    if (!exact.empty() && !longer) {
        int count = count_;
        pending_.clear();
        count_ = 0;
        run(exact, count);
        return true;
    }
    if (longer) {
        // Wait for the next key; when none comes, run what's complete so far.
        std::weak_ptr<void> alive = life_;
        timeout_ = g_timeout_add((guint)std::max(100, settings().vimTimeout), [](gpointer p) -> gboolean {
            auto *v = static_cast<VimMode *>(p);
            v->timeout_ = 0;
            v->flushPending();
            return G_SOURCE_REMOVE;
        }, this);
        updateBar();
        return true;
    }
    // Nothing starts like this. A key with Ctrl or Alt that no binding uses goes on to Beluga's shortcuts;
    // other keys are used up, so they don't start a type-ahead search.
    bool single = pending_.size() == 1;
    pending_.clear();
    count_ = 0;
    updateBar();
    if (single && (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK))) return false;
    if (single && keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35) return false;
    if (single && (keyval == GDK_KEY_Delete || keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab || keyval == GDK_KEY_Menu))
        return false;
    return true;
}

void VimMode::feed(const std::string &sequence)
{
    for (auto &t : vim_parse(sequence)) {
        guint keyval = 0;
        GdkModifierType mods = GdkModifierType(0);
        std::string k = t;
        if (k.size() > 2 && k[0] == '<' && k.back() == '>') {
            std::string inner = k.substr(1, k.size() - 2);
            while (inner.size() > 2 && inner[1] == '-') {
                if (inner[0] == 'C') mods = GdkModifierType(mods | GDK_CONTROL_MASK);
                if (inner[0] == 'A') mods = GdkModifierType(mods | GDK_ALT_MASK);
                if (inner[0] == 'S') mods = GdkModifierType(mods | GDK_SHIFT_MASK);
                inner = inner.substr(2);
            }
            for (auto &n : kNames)
                if (inner == n.name) keyval = n.keyval;
            if (!keyval) keyval = gdk_unicode_to_keyval(g_utf8_get_char(inner.c_str()));
        } else {
            keyval = gdk_unicode_to_keyval(g_utf8_get_char(k.c_str()));
            if (g_unichar_isupper(g_utf8_get_char(k.c_str()))) mods = GDK_SHIFT_MASK;
        }
        key(keyval, mods);
    }
    if (timeout_) flushPending();
}

// ---- the command line ---------------------------------------------------------------------------------

std::string VimMode::command(const std::string &rawLine)
{
    std::string line = trim(rawLine);
    if (!line.empty() && line[0] == ':') line = trim(line.substr(1));
    if (line.empty()) return {};
    ViewContainer *c = win_->activeContainer();
    GActionGroup *ag = G_ACTION_GROUP(win_->window());
    auto act = [&](const char *name, GVariant *param = nullptr) { g_action_group_activate_action(ag, name, param); };
    // :!command runs in the folder shown.
    if (line[0] == '!') {
        std::string cmd = trim(line.substr(1));
        std::string dir = c ? take(g_filename_from_uri(c->uri().c_str(), nullptr, nullptr)) : std::string();
        const char *argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
        GError *err = nullptr;
        if (!g_spawn_async(dir.empty() ? nullptr : dir.c_str(), (char **)argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &err)) {
            std::string m = err ? err->message : "";
            g_clear_error(&err);
            return m;
        }
        return {};
    }
    std::string name = line, arg;
    size_t sp = line.find_first_of(" \t");
    if (sp != std::string::npos) {
        name = line.substr(0, sp);
        arg = trim(line.substr(sp + 1));
    }
    bool bang = !name.empty() && name.back() == '!';
    if (bang) name.pop_back();
    auto is = [&](std::initializer_list<const char *> names) {
        for (const char *n : names)
            if (name == n) return true;
        return false;
    };
    auto location = [&](const std::string &text) -> std::string {
        std::string t = text;
        if (starts_with(t, "~")) t = home_dir() + t.substr(1);
        Ref<GFile> f(file_for_input(t, c ? Ref<GFile>(g_file_new_for_uri(c->uri().c_str())).get() : nullptr));
        return f ? file_uri(f.get()) : std::string();
    };

    if (is({"q", "quit", "clo", "close"})) {
        if (bang) win_->closeWithoutAsking();
        else gtk_window_close(GTK_WINDOW(win_->window()));
    } else if (is({"qa", "qall", "quitall", "qa", "wqa", "xa"})) {
        if (bang) App::instance().quit();
        else
            for (MainWindow *w : std::vector<MainWindow *>(App::instance().windows())) gtk_window_close(GTK_WINDOW(w->window()));
    } else if (is({"e", "edit", "cd", "o", "open"})) {
        if (arg.empty()) {
            if (c) c->goHome();
        } else {
            std::string uri = location(arg);
            if (uri.empty() || !c) return printf_str(_("Can't open “%s”."), arg.c_str());
            c->navigate(uri);
        }
    } else if (is({"tabnew", "tabe", "tabedit", "tabnext", "tabn", "tabprevious", "tabp", "tabN", "tabc", "tabclose", "tabo", "tabonly"})) {
        if (is({"tabnew", "tabe", "tabedit"})) {
            std::string uri = arg.empty() ? (c ? c->uri() : std::string()) : location(arg);
            if (uri.empty()) return printf_str(_("Can't open “%s”."), arg.c_str());
            win_->openTab(uri, {}, true);
        } else if (is({"tabnext", "tabn"})) act("next-tab");
        else if (is({"tabprevious", "tabp", "tabN"})) act("prev-tab");
        else if (is({"tabc", "tabclose"})) act("close-tab");
        else {
            TabPage *keep = win_->activeTab();
            for (int i = win_->tabCount() - 1; i >= 0; --i)
                if (win_->tab(i) != keep) win_->closeTab(i);
        }
    } else if (is({"sp", "split", "vs", "vsp", "vsplit"})) {
        TabPage *t = win_->activeTab();
        if (!t) return {};
        std::string uri = arg.empty() ? (c ? c->uri() : std::string()) : location(arg);
        if (uri.empty()) return printf_str(_("Can't open “%s”."), arg.c_str());
        if (!t->split()) t->setSplit(true, uri);
        else t->other(t->active())->navigate(uri);
    } else if (is({"on", "only"})) {
        TabPage *t = win_->activeTab();
        if (t && t->split()) t->closeView(t->other(t->active()));
    } else if (is({"mkdir", "md", "touch"})) {
        if (arg.empty()) return _("A name is needed.");
        if (!c) return {};
        Ref<GFile> dir(g_file_new_for_uri(c->uri().c_str()));
        Ref<GFile> created;
        GError *err = nullptr;
        bool ok = name == "touch" ? create_empty_file(dir.get(), arg, created, &err) : make_directory(dir.get(), arg, created, &err);
        if (!ok) {
            std::string m = err ? err->message : "";
            g_clear_error(&err);
            return m;
        }
    } else if (is({"set", "se"})) {
        if (arg == "hidden" || arg == "hidden!" || arg == "nohidden" || arg == "invhidden") {
            View *v = win_->activeView();
            bool on = v && v->props().hiddenFilesShown;
            if ((arg == "hidden" && !on) || (arg == "nohidden" && on) || arg == "hidden!" || arg == "invhidden") act("show-hidden");
        } else if (starts_with(arg, "view=")) {
            std::string m = arg.substr(5);
            if (m != "icons" && m != "compact" && m != "details" && m != "columns") return printf_str(_("Unknown view: %s"), m.c_str());
            act("view-mode", g_variant_new_string(m.c_str()));
        } else if (arg == "previews" || arg == "nopreviews") {
            View *v = win_->activeView();
            bool on = v && v->props().previewsShown;
            if ((arg == "previews") != on) act("show-previews");
        } else {
            return printf_str(_("Unknown option: %s"), arg.c_str());
        }
    } else if (is({"sort"})) {
        static const std::pair<const char *, const char *> roles[] = {
            {"name", "text"}, {"size", "size"}, {"date", "modificationtime"}, {"modified", "modificationtime"},
            {"type", "type"}, {"extension", "extension"}};
        std::string role;
        for (auto &[k, r] : roles)
            if (starts_with(arg, k)) role = r;
        if (role.empty()) return _("Sort by name, size, date or type.");
        act("sort-by", g_variant_new_string(role.c_str()));
    } else if (is({"term", "terminal", "sh", "shell"})) {
        act("open-terminal");
    } else if (is({"pwd"})) {
        showMessage(c ? display_location(c->uri()) : std::string(), false);
        return {};
    } else if (is({"du"})) {
        act("show-disk-usage");
    } else if (is({"h", "help", "map", "maps"})) {
        show_preferences(win_->widget(), "vim");
    } else if (is({"reload", "e!"})) {
        act("reload");
    } else {
        return printf_str(_("Not a command: %s"), name.c_str());
    }
    return {};
}

} // namespace beluga
