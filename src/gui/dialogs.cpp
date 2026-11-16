// SPDX-License-Identifier: GPL-2.0-or-later
#include "dialogs.hpp"

#include "gtkutil.hpp"
#include "settings.hpp"
#include "undo.hpp"

#include <memory>

namespace beluga {

GtkWindow *window_of(GtkWidget *w)
{
    if (!w) return nullptr;
    GtkRoot *r = gtk_widget_get_root(w);
    return r && GTK_IS_WINDOW(r) ? GTK_WINDOW(r) : nullptr;
}

// ---- question / error ------------------------------------------------------------------

void ask(GtkWidget *parent, const std::string &message, const std::string &detail, const std::vector<std::string> &buttons,
         int defaultButton, int cancelButton, std::function<void(int)> cb)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message.c_str());
    if (!detail.empty()) gtk_alert_dialog_set_detail(d, detail.c_str());
    std::vector<const char *> labels;
    for (auto &b : buttons) labels.push_back(b.c_str());
    labels.push_back(nullptr);
    gtk_alert_dialog_set_buttons(d, labels.data());
    gtk_alert_dialog_set_default_button(d, defaultButton);
    gtk_alert_dialog_set_cancel_button(d, cancelButton);
    auto *data = new std::function<void(int)>(std::move(cb));
    gtk_alert_dialog_choose(d, window_of(parent), nullptr, [](GObject *src, GAsyncResult *res, gpointer p) {
        std::unique_ptr<std::function<void(int)>> f(static_cast<std::function<void(int)> *>(p));
        int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
        if (*f) (*f)(r);
    }, data);
    g_object_unref(d);
}

void show_error(GtkWidget *parent, const std::string &message, const std::string &detail)
{
    ask(parent, message, detail, {_("Close")}, 0, 0, nullptr);
}

// ---- dialog scaffolding --------------------------------------------------------------------

namespace {

struct DialogParts {
    GtkWidget *window;
    GtkWidget *content;
    GtkWidget *buttons;
};

DialogParts make_dialog(GtkWidget *parent, const std::string &title, int width = 420)
{
    DialogParts p;
    p.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(p.window), title.c_str());
    gtk_window_set_modal(GTK_WINDOW(p.window), TRUE);
    if (GtkWindow *pw = window_of(parent)) gtk_window_set_transient_for(GTK_WINDOW(p.window), pw);
    gtk_window_set_default_size(GTK_WINDOW(p.window), width, -1);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(p.window), TRUE);
    gtk_widget_add_css_class(p.window, "beluga-dialog");
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(outer, 16);
    gtk_widget_set_margin_end(outer, 16);
    gtk_widget_set_margin_top(outer, 16);
    gtk_widget_set_margin_bottom(outer, 16);
    p.content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(p.content, TRUE);
    p.buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(p.buttons, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(outer), p.content);
    gtk_box_append(GTK_BOX(outer), p.buttons);
    gtk_window_set_child(GTK_WINDOW(p.window), outer);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            gtk_window_close(GTK_WINDOW(w));
            return TRUE;
        }
        return FALSE;
    }), p.window);
    gtk_widget_add_controller(p.window, key);
    return p;
}

GtkWidget *add_button(DialogParts &p, const std::string &label, bool suggested = false)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label.c_str());
    if (suggested) gtk_widget_add_css_class(b, "suggested-action");
    gtk_box_append(GTK_BOX(p.buttons), b);
    return b;
}

// Runs fn once when clicked, then closes the window.
void on_click_close(GtkWidget *button, GtkWidget *window, std::function<void()> fn)
{
    auto *data = new std::function<void()>(std::move(fn));
    g_object_set_data(G_OBJECT(button), "beluga-window", window);
    g_signal_connect_data(button, "clicked", CB([](GtkButton *b, gpointer p) {
        auto *f = static_cast<std::function<void()> *>(p);
        GtkWidget *w = GTK_WIDGET(g_object_get_data(G_OBJECT(b), "beluga-window"));
        auto copy = *f;
        gtk_window_destroy(GTK_WINDOW(w));
        if (copy) copy();
    }), data, [](gpointer p, GClosure *) { delete static_cast<std::function<void()> *>(p); }, GConnectFlags(0));
}

} // namespace

void ask_name(GtkWidget *parent, const std::string &title, const std::string &label, const std::string &initial,
              int selectEnd, const std::string &okLabel, std::function<void(const std::string &)> cb)
{
    DialogParts p = make_dialog(parent, title);
    GtkWidget *l = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(p.content), l);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(p.content), entry);
    GtkWidget *warn = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(warn), 0);
    gtk_widget_add_css_class(warn, "error");
    gtk_widget_set_visible(warn, FALSE);
    gtk_box_append(GTK_BOX(p.content), warn);
    GtkWidget *cancel = add_button(p, _("_Cancel"));
    GtkWidget *ok = add_button(p, okLabel, true);
    gtk_window_set_default_widget(GTK_WINDOW(p.window), ok);
    on_click_close(cancel, p.window, nullptr);
    auto shared = std::make_shared<std::function<void(const std::string &)>>(std::move(cb));
    Ref<GtkWidget> keepEntry = Ref<GtkWidget>::share(entry);
    on_click_close(ok, p.window, [keepEntry, shared]() {
        std::string text = gtk_editable_get_text(GTK_EDITABLE(keepEntry.get()));
        if (*shared) (*shared)(text);
    });
    struct Check { GtkWidget *ok, *warn; };
    auto *chk = new Check{ok, warn};
    g_signal_connect_data(entry, "changed", CB([](GtkEditable *e, gpointer d) {
        auto *c = static_cast<Check *>(d);
        std::string t = gtk_editable_get_text(e);
        bool bad = trim(t).empty() || t.find('/') != std::string::npos;
        gtk_widget_set_sensitive(c->ok, !bad);
        gtk_label_set_text(GTK_LABEL(c->warn), t.find('/') != std::string::npos ? _("The name cannot contain “/”.") : "");
        gtk_widget_set_visible(c->warn, t.find('/') != std::string::npos);
        if (!t.empty() && t[0] == '.') {
            gtk_label_set_text(GTK_LABEL(c->warn), _("A name starting with a dot is hidden."));
            gtk_widget_set_visible(c->warn, TRUE);
        }
    }), chk, [](gpointer d, GClosure *) { delete static_cast<Check *>(d); }, GConnectFlags(0));
    gtk_widget_set_sensitive(ok, !trim(initial).empty());
    gtk_window_present(GTK_WINDOW(p.window));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, selectEnd);
}

// ---- rename --------------------------------------------------------------------------------

static void rename_many(GtkWidget *parent, std::vector<Ref<GFile>> files, std::vector<bool> dirs, const std::string &pattern, int start)
{
    int hashes = 0;
    size_t hpos = pattern.find('#');
    if (hpos != std::string::npos)
        while (hpos + hashes < pattern.size() && pattern[hpos + hashes] == '#') hashes++;
    UndoCommand cmd;
    cmd.type = UndoCommand::Rename;
    std::vector<std::string> errors;
    int digits = std::max(hashes, (int)std::to_string(start + (int)files.size() - 1).size());
    for (size_t i = 0; i < files.size(); ++i) {
        std::string num = printf_str("%0*d", hashes > 0 ? hashes : 1, start + (int)i);
        if (hashes == 0) num = printf_str("%0*d", digits, start + (int)i);
        std::string base = hpos == std::string::npos ? pattern + " " + num : pattern.substr(0, hpos) + num + pattern.substr(hpos + hashes);
        std::string oldName = take(g_file_get_basename(files[i].get()));
        std::string name = base + split_extension(oldName, dirs[i]).second;
        Ref<GFile> renamed;
        GError *err = nullptr;
        if (rename_file(files[i].get(), name, renamed, &err)) {
            cmd.pairs.emplace_back(file_uri(files[i].get()), file_uri(renamed.get()));
        } else {
            errors.push_back(oldName + ": " + err->message);
            g_error_free(err);
        }
    }
    UndoManager::instance().push(cmd);
    if (!errors.empty()) show_error(parent, _("Some items could not be renamed."), join(errors, "\n"));
}

void show_rename_dialog(GtkWidget *parent, const std::vector<BelugaItem *> &items)
{
    if (items.empty()) return;
    if (items.size() == 1) {
        Ref<BelugaItem> keep = Ref<BelugaItem>::share(items[0]);
        const ItemData &d = item_data(items[0]);
        int stem = (int)g_utf8_strlen(split_extension(d.name, d.isDir).first.c_str(), -1);
        Ref<GtkWidget> par = Ref<GtkWidget>::share(parent);
        ask_name(parent, _("Rename Item"), printf_str(_("Rename “%s” to:"), d.name.c_str()), d.name, stem, _("_Rename"),
                 [keep, par](const std::string &name) {
            ItemData &dd = item_data(keep.get());
            if (name == dd.name || trim(name).empty()) return;
            auto perform = [keep, par, name] {
                ItemData &d2 = item_data(keep.get());
                Ref<GFile> renamed;
                GError *err = nullptr;
                std::string oldUri = d2.uri;
                if (!rename_file(d2.file.get(), name, renamed, &err)) {
                    show_error(par.get(), printf_str(_("Could not rename “%s”."), d2.name.c_str()), err->message);
                    g_error_free(err);
                    return;
                }
                UndoCommand c;
                c.type = UndoCommand::Rename;
                c.pairs.emplace_back(oldUri, file_uri(renamed.get()));
                UndoManager::instance().push(c);
            };
            std::string oldExt = split_extension(dd.name, dd.isDir).second, newExt = split_extension(name, dd.isDir).second;
            if (settings().confirmRenameFileType && !dd.isDir && casefold(oldExt) != casefold(newExt))
                ask(par.get(), _("Change the file type?"),
                    printf_str(_("Renaming “%s” to “%s” changes its extension."), dd.name.c_str(), name.c_str()),
                    {_("Cancel"), _("Rename")}, 1, 0, [perform](int r) { if (r == 1) perform(); });
            else
                perform();
        });
        return;
    }
    DialogParts p = make_dialog(parent, _("Rename Items"));
    GtkWidget *l = gtk_label_new(printf_str(_("Rename the %zu selected items to:"), items.size()).c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(p.content), l);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), _("New name #"));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(p.content), entry);
    GtkWidget *hint = gtk_label_new(_("# is replaced by ascending numbers; use ## for leading zeros. Extensions are kept."));
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_append(GTK_BOX(p.content), hint);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), gtk_label_new(_("Start numbering at:")));
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 1000000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), 1);
    gtk_box_append(GTK_BOX(row), spin);
    gtk_box_append(GTK_BOX(p.content), row);
    GtkWidget *cancel = add_button(p, _("_Cancel"));
    GtkWidget *ok = add_button(p, _("_Rename"), true);
    gtk_window_set_default_widget(GTK_WINDOW(p.window), ok);
    on_click_close(cancel, p.window, nullptr);
    std::vector<Ref<GFile>> files;
    std::vector<bool> dirs;
    for (BelugaItem *it : items) {
        files.push_back(item_data(it).file);
        dirs.push_back(item_data(it).isDir);
    }
    Ref<GtkWidget> par = Ref<GtkWidget>::share(parent);
    Ref<GtkWidget> e = Ref<GtkWidget>::share(entry), s = Ref<GtkWidget>::share(spin);
    on_click_close(ok, p.window, [par, e, s, files, dirs] {
        rename_many(par.get(), files, dirs, gtk_editable_get_text(GTK_EDITABLE(e.get())),
                    gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s.get())));
    });
    gtk_window_present(GTK_WINDOW(p.window));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

// ---- popups -----------------------------------------------------------------------------------

void popup_menu(GtkWidget *widget, GMenuModel *model, double x, double y)
{
    GtkWidget *pop = gtk_popover_menu_new_from_model_full(model, GTK_POPOVER_MENU_NESTED);
    gtk_widget_set_parent(pop, widget);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    g_signal_connect(pop, "closed", CB([](GtkPopover *p) {
        g_idle_add([](gpointer w) -> gboolean {
            if (gtk_widget_get_parent(GTK_WIDGET(w))) gtk_widget_unparent(GTK_WIDGET(w));
            g_object_unref(w);
            return G_SOURCE_REMOVE;
        }, g_object_ref(p));
    }), nullptr);
    gtk_popover_popup(GTK_POPOVER(pop));
}

void show_drop_menu(GtkWidget *widget, double x, double y, bool canMove, std::function<void(GdkDragAction)> cb)
{
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_add_css_class(pop, "menu");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    struct State {
        std::function<void(GdkDragAction)> cb;
        bool done = false;
    };
    auto *st = new State{std::move(cb)};
    g_object_set_data_full(G_OBJECT(pop), "beluga-state", st, [](gpointer p) {
        auto *s = static_cast<State *>(p);
        if (!s->done && s->cb) s->cb(GdkDragAction(0));
        delete s;
    });
    struct Entry { const char *label; const char *icon; GdkDragAction action; };
    Entry entries[] = {
        {_("_Move Here"), "go-jump", GDK_ACTION_MOVE},
        {_("_Copy Here"), "edit-copy", GDK_ACTION_COPY},
        {_("_Link Here"), "insert-link", GDK_ACTION_LINK},
        {_("C_ancel"), "process-stop", GdkDragAction(0)},
    };
    for (auto &e : entries) {
        if (e.action == GDK_ACTION_MOVE && !canMove) continue;
        if (e.action == 0) gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        GtkWidget *b = gtk_button_new();
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(e.icon));
        GtkWidget *l = gtk_label_new_with_mnemonic(e.label);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(row), l);
        gtk_button_set_child(GTK_BUTTON(b), row);
        gtk_widget_add_css_class(b, "flat");
        g_object_set_data(G_OBJECT(b), "beluga-action", GINT_TO_POINTER((int)e.action));
        g_object_set_data(G_OBJECT(b), "beluga-pop", pop);
        g_signal_connect(b, "clicked", CB([](GtkButton *btn) {
            GtkWidget *pp = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "beluga-pop"));
            auto *s = static_cast<State *>(g_object_get_data(G_OBJECT(pp), "beluga-state"));
            if (s && !s->done) {
                s->done = true;
                auto action = GdkDragAction(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "beluga-action")));
                auto cb = s->cb;
                gtk_popover_popdown(GTK_POPOVER(pp));
                if (cb) cb(action);
            }
        }), nullptr);
        gtk_box_append(GTK_BOX(box), b);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_set_parent(pop, widget);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", CB([](GtkPopover *p) {
        g_idle_add([](gpointer w) -> gboolean {
            if (gtk_widget_get_parent(GTK_WIDGET(w))) gtk_widget_unparent(GTK_WIDGET(w));
            g_object_unref(w);
            return G_SOURCE_REMOVE;
        }, g_object_ref(p));
    }), nullptr);
    gtk_popover_popup(GTK_POPOVER(pop));
}

// ---- places -------------------------------------------------------------------------------------

void edit_place_dialog(GtkWidget *parent, const Place &initial, bool isNew, std::function<void(const Place &)> cb)
{
    DialogParts p = make_dialog(parent, isNew ? _("Add Places Entry") : _("Edit Places Entry"), 460);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    auto addRow = [&](int row, const char *label, GtkWidget *w) {
        GtkWidget *l = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        gtk_widget_set_hexpand(w, TRUE);
        gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    };
    GtkWidget *label = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(label), initial.label.c_str());
    addRow(0, _("Label:"), label);
    GtkWidget *locRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *loc = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(loc), display_location(initial.uri).c_str());
    gtk_widget_set_hexpand(loc, TRUE);
    GtkWidget *browse = gtk_button_new_from_icon_name("document-open");
    gtk_widget_set_tooltip_text(browse, _("Choose a folder"));
    gtk_box_append(GTK_BOX(locRow), loc);
    gtk_box_append(GTK_BOX(locRow), browse);
    addRow(1, _("Location:"), locRow);
    GtkWidget *iconRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *icon = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(icon), initial.icon.c_str());
    gtk_widget_set_hexpand(icon, TRUE);
    GtkWidget *preview = gtk_image_new_from_icon_name(initial.icon.c_str());
    gtk_image_set_pixel_size(GTK_IMAGE(preview), 22);
    gtk_box_append(GTK_BOX(iconRow), preview);
    gtk_box_append(GTK_BOX(iconRow), icon);
    addRow(2, _("Icon name:"), iconRow);
    gtk_box_append(GTK_BOX(p.content), grid);
    g_signal_connect(icon, "changed", CB([](GtkEditable *e, gpointer img) {
        gtk_image_set_from_icon_name(GTK_IMAGE(img), gtk_editable_get_text(e));
    }), preview);
    g_signal_connect(browse, "clicked", CB([](GtkButton *b, gpointer entry) {
        GtkFileDialog *fd = gtk_file_dialog_new();
        gtk_file_dialog_set_title(fd, _("Choose a Folder"));
        gtk_file_dialog_select_folder(fd, window_of(GTK_WIDGET(b)), nullptr, [](GObject *src, GAsyncResult *res, gpointer e) {
            GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
            if (f) {
                gtk_editable_set_text(GTK_EDITABLE(e), display_location(f).c_str());
                g_object_unref(f);
            }
            g_object_unref(e);
        }, g_object_ref(entry));
        g_object_unref(fd);
    }), loc);
    GtkWidget *cancel = add_button(p, _("_Cancel"));
    GtkWidget *ok = add_button(p, isNew ? _("_Add") : _("_Save"), true);
    gtk_window_set_default_widget(GTK_WINDOW(p.window), ok);
    on_click_close(cancel, p.window, nullptr);
    Ref<GtkWidget> rl = Ref<GtkWidget>::share(label), rloc = Ref<GtkWidget>::share(loc), ricon = Ref<GtkWidget>::share(icon);
    Place base = initial;
    on_click_close(ok, p.window, [rl, rloc, ricon, base, cb] {
        Place pl = base;
        pl.label = trim(gtk_editable_get_text(GTK_EDITABLE(rl.get())));
        std::string loc = gtk_editable_get_text(GTK_EDITABLE(rloc.get()));
        if (is_virtual_uri(loc) || starts_with(loc, "network:")) pl.uri = loc;
        else {
            Ref<GFile> f(file_for_input(loc, nullptr));
            if (f) pl.uri = file_uri(f.get());
        }
        pl.icon = trim(gtk_editable_get_text(GTK_EDITABLE(ricon.get())));
        if (pl.icon.empty()) pl.icon = Places::iconForUri(pl.uri);
        if (pl.label.empty()) pl.label = basename_of(display_location(pl.uri));
        if (!pl.uri.empty() && cb) cb(pl);
    });
    gtk_window_present(GTK_WINDOW(p.window));
}

// ---- view display style -----------------------------------------------------------------------

void view_properties_dialog(GtkWidget *parent, const std::string &uri, const ViewProps &current,
                            std::function<void(const ViewProps &, int)> cb)
{
    DialogParts p = make_dialog(parent, _("View Display Style"), 440);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int row = 0;
    auto label = [&](const char *t) {
        GtkWidget *l = gtk_label_new(t);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    };
    const char *modes[] = {_("Icons"), _("Compact"), _("Details"), _("Columns"), nullptr};
    GtkWidget *mode = gtk_drop_down_new_from_strings(modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mode), current.viewMode);
    label(_("View mode:"));
    gtk_grid_attach(GTK_GRID(grid), mode, 1, row++, 1, 1);
    std::vector<const char *> roleTitles;
    std::vector<std::string> roleIds;
    for (auto &r : all_roles()) {
        roleTitles.push_back(_(r.title));
        roleIds.push_back(r.id);
    }
    roleTitles.push_back(nullptr);
    GtkWidget *sort = gtk_drop_down_new_from_strings(roleTitles.data());
    for (size_t i = 0; i < roleIds.size(); ++i)
        if (roleIds[i] == current.sortRole) gtk_drop_down_set_selected(GTK_DROP_DOWN(sort), i);
    label(_("Sorting:"));
    gtk_grid_attach(GTK_GRID(grid), sort, 1, row++, 1, 1);
    const char *orders[] = {_("Ascending"), _("Descending"), nullptr};
    GtkWidget *order = gtk_drop_down_new_from_strings(orders);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(order), current.sortDescending ? 1 : 0);
    gtk_grid_attach(GTK_GRID(grid), order, 1, row++, 1, 1);
    auto check = [&](const char *t, bool on) {
        GtkWidget *c = gtk_check_button_new_with_mnemonic(t);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), on);
        gtk_grid_attach(GTK_GRID(grid), c, 1, row++, 1, 1);
        return c;
    };
    GtkWidget *foldersFirst = check(_("Show _folders first"), current.sortFoldersFirst);
    GtkWidget *hiddenLast = check(_("Show hidden files _last"), current.sortHiddenLast);
    GtkWidget *previews = check(_("Show _previews"), current.previewsShown);
    GtkWidget *groups = check(_("Show in _groups"), current.groupedSorting);
    GtkWidget *hidden = check(_("Show _hidden files"), current.hiddenFilesShown);
    gtk_box_append(GTK_BOX(p.content), grid);

    GtkWidget *frame = gtk_frame_new(_("Apply to"));
    GtkWidget *radios = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(radios, 8);
    gtk_widget_set_margin_end(radios, 8);
    gtk_widget_set_margin_top(radios, 6);
    gtk_widget_set_margin_bottom(radios, 6);
    GtkWidget *r0 = gtk_check_button_new_with_label(_("Current folder"));
    GtkWidget *r1 = gtk_check_button_new_with_label(_("Current folder including all subfolders"));
    GtkWidget *r2 = gtk_check_button_new_with_label(_("All folders"));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(r1), GTK_CHECK_BUTTON(r0));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(r2), GTK_CHECK_BUTTON(r0));
    bool global = settings().globalViewProps;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(global ? r2 : r0), TRUE);
    if (global) {
        gtk_widget_set_sensitive(r0, FALSE);
        gtk_widget_set_sensitive(r1, FALSE);
    }
    gtk_box_append(GTK_BOX(radios), r0);
    gtk_box_append(GTK_BOX(radios), r1);
    gtk_box_append(GTK_BOX(radios), r2);
    gtk_frame_set_child(GTK_FRAME(frame), radios);
    if (!is_virtual_uri(uri)) gtk_box_append(GTK_BOX(p.content), frame);

    GtkWidget *cancel = add_button(p, _("_Cancel"));
    GtkWidget *ok = add_button(p, _("_OK"), true);
    on_click_close(cancel, p.window, nullptr);
    struct W { Ref<GtkWidget> mode, sort, order, ff, hl, pv, gr, hd, r1, r2; };
    W w{Ref<GtkWidget>::share(mode), Ref<GtkWidget>::share(sort), Ref<GtkWidget>::share(order), Ref<GtkWidget>::share(foldersFirst),
        Ref<GtkWidget>::share(hiddenLast), Ref<GtkWidget>::share(previews), Ref<GtkWidget>::share(groups),
        Ref<GtkWidget>::share(hidden), Ref<GtkWidget>::share(r1), Ref<GtkWidget>::share(r2)};
    ViewProps base = current;
    on_click_close(ok, p.window, [w, base, roleIds, cb] {
        ViewProps np = base;
        np.viewMode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(w.mode.get()));
        guint s = gtk_drop_down_get_selected(GTK_DROP_DOWN(w.sort.get()));
        if (s < roleIds.size()) np.sortRole = roleIds[s];
        np.sortDescending = gtk_drop_down_get_selected(GTK_DROP_DOWN(w.order.get())) == 1;
        np.sortFoldersFirst = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.ff.get()));
        np.sortHiddenLast = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.hl.get()));
        np.previewsShown = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.pv.get()));
        np.groupedSorting = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.gr.get()));
        np.hiddenFilesShown = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.hd.get()));
        if (np.viewMode != base.viewMode) np.zoomLevel = -1;
        int scope = gtk_check_button_get_active(GTK_CHECK_BUTTON(w.r2.get())) ? 2
                  : gtk_check_button_get_active(GTK_CHECK_BUTTON(w.r1.get())) ? 1 : 0;
        if (cb) cb(np, scope);
    });
    gtk_window_present(GTK_WINDOW(p.window));
}

// ---- file operation questions -----------------------------------------------------------------

static GtkWidget *file_summary(const char *title, GFileInfo *info)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = info && g_file_info_get_icon(info) ? gtk_image_new_from_gicon(g_file_info_get_icon(info)) : gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    gtk_box_append(GTK_BOX(box), img);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "heading");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(texts), t);
    std::string detail;
    if (info) {
        bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        if (!dir) detail = format_size(g_file_info_get_size(info));
        GDateTime *dt = g_file_info_get_modification_date_time(info);
        if (dt) {
            if (!detail.empty()) detail += ", ";
            detail += printf_str(_("modified %s"), format_date(g_date_time_to_unix(dt), DateStyle::Short).c_str());
            g_date_time_unref(dt);
        }
    }
    GtkWidget *d = gtk_label_new(detail.c_str());
    gtk_widget_add_css_class(d, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(d), 0);
    gtk_box_append(GTK_BOX(texts), d);
    gtk_box_append(GTK_BOX(box), texts);
    return box;
}

void GtkJobUi::conflict(FileJob &job, const ConflictRequest &req, std::function<void(ConflictReply)> reply)
{
    GtkApplication *app = GTK_APPLICATION(g_application_get_default());
    GtkWindow *parent = app ? gtk_application_get_active_window(app) : nullptr;
    std::string name = take(g_file_get_basename(req.dest.get()));
    Ref<GFile> destDir(g_file_get_parent(req.dest.get()));
    DialogParts p = make_dialog(parent ? GTK_WIDGET(parent) : nullptr,
                                req.folderConflict ? _("Folder Already Exists") : _("File Already Exists"), 520);
    GtkWidget *msg = gtk_label_new(nullptr);
    std::string text = req.folderConflict
        ? printf_str(_("A folder named “%s” already exists in “%s”."), name.c_str(), display_location(destDir.get()).c_str())
        : printf_str(_("A file named “%s” already exists in “%s”."), name.c_str(), display_location(destDir.get()).c_str());
    gtk_label_set_text(GTK_LABEL(msg), text.c_str());
    gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
    gtk_label_set_xalign(GTK_LABEL(msg), 0);
    gtk_box_append(GTK_BOX(p.content), msg);
    gtk_box_append(GTK_BOX(p.content), file_summary(_("Existing item"), req.destInfo.get()));
    gtk_box_append(GTK_BOX(p.content), file_summary(_("New item"), req.sourceInfo.get()));
    GtkWidget *renameRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), req.suggestedName.c_str());
    gtk_widget_set_hexpand(entry, TRUE);
    GtkWidget *suggest = gtk_button_new_with_mnemonic(_("_Suggest New Name"));
    gtk_box_append(GTK_BOX(renameRow), gtk_label_new(_("New name:")));
    gtk_box_append(GTK_BOX(renameRow), entry);
    gtk_box_append(GTK_BOX(renameRow), suggest);
    gtk_box_append(GTK_BOX(p.content), renameRow);
    g_object_set_data_full(G_OBJECT(suggest), "beluga-name", g_strdup(req.suggestedName.c_str()), g_free);
    g_signal_connect(suggest, "clicked", CB([](GtkButton *b, gpointer e) {
        gtk_editable_set_text(GTK_EDITABLE(e), static_cast<const char *>(g_object_get_data(G_OBJECT(b), "beluga-name")));
    }), entry);
    GtkWidget *all = nullptr;
    if (req.multiple) {
        all = gtk_check_button_new_with_mnemonic(_("Apply to _all"));
        gtk_box_append(GTK_BOX(p.content), all);
    }

    struct State {
        std::function<void(ConflictReply)> reply;
        bool done = false;
    };
    auto st = std::make_shared<State>();
    st->reply = std::move(reply);
    Ref<GtkWidget> e = Ref<GtkWidget>::share(entry);
    Ref<GtkWidget> a = all ? Ref<GtkWidget>::share(all) : Ref<GtkWidget>();
    auto answer = [st, e, a](ConflictChoice c) {
        if (st->done) return;
        st->done = true;
        ConflictReply r;
        r.choice = c;
        r.applyToAll = a && gtk_check_button_get_active(GTK_CHECK_BUTTON(a.get()));
        r.newName = gtk_editable_get_text(GTK_EDITABLE(e.get()));
        st->reply(r);
    };
    on_click_close(add_button(p, _("_Cancel")), p.window, [answer] { answer(ConflictChoice::Cancel); });
    on_click_close(add_button(p, _("S_kip")), p.window, [answer] { answer(ConflictChoice::Skip); });
    if (req.folderConflict) on_click_close(add_button(p, _("_Write Into")), p.window, [answer] { answer(ConflictChoice::Merge); });
    else if (!req.sameFile) on_click_close(add_button(p, _("_Overwrite")), p.window, [answer] { answer(ConflictChoice::Overwrite); });
    GtkWidget *renameBtn = add_button(p, _("_Rename"), true);
    on_click_close(renameBtn, p.window, [answer] { answer(ConflictChoice::Rename); });
    gtk_window_set_default_widget(GTK_WINDOW(p.window), renameBtn);
    auto *closeData = new std::function<void()>([answer] { answer(ConflictChoice::Cancel); });
    g_signal_connect_data(p.window, "close-request", CB([](GtkWindow *, gpointer d) -> gboolean {
        (*static_cast<std::function<void()> *>(d))();
        return FALSE;
    }), closeData, [](gpointer d, GClosure *) { delete static_cast<std::function<void()> *>(d); }, GConnectFlags(0));
    gtk_window_present(GTK_WINDOW(p.window));
}

void GtkJobUi::error(FileJob &job, const std::string &message, bool canSkip, std::function<void(ErrorChoice)> reply)
{
    GtkApplication *app = GTK_APPLICATION(g_application_get_default());
    GtkWindow *parent = app ? gtk_application_get_active_window(app) : nullptr;
    std::vector<std::string> buttons = {_("Cancel")};
    std::vector<ErrorChoice> map = {ErrorChoice::Cancel};
    if (canSkip) {
        buttons.push_back(_("Skip"));
        map.push_back(ErrorChoice::Skip);
        buttons.push_back(_("Skip All"));
        map.push_back(ErrorChoice::SkipAll);
    }
    buttons.push_back(_("Retry"));
    map.push_back(ErrorChoice::Retry);
    ask(parent ? GTK_WIDGET(parent) : nullptr, job.title(), message, buttons, canSkip ? 1 : 0, 0, [reply, map](int r) {
        reply(r >= 0 && r < (int)map.size() ? map[r] : ErrorChoice::Cancel);
    });
}

} // namespace beluga
