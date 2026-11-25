// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: DolphinContextMenu. Menus for items, the empty view area, the trash
// and search results. Entries call window actions.
#include "mainwindow.hpp"

#include "app.hpp"
#include "customactions.hpp"
#include "dialogs.hpp"
#include "openwith.hpp"
#include "places.hpp"
#include "settings.hpp"
#include "tabpage.hpp"

#include <algorithm>

namespace beluga {

GMenu *build_sort_menu();
GMenu *build_view_mode_menu();

namespace {

GMenu *sec(GMenu *parent)
{
    GMenu *s = g_menu_new();
    g_menu_append_section(parent, nullptr, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

void item(GMenu *m, const char *label, const char *action, const char *icon = nullptr)
{
    GMenuItem *mi = g_menu_item_new(label, action);
    if (icon) {
        GIcon *i = g_themed_icon_new(icon);
        g_menu_item_set_icon(mi, i);
        g_object_unref(i);
    }
    g_menu_append_item(m, mi);
    g_object_unref(mi);
}

void item_target(GMenu *m, const std::string &label, const char *action, GVariant *target, GIcon *icon = nullptr)
{
    GMenuItem *mi = g_menu_item_new(label.c_str(), nullptr);
    g_menu_item_set_action_and_target_value(mi, action, target);
    if (icon) g_menu_item_set_icon(mi, icon);
    g_menu_append_item(m, mi);
    g_object_unref(mi);
}

void add_create_new(GMenu *m)
{
    GMenu *sub = g_menu_new();
    GMenu *s = sec(sub);
    item(s, _("Folder…"), "win.create-folder", "folder-new");
    item(s, _("Text File…"), "win.create-file", "text-plain");
    item(s, _("Empty File…"), "win.create-empty-file", "document-new");
    item(s, _("Link to File or Folder…"), "win.create-link", "insert-link");
    std::string tdir = user_dir(G_USER_DIRECTORY_TEMPLATES);
    if (!tdir.empty()) {
        std::vector<std::string> names;
        if (GDir *d = g_dir_open(tdir.c_str(), 0, nullptr)) {
            while (const char *n = g_dir_read_name(d))
                if (n[0] != '.' && g_file_test((tdir + "/" + n).c_str(), G_FILE_TEST_IS_REGULAR)) names.push_back(n);
            g_dir_close(d);
        }
        std::sort(names.begin(), names.end());
        if (!names.empty()) {
            GMenu *t = sec(sub);
            for (auto &n : names) {
                std::string path = tdir + "/" + n;
                char *ct = g_content_type_guess(n.c_str(), nullptr, 0, nullptr);
                GIcon *icon = g_content_type_get_icon(ct);
                item_target(t, split_extension(n).first + "…", "win.new-from-template", g_variant_new_string(path.c_str()), icon);
                g_object_unref(icon);
                g_free(ct);
            }
        }
    }
    g_menu_append_submenu(m, _("Create New"), G_MENU_MODEL(sub));
    g_object_unref(sub);
}

void add_copy_move(GMenu *m, MainWindow *w)
{
    TabPage *t = w->activeTab();
    for (int move = 0; move < 2; ++move) {
        GMenu *sub = g_menu_new();
        const char *action = move ? "win.move-to" : "win.copy-to";
        if (t && t->split() && !t->other(t->active())->view()->isVirtual()) {
            ViewContainer *o = t->other(t->active());
            item_target(sub, printf_str(_("Other View (%s)"), o->title().c_str()), action, g_variant_new_string(o->uri().c_str()));
        }
        for (auto &p : Places::instance().list()) {
            if (p.hidden || !starts_with(p.uri, "file:")) continue;
            GIcon *icon = g_themed_icon_new(p.icon.c_str());
            item_target(sub, p.label, action, g_variant_new_string(p.uri.c_str()), icon);
            g_object_unref(icon);
        }
        item_target(sub, _("Browse…"), action, g_variant_new_string("browse"));
        g_menu_append_submenu(m, move ? _("Move To") : _("Copy To"), G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
}

void add_open_with(GMenu *m, const std::vector<BelugaItem *> &items)
{
    std::string mime = item_data(items[0]).mime;
    for (BelugaItem *it : items)
        if (item_data(it).mime != mime) {
            item(m, _("Open"), "win.open-selected", "document-open");
            item(m, _("Open With…"), "win.open-with");
            return;
        }
    auto apps = apps_for_type(mime);
    if (!apps.empty()) {
        GAppInfo *def = apps[0].get();
        const char *id = g_app_info_get_id(def);
        if (id)
            item_target(m, printf_str(_("Open with %s"), g_app_info_get_display_name(def)), "win.open-with-app",
                        g_variant_new_string(id), g_app_info_get_icon(def));
    }
    GMenu *sub = g_menu_new();
    GMenu *s = sec(sub);
    for (size_t i = 1; i < apps.size() && i < 16; ++i) {
        const char *id = g_app_info_get_id(apps[i].get());
        if (!id) continue;
        item_target(s, g_app_info_get_display_name(apps[i].get()), "win.open-with-app", g_variant_new_string(id),
                    g_app_info_get_icon(apps[i].get()));
    }
    item(sec(sub), _("Other Application…"), "win.open-with");
    g_menu_append_submenu(m, _("Open With"), G_MENU_MODEL(sub));
    g_object_unref(sub);
}

void add_custom_actions(GMenu *m, const std::vector<BelugaItem *> &items)
{
    std::vector<ActionTarget> targets;
    for (BelugaItem *it : items) {
        const ItemData &d = item_data(it);
        targets.push_back({take(g_file_get_path(d.file.get())), d.uri, d.name, d.mime, d.isDir});
    }
    auto actions = load_custom_actions();
    GMenu *s = nullptr;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (!action_applies(actions[i], targets)) continue;
        if (!s) s = sec(m);
        GIcon *icon = actions[i].icon.empty() ? nullptr : g_themed_icon_new(actions[i].icon.c_str());
        item_target(s, actions[i].name, "win.custom-action", g_variant_new_int32((int)i), icon);
        if (icon) g_object_unref(icon);
    }
}

} // namespace

void MainWindow::showContextMenu(ViewContainer *c, int pos, double x, double y)
{
    containerActivated(c);
    updateActions();
    View *v = c->view();
    const Settings &st = settings();
    GMenu *m = g_menu_new();
    auto items = pos >= 0 ? v->selectedItems() : std::vector<BelugaItem *>();
    TabPage *t = activeTab();
    bool split = t && t->split();

    if (items.empty()) {
        if (v->isTrash()) {
            item(sec(m), _("Empty Trash"), "win.empty-trash", "trash-empty");
        } else if (!v->isVirtual()) {
            GMenu *s = sec(m);
            add_create_new(s);
            s = sec(m);
            item(s, _("Paste"), "win.paste", "edit-paste");
            s = sec(m);
            if (st.ctxAddToPlaces) item(s, _("Add to Places"), "win.add-to-places", "list-add");
            if (st.ctxOpenTerminal) item(s, _("Open Terminal Here"), "win.open-terminal", "utilities-terminal");
            if (st.ctxCopyLocation) item(s, _("Copy Location"), "win.copy-location", "edit-copy-path");
        }
        GMenu *s = sec(m);
        if (st.ctxSortBy) {
            GMenu *sort = build_sort_menu();
            g_menu_append_submenu(s, _("Sort By"), G_MENU_MODEL(sort));
            g_object_unref(sort);
        }
        if (st.ctxViewMode) {
            GMenu *mode = build_view_mode_menu();
            g_menu_append_submenu(s, _("View Mode"), G_MENU_MODEL(mode));
            g_object_unref(mode);
        }
        item(s, _("Show Hidden Files"), "win.show-hidden");
        if (!v->isVirtual()) item(s, _("Remember View for This Folder"), "win.remember-view");
        if (!v->isVirtual()) {
            GMenu *t = sec(m);
            item(t, _("Disk Usage"), "win.disk-usage", "drive-harddisk");
            item(t, _("Properties"), "win.folder-properties", "document-properties");
        }
    } else if (v->isTrash()) {
        GMenu *s = sec(m);
        item(s, _("Restore"), "win.restore", "edit-undo");
        s = sec(m);
        item(s, _("Copy"), "win.copy", "edit-copy");
        item(s, _("Delete"), "win.delete", "edit-delete");
        item(sec(m), _("Properties"), "win.properties", "document-properties");
    } else {
        bool allDirs = std::all_of(items.begin(), items.end(), [](BelugaItem *i) { return item_data(i).isDir; });
        bool anyDir = std::any_of(items.begin(), items.end(), [](BelugaItem *i) { return item_data(i).isDir; });
        GMenu *s = sec(m);
        if (allDirs) {
            if (items.size() == 1) item(s, _("Open"), "win.open-selected", "document-open");
            if (st.ctxOpenInNewTab) item(s, items.size() == 1 ? _("Open in New Tab") : _("Open in New Tabs"), "win.open-in-new-tab", "tab-new");
            if (st.ctxOpenInNewWindow) item(s, _("Open in New Window"), "win.open-in-new-window", "window-new");
            if (st.ctxOpenInSplitView && items.size() == 1) item(s, _("Open in Split View"), "win.open-in-split-view", "view-split-left-right");
        } else if (!anyDir) {
            add_open_with(s, items);
        } else {
            item(s, _("Open"), "win.open-selected", "document-open");
        }
        if (v->isVirtual()) item(s, _("Open Containing Folder"), "win.open-containing-folder", "document-open-folder");
        s = sec(m);
        item(s, _("Cut"), "win.cut", "edit-cut");
        item(s, _("Copy"), "win.copy", "edit-copy");
        if (st.ctxCopyLocation) item(s, _("Copy Location"), "win.copy-location", "edit-copy-path");
        if (items.size() == 1 && allDirs) item(s, _("Paste into Folder"), "win.paste-into", "edit-paste");
        s = sec(m);
        item(s, _("Rename…"), "win.rename", "edit-rename");
        if (st.ctxDuplicateHere && !v->isVirtual()) item(s, _("Duplicate Here"), "win.duplicate", "edit-copy");
        item(s, _("Move to Trash"), "win.move-to-trash", "user-trash");
        if (st.ctxDeletePermanently) item(s, _("Delete"), "win.delete", "edit-delete");
        s = sec(m);
        if (st.ctxCopyMoveMenu) add_copy_move(s, this);
        if (split && st.ctxCopyToOtherView) item(s, _("Copy to Other View"), "win.copy-to-other-view");
        if (split && st.ctxMoveToOtherView) item(s, _("Move to Other View"), "win.move-to-other-view");
        s = sec(m);
        if (items.size() == 1 && allDirs && st.ctxAddToPlaces) item(s, _("Add to Places"), "win.add-to-places", "list-add");
        if (st.ctxOpenTerminal) item(s, _("Open Terminal Here"), "win.open-terminal-here", "utilities-terminal");
        if (items.size() == 1 && allDirs) item(s, _("Disk Usage"), "win.disk-usage", "drive-harddisk");
        if (items.size() == 1 && item_data(items[0]).isLink) item(s, _("Show Target"), "win.show-target", "go-jump");
        add_custom_actions(m, items);
        item(sec(m), _("Properties"), "win.properties", "document-properties");
    }
    popup_menu(v->itemView()->widget(), G_MENU_MODEL(m), x, y);
    g_object_unref(m);
}

} // namespace beluga
