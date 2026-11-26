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

// Changes show at once (debounced); Cancel puts back the settings the dialog was opened (or last applied) with.
guint g_saveSource = 0;
Settings *g_snapshot = nullptr;
GtkWidget *g_apply = nullptr;

void changed()
{
    if (g_apply) gtk_widget_set_sensitive(g_apply, TRUE);
    if (g_saveSource) g_source_remove(g_saveSource);
    g_saveSource = g_timeout_add(300, [](gpointer) -> gboolean {
        g_saveSource = 0;
        settings().save();
        notify_settings_changed();
        App::instance().updateAccels();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

void save_now()
{
    if (g_saveSource) g_source_remove(g_saveSource);
    g_saveSource = 0;
    settings().save();
    notify_settings_changed();
    App::instance().updateAccels();
}

using IntGet = std::function<int()>;
using IntSet = std::function<void(int)>;

std::string plain(const char *mnemonic)
{
    std::string r;
    for (const char *c = mnemonic; c && *c; ++c)
        if (*c != '_') r += *c;
    return r;
}

// ---- a page of the custom actions and shortcuts (a column of widgets) -------------------------

struct Page {
    GtkWidget *scroll;
    GtkWidget *box;
    std::string *words = nullptr;   // what the sidebar search looks for
};

Page column_page(std::string *words)
{
    Page p;
    p.words = words;
    p.scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p.scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(p.box, 18);
    gtk_widget_set_margin_end(p.box, 18);
    gtk_widget_set_margin_top(p.box, 14);
    gtk_widget_set_margin_bottom(p.box, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p.scroll), p.box);
    return p;
}

void heading(Page &p, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (gtk_widget_get_first_child(p.box)) gtk_widget_set_margin_top(l, 12);
    gtk_box_append(GTK_BOX(p.box), l);
    if (p.words) *p.words += std::string(" ") + text;
}

void note(Page &p, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(p.box), l);
}

// ---- Dolphin-style forms: labels on the left, right-aligned; the settings to their right -------

struct Form {
    GtkWidget *grid;
    int row = 0;
    std::string pending;     // the label for the next row
    std::string *words;
};

Form form_tab(GtkWidget *notebook, const char *title, std::string *words)
{
    Form f;
    f.words = words;
    *words += std::string(" ") + title;
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    f.grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(f.grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(f.grid), 10);
    gtk_widget_set_halign(f.grid, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(f.grid, 14);
    gtk_widget_set_margin_bottom(f.grid, 18);
    gtk_widget_set_margin_start(f.grid, 18);
    gtk_widget_set_margin_end(f.grid, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), f.grid);
    if (notebook) {
        // Tabs share the width, as in Dolphin's settings.
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, gtk_label_new(title));
        g_object_set(gtk_notebook_get_page(GTK_NOTEBOOK(notebook), scroll), "tab-expand", TRUE, nullptr);
    }
    g_object_set_data(G_OBJECT(f.grid), "beluga-scroll", scroll);
    return f;
}

void label(Form &f, const char *text) { f.pending = text; }

GtkWidget *put(Form &f, GtkWidget *w)
{
    if (!f.pending.empty()) {
        GtkWidget *l = gtk_label_new(f.pending.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(f.grid), l, 0, f.row, 1, 1);
        *f.words += " " + f.pending;
        f.pending.clear();
    }
    gtk_widget_set_halign(w, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(f.grid), w, 1, f.row++, 1, 1);
    return w;
}

void gap(Form &f)
{
    GtkWidget *g = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(g, -1, 8);
    gtk_grid_attach(GTK_GRID(f.grid), g, 0, f.row++, 2, 1);
}

// A title over the rows below it, across both columns (Dolphin's group titles).
void title(Form &f, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (f.row > 0) gtk_widget_set_margin_top(l, 8);
    gtk_grid_attach(GTK_GRID(f.grid), l, 0, f.row++, 2, 1);
    *f.words += std::string(" ") + text;
}

void note(Form &f, const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    put(f, l);
}

GtkWidget *check(Form &f, const char *text, bool &field)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(text);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), field);
    g_signal_connect(c, "toggled", CB([](GtkCheckButton *b, gpointer fp) {
        *static_cast<bool *>(fp) = gtk_check_button_get_active(b);
        changed();
    }), &field);
    *f.words += " " + plain(text);
    return put(f, c);
}

// Radio buttons, one per row; the first gets the pending label.
std::vector<GtkWidget *> radios(Form &f, std::vector<const char *> options, IntGet get, IntSet set)
{
    std::vector<GtkWidget *> out;
    auto *fn = new IntSet(std::move(set));
    GtkWidget *first = nullptr;
    int cur = get();
    for (size_t i = 0; i < options.size(); ++i) {
        GtkWidget *r = gtk_check_button_new_with_mnemonic(options[i]);
        if (first) gtk_check_button_set_group(GTK_CHECK_BUTTON(r), GTK_CHECK_BUTTON(first));
        else first = r;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r), (int)i == cur);
        g_object_set_data(G_OBJECT(r), "beluga-index", GINT_TO_POINTER((int)i));
        g_signal_connect(r, "toggled", CB([](GtkCheckButton *b, gpointer p) {
            if (!gtk_check_button_get_active(b)) return;
            (*static_cast<IntSet *>(p))(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "beluga-index")));
            changed();
        }), fn);
        *f.words += " " + plain(options[i]);
        out.push_back(put(f, r));
    }
    g_object_set_data_full(G_OBJECT(first), "beluga-fn", fn, [](gpointer p) { delete static_cast<IntSet *>(p); });
    return out;
}

GtkWidget *combo(std::vector<const char *> options, IntGet get, IntSet set, std::string *words)
{
    for (auto *o : options) *words += std::string(" ") + o;
    options.push_back(nullptr);
    GtkWidget *d = gtk_drop_down_new_from_strings(options.data());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d), get());
    auto *fn = new IntSet(std::move(set));
    g_signal_connect_data(d, "notify::selected", CB([](GtkDropDown *dd, GParamSpec *, gpointer p) {
        (*static_cast<IntSet *>(p))((int)gtk_drop_down_get_selected(dd));
        changed();
    }), fn, [](gpointer p, GClosure *) { delete static_cast<IntSet *>(p); }, GConnectFlags(0));
    return d;
}

GtkWidget *combo(Form &f, std::vector<const char *> options, IntGet get, IntSet set)
{
    return put(f, combo(std::move(options), std::move(get), std::move(set), f.words));
}

GtkWidget *spin(double min, double max, double step, IntGet get, IntSet set)
{
    GtkWidget *s = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), get());
    auto *fn = new IntSet(std::move(set));
    g_signal_connect_data(s, "value-changed", CB([](GtkSpinButton *sb, gpointer p) {
        (*static_cast<IntSet *>(p))(gtk_spin_button_get_value_as_int(sb));
        changed();
    }), fn, [](gpointer p, GClosure *) { delete static_cast<IntSet *>(p); }, GConnectFlags(0));
    return s;
}

// "prefix [spin] suffix" on one row.
GtkWidget *inline_row(std::initializer_list<GtkWidget *> parts)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (GtkWidget *w : parts) {
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(b), w);
    }
    return b;
}

GtkWidget *text_label(const char *text, std::string *words)
{
    *words += std::string(" ") + text;
    return gtk_label_new_with_mnemonic(text);
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

// Dolphin's "Label font:" row: the system font, or one chosen with the font button.
void font_row(Form &f, ModeSettings &m)
{
    GtkWidget *sys = gtk_check_button_new_with_mnemonic(_("Use the system _font"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(sys), m.useSystemFont);
    GtkFontDialog *fd = gtk_font_dialog_new();
    GtkWidget *fb = gtk_font_dialog_button_new(fd);
    if (!m.font.empty()) {
        PangoFontDescription *desc = pango_font_description_from_string(m.font.c_str());
        gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(fb), desc);
        pango_font_description_free(desc);
    }
    gtk_widget_set_sensitive(fb, !m.useSystemFont);
    g_object_set_data(G_OBJECT(sys), "beluga-fb", fb);
    g_signal_connect(sys, "toggled", CB([](GtkCheckButton *b, gpointer mp) {
        static_cast<ModeSettings *>(mp)->useSystemFont = gtk_check_button_get_active(b);
        gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(b), "beluga-fb")), !gtk_check_button_get_active(b));
        changed();
    }), &m);
    g_signal_connect(fb, "notify::font-desc", CB([](GtkFontDialogButton *b, GParamSpec *, gpointer mp) {
        PangoFontDescription *d = gtk_font_dialog_button_get_font_desc(b);
        if (d) static_cast<ModeSettings *>(mp)->font = take(pango_font_description_to_string(d));
        changed();
    }), &m);
    label(f, _("Label font:"));
    put(f, inline_row({sys, fb}));
}

void mode_sizes(Form &f, ModeSettings &m)
{
    label(f, _("Default icon size:"));
    combo(f, size_labels(), [&m] { return size_index(m.iconSize); }, [&m](int i) { m.iconSize = kSizes[i]; });
    label(f, _("Preview icon size:"));
    combo(f, size_labels(), [&m] { return size_index(m.previewSize); }, [&m](int i) { m.previewSize = kSizes[i]; });
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

namespace {

// The settings Restore Defaults puts back on each page (Dolphin restores the page that is shown).
void restore_defaults(const std::string &page)
{
    Settings &s = settings();
    const Settings d;
    if (page == "interface") {
        s.rememberOpenedTabs = d.rememberOpenedTabs;
        s.homeUrl = file_uri(Ref<GFile>(g_file_new_for_path(home_dir().c_str())).get());
        s.openExternallyCalledFolderInNewTab = d.openExternallyCalledFolderInNewTab;
        s.showFullPathInTitlebar = d.showFullPathInTitlebar;
        s.filterBar = d.filterBar;
        s.alwaysShowTabBar = d.alwaysShowTabBar;
        s.showCloseButtonOnTabs = d.showCloseButtonOnTabs;
        s.tabStyle = d.tabStyle;
        s.openNewTabAfterLastTab = d.openNewTabAfterLastTab;
        s.closeSplitViewChoice = d.closeSplitViewChoice;
        s.splitView = d.splitView;
        s.terminalCommand = d.terminalCommand;
        s.previewsDefault = d.previewsDefault;
        s.disabledPreviewMime = d.disabledPreviewMime;
        s.maxLocalPreviewMiB = d.maxLocalPreviewMiB;
        s.maxRemotePreviewMiB = d.maxRemotePreviewMiB;
        s.confirmTrash = d.confirmTrash;
        s.confirmDelete = d.confirmDelete;
        s.confirmEmptyTrash = d.confirmEmptyTrash;
        s.confirmClosingMultipleTabs = d.confirmClosingMultipleTabs;
        s.confirmOpenManyFolders = d.confirmOpenManyFolders;
        s.confirmOpenManyTerminals = d.confirmOpenManyTerminals;
        s.confirmRenameFileType = d.confirmRenameFileType;
        s.confirmActAsAdmin = d.confirmActAsAdmin;
        s.executableAction = d.executableAction;
        s.infoPreviews = d.infoPreviews;
        s.infoAutoPlay = d.infoAutoPlay;
        s.infoShowHovered = d.infoShowHovered;
        s.infoLongDates = d.infoLongDates;
        s.foldersHiddenShown = d.foldersHiddenShown;
        s.foldersLimitToHome = d.foldersLimitToHome;
        s.foldersAutoScroll = d.foldersAutoScroll;
        s.placesIconSize = d.placesIconSize;
        s.statusBar = d.statusBar;
        s.statusBarShown = d.statusBarShown;
        s.showZoomSlider = d.showZoomSlider;
        s.showSpaceInfo = d.showSpaceInfo;
        s.editableUrl = d.editableUrl;
        s.showFullPath = d.showFullPath;
    } else if (page == "view") {
        s.globalViewProps = d.globalViewProps;
        s.browseArchives = d.browseArchives;
        s.autoExpandFolders = d.autoExpandFolders;
        s.singleClick = d.singleClick;
        s.searchIndex = d.searchIndex;
        s.showToolTips = d.showToolTips;
        s.showSelectionToggle = d.showSelectionToggle;
        s.renameInline = d.renameInline;
        s.hideBackupFiles = d.hideBackupFiles;
        s.doubleClickViewAction = d.doubleClickViewAction;
        s.sortingChoice = d.sortingChoice;
        s.dirSizeMode = d.dirSizeMode;
        s.recursiveDirSizeLimit = d.recursiveDirSizeLimit;
        s.useShortRelativeDates = d.useShortRelativeDates;
        s.permissionsFormat = d.permissionsFormat;
        s.elidingMode = d.elidingMode;
        s.icons = d.icons;
        s.iconsTextWidth = d.iconsTextWidth;
        s.iconsMaxLines = d.iconsMaxLines;
        s.iconsCompactSpacing = d.iconsCompactSpacing;
        s.compact = d.compact;
        s.compactMaxTextWidth = d.compactMaxTextWidth;
        s.details = d.details;
        s.detailsExpandableFolders = d.detailsExpandableFolders;
        s.detailsHighlightEntireRow = d.detailsHighlightEntireRow;
        s.detailsDiskUsage = d.detailsDiskUsage;
    } else if (page == "contextmenu") {
        s.ctxCopyMoveMenu = d.ctxCopyMoveMenu;
        s.ctxAddToPlaces = d.ctxAddToPlaces;
        s.ctxSortBy = d.ctxSortBy;
        s.ctxViewMode = d.ctxViewMode;
        s.ctxOpenInNewTab = d.ctxOpenInNewTab;
        s.ctxOpenInNewWindow = d.ctxOpenInNewWindow;
        s.ctxOpenInSplitView = d.ctxOpenInSplitView;
        s.ctxCopyLocation = d.ctxCopyLocation;
        s.ctxDuplicateHere = d.ctxDuplicateHere;
        s.ctxOpenTerminal = d.ctxOpenTerminal;
        s.ctxCopyToOtherView = d.ctxCopyToOtherView;
        s.ctxMoveToOtherView = d.ctxMoveToOtherView;
        s.ctxDeletePermanently = d.ctxDeletePermanently;
    } else if (page == "trash") {
        s.trashUseTimeLimit = d.trashUseTimeLimit;
        s.trashDays = d.trashDays;
        s.trashUseSizeLimit = d.trashUseSizeLimit;
        s.trashPercent = d.trashPercent;
        s.trashLimitAction = d.trashLimitAction;
    } else if (page == "shortcuts") {
        s.shortcuts.clear();
    }
}

// The previews size limits as Dolphin words them: 0 means any size (local) or no file (remote).
GtkWidget *preview_limit(int &field, bool remote, std::string *words)
{
    GtkWidget *sp = spin(0, 100000, 1, [&field] { return field; }, [&field](int v) { field = v; });
    gtk_editable_set_width_chars(GTK_EDITABLE(sp), 16);
    g_object_set_data(G_OBJECT(sp), "beluga-remote", GINT_TO_POINTER(remote));
    g_signal_connect(sp, "output", CB([](GtkSpinButton *b) -> gboolean {
        int v = gtk_spin_button_get_value_as_int(b);
        bool remote = g_object_get_data(G_OBJECT(b), "beluga-remote") != nullptr;
        std::string t = v == 0 ? std::string(remote ? _("no file") : _("files of any size"))
                               : printf_str(_("files below %d MiB"), v);
        gtk_editable_set_text(GTK_EDITABLE(b), t.c_str());
        return TRUE;
    }), nullptr);
    g_signal_connect(sp, "input", CB([](GtkSpinButton *b, double *out) -> gint {
        const char *t = gtk_editable_get_text(GTK_EDITABLE(b));
        while (*t && !g_ascii_isdigit(*t)) ++t;
        *out = g_ascii_strtod(t, nullptr);
        return TRUE;
    }), nullptr);
    *words += std::string(" ") + _("files of any size") + " " + _("no file");
    return sp;
}

struct Prefs {
    GtkWidget *win = nullptr, *stack = nullptr, *side = nullptr, *title = nullptr;
    std::vector<std::pair<std::string, std::string>> words;   // page name → its words, for the search
    std::vector<GtkWidget *> rows;
};

GtkWidget *build(GtkWindow *parent, const std::string &startPage, int startTab);

void close_prefs(GtkWidget *win, bool keep)
{
    if (!keep && g_snapshot) {
        settings() = *g_snapshot;
        save_now();
    } else if (g_saveSource) {
        save_now();
    }
    delete g_snapshot;
    g_snapshot = nullptr;
    g_apply = nullptr;
    gtk_window_destroy(GTK_WINDOW(win));
}

GtkWidget *g_open = nullptr;

GtkWidget *build(GtkWindow *parent, const std::string &startPage, int startTab)
{
    Settings &s = settings();
    auto *P = new Prefs;
    GtkWidget *win = gtk_window_new();
    P->win = win;
    g_open = win;
    g_object_set_data_full(G_OBJECT(win), "beluga-prefs", P, [](gpointer p) { delete static_cast<Prefs *>(p); });
    // Destroyed without OK or Cancel (the application closing): keep the changes and forget the dialog.
    g_signal_connect(win, "destroy", CB([](GtkWidget *w) {
        if (g_open != w) return;   // replaced by Restore Defaults
        g_open = nullptr;
        g_apply = nullptr;
        if (g_saveSource) save_now();
        delete g_snapshot;
        g_snapshot = nullptr;
    }), nullptr);
    gtk_window_set_title(GTK_WINDOW(win), _("Configure Beluga"));
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);
        if (GtkApplication *app = gtk_window_get_application(parent)) gtk_window_set_application(GTK_WINDOW(win), app);
    }
    gtk_window_set_default_size(GTK_WINDOW(win), 980, 640);
    gtk_widget_add_css_class(win, "beluga-preferences");
    // Closing the window (Escape, the compositor) is Cancel, as in Dolphin.
    g_signal_connect(win, "close-request", CB([](GtkWindow *w) -> gboolean {
        close_prefs(GTK_WIDGET(w), false);
        return TRUE;
    }), nullptr);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), hbox);

    // Left: the search field and the pages as icons with their names.
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(left, "beluga-prefs-side");
    GtkWidget *search = gtk_search_entry_new();
    g_object_set(search, "placeholder-text", _("Search…"), nullptr);
    gtk_widget_set_margin_start(search, 4);
    gtk_widget_set_margin_end(search, 4);
    gtk_widget_set_margin_top(search, 4);
    gtk_box_append(GTK_BOX(left), search);
    P->side = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(P->side), GTK_SELECTION_BROWSE);
    gtk_widget_set_vexpand(P->side, TRUE);
    gtk_widget_set_size_request(P->side, 150, -1);
    gtk_box_append(GTK_BOX(left), P->side);
    gtk_box_append(GTK_BOX(hbox), left);
    gtk_box_append(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    // Right: the page's name, the page, and the buttons.
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(right, TRUE);
    P->title = gtk_label_new(nullptr);
    gtk_widget_add_css_class(P->title, "beluga-prefs-title");
    gtk_label_set_xalign(GTK_LABEL(P->title), 0);
    gtk_box_append(GTK_BOX(right), P->title);
    P->stack = gtk_stack_new();
    gtk_widget_set_vexpand(P->stack, TRUE);
    gtk_box_append(GTK_BOX(right), P->stack);
    gtk_box_append(GTK_BOX(right), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(buttons, "beluga-prefs-buttons");
    GtkWidget *restore = gtk_button_new_with_mnemonic(_("_Restore Defaults"));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *ok = gtk_button_new_with_mnemonic(_("_OK"));
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    g_apply = gtk_button_new_with_mnemonic(_("_Apply"));
    gtk_widget_set_sensitive(g_apply, FALSE);
    gtk_widget_add_css_class(ok, "suggested-action");
    for (GtkWidget *b : {restore, spacer, ok, cancel, g_apply}) gtk_box_append(GTK_BOX(buttons), b);
    gtk_box_append(GTK_BOX(right), buttons);
    gtk_box_append(GTK_BOX(hbox), right);

    auto add_page = [&](const char *name, const char *pageTitle, const char *const *icons, GtkWidget *content, std::string words) {
        gtk_stack_add_named(GTK_STACK(P->stack), content, name);
        GtkWidget *rowBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_top(rowBox, 6);
        gtk_widget_set_margin_bottom(rowBox, 6);
        GIcon *gi = g_themed_icon_new_from_names((char **)icons, -1);
        GtkWidget *img = gtk_image_new_from_gicon(gi);
        g_object_unref(gi);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
        gtk_box_append(GTK_BOX(rowBox), img);
        GtkWidget *l = gtk_label_new(pageTitle);
        gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_box_append(GTK_BOX(rowBox), l);
        gtk_list_box_append(GTK_LIST_BOX(P->side), rowBox);
        GtkWidget *row = gtk_widget_get_parent(rowBox);
        g_object_set_data_full(G_OBJECT(row), "beluga-page", g_strdup(name), g_free);
        g_object_set_data_full(G_OBJECT(row), "beluga-title", g_strdup(pageTitle), g_free);
        P->rows.push_back(row);
        P->words.emplace_back(name, take(g_utf8_casefold((std::string(pageTitle) + " " + words).c_str(), -1)));
    };

    // ---- Interface: Folders & Tabs, Previews, Confirmations, Panels, Status & Location bars
    {
        std::string w;
        GtkWidget *nb = gtk_notebook_new();
        gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);

        Form f = form_tab(nb, _("Folders & Tabs"), &w);
        label(f, _("Show on startup:"));
        auto start = radios(f, {_("Folders, tabs, and window state from last time"), _("This location:")},
                            [&s] { return s.rememberOpenedTabs ? 0 : 1; }, [&s](int i) { s.rememberOpenedTabs = i == 0; });
        (void)start;
        GtkWidget *home = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(home), display_location(s.homeUrl).c_str());
        gtk_widget_set_size_request(home, 320, -1);
        g_signal_connect(home, "changed", CB([](GtkEditable *e) {
            Ref<GFile> f(file_for_input(gtk_editable_get_text(e), nullptr));
            if (f) {
                settings().homeUrl = file_uri(f.get());
                changed();
            }
        }), nullptr);
        GtkWidget *useCurrent = gtk_button_new_with_label(_("Use Current Location"));
        GtkWidget *useDefault = gtk_button_new_with_label(_("Use Default Location"));
        g_signal_connect(useCurrent, "clicked", CB([](GtkButton *, gpointer e) {
            GtkWindow *pw = gtk_window_get_transient_for(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(e))));
            std::string uri = pw ? App::instance().currentUriOf(pw) : std::string();
            if (!uri.empty()) gtk_editable_set_text(GTK_EDITABLE(e), display_location(uri).c_str());
        }), home);
        g_signal_connect(useDefault, "clicked", CB([](GtkButton *, gpointer e) {
            gtk_editable_set_text(GTK_EDITABLE(e), home_dir().c_str());
        }), home);
        put(f, home);
        put(f, inline_row({useCurrent, useDefault}));
        gap(f);
        label(f, _("Opening Folders:"));
        check(f, _("Keep a single Beluga window, opening new folders in tabs"), s.openExternallyCalledFolderInNewTab);
        label(f, _("Window:"));
        check(f, _("Show full path in title bar"), s.showFullPathInTitlebar);
        check(f, _("Show filter bar"), s.filterBar);
        gap(f);
        label(f, _("Tabs:"));
        check(f, _("Always show tab bar"), s.alwaysShowTabBar);
        check(f, _("Show close button on tabs"), s.showCloseButtonOnTabs);
        label(f, _("Tab style:"));
        radios(f, {_("Tab width adapts to folder name"), _("Tabs all have the same fixed width")},
               [&s] { return std::min(s.tabStyle, 1); }, [&s](int i) { s.tabStyle = i; });
        note(f, _("Fixed-width tabs get narrower, all by the same amount, when they don't fit."));
        label(f, _("Open new tabs:"));
        radios(f, {_("After current tab"), _("At end of tab bar")},
               [&s] { return s.openNewTabAfterLastTab ? 1 : 0; }, [&s](int i) { s.openNewTabAfterLastTab = i == 1; });
        gap(f);
        label(f, _("Split view:"));
        put(f, inline_row({text_label(_("When closing a split view"), &w),
                           combo({_("Close the active pane"), _("Close the inactive pane"), _("Close the right pane")},
                                 [&s] { return s.closeSplitViewChoice; }, [&s](int i) { s.closeSplitViewChoice = i; }, &w)}));
        check(f, _("Open new windows in split view mode"), s.splitView);
        gap(f);
        label(f, _("Terminal:"));
        GtkWidget *term = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(term), s.terminalCommand.c_str());
        gtk_widget_set_size_request(term, 320, -1);
        std::string detected = terminal::detect();
        g_object_set(term, "placeholder-text",
                     detected.empty() ? _("No terminal found") : printf_str(_("Detected: %s"), detected.c_str()).c_str(), nullptr);
        g_signal_connect(term, "changed", CB([](GtkEditable *e) {
            settings().terminalCommand = gtk_editable_get_text(e);
            changed();
        }), nullptr);
        put(f, term);
        note(f, _("Used by F4 and Open Terminal Here. %d in the command is replaced by the folder; "
                  "otherwise the terminal starts in it. This used to be a KDE system setting."));

        Form pv = form_tab(nb, _("Previews"), &w);
        label(pv, _("New folders:"));
        check(pv, _("Show _previews in new folders"), s.previewsDefault);
        label(pv, _("Show previews in the view for:"));
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
            w += std::string(" ") + _(t.label);
            put(pv, c);
        }
        gap(pv);
        label(pv, _("Local storage:"));
        put(pv, inline_row({text_label(_("Show previews for"), &w), preview_limit(s.maxLocalPreviewMiB, false, &w)}));
        label(pv, _("Remote storage:"));
        put(pv, inline_row({text_label(_("Show previews for"), &w), preview_limit(s.maxRemotePreviewMiB, true, &w)}));

        Form c = form_tab(nb, _("Confirmations"), &w);
        title(c, _("Ask for confirmation when:"));
        check(c, _("Moving files or folders to trash"), s.confirmTrash);
        check(c, _("Emptying trash"), s.confirmEmptyTrash);
        check(c, _("Deleting files or folders"), s.confirmDelete);
        check(c, _("Renaming a file will change its type"), s.confirmRenameFileType);
        title(c, _("Ask for confirmation in Beluga when:"));
        check(c, _("Closing windows with multiple tabs"), s.confirmClosingMultipleTabs);
        check(c, _("Opening many folders at once"), s.confirmOpenManyFolders);
        check(c, _("Opening many terminals at once"), s.confirmOpenManyTerminals);
        check(c, _("Switching to act as an administrator"), s.confirmActAsAdmin);
        title(c, _("When opening an executable file:"));
        radios(c, {_("Always ask"), _("Open in application"), _("Run script")},
               [&s] { return (int)s.executableAction; }, [&s](int i) { s.executableAction = (ExecAction)i; });

        Form pn = form_tab(nb, _("Panels"), &w);
        label(pn, _("Information Panel:"));
        check(pn, _("Show previews"), s.infoPreviews);
        check(pn, _("Auto-play media files"), s.infoAutoPlay);
        check(pn, _("Show item on hover"), s.infoShowHovered);
        check(pn, _("Use _long date format"), s.infoLongDates);
        gap(pn);
        label(pn, _("Folders Panel:"));
        check(pn, _("Limit to home directory when inside it"), s.foldersLimitToHome);
        check(pn, _("Show _hidden folders"), s.foldersHiddenShown);
        check(pn, _("Scroll to the current folder _automatically"), s.foldersAutoScroll);
        gap(pn);
        label(pn, _("Places Panel:"));
        put(pn, inline_row({text_label(_("Icon size:"), &w),
                            combo({"16 px", "22 px", "32 px", "48 px"},
                                  [&s] { return s.placesIconSize <= 16 ? 0 : s.placesIconSize <= 22 ? 1 : s.placesIconSize <= 32 ? 2 : 3; },
                                  [&s](int i) { static const int v[] = {16, 22, 32, 48}; s.placesIconSize = v[i]; }, &w)}));

        Form sb = form_tab(nb, _("Status & Location bars"), &w);
        label(sb, _("Status Bar:"));
        radios(sb, {_("Small"), _("Full width"), _("Disabled")}, [&s] { return (int)s.statusBar; }, [&s](int i) {
            s.statusBar = (StatusBarMode)i;
            if (s.statusBar != StatusBarMode::Hidden) s.statusBarShown = s.statusBar;
        });
        check(sb, _("Show zoom slider"), s.showZoomSlider);
        check(sb, _("Show free _space"), s.showSpaceInfo);
        gap(sb);
        label(sb, _("Location bar:"));
        check(sb, _("Make location bar editable"), s.editableUrl);
        check(sb, _("Show full path inside location bar"), s.showFullPath);

        static const char *icons[] = {"system-file-manager", "org.beluga.Beluga", "preferences-desktop", "folder", nullptr};
        add_page("interface", _("Interface"), icons, nb, w);
    }

    // ---- View: General, Content Display, Icons, Compact, Details
    {
        std::string w;
        GtkWidget *nb = gtk_notebook_new();
        gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);

        Form g = form_tab(nb, _("General"), &w);
        label(g, _("Display style:"));
        radios(g, {_("Use common display style for all folders"), _("Remember display style for each folder")},
               [&s] { return s.globalViewProps ? 0 : 1; }, [&s](int i) { s.globalViewProps = i == 0; });
        note(g, _("Either way, View › Remember View for This Folder gives one folder its own style. Some special "
                  "views like search, recent files, or trash still use a style of their own."));
        gap(g);
        label(g, _("Browsing:"));
        check(g, _("Browse compressed files as folders"), s.browseArchives);
        check(g, _("Open folders during drag operations"), s.autoExpandFolders);
        check(g, _("Open files and folders with a single click"), s.singleClick);
        check(g, _("Keep an _index of the home folder for instant search"), s.searchIndex);
        gap(g);
        label(g, _("Miscellaneous:"));
        check(g, _("Show item information on hover"), s.showToolTips);
        check(g, _("Show selection marker"), s.showSelectionToggle);
        check(g, _("Rename single items inline"), s.renameInline);
        check(g, _("Also hide backup files while hiding hidden files"), s.hideBackupFiles);
        gap(g);
        label(g, _("Background:"));
        put(g, inline_row({text_label(_("Double-click triggers"), &w),
                           combo({_("Nothing"), _("Select All"), _("Go Up"), _("New Tab"), _("Show Hidden Files")},
                                 [&s] { return s.doubleClickViewAction; }, [&s](int i) { s.doubleClickViewAction = i; }, &w)}));

        Form cd = form_tab(nb, _("Content Display"), &w);
        label(cd, _("Sorting mode:"));
        radios(cd, {_("Natural"), _("Alphabetical, case insensitive"), _("Alphabetical, case sensitive")},
               [&s] { return s.sortingChoice; }, [&s](int i) { s.sortingChoice = i; });
        gap(cd);
        label(cd, _("Folder size:"));
        auto fs = radios(cd, {_("Show number of items"), _("Show size of contents, up to "), _("Show no size")},
                         [&s] { return (int)s.dirSizeMode; }, [&s](int i) { s.dirSizeMode = (DirSizeMode)i; });
        // "Show size of contents, up to [10] levels deep" on one row, as in Dolphin.
        GtkWidget *depth = spin(1, 100, 1, [&s] { return s.recursiveDirSizeLimit; }, [&s](int i) { s.recursiveDirSizeLimit = i; });
        GtkWidget *sizeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        g_object_ref(fs[1]);   // the grid's reference goes with gtk_grid_remove()
        gtk_grid_remove(GTK_GRID(cd.grid), fs[1]);
        gtk_box_append(GTK_BOX(sizeRow), fs[1]);
        g_object_unref(fs[1]);
        gtk_box_append(GTK_BOX(sizeRow), depth);
        gtk_box_append(GTK_BOX(sizeRow), text_label(_("levels deep"), &w));
        gtk_widget_set_halign(sizeRow, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(cd.grid), sizeRow, 1, cd.row - 2, 1, 1);
        gap(cd);
        label(cd, _("Date style:"));
        radios(cd, {_("Relative (Today, Yesterday…)"), _("Absolute")},
               [&s] { return s.useShortRelativeDates ? 0 : 1; }, [&s](int i) { s.useShortRelativeDates = i == 0; });
        gap(cd);
        label(cd, _("Permissions style:"));
        radios(cd, {_("Symbolic (rwxr-xr-x)"), _("Numeric (0755)"), _("Combined")},
               [&s] { return s.permissionsFormat; }, [&s](int i) { s.permissionsFormat = i; });
        gap(cd);
        label(cd, _("Long file names:"));
        radios(cd, {_("Elide in the middle"), _("Elide at the end")},
               [&s] { return s.elidingMode == 1 ? 0 : 1; }, [&s](int i) { s.elidingMode = i == 0 ? 1 : 0; });

        Form ic = form_tab(nb, _("Icons"), &w);
        mode_sizes(ic, s.icons);
        font_row(ic, s.icons);
        label(ic, _("Label width:"));
        combo(ic, {_("Small"), _("Medium"), _("Large"), _("Huge")}, [&s] { return s.iconsTextWidth; }, [&s](int i) { s.iconsTextWidth = i; });
        label(ic, _("Maximum lines:"));
        combo(ic, {_("Unlimited"), "1", "2", "3", "4", "5"}, [&s] { return std::min(s.iconsMaxLines, 5); },
              [&s](int i) { s.iconsMaxLines = i; });
        label(ic, _("Spacing:"));
        check(ic, _("Compact"), s.iconsCompactSpacing);

        Form cp = form_tab(nb, _("Compact"), &w);
        mode_sizes(cp, s.compact);
        font_row(cp, s.compact);
        label(cp, _("Maximum width:"));
        combo(cp, {_("Unlimited"), _("Small"), _("Medium"), _("Large")}, [&s] { return s.compactMaxTextWidth; },
              [&s](int i) { s.compactMaxTextWidth = i; });

        Form dt = form_tab(nb, _("Details"), &w);
        mode_sizes(dt, s.details);
        font_row(dt, s.details);
        label(dt, _("Folders:"));
        check(dt, _("Expandable"), s.detailsExpandableFolders);
        label(dt, _("Open files and folders:"));
        radios(dt, {_("By clicking anywhere on the row"), _("By clicking on icon or name")},
               [&s] { return s.detailsHighlightEntireRow ? 0 : 1; }, [&s](int i) { s.detailsHighlightEntireRow = i == 0; });
        label(dt, _("Disk usage:"));
        check(dt, _("Show a Disk _Usage column"), s.detailsDiskUsage);
        note(dt, _("The space each file and each whole folder takes up on disk, with a bar for its share of the folder, "
                   "like Tools › Disk Usage. Folders are read in full in the background; sort by the column to see the "
                   "biggest first. \"Stay on this drive\" in the Disk Usage window applies here too."));

        static const char *icons[] = {"preferences-desktop-icons", "view-list-icons", "view-grid-symbolic", nullptr};
        add_page("view", _("View"), icons, nb, w);
    }

    // ---- Context Menu: the entries to show, and your own actions
    {
        std::string w;
        Page m = column_page(&w);
        heading(m, _("Show in the context menu"));
        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        auto entry = [&](const char *text, bool &field) {
            GtkWidget *c = gtk_check_button_new_with_label(text);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(c), field);
            g_signal_connect(c, "toggled", CB([](GtkCheckButton *b, gpointer fp) {
                *static_cast<bool *>(fp) = gtk_check_button_get_active(b);
                changed();
            }), &field);
            gtk_list_box_append(GTK_LIST_BOX(list), c);
            w += std::string(" ") + text;
        };
        entry(_("'Copy To' and 'Move To'"), s.ctxCopyMoveMenu);
        entry(_("Add to Places"), s.ctxAddToPlaces);
        entry(_("Sort By"), s.ctxSortBy);
        entry(_("View Mode"), s.ctxViewMode);
        entry(_("Open in New Tab"), s.ctxOpenInNewTab);
        entry(_("Open in New Window"), s.ctxOpenInNewWindow);
        entry(_("Open in Split View"), s.ctxOpenInSplitView);
        entry(_("Copy Location"), s.ctxCopyLocation);
        entry(_("Duplicate Here"), s.ctxDuplicateHere);
        entry(_("Open Terminal Here"), s.ctxOpenTerminal);
        entry(_("Copy to Other View"), s.ctxCopyToOtherView);
        entry(_("Move to Other View"), s.ctxMoveToOtherView);
        entry(_("Delete (in addition to Move to Trash)"), s.ctxDeletePermanently);
        GtkWidget *frame = gtk_frame_new(nullptr);
        gtk_frame_set_child(GTK_FRAME(frame), list);
        gtk_box_append(GTK_BOX(m.box), frame);
        actions_section(m);
        static const char *icons[] = {"preferences-desktop-menu", "application-menu", "open-menu-symbolic", nullptr};
        add_page("contextmenu", _("Context Menu"), icons, m.scroll, w);
    }

    // ---- Trash
    {
        std::string w;
        Form t = form_tab(nullptr, _("Trash"), &w);
        label(t, _("Cleanup:"));
        GtkWidget *older = gtk_check_button_new_with_label(_("Delete files older than"));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(older), s.trashUseTimeLimit);
        g_signal_connect(older, "toggled", CB([](GtkCheckButton *b) {
            settings().trashUseTimeLimit = gtk_check_button_get_active(b);
            changed();
        }), nullptr);
        put(t, inline_row({older, spin(1, 3650, 1, [&s] { return s.trashDays; }, [&s](int i) { s.trashDays = i; }),
                           text_label(_("days"), &w)}));
        label(t, _("Size:"));
        GtkWidget *limit = gtk_check_button_new_with_label(_("Limit to"));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(limit), s.trashUseSizeLimit);
        g_signal_connect(limit, "toggled", CB([](GtkCheckButton *b) {
            settings().trashUseSizeLimit = gtk_check_button_get_active(b);
            changed();
        }), nullptr);
        put(t, inline_row({limit, spin(1, 100, 1, [&s] { return (int)s.trashPercent; }, [&s](int i) { s.trashPercent = i; }),
                           text_label(_("percent of the disk"), &w)}));
        label(t, _("Full trash:"));
        put(t, inline_row({text_label(_("When the limit is reached:"), &w),
                           combo({_("Show a warning"), _("Delete the oldest items"), _("Delete the largest items")},
                                 [&s] { return (int)s.trashLimitAction; }, [&s](int i) { s.trashLimitAction = (TrashLimitAction)i; }, &w)}));
        gap(t);
        note(t, _("These used to be KDE system settings. Limits are applied when Beluga starts and after moving items to the trash."));
        GtkWidget *sizeLabel = gtk_label_new(_("Calculating trash size…"));
        label(t, _("Trash:"));
        put(t, sizeLabel);
        Ref<GtkWidget> sl = Ref<GtkWidget>::share(sizeLabel);
        std::thread([sl] {
            guint64 size = trash::home_trash_size();
            run_on_main([sl, size] { gtk_label_set_text(GTK_LABEL(sl.get()), printf_str(_("The trash currently uses %s."), format_size(size).c_str()).c_str()); });
        }).detach();
        GtkWidget *empty = gtk_button_new_with_mnemonic(_("_Empty Trash Now"));
        g_signal_connect(empty, "clicked", CB([](GtkButton *b) {
            ask(GTK_WIDGET(b), _("Empty the trash?"), _("All items in the trash will be deleted permanently."), {_("Cancel"), _("Empty Trash")}, 1, 0,
                [](int r) { if (r == 1) FileJob::emptyTrash()->start(); });
        }), nullptr);
        put(t, empty);
        static const char *icons[] = {"user-trash", "user-trash-full", "user-trash-symbolic", nullptr};
        add_page("trash", _("Trash"), icons, GTK_WIDGET(g_object_get_data(G_OBJECT(t.grid), "beluga-scroll")), w);
    }

    // ---- Keyboard shortcuts
    {
        std::string w;
        Page k = column_page(&w);
        shortcuts_page(k);
        for (auto &sc : shortcut_table()) w += std::string(" ") + _(sc.title);
        static const char *icons[] = {"preferences-desktop-keyboard-shortcuts", "input-keyboard", "preferences-desktop-keyboard", nullptr};
        add_page("shortcuts", _("Keyboard Shortcuts"), icons, k.scroll, w);
    }

    // Selecting a page shows it with its name above.
    g_signal_connect(P->side, "row-selected", CB([](GtkListBox *, GtkListBoxRow *row, gpointer p) {
        if (!row) return;
        auto *P = static_cast<Prefs *>(p);
        gtk_stack_set_visible_child_name(GTK_STACK(P->stack), static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-page")));
        gtk_label_set_text(GTK_LABEL(P->title), static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-title")));
    }), P);
    // The search shows only the pages that have a setting with those words.
    g_signal_connect(search, "search-changed", CB([](GtkSearchEntry *e, gpointer p) {
        auto *P = static_cast<Prefs *>(p);
        std::string q = take(g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(e)), -1));
        GtkListBoxRow *firstShown = nullptr;
        for (size_t i = 0; i < P->rows.size(); ++i) {
            bool show = q.empty() || P->words[i].second.find(q) != std::string::npos;
            gtk_widget_set_visible(P->rows[i], show);
            if (show && !firstShown) firstShown = GTK_LIST_BOX_ROW(P->rows[i]);
        }
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(P->side));
        if (firstShown && (!sel || !gtk_widget_get_visible(GTK_WIDGET(sel)))) gtk_list_box_select_row(GTK_LIST_BOX(P->side), firstShown);
    }), P);

    g_signal_connect(ok, "clicked", CB([](GtkButton *, gpointer w) { close_prefs(GTK_WIDGET(w), true); }), win);
    g_signal_connect(cancel, "clicked", CB([](GtkButton *, gpointer w) { close_prefs(GTK_WIDGET(w), false); }), win);
    g_signal_connect(g_apply, "clicked", CB([](GtkButton *b) {
        save_now();
        if (g_snapshot) *g_snapshot = settings();
        gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    }), nullptr);
    // Restore Defaults: the page shown goes back to its defaults; the dialog is built again to show them.
    g_signal_connect(restore, "clicked", CB([](GtkButton *, gpointer p) {
        auto *P = static_cast<Prefs *>(p);
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(P->side));
        std::string page = row ? static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-page")) : "interface";
        GtkWidget *child = gtk_stack_get_visible_child(GTK_STACK(P->stack));
        int tab = child && GTK_IS_NOTEBOOK(child) ? gtk_notebook_get_current_page(GTK_NOTEBOOK(child)) : 0;
        restore_defaults(page);
        save_now();
        GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(P->win));
        GtkWidget *old = P->win;
        GtkWidget *nw = build(parent, page, tab);
        gtk_widget_set_sensitive(g_apply, TRUE);
        gtk_window_destroy(GTK_WINDOW(old));
        gtk_window_present(GTK_WINDOW(nw));
    }), P);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
        if (keyval != GDK_KEY_Escape) return FALSE;
        close_prefs(GTK_WIDGET(w), false);
        return TRUE;
    }), win);
    gtk_widget_add_controller(win, key);

    // The first page (or the one Restore Defaults rebuilt), and its tab.
    for (GtkWidget *row : P->rows)
        if (startPage.empty() || startPage == static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-page"))) {
            gtk_list_box_select_row(GTK_LIST_BOX(P->side), GTK_LIST_BOX_ROW(row));
            GtkWidget *child = gtk_stack_get_visible_child(GTK_STACK(P->stack));
            if (child && GTK_IS_NOTEBOOK(child)) gtk_notebook_set_current_page(GTK_NOTEBOOK(child), startTab);
            break;
        }
    return win;
}

} // namespace

void show_preferences(GtkWidget *parent)
{
    if (g_open) {
        gtk_window_present(GTK_WINDOW(g_open));
        return;
    }
    delete g_snapshot;
    g_snapshot = new Settings(settings());
    GtkWidget *w = build(window_of(parent), {}, 0);
    gtk_window_present(GTK_WINDOW(w));
}

} // namespace beluga
