// SPDX-License-Identifier: GPL-2.0-or-later
#include "preferences.hpp"

#include "app.hpp"
#include "customactions.hpp"
#include "dialogs.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"
#include "terminal.hpp"
#include "trash.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <thread>

namespace beluga {

namespace {

guint g_saveSource = 0;

void changed()
{
    if (g_saveSource) g_source_remove(g_saveSource);
    g_saveSource = g_timeout_add(300, [](gpointer) -> gboolean {
        g_saveSource = 0;
        settings().save();
        notify_settings_changed();
        App::instance().updateAccels();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

using IntGet = std::function<int()>;
using IntSet = std::function<void(int)>;

struct Page {
    GtkWidget *scroll;
    GtkWidget *box;
};

Page page(GtkWidget *stack, const char *name, const char *title)
{
    Page p;
    p.scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p.scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(p.box, 18);
    gtk_widget_set_margin_end(p.box, 18);
    gtk_widget_set_margin_top(p.box, 14);
    gtk_widget_set_margin_bottom(p.box, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p.scroll), p.box);
    gtk_stack_add_titled(GTK_STACK(stack), p.scroll, name, title);
    return p;
}

void heading(Page &p, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (gtk_widget_get_first_child(p.box)) gtk_widget_set_margin_top(l, 12);
    gtk_box_append(GTK_BOX(p.box), l);
}

void note(Page &p, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(p.box), l);
}

GtkWidget *check(Page &p, const char *label, bool &field)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), field);
    g_signal_connect(c, "toggled", CB([](GtkCheckButton *b, gpointer f) {
        *static_cast<bool *>(f) = gtk_check_button_get_active(b);
        changed();
    }), &field);
    gtk_box_append(GTK_BOX(p.box), c);
    return c;
}

GtkWidget *row(Page &p, const char *label, GtkWidget *w)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_box_append(GTK_BOX(r), l);
    gtk_box_append(GTK_BOX(r), w);
    gtk_box_append(GTK_BOX(p.box), r);
    return r;
}

GtkWidget *choice(Page &p, const char *label, std::vector<const char *> options, IntGet get, IntSet set)
{
    options.push_back(nullptr);
    GtkWidget *d = gtk_drop_down_new_from_strings(options.data());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d), get());
    auto *fn = new IntSet(std::move(set));
    g_signal_connect_data(d, "notify::selected", CB([](GtkDropDown *dd, GParamSpec *, gpointer f) {
        (*static_cast<IntSet *>(f))((int)gtk_drop_down_get_selected(dd));
        changed();
    }), fn, [](gpointer f, GClosure *) { delete static_cast<IntSet *>(f); }, GConnectFlags(0));
    row(p, label, d);
    return d;
}

GtkWidget *spin(Page &p, const char *label, double min, double max, double step, IntGet get, IntSet set)
{
    GtkWidget *s = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), get());
    auto *fn = new IntSet(std::move(set));
    g_signal_connect_data(s, "value-changed", CB([](GtkSpinButton *sb, gpointer f) {
        (*static_cast<IntSet *>(f))(gtk_spin_button_get_value_as_int(sb));
        changed();
    }), fn, [](gpointer f, GClosure *) { delete static_cast<IntSet *>(f); }, GConnectFlags(0));
    row(p, label, s);
    return s;
}

GtkWidget *text(Page &p, const char *label, std::string &field, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), field.c_str());
    gtk_widget_set_size_request(e, 260, -1);
    if (placeholder) g_object_set(e, "placeholder-text", placeholder, nullptr);
    g_signal_connect(e, "changed", CB([](GtkEditable *ed, gpointer f) {
        *static_cast<std::string *>(f) = gtk_editable_get_text(ed);
        changed();
    }), &field);
    row(p, label, e);
    return e;
}

const int kSizes[] = {16, 22, 32, 48, 64, 80, 96, 112, 128, 160, 192, 256};
std::vector<const char *> size_labels()
{
    static std::vector<std::string> s;
    if (s.empty())
        for (int v : kSizes) s.push_back(printf_str(_("%d px"), v));
    std::vector<const char *> out;
    for (auto &x : s) out.push_back(x.c_str());
    return out;
}
int size_index(int px)
{
    int best = 0;
    for (int i = 0; i < (int)G_N_ELEMENTS(kSizes); ++i)
        if (kSizes[i] <= px) best = i;
    return best;
}

void font_row(Page &p, ModeSettings &m)
{
    GtkWidget *sys = check(p, _("Use the system _font"), m.useSystemFont);
    GtkFontDialog *fd = gtk_font_dialog_new();
    GtkWidget *fb = gtk_font_dialog_button_new(fd);
    if (!m.font.empty()) {
        PangoFontDescription *desc = pango_font_description_from_string(m.font.c_str());
        gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(fb), desc);
        pango_font_description_free(desc);
    }
    gtk_widget_set_sensitive(fb, !m.useSystemFont);
    g_signal_connect(sys, "toggled", CB([](GtkCheckButton *b, gpointer w) {
        gtk_widget_set_sensitive(GTK_WIDGET(w), !gtk_check_button_get_active(b));
    }), fb);
    g_signal_connect(fb, "notify::font-desc", CB([](GtkFontDialogButton *b, GParamSpec *, gpointer mp) {
        PangoFontDescription *d = gtk_font_dialog_button_get_font_desc(b);
        if (d) static_cast<ModeSettings *>(mp)->font = take(pango_font_description_to_string(d));
        changed();
    }), &m);
    row(p, _("View font:"), fb);
}

void mode_sizes(Page &p, ModeSettings &m)
{
    choice(p, _("Icon size:"), size_labels(), [&m] { return size_index(m.iconSize); }, [&m](int i) { m.iconSize = kSizes[i]; });
    choice(p, _("Preview size:"), size_labels(), [&m] { return size_index(m.previewSize); }, [&m](int i) { m.previewSize = kSizes[i]; });
}

// ---- custom actions -------------------------------------------------------------------------

void edit_action(GtkWidget *parent, const CustomAction &init, std::function<void(const CustomAction &)> done)
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), _("Context Menu Action"));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), window_of(parent));
    gtk_window_set_default_size(GTK_WINDOW(win), 520, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_window_set_child(GTK_WINDOW(win), box);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int r = 0;
    auto entry = [&](const char *label, const std::string &value, const char *ph) {
        GtkWidget *l = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        GtkWidget *e = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(e), value.c_str());
        if (ph) g_object_set(e, "placeholder-text", ph, nullptr);
        gtk_widget_set_hexpand(e, TRUE);
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), e, 1, r++, 1, 1);
        return e;
    };
    GtkWidget *name = entry(_("Name:"), init.name, nullptr);
    GtkWidget *icon = entry(_("Icon name:"), init.icon, "e.g. archive-extract");
    GtkWidget *cmd = entry(_("Command:"), init.command, "e.g. gimp %F");
    GtkWidget *pat = entry(_("File names:"), init.patterns, "*.jpg;*.png (empty: all)");
    GtkWidget *mime = entry(_("Types:"), init.mimeTypes, "image/*;application/pdf (empty: all)");
    gtk_box_append(GTK_BOX(box), grid);
    GtkWidget *hint = gtk_label_new(_("%f first file, %F all files, %u / %U as URIs, %n / %N names, %d current folder."));
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_append(GTK_BOX(box), hint);
    auto chk = [&](const char *label, bool on) {
        GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), on);
        gtk_box_append(GTK_BOX(box), c);
        return c;
    };
    GtkWidget *files = chk(_("Show for _files"), init.forFiles);
    GtkWidget *folders = chk(_("Show for f_olders"), init.forFolders);
    GtkWidget *multi = chk(_("Show when _several items are selected"), init.multiple);
    GtkWidget *term = chk(_("Run in a _terminal"), init.runInTerminal);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    GtkWidget *ok = gtk_button_new_with_mnemonic(_("_OK"));
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), ok);
    gtk_box_append(GTK_BOX(box), buttons);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    struct W { GtkWidget *win, *name, *icon, *cmd, *pat, *mime, *files, *folders, *multi, *term; std::function<void(const CustomAction &)> done; };
    auto *w = new W{win, name, icon, cmd, pat, mime, files, folders, multi, term, std::move(done)};
    g_object_set_data_full(G_OBJECT(win), "beluga-w", w, [](gpointer p) { delete static_cast<W *>(p); });
    g_signal_connect(ok, "clicked", CB([](GtkButton *, gpointer win) {
        auto *w = static_cast<W *>(g_object_get_data(G_OBJECT(win), "beluga-w"));
        CustomAction a;
        auto t = [](GtkWidget *e) { return trim(gtk_editable_get_text(GTK_EDITABLE(e))); };
        a.name = t(w->name);
        a.icon = t(w->icon);
        a.command = t(w->cmd);
        a.patterns = t(w->pat);
        a.mimeTypes = t(w->mime);
        a.forFiles = gtk_check_button_get_active(GTK_CHECK_BUTTON(w->files));
        a.forFolders = gtk_check_button_get_active(GTK_CHECK_BUTTON(w->folders));
        a.multiple = gtk_check_button_get_active(GTK_CHECK_BUTTON(w->multi));
        a.runInTerminal = gtk_check_button_get_active(GTK_CHECK_BUTTON(w->term));
        if (a.name.empty() || a.command.empty()) return;
        auto done = w->done;
        gtk_window_destroy(GTK_WINDOW(w->win));
        done(a);
    }), win);
    gtk_window_present(GTK_WINDOW(win));
}

struct ActionsUi {
    GtkWidget *list;
    std::vector<CustomAction> actions;
};

void fill_actions(ActionsUi *ui)
{
    list_box_remove_rows(GTK_LIST_BOX(ui->list));
    for (auto &a : ui->actions) {
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(r), gtk_image_new_from_icon_name(a.icon.empty() ? "system-run" : a.icon.c_str()));
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *n = gtk_label_new(a.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(n), 0);
        GtkWidget *c = gtk_label_new(a.command.c_str());
        gtk_label_set_xalign(GTK_LABEL(c), 0);
        gtk_label_set_ellipsize(GTK_LABEL(c), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(c, "dim-label");
        gtk_box_append(GTK_BOX(texts), n);
        gtk_box_append(GTK_BOX(texts), c);
        gtk_box_append(GTK_BOX(r), texts);
        gtk_list_box_append(GTK_LIST_BOX(ui->list), r);
    }
}

void actions_section(Page &p)
{
    heading(p, _("Custom Actions"));
    note(p, _("Your own entries for the context menu of files and folders."));
    auto *ui = new ActionsUi;
    ui->actions = load_custom_actions();
    ui->list = gtk_list_box_new();
    gtk_widget_add_css_class(ui->list, "boxed-list");
    g_object_set_data_full(G_OBJECT(ui->list), "beluga-ui", ui, [](gpointer d) { delete static_cast<ActionsUi *>(d); });
    fill_actions(ui);
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_child(GTK_FRAME(frame), ui->list);
    gtk_box_append(GTK_BOX(p.box), frame);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *addB = gtk_button_new_with_mnemonic(_("_Add…"));
    GtkWidget *editB = gtk_button_new_with_mnemonic(_("_Edit…"));
    GtkWidget *delB = gtk_button_new_with_mnemonic(_("_Remove"));
    gtk_box_append(GTK_BOX(buttons), addB);
    gtk_box_append(GTK_BOX(buttons), editB);
    gtk_box_append(GTK_BOX(buttons), delB);
    gtk_box_append(GTK_BOX(p.box), buttons);
    g_signal_connect(addB, "clicked", CB([](GtkButton *b, gpointer d) {
        auto *ui = static_cast<ActionsUi *>(d);
        CustomAction a;
        a.forFiles = true;
        edit_action(GTK_WIDGET(b), a, [ui](const CustomAction &na) {
            ui->actions.push_back(na);
            save_custom_actions(ui->actions);
            fill_actions(ui);
        });
    }), ui);
    g_signal_connect(editB, "clicked", CB([](GtkButton *b, gpointer d) {
        auto *ui = static_cast<ActionsUi *>(d);
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->list));
        if (!row) return;
        int i = gtk_list_box_row_get_index(row);
        edit_action(GTK_WIDGET(b), ui->actions[i], [ui, i](const CustomAction &na) {
            ui->actions[i] = na;
            save_custom_actions(ui->actions);
            fill_actions(ui);
        });
    }), ui);
    g_signal_connect(delB, "clicked", CB([](GtkButton *, gpointer d) {
        auto *ui = static_cast<ActionsUi *>(d);
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ui->list));
        if (!row) return;
        ui->actions.erase(ui->actions.begin() + gtk_list_box_row_get_index(row));
        save_custom_actions(ui->actions);
        fill_actions(ui);
    }), ui);
}

// ---- shortcuts ---------------------------------------------------------------------------------

void shortcuts_page(Page &p)
{
    note(p, _("Accelerators are written like <Control>c or <Shift>Delete. Separate several with commas. "
              "Leave a field empty to use the default."));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int r = 0;
    for (auto &s : shortcut_table()) {
        GtkWidget *l = gtk_label_new(s.title);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        GtkWidget *e = gtk_entry_new();
        gtk_widget_set_size_request(e, 240, -1);
        g_object_set(e, "placeholder-text", join(s.defaults, ", ").c_str(), nullptr);
        auto it = settings().shortcuts.find(s.action);
        if (it != settings().shortcuts.end()) gtk_editable_set_text(GTK_EDITABLE(e), join(it->second, ", ").c_str());
        g_object_set_data_full(G_OBJECT(e), "beluga-action", g_strdup(s.action), g_free);
        g_signal_connect(e, "changed", CB([](GtkEditable *ed) {
            std::string action = static_cast<const char *>(g_object_get_data(G_OBJECT(ed), "beluga-action"));
            std::vector<std::string> accels;
            bool valid = true;
            for (auto &a : split(gtk_editable_get_text(ed), ',')) {
                std::string t = trim(a);
                if (t.empty()) continue;
                guint key = 0;
                GdkModifierType mods;
                if (!gtk_accelerator_parse(t.c_str(), &key, &mods) || key == 0) valid = false;
                accels.push_back(t);
            }
            if (!valid) {
                gtk_widget_add_css_class(GTK_WIDGET(ed), "error");
                return;
            }
            gtk_widget_remove_css_class(GTK_WIDGET(ed), "error");
            if (accels.empty()) settings().shortcuts.erase(action);
            else settings().shortcuts[action] = accels;
            changed();
        }), nullptr);
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), e, 1, r++, 1, 1);
    }
    gtk_box_append(GTK_BOX(p.box), grid);
}

} // namespace

void show_preferences(GtkWidget *parent)
{
    static GtkWidget *open = nullptr;
    if (open) {
        gtk_window_present(GTK_WINDOW(open));
        return;
    }
    Settings &s = settings();
    GtkWidget *win = gtk_window_new();
    open = win;
    g_signal_connect(win, "destroy", CB([](GtkWidget *) { open = nullptr; }), nullptr);
    gtk_window_set_title(GTK_WINDOW(win), _("Configure Beluga"));
    if (GtkWindow *pw = window_of(parent)) {
        gtk_window_set_transient_for(GTK_WINDOW(win), pw);
        if (GtkApplication *app = gtk_window_get_application(pw)) gtk_window_set_application(GTK_WINDOW(win), app);
    }
    gtk_window_set_default_size(GTK_WINDOW(win), 820, 620);
    gtk_widget_add_css_class(win, "beluga-preferences");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    GtkWidget *side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
    gtk_widget_set_size_request(side, 190, -1);
    gtk_box_append(GTK_BOX(hbox), side);
    gtk_box_append(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(hbox), stack);
    gtk_window_set_child(GTK_WINDOW(win), hbox);

    // ---- General
    Page g = page(stack, "general", _("General"));
    heading(g, _("Startup"));
    GtkWidget *home = text(g, _("_Home folder:"), s.homeUrl, nullptr);
    gtk_editable_set_text(GTK_EDITABLE(home), display_location(s.homeUrl).c_str());
    g_signal_handlers_disconnect_matched(home, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, &s.homeUrl);
    g_signal_connect(home, "changed", CB([](GtkEditable *e) {
        Ref<GFile> f(file_for_input(gtk_editable_get_text(e), nullptr));
        if (f) {
            settings().homeUrl = file_uri(f.get());
            changed();
        }
    }), nullptr);
    check(g, _("_Remember open folders and tabs"), s.rememberOpenedTabs);
    check(g, _("Begin in _split view mode"), s.splitView);
    check(g, _("Show _filter bar"), s.filterBar);
    check(g, _("Make location bar _editable"), s.editableUrl);
    check(g, _("Show full path inside _location bar"), s.showFullPath);
    check(g, _("Show full path in _title bar"), s.showFullPathInTitlebar);
    heading(g, _("Mouse"));
    note(g, _("These used to be KDE system settings."));
    choice(g, _("Clicking files or folders:"), {_("Selects them (double-click opens)"), _("Opens them")},
           [&s] { return s.singleClick ? 1 : 0; }, [&s](int i) { s.singleClick = i == 1; });
    choice(g, _("Double-clicking empty space:"), {_("Does nothing"), _("Selects all items"), _("Goes up"), _("Opens a new tab"), _("Toggles hidden files")},
           [&s] { return s.doubleClickViewAction; }, [&s](int i) { s.doubleClickViewAction = i; });
    heading(g, _("Tabs and Split View"));
    check(g, _("Open new tabs after the _last tab"), s.openNewTabAfterLastTab);
    check(g, _("_Always show the tab bar"), s.alwaysShowTabBar);
    check(g, _("Show _close buttons on tabs"), s.showCloseButtonOnTabs);
    check(g, _("Open folders from other applications in a new _tab"), s.openExternallyCalledFolderInNewTab);
    choice(g, _("Closing split view closes:"), {_("The active view"), _("The inactive view"), _("The right view")},
           [&s] { return s.closeSplitViewChoice; }, [&s](int i) { s.closeSplitViewChoice = i; });
    heading(g, _("Behavior"));
    check(g, _("Rename single items _inline"), s.renameInline);
    check(g, _("Show selection _marker"), s.showSelectionToggle);
    check(g, _("Show item information on _hover"), s.showToolTips);
    check(g, _("Open folders while _dragging items over them"), s.autoExpandFolders);
    heading(g, _("Terminal"));
    note(g, _("Used by F4 and Open Terminal Here. %d in the command is replaced by the folder; "
              "otherwise the terminal starts in it. This used to be a KDE system setting."));
    std::string detected = terminal::detect();
    text(g, _("Terminal _command:"), s.terminalCommand,
         detected.empty() ? _("No terminal found") : printf_str(_("Detected: %s"), detected.c_str()).c_str());

    // ---- View
    Page v = page(stack, "view", _("View"));
    heading(v, _("General"));
    check(v, _("Use the same display style for _all folders"), s.globalViewProps);
    note(v, _("When this is off, every folder remembers its own style. Either way, View › Remember View for "
              "This Folder gives one folder its own style."));
    choice(v, _("Sorting:"), {_("Natural"), _("Alphabetical, case insensitive"), _("Alphabetical, case sensitive")},
           [&s] { return s.sortingChoice; }, [&s](int i) { s.sortingChoice = i; });
    choice(v, _("Folder size:"), {_("Number of items"), _("Size of contents"), _("None")},
           [&s] { return (int)s.dirSizeMode; }, [&s](int i) { s.dirSizeMode = (DirSizeMode)i; });
    spin(v, _("Folder size depth limit:"), 1, 100, 1, [&s] { return s.recursiveDirSizeLimit; }, [&s](int i) { s.recursiveDirSizeLimit = i; });
    choice(v, _("Dates:"), {_("Relative (Today, Yesterday…)"), _("Short")},
           [&s] { return s.useShortRelativeDates ? 0 : 1; }, [&s](int i) { s.useShortRelativeDates = i == 0; });
    choice(v, _("Permissions:"), {_("Symbolic (rwxr-xr-x)"), _("Numeric (0755)"), _("Combined")},
           [&s] { return s.permissionsFormat; }, [&s](int i) { s.permissionsFormat = i; });
    choice(v, _("Shorten long names:"), {_("At the end"), _("In the middle")},
           [&s] { return s.elidingMode; }, [&s](int i) { s.elidingMode = i; });
    check(v, _("Hide _backup files (ending in ~)"), s.hideBackupFiles);
    heading(v, _("Icons"));
    mode_sizes(v, s.icons);
    choice(v, _("Label width:"), {_("Small"), _("Medium"), _("Large"), _("Huge")},
           [&s] { return s.iconsTextWidth; }, [&s](int i) { s.iconsTextWidth = i; });
    spin(v, _("Maximum label lines (0: no limit):"), 0, 10, 1, [&s] { return s.iconsMaxLines; }, [&s](int i) { s.iconsMaxLines = i; });
    check(v, _("Co_mpact spacing"), s.iconsCompactSpacing);
    font_row(v, s.icons);
    heading(v, _("Compact"));
    mode_sizes(v, s.compact);
    choice(v, _("Maximum label width:"), {_("Unlimited"), _("Small"), _("Medium"), _("Large")},
           [&s] { return s.compactMaxTextWidth; }, [&s](int i) { s.compactMaxTextWidth = i; });
    font_row(v, s.compact);
    heading(v, _("Details"));
    mode_sizes(v, s.details);
    check(v, _("E_xpandable folders"), s.detailsExpandableFolders);
    check(v, _("Highlight the entire _row"), s.detailsHighlightEntireRow);
    font_row(v, s.details);

    // ---- Previews
    Page pv = page(stack, "previews", _("Previews"));
    check(pv, _("Show _previews in new folders"), s.previewsDefault);
    heading(pv, _("Show previews for"));
    struct T { const char *label; const char *mime; };
    static const T types[] = {{N_("Images"), "image/*"}, {N_("Videos"), "video/*"}, {N_("Audio (cover art)"), "audio/*"},
                              {N_("PDF documents"), "application/pdf"}, {N_("Text files"), "text/*"}, {N_("Fonts"), "font/*"},
                              {N_("Office documents"), "application/vnd.oasis.opendocument.text"}};
    for (const T &t : types) {
        GtkWidget *c = gtk_check_button_new_with_label(_(t.label));
        bool disabled = std::find(s.disabledPreviewMime.begin(), s.disabledPreviewMime.end(), t.mime) != s.disabledPreviewMime.end();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), !disabled);
        g_object_set_data(G_OBJECT(c), "beluga-mime", (gpointer)t.mime);
        g_signal_connect(c, "toggled", CB([](GtkCheckButton *b) {
            std::string m = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "beluga-mime"));
            auto &list = settings().disabledPreviewMime;
            list.erase(std::remove(list.begin(), list.end(), m), list.end());
            if (!gtk_check_button_get_active(b)) list.push_back(m);
            changed();
        }), nullptr);
        gtk_box_append(GTK_BOX(pv.box), c);
    }
    heading(pv, _("Limits"));
    spin(pv, _("Skip local files larger than (MiB, 0: no limit):"), 0, 100000, 10, [&s] { return s.maxLocalPreviewMiB; }, [&s](int i) { s.maxLocalPreviewMiB = i; });
    spin(pv, _("Remote files up to (MiB, 0: never):"), 0, 100000, 1, [&s] { return s.maxRemotePreviewMiB; }, [&s](int i) { s.maxRemotePreviewMiB = i; });

    // ---- Confirmations
    Page c = page(stack, "confirmations", _("Confirmations"));
    heading(c, _("Ask for confirmation when"));
    check(c, _("Moving files or folders to the _trash"), s.confirmTrash);
    check(c, _("_Deleting files or folders"), s.confirmDelete);
    check(c, _("_Emptying the trash"), s.confirmEmptyTrash);
    check(c, _("Closing windows with _multiple tabs"), s.confirmClosingMultipleTabs);
    check(c, _("Opening many _folders at once"), s.confirmOpenManyFolders);
    check(c, _("Opening many t_erminals at once"), s.confirmOpenManyTerminals);
    check(c, _("Renaming changes a file's _type"), s.confirmRenameFileType);
    choice(c, _("When opening an executable file:"), {_("Always ask"), _("Open in an application"), _("Run it")},
           [&s] { return (int)s.executableAction; }, [&s](int i) { s.executableAction = (ExecAction)i; });

    // ---- Interface: bars and panels
    Page b = page(stack, "panels", _("Bars and Panels"));
    heading(b, _("Status Bar"));
    choice(b, _("Status bar:"), {_("Small (shows only when hovering or selecting)"), _("Full width"), _("Hidden")},
           [&s] { return (int)s.statusBar; }, [&s](int i) { s.statusBar = (StatusBarMode)i; });
    check(b, _("Show _zoom slider"), s.showZoomSlider);
    check(b, _("Show free _space"), s.showSpaceInfo);
    heading(b, _("Places Panel"));
    choice(b, _("Icon size:"), {"16 px", "22 px", "32 px", "48 px"},
           [&s] { return s.placesIconSize <= 16 ? 0 : s.placesIconSize <= 22 ? 1 : s.placesIconSize <= 32 ? 2 : 3; },
           [&s](int i) { static const int v[] = {16, 22, 32, 48}; s.placesIconSize = v[i]; });
    heading(b, _("Folders Panel"));
    check(b, _("Show _hidden folders"), s.foldersHiddenShown);
    check(b, _("Limit to the home folder when inside it"), s.foldersLimitToHome);
    check(b, _("Scroll to the current folder _automatically"), s.foldersAutoScroll);
    heading(b, _("Information Panel"));
    check(b, _("Show _previews"), s.infoPreviews);
    check(b, _("Play media files _automatically"), s.infoAutoPlay);
    check(b, _("Show the _hovered item"), s.infoShowHovered);
    check(b, _("Use _long date format"), s.infoLongDates);

    // ---- Context menu
    Page m = page(stack, "contextmenu", _("Context Menu"));
    heading(m, _("Show in the context menu"));
    check(m, _("'Copy To' and 'Move To'"), s.ctxCopyMoveMenu);
    check(m, _("Add to Places"), s.ctxAddToPlaces);
    check(m, _("Sort By"), s.ctxSortBy);
    check(m, _("View Mode"), s.ctxViewMode);
    check(m, _("Open in New Tab"), s.ctxOpenInNewTab);
    check(m, _("Open in New Window"), s.ctxOpenInNewWindow);
    check(m, _("Open in Split View"), s.ctxOpenInSplitView);
    check(m, _("Copy Location"), s.ctxCopyLocation);
    check(m, _("Duplicate Here"), s.ctxDuplicateHere);
    check(m, _("Open Terminal Here"), s.ctxOpenTerminal);
    check(m, _("Copy to Other View"), s.ctxCopyToOtherView);
    check(m, _("Move to Other View"), s.ctxMoveToOtherView);
    check(m, _("Delete (in addition to Move to Trash)"), s.ctxDeletePermanently);
    actions_section(m);

    // ---- Trash
    Page t = page(stack, "trash", _("Trash"));
    note(t, _("These used to be KDE system settings. Limits are applied when Beluga starts and after moving items to the trash."));
    GtkWidget *timeCheck = check(t, _("_Delete items that have been in the trash longer than:"), s.trashUseTimeLimit);
    (void)timeCheck;
    spin(t, _("Days:"), 1, 3650, 1, [&s] { return s.trashDays; }, [&s](int i) { s.trashDays = i; });
    check(t, _("_Limit the trash to a share of the disk:"), s.trashUseSizeLimit);
    spin(t, _("Percent of the disk:"), 1, 100, 1, [&s] { return (int)s.trashPercent; }, [&s](int i) { s.trashPercent = i; });
    choice(t, _("When the limit is reached:"), {_("Show a warning"), _("Delete the oldest items"), _("Delete the largest items")},
           [&s] { return (int)s.trashLimitAction; }, [&s](int i) { s.trashLimitAction = (TrashLimitAction)i; });
    GtkWidget *sizeLabel = gtk_label_new(_("Calculating trash size…"));
    gtk_label_set_xalign(GTK_LABEL(sizeLabel), 0);
    gtk_box_append(GTK_BOX(t.box), sizeLabel);
    Ref<GtkWidget> sl = Ref<GtkWidget>::share(sizeLabel);
    std::thread([sl] {
        guint64 size = trash::home_trash_size();
        run_on_main([sl, size] { gtk_label_set_text(GTK_LABEL(sl.get()), printf_str(_("The trash currently uses %s."), format_size(size).c_str()).c_str()); });
    }).detach();
    GtkWidget *empty = gtk_button_new_with_mnemonic(_("_Empty Trash Now"));
    gtk_widget_set_halign(empty, GTK_ALIGN_START);
    g_signal_connect(empty, "clicked", CB([](GtkButton *b) {
        ask(GTK_WIDGET(b), _("Empty the trash?"), _("All items in the trash will be deleted permanently."), {_("Cancel"), _("Empty Trash")}, 1, 0,
            [](int r) { if (r == 1) FileJob::emptyTrash()->start(); });
    }), nullptr);
    gtk_box_append(GTK_BOX(t.box), empty);

    // ---- Shortcuts
    Page k = page(stack, "shortcuts", _("Keyboard Shortcuts"));
    shortcuts_page(k);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            gtk_window_destroy(GTK_WINDOW(w));
            return TRUE;
        }
        return FALSE;
    }), win);
    gtk_widget_add_controller(win, key);
    gtk_window_present(GTK_WINDOW(win));
}

} // namespace beluga
