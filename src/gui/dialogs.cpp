// SPDX-License-Identifier: GPL-2.0-or-later
#include "dialogs.hpp"

#include "netlocation.hpp"

#include "gtkutil.hpp"
#include "settings.hpp"
#include "undo.hpp"

#include <algorithm>
#include <functional>
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

namespace {
struct IconItem {
    std::string id, label, action;
    GVariant *target;   // owned
    GIcon *icon;        // owned
};
std::vector<IconItem> &pending_icon_items()
{
    static std::vector<IconItem> v;
    return v;
}
} // namespace

void menu_icon_item(GMenu *menu, const std::string &label, const char *action, GVariant *target, GIcon *icon)
{
    auto &v = pending_icon_items();
    std::string id = printf_str("beluga-icon-%zu", v.size());
    GMenuItem *mi = g_menu_item_new(label.c_str(), nullptr);
    g_menu_item_set_attribute(mi, "custom", "s", id.c_str());
    g_menu_append_item(menu, mi);
    g_object_unref(mi);
    v.push_back({id, label, action, target ? g_variant_ref_sink(target) : nullptr, icon ? G_ICON(g_object_ref(icon)) : nullptr});
}

void fit_menu(GtkPopover *pop)
{
    int cap = 900;
    GtkNative *parent = gtk_widget_get_native(gtk_widget_get_parent(GTK_WIDGET(pop)));
    if (GdkSurface *ps = parent ? gtk_native_get_surface(parent) : nullptr)
        if (GdkMonitor *m = gdk_display_get_monitor_at_surface(gdk_surface_get_display(ps), ps)) {
            GdkRectangle r;
            gdk_monitor_get_geometry(m, &r);
            cap = std::max(200, r.height - 80);
        }
    g_object_set_data(G_OBJECT(pop), "beluga-cap", GINT_TO_POINTER(cap));
    // GTK measures the rows shorter than they are once laid out (by ~30 px), so the page scrolled. When a page's
    // scroll range is larger than what it shows, give it that height (up to most of the screen).
    std::function<void(GtkWidget *)> walk = [&](GtkWidget *w) {
        if (GTK_IS_SCROLLED_WINDOW(w) && !g_object_get_data(G_OBJECT(w), "beluga-fit")) {
            g_object_set_data(G_OBJECT(w), "beluga-fit", GINT_TO_POINTER(1));
            GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w));
            g_signal_connect(adj, "changed", CB([](GtkAdjustment *a, gpointer sw) {
                double upper = gtk_adjustment_get_upper(a), page = gtk_adjustment_get_page_size(a);
                GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(sw), GTK_TYPE_POPOVER);
                if (!popover || page <= 0 || upper <= page + 0.5) return;
                int cap = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popover), "beluga-cap"));
                int want = (int)std::min<double>(upper, cap > 0 ? cap : upper);
                if (want <= gtk_scrolled_window_get_min_content_height(GTK_SCROLLED_WINDOW(sw))) return;
                gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), want);
                gtk_popover_present(GTK_POPOVER(popover));
            }), w);
        }
        for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) walk(c);
    };
    walk(GTK_WIDGET(pop));
}

void popup_menu(GtkWidget *widget, GMenuModel *model, double x, double y)
{
    GtkWidget *pop = gtk_popover_menu_new_from_model_full(model, GTK_POPOVER_MENU_NESTED);
    // Entries with icons (menu_icon_item): a flat button with the icon and the label, running the action.
    for (IconItem &it : pending_icon_items()) {
        GtkWidget *b = gtk_button_new();
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_add_css_class(b, "beluga-menu-row");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *img = it.icon ? gtk_image_new_from_gicon(it.icon) : gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
        gtk_box_append(GTK_BOX(box), img);
        GtkWidget *l = gtk_label_new(it.label.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_box_append(GTK_BOX(box), l);
        gtk_button_set_child(GTK_BUTTON(b), box);
        g_object_set_data_full(G_OBJECT(b), "beluga-action", g_strdup(it.action.c_str()), g_free);
        if (it.target) g_object_set_data_full(G_OBJECT(b), "beluga-target", g_variant_ref(it.target), (GDestroyNotify)g_variant_unref);
        g_signal_connect(b, "clicked", CB([](GtkButton *btn) {
            auto *action = static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "beluga-action"));
            auto *target = static_cast<GVariant *>(g_object_get_data(G_OBJECT(btn), "beluga-target"));
            std::string name = action;
            GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
            gtk_widget_activate_action_variant(GTK_WIDGET(btn), name.c_str(), target);
            if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
        }), nullptr);
        gtk_popover_menu_add_child(GTK_POPOVER_MENU(pop), b, it.id.c_str());
        if (it.target) g_variant_unref(it.target);
        if (it.icon) g_object_unref(it.icon);
    }
    pending_icon_items().clear();
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
    // GTK gave context menus a surface 30 px shorter than they need, so they scrolled although they fit.
    fit_menu(GTK_POPOVER(pop));
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
    GtkWidget *pick = gtk_button_new_with_mnemonic(_("C_hoose…"));
    g_signal_connect(pick, "clicked", CB([](GtkButton *b, gpointer entry) {
        Ref<GtkWidget> keep = Ref<GtkWidget>::share(GTK_WIDGET(entry));
        choose_icon(GTK_WIDGET(b), gtk_editable_get_text(GTK_EDITABLE(entry)), [keep](const std::string &icon) {
            gtk_editable_set_text(GTK_EDITABLE(keep.get()), icon.c_str());
        });
    }), icon);
    gtk_box_append(GTK_BOX(iconRow), pick);
    addRow(2, _("Icon name:"), iconRow);
    gtk_box_append(GTK_BOX(p.content), grid);
    g_signal_connect(icon, "changed", CB([](GtkEditable *e, gpointer img) {
        GIcon *gi = icon_from_string(gtk_editable_get_text(e));
        gtk_image_set_from_gicon(GTK_IMAGE(img), gi);
        if (gi) g_object_unref(gi);
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

namespace {

struct NetForm {
    GtkWidget *protocol, *host, *port, *logon, *user, *password, *keyRow, *key, *folder, *name, *address, *note, *ok;
    GtkWidget *userLabel, *passwordLabel, *keyLabel;
    std::vector<Logon> logons;   // what each row of the Logon type list means
    bool syncing = false;
    ~NetForm()
    {
        for (GtkWidget *w : {protocol, host, port, logon, user, password, key, folder, name, address})
            if (w) g_object_unref(w);
    }
};

std::string text_of(GtkWidget *e) { return trim(gtk_editable_get_text(GTK_EDITABLE(e))); }

void set_text_quietly(NetForm *f, GtkWidget *e, const std::string &t)
{
    if (text_of(e) != t) gtk_editable_set_text(GTK_EDITABLE(e), t.c_str());
}

const char *logon_label(Logon l)
{
    switch (l) {
    case Logon::Anonymous: return _("Anonymous");
    case Logon::Normal: return _("Normal");
    case Logon::Ask: return _("Ask for password");
    case Logon::KeyFile: return _("Key file");
    }
    return "";
}

Logon current_logon(NetForm *f)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(f->logon));
    return i < f->logons.size() ? f->logons[i] : Logon::Ask;
}

NetLocation form_location(NetForm *f)
{
    NetLocation n;
    n.protocol = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(f->protocol));
    n.host = text_of(f->host);
    n.user = text_of(f->user);
    n.folder = text_of(f->folder);
    std::string port = text_of(f->port);
    n.port = port.empty() ? 0 : atoi(port.c_str());
    if (n.port < 0 || n.port > 65535) n.port = 0;
    n.logon = current_logon(f);
    n.password = gtk_editable_get_text(GTK_EDITABLE(f->password));
    n.keyFile = text_of(f->key);
    return n;
}

// The Logon type list offers what the protocol can do: no Anonymous for SFTP, Key file only for SFTP.
void fill_logons(NetForm *f)
{
    const NetProtocol &p = net_protocols()[gtk_drop_down_get_selected(GTK_DROP_DOWN(f->protocol))];
    Logon was = current_logon(f);
    f->logons.clear();
    if (p.anonymous) f->logons.push_back(Logon::Anonymous);
    f->logons.push_back(Logon::Normal);
    f->logons.push_back(Logon::Ask);
    if (p.keyFile) f->logons.push_back(Logon::KeyFile);
    GtkStringList *list = gtk_string_list_new(nullptr);
    guint sel = 0;
    for (size_t i = 0; i < f->logons.size(); ++i) {
        gtk_string_list_append(list, logon_label(f->logons[i]));
        if (f->logons[i] == was) sel = (guint)i;
        else if (f->logons[i] == Logon::Ask && std::find(f->logons.begin(), f->logons.end(), was) == f->logons.end()) sel = (guint)i;
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(f->logon), G_LIST_MODEL(list));
    g_object_unref(list);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f->logon), sel);
}

// Fields → address, and which fields the logon type needs.
void net_update(NetForm *f)
{
    if (f->syncing) return;
    f->syncing = true;
    const NetProtocol &p = net_protocols()[gtk_drop_down_get_selected(GTK_DROP_DOWN(f->protocol))];
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->port), p.port ? std::to_string(p.port).c_str() : "");
    Logon l = current_logon(f);
    bool user = l != Logon::Anonymous, password = l == Logon::Normal, key = l == Logon::KeyFile;
    gtk_widget_set_visible(f->user, user);
    gtk_widget_set_visible(f->userLabel, user);
    gtk_widget_set_visible(f->password, password);
    gtk_widget_set_visible(f->passwordLabel, password);
    gtk_widget_set_visible(f->keyRow, key);
    gtk_widget_set_visible(f->keyLabel, key);
    gtk_widget_set_visible(f->note, key);
    NetLocation n = form_location(f);
    std::string uri = net_location_uri(n);
    set_text_quietly(f, f->address, uri);
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->name), n.host.c_str());
    gtk_widget_set_sensitive(f->ok, !uri.empty() && (!key || g_file_test(n.keyFile.c_str(), G_FILE_TEST_IS_REGULAR)));
    f->syncing = false;
}

// Address → fields, for pasting a whole sftp://user@host:port/folder.
void address_changed(NetForm *f)
{
    if (f->syncing) return;
    NetLocation n;
    if (!net_location_from_uri(text_of(f->address), n)) return;
    f->syncing = true;
    if ((int)gtk_drop_down_get_selected(GTK_DROP_DOWN(f->protocol)) != n.protocol) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(f->protocol), (guint)n.protocol);
        fill_logons(f);
    }
    set_text_quietly(f, f->host, n.host);
    set_text_quietly(f, f->port, n.port ? std::to_string(n.port) : std::string());
    set_text_quietly(f, f->folder, n.folder);
    if (!n.user.empty()) {
        set_text_quietly(f, f->user, n.user);
        if (current_logon(f) == Logon::Anonymous) gtk_drop_down_set_selected(GTK_DROP_DOWN(f->logon), 1);
    }
    f->syncing = false;
    // Show the fields the logon needs, but leave the address as typed.
    std::string typed = text_of(f->address);
    net_update(f);
    f->syncing = true;
    set_text_quietly(f, f->address, typed);
    f->syncing = false;
}

} // namespace

void add_network_location_dialog(GtkWidget *parent, std::function<void(const NetLocation &, const std::string &)> cb)
{
    DialogParts p = make_dialog(parent, _("Add Network Location"), 480);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    int row = 0;
    auto addRow = [&](const char *label, GtkWidget *w) {
        GtkWidget *l = gtk_label_new_with_mnemonic(label);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        gtk_widget_set_hexpand(w, TRUE);
        gtk_grid_attach(GTK_GRID(grid), w, 1, row++, 1, 1);
        return l;
    };
    auto form = std::make_shared<NetForm>();
    NetForm *f = form.get();

    // Like FileZilla's Site Manager: protocol, host and port, then how to log on.
    std::vector<const char *> labels;
    for (auto &pr : net_protocols()) labels.push_back(_(pr.label));
    labels.push_back(nullptr);
    f->protocol = gtk_drop_down_new_from_strings(labels.data());
    addRow(_("Protocol:"), f->protocol);
    GtkWidget *hostRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    f->host = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->host), "example.com");
    gtk_widget_set_hexpand(f->host, TRUE);
    gtk_box_append(GTK_BOX(hostRow), f->host);
    GtkWidget *portLabel = gtk_label_new_with_mnemonic(_("Port:"));
    gtk_box_append(GTK_BOX(hostRow), portLabel);
    f->port = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(f->port), GTK_INPUT_PURPOSE_DIGITS);
    gtk_editable_set_width_chars(GTK_EDITABLE(f->port), 6);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(f->port), 6);
    gtk_widget_set_hexpand(f->port, FALSE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(portLabel), f->port);
    gtk_box_append(GTK_BOX(hostRow), f->port);
    GtkWidget *hostLabel = addRow(_("Host:"), hostRow);
    gtk_label_set_mnemonic_widget(GTK_LABEL(hostLabel), f->host);
    f->logon = gtk_drop_down_new(nullptr, nullptr);
    addRow(_("Logon type:"), f->logon);
    f->user = gtk_entry_new();
    f->userLabel = addRow(_("User:"), f->user);
    f->password = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(f->password), TRUE);
    f->passwordLabel = addRow(_("Password:"), f->password);
    f->keyRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    f->key = gtk_entry_new();
    gtk_widget_set_hexpand(f->key, TRUE);
    GtkWidget *browse = gtk_button_new_with_mnemonic(_("Browse…"));
    gtk_box_append(GTK_BOX(f->keyRow), f->key);
    gtk_box_append(GTK_BOX(f->keyRow), browse);
    f->keyLabel = addRow(_("Key file:"), f->keyRow);
    gtk_label_set_mnemonic_widget(GTK_LABEL(f->keyLabel), f->key);
    f->folder = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->folder), "/");
    addRow(_("Folder:"), f->folder);
    f->name = gtk_entry_new();
    addRow(_("Name:"), f->name);
    f->address = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->address), "sftp://user@example.com/folder");
    gtk_widget_set_tooltip_text(f->address, _("Type or paste an address to fill in the fields above."));
    addRow(_("Address:"), f->address);
    gtk_box_append(GTK_BOX(p.content), grid);
    f->note = gtk_label_new(_("The SFTP connection uses ssh's settings, so Beluga adds a Host entry for this key to "
                              "~/.ssh/config."));
    gtk_label_set_wrap(GTK_LABEL(f->note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(f->note), 0);
    gtk_widget_set_margin_top(f->note, 8);
    gtk_widget_add_css_class(f->note, "dim-label");
    gtk_box_append(GTK_BOX(p.content), f->note);
    for (GtkWidget *w : {f->protocol, f->host, f->port, f->logon, f->user, f->password, f->key, f->folder, f->name, f->address})
        g_object_ref(w);

    GtkWidget *cancel = add_button(p, _("_Cancel"));
    f->ok = add_button(p, _("_Add"), true);
    gtk_window_set_default_widget(GTK_WINDOW(p.window), f->ok);
    for (GtkWidget *e : {f->host, f->port, f->user, f->key, f->folder, f->name, f->address})
        gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    for (GtkWidget *e : {f->host, f->port, f->user, f->key, f->folder})
        g_signal_connect(e, "changed", CB([](GtkEditable *, gpointer d) { net_update(static_cast<NetForm *>(d)); }), f);
    g_signal_connect(f->address, "changed", CB([](GtkEditable *, gpointer d) { address_changed(static_cast<NetForm *>(d)); }), f);
    g_signal_connect(f->protocol, "notify::selected", CB([](GObject *, GParamSpec *, gpointer d) {
        auto *nf = static_cast<NetForm *>(d);
        if (nf->syncing) return;
        nf->syncing = true;
        fill_logons(nf);
        nf->syncing = false;
        net_update(nf);
    }), f);
    g_signal_connect(f->logon, "notify::selected", CB([](GObject *, GParamSpec *, gpointer d) { net_update(static_cast<NetForm *>(d)); }), f);
    g_signal_connect(browse, "clicked", CB([](GtkButton *b, gpointer entry) {
        GtkFileDialog *fd = gtk_file_dialog_new();
        gtk_file_dialog_set_title(fd, _("Choose a Key File"));
        Ref<GFile> ssh(g_file_new_for_path((home_dir() + "/.ssh").c_str()));
        gtk_file_dialog_set_initial_folder(fd, ssh.get());
        gtk_file_dialog_open(fd, window_of(GTK_WIDGET(b)), nullptr, [](GObject *src, GAsyncResult *res, gpointer e) {
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
            if (file) {
                char *path = g_file_get_path(file);
                if (path) gtk_editable_set_text(GTK_EDITABLE(e), path);
                g_free(path);
                g_object_unref(file);
            }
            g_object_unref(e);
        }, g_object_ref(entry));
        g_object_unref(fd);
    }), f->key);
    g_object_set_data_full(G_OBJECT(p.window), "beluga-net-form", new std::shared_ptr<NetForm>(form),
                           [](gpointer d) { delete static_cast<std::shared_ptr<NetForm> *>(d); });
    fill_logons(f);
    net_update(f);

    on_click_close(cancel, p.window, nullptr);
    on_click_close(f->ok, p.window, [form, cb] {
        NetForm *nf = form.get();
        NetLocation n = form_location(nf);
        if (net_location_uri(n).empty()) return;
        std::string name = text_of(nf->name);
        if (cb) cb(n, name.empty() ? n.host : name);
    });
    gtk_window_present(GTK_WINDOW(p.window));
    gtk_widget_grab_focus(f->host);
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
    // A folder with its own settings, or every folder when each remembers its own view, starts on
    // "Current folder"; otherwise the change is to the default for all folders.
    bool own = !settings().globalViewProps || has_own_view_props(uri);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(own ? r0 : r2), TRUE);
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
    // Any other way the dialog goes away (closed by hand, or destroyed with the window it belongs
    // to) cancels: the job's thread waits for an answer and would otherwise wait forever.
    auto *closeData = new std::function<void()>([answer] { answer(ConflictChoice::Cancel); });
    // Deferred: the buttons destroy the dialog before they answer, and their answer must win.
    g_signal_connect_data(p.window, "destroy", CB([](GtkWidget *, gpointer d) {
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer f) -> gboolean {
            (*static_cast<std::function<void()> *>(f))();
            return G_SOURCE_REMOVE;
        }, new std::function<void()>(*static_cast<std::function<void()> *>(d)),
           [](gpointer f) { delete static_cast<std::function<void()> *>(f); });
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

void choose_icon(GtkWidget *parent, const std::string &current, std::function<void(const std::string &)> done)
{
    struct Ui {
        GtkWidget *win, *grid, *search;
        GtkSingleSelection *sel;
        GtkStringFilter *filter;
        std::function<void(const std::string &)> done;
    };
    auto *ui = new Ui{};
    ui->done = std::move(done);
    GtkWidget *win = gtk_window_new();
    ui->win = win;
    g_object_set_data_full(G_OBJECT(win), "beluga-ui", ui, [](gpointer p) { delete static_cast<Ui *>(p); });
    gtk_window_set_title(GTK_WINDOW(win), _("Choose an Icon"));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    if (GtkWindow *pw = window_of(parent)) gtk_window_set_transient_for(GTK_WINDOW(win), pw);
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 520);
    gtk_widget_add_css_class(win, "beluga-dialog");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);
    ui->search = gtk_search_entry_new();
    g_object_set(ui->search, "placeholder-text", _("Search icons…"), nullptr);
    gtk_box_append(GTK_BOX(box), ui->search);

    // The theme's icons, coloured ones first (symbolic ones are for buttons).
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(win));
    char **names = gtk_icon_theme_get_icon_names(theme);
    std::vector<std::string> list;
    for (char **n = names; n && *n; ++n)
        if (!g_str_has_suffix(*n, "-symbolic")) list.push_back(*n);
    g_strfreev(names);
    std::sort(list.begin(), list.end());
    GtkStringList *model = gtk_string_list_new(nullptr);
    for (auto &n : list) gtk_string_list_append(model, n.c_str());
    ui->filter = gtk_string_filter_new(gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string"));
    gtk_string_filter_set_match_mode(ui->filter, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
    GtkFilterListModel *filtered = gtk_filter_list_model_new(G_LIST_MODEL(model), GTK_FILTER(g_object_ref(ui->filter)));
    ui->sel = gtk_single_selection_new(G_LIST_MODEL(filtered));
    gtk_single_selection_set_autoselect(ui->sel, FALSE);
    gtk_single_selection_set_can_unselect(ui->sel, TRUE);
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(ui->sel));
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", CB([](GtkSignalListItemFactory *, GtkListItem *li) {
        GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *img = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
        gtk_box_append(GTK_BOX(b), img);
        GtkWidget *l = gtk_label_new(nullptr);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
        gtk_widget_add_css_class(l, "caption");
        gtk_box_append(GTK_BOX(b), l);
        gtk_list_item_set_child(li, b);
    }), nullptr);
    g_signal_connect(f, "bind", CB([](GtkSignalListItemFactory *, GtkListItem *li) {
        const char *name = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(li)));
        GtkWidget *b = gtk_list_item_get_child(li);
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_widget_get_first_child(b)), name);
        gtk_label_set_text(GTK_LABEL(gtk_widget_get_last_child(b)), name);
        gtk_widget_set_tooltip_text(b, name);
    }), nullptr);
    ui->grid = gtk_grid_view_new(GTK_SELECTION_MODEL(ui->sel), f);
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(ui->grid), 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui->grid);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);
    g_signal_connect(ui->search, "search-changed", CB([](GtkSearchEntry *e, gpointer p) {
        gtk_string_filter_set_search(static_cast<Ui *>(p)->filter, gtk_editable_get_text(GTK_EDITABLE(e)));
    }), ui);
    if (!current.empty() && current[0] != '/') gtk_editable_set_text(GTK_EDITABLE(ui->search), current.c_str());

    auto finish = [](Ui *ui, const std::string &icon) {
        auto done = ui->done;
        gtk_window_destroy(GTK_WINDOW(ui->win));
        done(icon);
    };
    static auto finishF = finish;
    g_signal_connect(ui->grid, "activate", CB([](GtkGridView *, guint, gpointer p) {
        auto *ui = static_cast<Ui *>(p);
        if (gpointer it = gtk_single_selection_get_selected_item(ui->sel))
            finishF(ui, gtk_string_object_get_string(GTK_STRING_OBJECT(it)));
    }), ui);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *browse = gtk_button_new_with_mnemonic(_("_Image File…"));
    GtkWidget *reset = gtk_button_new_with_mnemonic(_("_Default Icon"));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    GtkWidget *ok = gtk_button_new_with_mnemonic(_("_OK"));
    gtk_widget_add_css_class(ok, "suggested-action");
    for (GtkWidget *b : {browse, reset, spacer, cancel, ok}) gtk_box_append(GTK_BOX(buttons), b);
    gtk_box_append(GTK_BOX(box), buttons);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    g_signal_connect(reset, "clicked", CB([](GtkButton *, gpointer p) { finishF(static_cast<Ui *>(p), ""); }), ui);
    g_signal_connect(ok, "clicked", CB([](GtkButton *, gpointer p) {
        auto *ui = static_cast<Ui *>(p);
        if (gpointer it = gtk_single_selection_get_selected_item(ui->sel))
            finishF(ui, gtk_string_object_get_string(GTK_STRING_OBJECT(it)));
    }), ui);
    g_signal_connect(browse, "clicked", CB([](GtkButton *, gpointer p) {
        auto *ui = static_cast<Ui *>(p);
        GtkFileDialog *fd = gtk_file_dialog_new();
        gtk_file_dialog_set_title(fd, _("Choose an Image"));
        GtkFileFilter *images = gtk_file_filter_new();
        gtk_file_filter_set_name(images, _("Images"));
        gtk_file_filter_add_mime_type(images, "image/*");
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, images);
        gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
        g_object_unref(filters);
        g_object_unref(images);
        Ref<GtkWidget> keep = Ref<GtkWidget>::share(ui->win);
        gtk_file_dialog_open(fd, GTK_WINDOW(ui->win), nullptr, [](GObject *src, GAsyncResult *res, gpointer p) {
            auto *keep = static_cast<GtkWidget *>(p);
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
            auto *ui = static_cast<Ui *>(g_object_get_data(G_OBJECT(keep), "beluga-ui"));
            if (file && ui) finishF(ui, take(g_file_get_path(file)));
            if (file) g_object_unref(file);
            g_object_unref(keep);
        }, g_object_ref(ui->win));
        g_object_unref(fd);
    }), ui);
    gtk_window_present(GTK_WINDOW(win));
}

} // namespace beluga
