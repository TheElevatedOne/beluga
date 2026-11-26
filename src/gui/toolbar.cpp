// SPDX-License-Identifier: GPL-2.0-or-later
#include "toolbar.hpp"

#include "dialogs.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <algorithm>
#include <memory>

namespace beluga {

namespace {
const char *back[] = {"go-previous", "go-previous-symbolic", nullptr};
const char *fwd[] = {"go-next", "go-next-symbolic", nullptr};
const char *up[] = {"go-up", "go-up-symbolic", nullptr};
const char *home[] = {"go-home", "user-home", nullptr};
const char *reload[] = {"view-refresh", "view-refresh-symbolic", nullptr};
const char *icons[] = {"view-list-icons", "view-grid-symbolic", nullptr};
const char *compact[] = {"view-list-details", "view-list-compact-symbolic", "view-list-symbolic", nullptr};
const char *details[] = {"view-list-tree", "view-list-symbolic", nullptr};
const char *columns[] = {"view-column", "view-file-columns", "view-columns-symbolic", "view-dual-symbolic", "view-list-symbolic", nullptr};
const char *sort[] = {"view-sort", "view-sort-ascending-symbolic", nullptr};
const char *splitIcons[] = {"view-split-left-right", "view-dual-symbolic", nullptr};
const char *search[] = {"edit-find", "system-search-symbolic", nullptr};
const char *filter[] = {"view-filter", "edit-find-replace", nullptr};
const char *menu[] = {"application-menu", "open-menu-symbolic", nullptr};
const char *usage[] = {"disk-quota", "baobab", "org.gnome.baobab", "drive-harddisk", nullptr};
const char *hidden[] = {"view-hidden", "view-visible", "view-reveal-symbolic", nullptr};
const char *previews[] = {"view-preview", "image-x-generic", nullptr};
const char *newTab[] = {"tab-new", "tab-new-symbolic", nullptr};
const char *newWindow[] = {"window-new", "window-new-symbolic", nullptr};
const char *newFolder[] = {"folder-new", "folder-new-symbolic", nullptr};
const char *terminal[] = {"utilities-terminal", "utilities-terminal-symbolic", nullptr};
const char *undo[] = {"edit-undo", "edit-undo-symbolic", nullptr};
const char *redo[] = {"edit-redo", "edit-redo-symbolic", nullptr};
const char *cut[] = {"edit-cut", "edit-cut-symbolic", nullptr};
const char *copy[] = {"edit-copy", "edit-copy-symbolic", nullptr};
const char *paste[] = {"edit-paste", "edit-paste-symbolic", nullptr};
const char *trashIcons[] = {"user-trash", "user-trash-symbolic", nullptr};
const char *del[] = {"edit-delete", "edit-delete-symbolic", nullptr};
const char *props[] = {"document-properties", "document-properties-symbolic", nullptr};
const char *places[] = {"compass", "bookmarks", "user-bookmarks-symbolic", nullptr};
const char *info[] = {"dialog-information", "help-about-symbolic", nullptr};
const char *configure[] = {"configure", "preferences-system", "emblem-system-symbolic", nullptr};
const char *location[] = {"edit-find-replace", "folder", nullptr};
const char *separator[] = {"", nullptr};
} // namespace

const std::vector<ToolItemInfo> &tool_items()
{
    static const std::vector<ToolItemInfo> t = {
        {"back", N_("Back"), back, ToolKind::Button, "win.back"},
        {"forward", N_("Forward"), fwd, ToolKind::Button, "win.forward"},
        {"up", N_("Up"), up, ToolKind::Button, "win.up"},
        {"home", N_("Home"), home, ToolKind::Button, "win.home"},
        {"reload", N_("Reload"), reload, ToolKind::Button, "win.reload"},
        {"view-menu", N_("View Mode"), icons, ToolKind::ViewMenu, nullptr},
        {"view-icons", N_("Icons View"), icons, ToolKind::Toggle, "win.view-mode::icons"},
        {"view-compact", N_("Compact View"), compact, ToolKind::Toggle, "win.view-mode::compact"},
        {"view-details", N_("Details View"), details, ToolKind::Toggle, "win.view-mode::details"},
        {"view-columns", N_("Columns View"), columns, ToolKind::Toggle, "win.view-mode::columns"},
        {"sort", N_("Sort By"), sort, ToolKind::SortMenu, nullptr},
        {"location", N_("Location Bar"), location, ToolKind::Location, nullptr},
        {"disk-usage", N_("Disk Usage"), usage, ToolKind::Toggle, "win.show-disk-usage"},
        {"split", N_("Split View"), splitIcons, ToolKind::Split, "win.split"},
        {"search", N_("Search"), search, ToolKind::Toggle, "win.toggle-search"},
        {"filter", N_("Filter"), filter, ToolKind::Toggle, "win.toggle-filter"},
        {"show-hidden", N_("Show Hidden Files"), hidden, ToolKind::Toggle, "win.show-hidden"},
        {"show-previews", N_("Show Previews"), previews, ToolKind::Toggle, "win.show-previews"},
        {"places", N_("Places Panel"), places, ToolKind::Toggle, "win.show-places"},
        {"information", N_("Information Panel"), info, ToolKind::Toggle, "win.show-info"},
        {"new-tab", N_("New Tab"), newTab, ToolKind::Button, "win.new-tab"},
        {"new-window", N_("New Window"), newWindow, ToolKind::Button, "win.new-window"},
        {"new-folder", N_("Create Folder"), newFolder, ToolKind::Button, "win.create-folder"},
        {"terminal", N_("Open Terminal"), terminal, ToolKind::Button, "win.open-terminal"},
        {"undo", N_("Undo"), undo, ToolKind::Button, "win.undo"},
        {"redo", N_("Redo"), redo, ToolKind::Button, "win.redo"},
        {"cut", N_("Cut"), cut, ToolKind::Button, "win.cut"},
        {"copy", N_("Copy"), copy, ToolKind::Button, "win.copy"},
        {"paste", N_("Paste"), paste, ToolKind::Button, "win.paste"},
        {"trash", N_("Move to Trash"), trashIcons, ToolKind::Button, "win.move-to-trash"},
        {"delete", N_("Delete"), del, ToolKind::Button, "win.delete"},
        {"properties", N_("Properties"), props, ToolKind::Button, "win.properties"},
        {"preferences", N_("Configure Beluga"), configure, ToolKind::Button, "win.preferences"},
        {"menu", N_("Menu"), menu, ToolKind::Menu, nullptr},
        {"separator", N_("--- separator ---"), separator, ToolKind::Separator, nullptr},
    };
    return t;
}

const ToolItemInfo *tool_item(const std::string &id)
{
    for (auto &i : tool_items())
        if (id == i.id) return &i;
    return nullptr;
}

std::vector<std::string> default_toolbar() { return Settings().toolbarItems; }

// ---- Configure Toolbars ---------------------------------------------------------------------------

namespace {

struct Editor {
    GtkWidget *win = nullptr, *available = nullptr, *current = nullptr, *filter = nullptr, *apply = nullptr;
    std::vector<std::string> items;   // the tool bar being edited
};

GtkWidget *item_row(const ToolItemInfo &info, bool moved = false)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    GIcon *gi = g_themed_icon_new_from_names((char **)info.icons, -1);
    GtkWidget *img = gtk_image_new_from_gicon(gi);
    g_object_unref(gi);
    gtk_box_append(GTK_BOX(box), img);
    // "*": not where the default tool bar has it.
    GtkWidget *l = gtk_label_new((std::string(_(info.title)) + (moved ? " *" : "")).c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), l);
    g_object_set_data(G_OBJECT(box), "beluga-id", (gpointer)info.id);
    return box;
}

const char *row_id(GtkListBoxRow *row)
{
    return row ? static_cast<const char *>(g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "beluga-id")) : nullptr;
}

void fill(Editor *e, int selectCurrent = -1)
{
    list_box_remove_rows(GTK_LIST_BOX(e->available));
    list_box_remove_rows(GTK_LIST_BOX(e->current));
    std::string q = take(g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(e->filter)), -1));
    for (auto &info : tool_items()) {
        bool used = info.kind != ToolKind::Separator && std::find(e->items.begin(), e->items.end(), info.id) != e->items.end();
        std::string title = take(g_utf8_casefold(_(info.title), -1));
        if (!used && (q.empty() || title.find(q) != std::string::npos)) gtk_list_box_append(GTK_LIST_BOX(e->available), item_row(info));
    }
    const auto defaults = default_toolbar();
    for (size_t i = 0; i < e->items.size(); ++i)
        if (const ToolItemInfo *info = tool_item(e->items[i]))
            gtk_list_box_append(GTK_LIST_BOX(e->current), item_row(*info, i >= defaults.size() || defaults[i] != e->items[i]));
    if (selectCurrent >= 0)
        if (GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(e->current), selectCurrent))
            gtk_list_box_select_row(GTK_LIST_BOX(e->current), r);
}

void edited(Editor *e, int select)
{
    fill(e, select);
    gtk_widget_set_sensitive(e->apply, TRUE);
}

void apply(Editor *e)
{
    settings().toolbarItems = e->items;
    settings().save();
    notify_settings_changed();
    gtk_widget_set_sensitive(e->apply, FALSE);
}

GtkWidget *icon_button(const char *const *icons, const char *tooltip)
{
    GtkWidget *b = gtk_button_new();
    GIcon *gi = g_themed_icon_new_from_names((char **)icons, -1);
    gtk_button_set_child(GTK_BUTTON(b), gtk_image_new_from_gicon(gi));
    g_object_unref(gi);
    gtk_widget_set_tooltip_text(b, tooltip);
    return b;
}

GtkWidget *list_column(const char *title, GtkWidget **list, GtkWidget *extra)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(col, TRUE);
    GtkWidget *l = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(col), l);
    if (extra) gtk_box_append(GTK_BOX(col), extra);
    *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(*list), GTK_SELECTION_SINGLE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), *list);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), *list);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_child(GTK_FRAME(frame), scroll);
    gtk_box_append(GTK_BOX(col), frame);
    return col;
}

} // namespace

void show_toolbar_editor(GtkWidget *parent)
{
    static GtkWidget *open = nullptr;
    if (open) {
        gtk_window_present(GTK_WINDOW(open));
        return;
    }
    auto *e = new Editor;
    e->items = settings().toolbarItems;
    GtkWidget *win = gtk_window_new();
    e->win = win;
    open = win;
    g_object_set_data_full(G_OBJECT(win), "beluga-editor", e, [](gpointer p) { delete static_cast<Editor *>(p); });
    g_signal_connect(win, "destroy", CB([](GtkWidget *) { open = nullptr; }), nullptr);
    gtk_window_set_title(GTK_WINDOW(win), _("Configure Toolbars"));
    if (GtkWindow *pw = window_of(parent)) {
        gtk_window_set_transient_for(GTK_WINDOW(win), pw);
        if (GtkApplication *app = gtk_window_get_application(pw)) gtk_window_set_application(GTK_WINDOW(win), app);
    }
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 560);
    gtk_widget_add_css_class(win, "beluga-dialog");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);
    GtkWidget *hint = gtk_label_new(_("Actions above the location bar in the list sit left of it, the others right of it. "
                                      "* marks actions that aren't where the default tool bar has them."));
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(box), hint);

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_vexpand(cols, TRUE);
    e->filter = gtk_search_entry_new();
    g_object_set(e->filter, "placeholder-text", _("Filter…"), nullptr);
    gtk_box_append(GTK_BOX(cols), list_column(_("A_vailable actions:"), &e->available, e->filter));
    static const char *addIcons[] = {"go-next", "go-next-symbolic", nullptr};
    static const char *removeIcons[] = {"go-previous", "go-previous-symbolic", nullptr};
    static const char *upIcons[] = {"go-up", "go-up-symbolic", nullptr};
    static const char *downIcons[] = {"go-down", "go-down-symbolic", nullptr};
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(mid, GTK_ALIGN_CENTER);
    GtkWidget *addB = icon_button(addIcons, _("Add to the tool bar"));
    GtkWidget *removeB = icon_button(removeIcons, _("Remove from the tool bar"));
    GtkWidget *upB = icon_button(upIcons, _("Move up (left on the tool bar)"));
    GtkWidget *downB = icon_button(downIcons, _("Move down (right on the tool bar)"));
    for (GtkWidget *b : {addB, removeB, upB, downB}) gtk_box_append(GTK_BOX(mid), b);
    gtk_box_append(GTK_BOX(cols), mid);
    gtk_box_append(GTK_BOX(cols), list_column(_("C_urrent actions:"), &e->current, nullptr));
    gtk_box_append(GTK_BOX(box), cols);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *defaults = gtk_button_new_with_mnemonic(_("_Defaults"));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *ok = gtk_button_new_with_mnemonic(_("_OK"));
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    e->apply = gtk_button_new_with_mnemonic(_("_Apply"));
    gtk_widget_set_sensitive(e->apply, FALSE);
    gtk_widget_add_css_class(ok, "suggested-action");
    for (GtkWidget *b : {defaults, spacer, ok, cancel, e->apply}) gtk_box_append(GTK_BOX(buttons), b);
    gtk_box_append(GTK_BOX(box), buttons);

    // Adds the available action after the selected current one (or at the end); the separator stays available.
    auto add = [](Editor *e) {
        const char *id = row_id(gtk_list_box_get_selected_row(GTK_LIST_BOX(e->available)));
        if (!id) return;
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(e->current));
        int at = cur ? gtk_list_box_row_get_index(cur) + 1 : (int)e->items.size();
        e->items.insert(e->items.begin() + at, id);
        edited(e, at);
    };
    auto remove = [](Editor *e) {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(e->current));
        if (!cur) return;
        int i = gtk_list_box_row_get_index(cur);
        e->items.erase(e->items.begin() + i);
        edited(e, std::min(i, (int)e->items.size() - 1));
    };
    auto move = [](Editor *e, int by) {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(e->current));
        if (!cur) return;
        int i = gtk_list_box_row_get_index(cur), j = i + by;
        if (j < 0 || j >= (int)e->items.size()) return;
        std::swap(e->items[i], e->items[j]);
        edited(e, j);
    };
    static auto addF = add;
    static auto removeF = remove;
    static auto moveF = move;
    g_signal_connect(addB, "clicked", CB([](GtkButton *, gpointer p) { addF(static_cast<Editor *>(p)); }), e);
    g_signal_connect(removeB, "clicked", CB([](GtkButton *, gpointer p) { removeF(static_cast<Editor *>(p)); }), e);
    g_signal_connect(upB, "clicked", CB([](GtkButton *, gpointer p) { moveF(static_cast<Editor *>(p), -1); }), e);
    g_signal_connect(downB, "clicked", CB([](GtkButton *, gpointer p) { moveF(static_cast<Editor *>(p), 1); }), e);
    g_signal_connect(e->filter, "search-changed", CB([](GtkSearchEntry *, gpointer p) { fill(static_cast<Editor *>(p)); }), e);
    g_signal_connect(defaults, "clicked", CB([](GtkButton *, gpointer p) {
        auto *e = static_cast<Editor *>(p);
        e->items = default_toolbar();
        edited(e, -1);
    }), e);
    g_signal_connect(ok, "clicked", CB([](GtkButton *, gpointer p) {
        auto *e = static_cast<Editor *>(p);
        apply(e);
        gtk_window_destroy(GTK_WINDOW(e->win));
    }), e);
    g_signal_connect(cancel, "clicked", CB([](GtkButton *, gpointer w) { gtk_window_destroy(GTK_WINDOW(w)); }), win);
    g_signal_connect(e->apply, "clicked", CB([](GtkButton *, gpointer p) { apply(static_cast<Editor *>(p)); }), e);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
        if (keyval != GDK_KEY_Escape) return FALSE;
        gtk_window_destroy(GTK_WINDOW(w));
        return TRUE;
    }), win);
    gtk_widget_add_controller(win, key);

    fill(e);
    gtk_window_present(GTK_WINDOW(win));
}

} // namespace beluga
