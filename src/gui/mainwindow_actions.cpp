// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: window actions ("win.*"), their state, and the helpers behind them.
#include "mainwindow.hpp"

#include "app.hpp"
#include "customactions.hpp"
#include "dialogs.hpp"
#include "diskusagewindow.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "openwith.hpp"
#include "places.hpp"
#include "preferences.hpp"
#include "settings.hpp"
#include "tabpage.hpp"
#include "trash.hpp"
#include "undo.hpp"

#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <memory>

namespace beluga {

namespace {

using Handler = void (*)(MainWindow *, GVariant *);

struct ActionDef {
    const char *name;
    const char *param;   // GVariant type string or nullptr
    const char *state;   // GVariant text or nullptr
    Handler fn;
};

struct Binding {
    MainWindow *win;
    Handler fn;
};

ViewContainer *AC(MainWindow *w) { return w->activeContainer(); }
View *AV(MainWindow *w) { return w->activeView(); }

// The local folder Disk Usage looks at: the one selected folder, else the folder shown.
std::string disk_usage_target(MainWindow *w)
{
    auto items = w->selectedItems();
    if (items.size() == 1 && item_data(items[0]).isDir) return take(g_file_get_path(item_data(items[0]).file.get()));
    View *v = AV(w);
    if (!v || v->isVirtual()) return {};
    return take(g_file_get_path(v->locationFile().get()));
}

const char *mode_name(int m) { return m == ModeCompact ? "compact" : m == ModeDetails ? "details" : m == ModeColumns ? "columns" : "icons"; }

void toggle_setting(MainWindow *, bool &value)
{
    value = !value;
    settings().save();
    notify_settings_changed();
}

void reopen_closed(MainWindow *w, int i)
{
    if (i < 0 || i >= (int)w->closedTabs_.size()) return;
    MainWindow::ClosedTab t = w->closedTabs_[i];
    w->closedTabs_.erase(w->closedTabs_.begin() + i);
    w->openTab(t.primary, t.secondary, true);
}

const std::vector<ActionDef> &action_defs()
{
    static const std::vector<ActionDef> defs = {
        {"nothing", nullptr, nullptr, [](MainWindow *, GVariant *) {}},
        // File
        {"new-window", nullptr, nullptr, [](MainWindow *, GVariant *) { App::instance().newWindow({settings().homeUrl}, false); }},
        {"new-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->openTab(AC(w) ? AC(w)->uri() : settings().homeUrl, {}, true); }},
        {"close-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->closeTab(w->tabIndex(w->activeTab())); }},
        {"undo-close-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) { reopen_closed(w, (int)w->closedTabs_.size() - 1); }},
        {"reopen-closed-tab", "i", nullptr, [](MainWindow *w, GVariant *p) { reopen_closed(w, g_variant_get_int32(p)); }},
        {"quit", nullptr, nullptr, [](MainWindow *, GVariant *) { App::instance().quit(); }},
        {"create-folder", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->newFolder(); }},
        {"create-file", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->newFile({}, _("New Text File.txt")); }},
        {"create-empty-file", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->newFile({}, _("New File")); }},
        {"new-from-template", "s", nullptr, [](MainWindow *w, GVariant *p) {
             std::string path = g_variant_get_string(p, nullptr);
             w->newFile(path, basename_of(path));
         }},
        {"create-link", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->newLink(); }},
        {"rename", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->renameSelected(); }},
        {"move-to-trash", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->trashOrDelete(false); }},
        {"delete", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->trashOrDelete(true); }},
        {"duplicate", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             std::vector<OpItem> items;
             for (BelugaItem *it : w->selectedItems()) {
                 const ItemData &d = item_data(it);
                 Ref<GFile> parent(g_file_get_parent(d.file.get()));
                 if (!parent) continue;
                 char *base = g_file_get_basename(d.file.get());
                 items.push_back({d.file, Ref<GFile>(g_file_get_child(parent.get(), duplicate_name(parent.get(), take(base), d.isDir).c_str()))});
             }
             if (items.empty()) return;
             auto job = FileJob::copyPairs(items);
             job->onFinished = [w, alive = w->lifeToken()](FileJob &j) {
                 if (alive.expired()) return;
                 std::vector<std::string> uris;
                 for (auto &i : j.done()) uris.push_back(file_uri(i.dest.get()));
                 if (AV(w)) AV(w)->selectUris(uris);
             };
             job->start();
         }},
        {"properties", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto files = w->selectedFiles();
             if (files.empty() && AV(w) && !AV(w)->isVirtual()) files.push_back(AV(w)->locationFile());
             w->showProperties(files);
         }},
        {"folder-properties", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (AV(w) && !AV(w)->isVirtual()) w->showProperties({AV(w)->locationFile()});
         }},
        {"add-to-places", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.size() == 1 && item_data(items[0]).isDir) w->addToPlaces(item_data(items[0]).uri);
             else if (AC(w)) w->addToPlaces(AC(w)->uri());
         }},
        {"show-target", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.size() != 1 || !item_data(items[0]).isLink) return;
             const char *t = g_file_info_get_symlink_target(item_data(items[0]).info.get());
             if (!t) return;
             Ref<GFile> parent(g_file_get_parent(item_data(items[0]).file.get()));
             Ref<GFile> target(g_file_resolve_relative_path(parent.get(), t));
             Ref<GFile> tparent(g_file_get_parent(target.get()));
             if (!tparent || !AC(w)) return;
             AC(w)->navigate(file_uri(tparent.get()));
             AV(w)->selectAndShow(file_uri(target.get()));
         }},
        {"restore", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             std::vector<trash::Entry> entries;
             for (BelugaItem *it : w->selectedItems()) {
                 trash::Entry e;
                 char *path = g_file_get_path(item_data(it).file.get());
                 if (path && trash::entry_for_trashed_path(path, e)) entries.push_back(e);
                 g_free(path);
             }
             if (!entries.empty()) FileJob::restore(entries)->start();
         }},
        {"empty-trash", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto run = [] { FileJob::emptyTrash()->start(); };
             if (!settings().confirmEmptyTrash) {
                 run();
                 return;
             }
             ask(w->widget(), _("Empty the trash?"), _("All items in the trash will be deleted permanently. This cannot be undone."),
                 {_("Cancel"), _("Empty Trash")}, 1, 0, [run](int r) { if (r == 1) run(); });
         }},
        // Edit
        {"undo", nullptr, nullptr, [](MainWindow *, GVariant *) { UndoManager::instance().undo(); }},
        {"redo", nullptr, nullptr, [](MainWindow *, GVariant *) { UndoManager::instance().redo(); }},
        {"cut", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->copySelection(true); }},
        {"copy", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->copySelection(false); }},
        {"copy-location", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             std::vector<std::string> paths;
             for (auto &f : w->selectedFiles()) paths.push_back(display_location(f.get()));
             if (paths.empty() && AC(w)) paths.push_back(display_location(AC(w)->uri()));
             gdk_clipboard_set_text(gtk_widget_get_clipboard(w->widget()), join(paths, "\n").c_str());
         }},
        {"paste", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (AV(w) && !AV(w)->isVirtual()) w->paste(AV(w)->locationFile().get());
         }},
        {"paste-into", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.size() == 1 && item_data(items[0]).isDir) w->paste(item_data(items[0]).file.get());
         }},
        {"filter", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (!AC(w)) return;
             if (AC(w)->filterBarVisible()) AC(w)->filterBar()->focus();
             else AC(w)->setFilterBarVisible(true);
         }},
        {"toggle-filter", nullptr, "false", [](MainWindow *w, GVariant *) {
             if (AC(w)) AC(w)->setFilterBarVisible(!AC(w)->filterBarVisible());
         }},
        {"find", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (!AC(w)) return;
             if (AC(w)->searchBarVisible()) AC(w)->searchBar()->focus();
             else AC(w)->setSearchBarVisible(true);
         }},
        {"toggle-search", nullptr, "false", [](MainWindow *w, GVariant *) {
             if (AC(w)) AC(w)->setSearchBarVisible(!AC(w)->searchBarVisible());
         }},
        {"selection-mode", nullptr, "false", [](MainWindow *w, GVariant *) {
             if (AC(w)) AC(w)->setSelectionMode(!AC(w)->selectionMode());
         }},
        {"select-all", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->selectAll(); }},
        {"invert-selection", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->invertSelection(); }},
        {"copy-to-other-view", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             TabPage *t = w->activeTab();
             if (t && t->split() && !t->other(t->active())->view()->isVirtual())
                 w->dropFiles(t->other(t->active())->view()->locationFile().get(), w->selectedFiles(), GDK_ACTION_COPY);
         }},
        {"move-to-other-view", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             TabPage *t = w->activeTab();
             if (t && t->split() && !t->other(t->active())->view()->isVirtual())
                 w->dropFiles(t->other(t->active())->view()->locationFile().get(), w->selectedFiles(), GDK_ACTION_MOVE);
         }},
        {"copy-to", "s", nullptr, [](MainWindow *w, GVariant *p) { w->copyOrMoveTo(g_variant_get_string(p, nullptr), false); }},
        {"move-to", "s", nullptr, [](MainWindow *w, GVariant *p) { w->copyOrMoveTo(g_variant_get_string(p, nullptr), true); }},
        // View
        {"view-mode", "s", "'icons'", [](MainWindow *w, GVariant *p) {
             std::string m = g_variant_get_string(p, nullptr);
             if (AV(w)) AV(w)->setViewMode(m == "compact" ? ModeCompact : m == "details" ? ModeDetails : m == "columns" ? ModeColumns : ModeIcons);
         }},
        {"zoom-in", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->zoomIn(); }},
        {"zoom-out", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->zoomOut(); }},
        {"zoom-reset", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->resetZoom(); }},
        {"sort-by", "s", "'text'", [](MainWindow *w, GVariant *p) { if (AV(w)) AV(w)->setSortRole(g_variant_get_string(p, nullptr)); }},
        {"sort-descending", nullptr, "false", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setSortDescending(!AV(w)->props().sortDescending); }},
        {"folders-first", nullptr, "true", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setFoldersFirst(!AV(w)->props().sortFoldersFirst); }},
        {"hidden-last", nullptr, "false", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setHiddenLast(!AV(w)->props().sortHiddenLast); }},
        {"show-groups", nullptr, "false", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setGrouped(!AV(w)->props().groupedSorting); }},
        {"show-previews", nullptr, "true", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setPreviewsShown(!AV(w)->props().previewsShown); }},
        {"show-hidden", nullptr, "false", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setHiddenShown(!AV(w)->props().hiddenFilesShown); }},
        {"disk-usage", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             std::string p = disk_usage_target(w);
             if (!p.empty()) show_disk_usage(w, p);
         }},
        {"remember-view", nullptr, "false", [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->setOwnProps(!AV(w)->hasOwnProps()); }},
        {"view-properties", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             View *v = AV(w);
             if (!v) return;
             std::string uri = v->location();
             view_properties_dialog(w->widget(), uri, v->props(), [w, uri](const ViewProps &p, int scope) {
                 if (is_virtual_uri(uri)) {
                     save_own_view_props(uri, p);   // the trash, search and recent views keep their own
                 } else if (scope == 2) {
                     // All folders: the new default, and no folder keeps its own settings.
                     reset_all_view_props();
                     save_default_view_props(p);
                 } else if (scope == 0) {
                     save_own_view_props(uri, p);
                 } else {
                     // Store the properties for every subfolder too.
                     std::vector<Ref<GFile>> stack{Ref<GFile>(g_file_new_for_uri(uri.c_str()))};
                     std::vector<std::string> folders;
                     while (!stack.empty() && folders.size() < 5000) {
                         Ref<GFile> d = stack.back();
                         stack.pop_back();
                         folders.push_back(file_uri(d.get()));
                         GFileEnumerator *en = g_file_enumerate_children(d.get(), "standard::name,standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
                         if (!en) continue;
                         GFileInfo *info;
                         while ((info = g_file_enumerator_next_file(en, nullptr, nullptr))) {
                             if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
                                 stack.push_back(Ref<GFile>(g_file_get_child(d.get(), g_file_info_get_name(info))));
                             g_object_unref(info);
                         }
                         g_object_unref(en);
                     }
                     save_own_view_props(folders, p);
                 }
                 if (AV(w) && AV(w)->location() == uri) AV(w)->reloadProps();
             });
         }},
        {"reset-view-props", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->restoreDefaultProps(); }},
        {"split", nullptr, "false", [](MainWindow *w, GVariant *) {
             if (TabPage *t = w->activeTab()) {
                 t->setSplit(!t->split());
                 w->updateActions();
                 w->updateTitle();
             }
         }},
        {"popout-split", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             TabPage *t = w->activeTab();
             if (!t || !t->split()) return;
             std::string uri = t->active()->uri();
             t->closeView(t->active());
             w->openInNewWindow(uri);
         }},
        {"split-to-tabs", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             TabPage *t = w->activeTab();
             if (!t || !t->split()) return;
             std::string uri = t->secondary()->uri();
             t->closeView(t->secondary());
             w->openTab(uri, {}, false);
         }},
        {"focus-other-view", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             TabPage *t = w->activeTab();
             if (t && t->split()) {
                 ViewContainer *o = t->other(t->active());
                 t->setActive(o);
                 o->view()->focus();
                 w->updateActions();
                 w->updateTitle();
                 w->syncPanels();
             }
         }},
        {"reload", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AC(w)) AC(w)->reload(); }},
        {"stop", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AV(w)) AV(w)->stop(); }},
        {"editable-location", nullptr, "false", [](MainWindow *w, GVariant *) { toggle_setting(w, settings().editableUrl); }},
        {"replace-location", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AC(w)) AC(w)->locationBar()->startEditing(true); }},
        {"show-places", nullptr, "true", [](MainWindow *w, GVariant *) { toggle_setting(w, settings().placesPanel); }},
        {"show-folders", nullptr, "false", [](MainWindow *w, GVariant *) { toggle_setting(w, settings().foldersPanel); }},
        {"show-info", nullptr, "false", [](MainWindow *w, GVariant *) { toggle_setting(w, settings().infoPanel); }},
        {"focus-places", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (!settings().placesPanel) toggle_setting(w, settings().placesPanel);
             w->focusPlaces();
         }},
        {"show-menubar", nullptr, "false", [](MainWindow *w, GVariant *) { toggle_setting(w, settings().showMenuBar); }},
        {"show-toolbar", nullptr, "true", [](MainWindow *w, GVariant *) { toggle_setting(w, settings().showToolBar); }},
        // Go
        {"back", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AC(w)) AC(w)->goBack(); }},
        {"forward", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AC(w)) AC(w)->goForward(); }},
        {"up", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AC(w)) AC(w)->goUp(); }},
        {"home", nullptr, nullptr, [](MainWindow *w, GVariant *) { if (AC(w)) AC(w)->goHome(); }},
        {"go-history", "i", nullptr, [](MainWindow *w, GVariant *p) { if (AC(w)) AC(w)->goHistory(g_variant_get_int32(p)); }},
        // Tabs
        {"activate-tab", "i", nullptr, [](MainWindow *w, GVariant *p) { w->activateTab(g_variant_get_int32(p)); }},
        {"last-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) { w->activateTab(w->tabCount() - 1); }},
        {"next-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (w->tabCount() > 1) w->activateTab((w->tabIndex(w->activeTab()) + 1) % w->tabCount());
         }},
        {"prev-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (w->tabCount() > 1) w->activateTab((w->tabIndex(w->activeTab()) + w->tabCount() - 1) % w->tabCount());
         }},
        {"tab-close", "i", nullptr, [](MainWindow *w, GVariant *p) { w->closeTab(g_variant_get_int32(p)); }},
        {"tab-close-others", "i", nullptr, [](MainWindow *w, GVariant *p) {
             TabPage *keep = w->tab(g_variant_get_int32(p));
             for (int i = w->tabCount() - 1; i >= 0; --i)
                 if (w->tab(i) != keep) w->closeTab(i);
         }},
        {"tab-detach", "i", nullptr, [](MainWindow *w, GVariant *p) { w->detachTab(g_variant_get_int32(p)); }},
        {"tab-duplicate", "i", nullptr, [](MainWindow *w, GVariant *p) {
             if (TabPage *t = w->tab(g_variant_get_int32(p)))
                 w->openTab(t->primary()->uri(), t->secondary() ? t->secondary()->uri() : std::string(), true);
         }},
        // Tools
        {"open-terminal", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             View *v = AV(w);
             std::string path;
             if (v && !v->isVirtual()) path = display_location(v->locationFile().get());
             if (!path.empty() && path[0] != '/') path.clear();
             w->openTerminal(path);
         }},
        {"open-terminal-here", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             std::vector<std::string> dirs;
             for (BelugaItem *it : w->selectedItems()) {
                 const ItemData &d = item_data(it);
                 Ref<GFile> f = d.isDir ? d.file : Ref<GFile>(g_file_get_parent(d.file.get()));
                 char *p = f ? g_file_get_path(f.get()) : nullptr;
                 if (p && std::find(dirs.begin(), dirs.end(), p) == dirs.end()) dirs.push_back(p);
                 g_free(p);
             }
             if (dirs.empty()) {
                 g_action_group_activate_action(G_ACTION_GROUP(w->widget()), "open-terminal", nullptr);
                 return;
             }
             auto run = [w, dirs] { for (auto &d : dirs) w->openTerminal(d); };
             if (dirs.size() > 3 && settings().confirmOpenManyTerminals)
                 ask(w->widget(), printf_str(_("Open %zu terminals?"), dirs.size()), {}, {_("Cancel"), _("Open Terminals")}, 1, 0,
                     [run](int r) { if (r == 1) run(); });
             else run();
         }},
        // Open
        {"open-in-new-tab", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.empty() && AC(w)) w->openInNewTab(AC(w)->uri(), true);
             for (BelugaItem *it : items)
                 if (item_data(it).isDir) w->openInNewTab(item_data(it).uri, false);
         }},
        {"open-in-new-window", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.empty() && AC(w)) w->openInNewWindow(AC(w)->uri());
             for (BelugaItem *it : items)
                 if (item_data(it).isDir) w->openInNewWindow(item_data(it).uri);
         }},
        {"open-in-split-view", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             TabPage *t = w->activeTab();
             if (!t) return;
             std::string uri = items.size() == 1 && item_data(items[0]).isDir ? item_data(items[0]).uri : t->active()->uri();
             if (t->split()) t->other(t->active())->navigate(uri);
             else t->setSplit(true, uri);
             w->updateActions();
         }},
        {"open-selected", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             if (AC(w)) w->openItems(AC(w), w->selectedItems(), GdkModifierType(0));
         }},
        {"open-with", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.empty()) return;
             std::vector<Ref<GFile>> files;
             for (BelugaItem *it : items) files.push_back(item_data(it).file);
             show_open_with_dialog(w->widget(), files, item_data(items[0]).mime);
         }},
        {"open-with-app", "s", nullptr, [](MainWindow *w, GVariant *p) {
             GDesktopAppInfo *app = g_desktop_app_info_new(g_variant_get_string(p, nullptr));
             if (!app) return;
             launch_with(w->widget(), G_APP_INFO(app), w->selectedFiles());
             g_object_unref(app);
         }},
        {"open-containing-folder", nullptr, nullptr, [](MainWindow *w, GVariant *) {
             auto items = w->selectedItems();
             if (items.empty() || !AC(w)) return;
             Ref<GFile> parent(g_file_get_parent(item_data(items[0]).file.get()));
             std::string target = item_data(items[0]).uri;
             if (!item_data(items[0]).originalPath.empty()) return;
             if (!parent) return;
             AC(w)->navigate(file_uri(parent.get()));
             AV(w)->selectAndShow(target);
         }},
        {"custom-action", "i", nullptr, [](MainWindow *w, GVariant *p) { w->runCustomAction(g_variant_get_int32(p)); }},
        {"preferences", nullptr, nullptr, [](MainWindow *w, GVariant *) { show_preferences(w->widget()); }},
    };
    return defs;
}

GSimpleAction *find(MainWindow *w, const char *name)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w->widget()), name);
    return a ? G_SIMPLE_ACTION(a) : nullptr;
}

void enable(MainWindow *w, const char *name, bool on)
{
    if (GSimpleAction *a = find(w, name)) g_simple_action_set_enabled(a, on);
}

void state(MainWindow *w, const char *name, GVariant *v)
{
    GSimpleAction *a = find(w, name);
    if (a) g_simple_action_set_state(a, v);
    else g_variant_unref(g_variant_ref_sink(v));
}

} // namespace

void MainWindow::setupActions()
{
    for (const ActionDef &d : action_defs()) {
        const GVariantType *pt = d.param ? G_VARIANT_TYPE(d.param) : nullptr;
        GSimpleAction *a;
        if (d.state) {
            GVariant *state = g_variant_parse(nullptr, d.state, nullptr, nullptr, nullptr); // not floating
            a = g_simple_action_new_stateful(d.name, pt, state);
            g_variant_unref(state);
        } else {
            a = g_simple_action_new(d.name, pt);
        }
        g_signal_connect_data(a, "activate", CB([](GSimpleAction *, GVariant *p, gpointer data) {
            auto *b = static_cast<Binding *>(data);
            b->fn(b->win, p);
            b->win->updateActions();
        }), new Binding{this, d.fn}, [](gpointer data, GClosure *) { delete static_cast<Binding *>(data); }, GConnectFlags(0));
        g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(a));
        g_object_unref(a);
    }
    for (auto &r : all_roles()) {
        if (std::string(r.id) == "text") continue;
        std::string name = std::string("role-") + r.id;
        GSimpleAction *a = g_simple_action_new_stateful(name.c_str(), nullptr, g_variant_new_boolean(FALSE));
        g_object_set_data_full(G_OBJECT(a), "beluga-role", g_strdup(r.id), g_free);
        g_signal_connect(a, "activate", CB([](GSimpleAction *a, GVariant *, gpointer p) {
            auto *w = static_cast<MainWindow *>(p);
            if (w->activeView()) w->activeView()->toggleRole(static_cast<const char *>(g_object_get_data(G_OBJECT(a), "beluga-role")));
            w->updateActions();
        }), this);
        g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(a));
        g_object_unref(a);
    }
    enable(this, "nothing", false);
}

void MainWindow::updateActions()
{
    if (updatingActions_) return;
    updatingActions_ = true;
    ViewContainer *c = activeContainer();
    View *v = c ? c->view() : nullptr;
    TabPage *t = activeTab();
    auto items = v ? v->selectedItems() : std::vector<BelugaItem *>();
    bool sel = !items.empty();
    bool trashView = v && v->isTrash();
    bool virt = v && v->isVirtual();
    bool writable = v && !virt;
    // Only for local folders: this runs on every selection change, and for sftp:// or smb:// it
    // would be a network round trip each time. Elsewhere the operation reports the error.
    if (writable && g_file_is_native(v->locationFile().get())) {
        GFileInfo *info = g_file_query_info(v->locationFile().get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
        if (info) {
            if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
                writable = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
            g_object_unref(info);
        }
    }
    bool singleDir = items.size() == 1 && item_data(items[0]).isDir;
    bool split = t && t->split();

    enable(this, "back", c && c->canGoBack());
    enable(this, "forward", c && c->canGoForward());
    enable(this, "up", c && c->canGoUp());
    enable(this, "undo-close-tab", !closedTabs_.empty());
    enable(this, "create-folder", writable);
    enable(this, "create-file", writable);
    enable(this, "create-empty-file", writable);
    enable(this, "new-from-template", writable);
    enable(this, "create-link", writable);
    enable(this, "rename", sel && !trashView);
    enable(this, "move-to-trash", sel && !trashView);
    enable(this, "delete", sel);
    enable(this, "duplicate", sel && !virt);
    enable(this, "restore", sel && trashView);
    enable(this, "empty-trash", trashView || !trash::is_empty());
    enable(this, "cut", sel && !trashView);
    enable(this, "copy", sel);
    enable(this, "paste", writable && App::instance().canPaste());
    enable(this, "paste-into", singleDir && App::instance().canPaste() && !trashView);
    enable(this, "add-to-places", c != nullptr);
    enable(this, "show-target", items.size() == 1 && item_data(items[0]).isLink);
    enable(this, "undo", UndoManager::instance().canUndo());
    enable(this, "redo", UndoManager::instance().canRedo());
    enable(this, "copy-to-other-view", sel && split);
    enable(this, "move-to-other-view", sel && split && !trashView);
    enable(this, "popout-split", split);
    enable(this, "split-to-tabs", split);
    enable(this, "focus-other-view", split);
    enable(this, "stop", v && v->loading());
    enable(this, "open-terminal-here", sel || !virt);
    enable(this, "open-with", sel);
    enable(this, "open-containing-folder", sel && virt && !trashView);
    enable(this, "folder-properties", !virt);
    enable(this, "remember-view", v && !virt);
    enable(this, "disk-usage", !disk_usage_target(this).empty());
    enable(this, "properties", sel || !virt);

    if (v) {
        const ViewProps &p = v->props();
        state(this, "view-mode", g_variant_new_string(mode_name(p.viewMode)));
        state(this, "sort-by", g_variant_new_string(p.sortRole.c_str()));
        state(this, "sort-descending", g_variant_new_boolean(p.sortDescending));
        state(this, "folders-first", g_variant_new_boolean(p.sortFoldersFirst));
        state(this, "hidden-last", g_variant_new_boolean(p.sortHiddenLast));
        state(this, "show-groups", g_variant_new_boolean(p.groupedSorting));
        state(this, "show-previews", g_variant_new_boolean(p.previewsShown));
        state(this, "show-hidden", g_variant_new_boolean(p.hiddenFilesShown));
        state(this, "remember-view", g_variant_new_boolean(v->hasOwnProps()));
        auto roles = v->shownRoles();
        for (auto &r : all_roles()) {
            if (std::string(r.id) == "text") continue;
            std::string name = std::string("role-") + r.id;
            state(this, name.c_str(), g_variant_new_boolean(std::find(roles.begin(), roles.end(), r.id) != roles.end()));
        }
        enable(this, "zoom-in", v->zoomLevel() < kMaxZoom);
        enable(this, "zoom-out", v->zoomLevel() > kMinZoom);
    }
    if (c) {
        state(this, "toggle-filter", g_variant_new_boolean(c->filterBarVisible()));
        state(this, "toggle-search", g_variant_new_boolean(c->searchBarVisible()));
        state(this, "selection-mode", g_variant_new_boolean(c->selectionMode()));
    }
    const Settings &s = settings();
    state(this, "split", g_variant_new_boolean(split));
    state(this, "editable-location", g_variant_new_boolean(s.editableUrl));
    state(this, "show-places", g_variant_new_boolean(s.placesPanel));
    state(this, "show-folders", g_variant_new_boolean(s.foldersPanel));
    state(this, "show-info", g_variant_new_boolean(s.infoPanel));
    state(this, "show-menubar", g_variant_new_boolean(s.showMenuBar));
    state(this, "show-toolbar", g_variant_new_boolean(s.showToolBar));
    updatingActions_ = false;
}

// ---- helpers ---------------------------------------------------------------------------------

std::vector<BelugaItem *> MainWindow::selectedItems() const
{
    View *v = activeView();
    return v ? v->selectedItems() : std::vector<BelugaItem *>();
}

std::vector<Ref<GFile>> MainWindow::selectedFiles() const
{
    View *v = activeView();
    return v ? v->selectedFiles() : std::vector<Ref<GFile>>();
}

void MainWindow::newFolder()
{
    View *v = activeView();
    if (!v || v->isVirtual()) return;
    Ref<GFile> dir = v->locationFile();
    std::string initial = unique_name(dir.get(), _("New Folder"));
    ask_name(win_, _("New Folder"), printf_str(_("Create new folder in:\n%s"), display_location(dir.get()).c_str()), initial, -1,
             _("C_reate"), [this, dir](const std::string &name) {
        Ref<GFile> created;
        GError *err = nullptr;
        if (!make_directory(dir.get(), trim(name), created, &err)) {
            show_error(win_, _("Could not create the folder."), err->message);
            g_error_free(err);
            return;
        }
        UndoCommand c;
        c.type = UndoCommand::Mkdir;
        c.pairs.emplace_back(std::string(), file_uri(created.get()));
        UndoManager::instance().push(c);
        if (View *av = activeView()) av->selectUris({file_uri(created.get())});
    });
}

void MainWindow::newFile(const std::string &templatePath, const std::string &defaultName)
{
    View *v = activeView();
    if (!v || v->isVirtual()) return;
    Ref<GFile> dir = v->locationFile();
    std::string initial = unique_name(dir.get(), defaultName);
    int stem = (int)g_utf8_strlen(split_extension(initial).first.c_str(), -1);
    ask_name(win_, _("New File"), printf_str(_("Create new file in:\n%s"), display_location(dir.get()).c_str()), initial, stem,
             _("C_reate"), [this, dir, templatePath](const std::string &name) {
        Ref<GFile> created;
        GError *err = nullptr;
        bool ok;
        if (templatePath.empty()) {
            ok = create_empty_file(dir.get(), trim(name), created, &err);
        } else {
            Ref<GFile> src(g_file_new_for_path(templatePath.c_str()));
            created = Ref<GFile>(g_file_get_child(dir.get(), trim(name).c_str()));
            ok = g_file_copy(src.get(), created.get(), G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &err);
        }
        if (!ok) {
            show_error(win_, _("Could not create the file."), err->message);
            g_error_free(err);
            return;
        }
        UndoCommand c;
        c.type = UndoCommand::CreateFile;
        c.pairs.emplace_back(std::string(), file_uri(created.get()));
        UndoManager::instance().push(c);
        if (View *av = activeView()) av->selectUris({file_uri(created.get())});
    });
}

void MainWindow::newLink()
{
    View *v = activeView();
    if (!v || v->isVirtual()) return;
    Ref<GFile> dir = v->locationFile();
    ask_name(win_, _("Link to File or Folder"), _("Path of the file or folder to link to:"), home_dir() + "/", -1, _("C_reate"),
             [this, dir](const std::string &target) {
        Ref<GFile> t(file_for_input(target, dir.get()));
        if (!t) return;
        std::string base = take(g_file_get_basename(t.get()));
        Ref<GFile> link(g_file_get_child(dir.get(), unique_name(dir.get(), base).c_str()));
        GError *err = nullptr;
        if (!g_file_make_symbolic_link(link.get(), display_location(t.get()).c_str(), nullptr, &err)) {
            show_error(win_, _("Could not create the link."), err->message);
            g_error_free(err);
            return;
        }
        UndoCommand c;
        c.type = UndoCommand::Link;
        c.pairs.emplace_back(file_uri(t.get()), file_uri(link.get()));
        UndoManager::instance().push(c);
        if (View *av = activeView()) av->selectUris({file_uri(link.get())});
    });
}

void MainWindow::trashOrDelete(bool del)
{
    View *v = activeView();
    if (!v) return;
    auto files = v->selectedFiles();
    if (files.empty()) return;
    if (v->isTrash()) del = true;
    std::string what = files.size() == 1 ? "“" + take(g_file_get_basename(files[0].get())) + "”" : format_items((int)files.size());
    if (v->isTrash() && files.size() == 1) {
        auto items = v->selectedItems();
        if (!items.empty()) what = "“" + item_data(items[0]).name + "”";
    }
    auto run = [files, del] { (del ? FileJob::remove(files) : FileJob::trash(files))->start(); };
    if (del && settings().confirmDelete)
        ask(win_, printf_str(_("Permanently delete %s?"), what.c_str()), _("This cannot be undone."),
            {_("Cancel"), _("Delete")}, 1, 0, [run](int r) { if (r == 1) run(); });
    else if (!del && settings().confirmTrash)
        ask(win_, printf_str(_("Move %s to the trash?"), what.c_str()), {}, {_("Cancel"), _("Move to Trash")}, 1, 0,
            [run](int r) { if (r == 1) run(); });
    else
        run();
}

void MainWindow::copySelection(bool cut)
{
    auto files = selectedFiles();
    if (files.empty()) return;
    App::instance().setClipboard(files, cut);
}

void MainWindow::paste(GFile *destDir)
{
    if (!destDir) return;
    Ref<GFile> dest = Ref<GFile>::share(destDir);
    App::instance().readClipboard([this, alive = lifeToken(), dest](std::vector<Ref<GFile>> files, bool cut) {
        if (files.empty() || alive.expired()) return;
        auto job = cut ? FileJob::move(files, dest.get()) : FileJob::copy(files, dest.get());
        std::string destUri = file_uri(dest.get());
        job->onFinished = [this, alive, destUri, cut](FileJob &j) {
            if (cut) App::instance().clearClipboardAfterMove();
            if (alive.expired()) return;
            View *v = activeView();
            if (!v || v->location() != destUri) return;
            std::vector<std::string> uris;
            for (auto &i : j.done()) uris.push_back(file_uri(i.dest.get()));
            v->selectUris(uris);
        };
        job->start();
    });
}

void MainWindow::copyOrMoveTo(const std::string &destUri, bool move)
{
    auto files = selectedFiles();
    if (files.empty()) return;
    if (destUri == "browse") {
        GtkFileDialog *fd = gtk_file_dialog_new();
        gtk_file_dialog_set_title(fd, move ? _("Move To") : _("Copy To"));
        struct D {
            MainWindow *w;
            std::weak_ptr<void> alive;
            std::vector<Ref<GFile>> files;
            bool move;
        };
        gtk_file_dialog_select_folder(fd, window(), nullptr, [](GObject *src, GAsyncResult *res, gpointer p) {
            std::unique_ptr<D> d(static_cast<D *>(p));
            GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
            if (!f) return;
            if (!d->alive.expired()) d->w->dropFiles(f, d->files, d->move ? GDK_ACTION_MOVE : GDK_ACTION_COPY);
            g_object_unref(f);
        }, new D{this, lifeToken(), files, move});
        g_object_unref(fd);
        return;
    }
    Ref<GFile> dest(g_file_new_for_uri(destUri.c_str()));
    dropFiles(dest.get(), files, move ? GDK_ACTION_MOVE : GDK_ACTION_COPY);
}

void MainWindow::addToPlaces(const std::string &uri)
{
    if (Places::instance().indexOf(uri) >= 0) return;
    Place p;
    p.uri = uri;
    p.icon = Places::iconForUri(uri);
    if (is_search_uri(uri)) {
        SearchQuery q;
        SearchQuery::fromUri(uri, q);
        p.label = q.title();
    } else {
        Ref<GFile> f(g_file_new_for_uri(uri.c_str()));
        p.label = take(g_file_get_basename(f.get()));
        if (p.label.empty() || p.label == "/") p.label = display_location(uri);
    }
    Places::instance().add(p);
}

void MainWindow::runCustomAction(int index)
{
    auto actions = load_custom_actions();
    if (index < 0 || index >= (int)actions.size()) return;
    std::vector<ActionTarget> targets;
    for (BelugaItem *it : selectedItems()) {
        const ItemData &d = item_data(it);
        targets.push_back({take(g_file_get_path(d.file.get())), d.uri, d.name, d.mime, d.isDir});
    }
    View *v = activeView();
    std::string folder = v && !v->isVirtual() ? display_location(v->locationFile().get()) : home_dir();
    if (folder.empty() || folder[0] != '/') folder = home_dir();
    GError *err = nullptr;
    if (!run_custom_action(actions[index], targets, folder, &err)) {
        show_error(win_, printf_str(_("Could not run “%s”."), actions[index].name.c_str()), err ? err->message : "");
        g_clear_error(&err);
    }
}

} // namespace beluga
