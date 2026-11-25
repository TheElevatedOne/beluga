// SPDX-License-Identifier: GPL-2.0-or-later
#include "openwith.hpp"

#include "dialogs.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"

#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <map>

namespace beluga {

std::vector<Ref<GAppInfo>> apps_for_type(const std::string &mime)
{
    std::vector<Ref<GAppInfo>> out;
    auto addApp = [&](GAppInfo *a) {
        if (!a) return;
        for (auto &x : out)
            if (g_app_info_equal(x.get(), a)) return;
        out.push_back(Ref<GAppInfo>::share(a));
    };
    GAppInfo *def = g_app_info_get_default_for_type(mime.c_str(), FALSE);
    addApp(def);
    if (def) g_object_unref(def);
    GList *rec = g_app_info_get_recommended_for_type(mime.c_str());
    for (GList *l = rec; l; l = l->next) addApp(G_APP_INFO(l->data));
    g_list_free_full(rec, g_object_unref);
    GList *all = g_app_info_get_all_for_type(mime.c_str());
    for (GList *l = all; l; l = l->next) addApp(G_APP_INFO(l->data));
    g_list_free_full(all, g_object_unref);
    return out;
}

static void add_recent(const std::vector<Ref<GFile>> &files)
{
    GtkRecentManager *rm = gtk_recent_manager_get_default();
    for (auto &f : files)
        if (g_file_is_native(f.get())) gtk_recent_manager_add_item(rm, file_uri(f.get()).c_str());
}

bool launch_with(GtkWidget *parent, GAppInfo *app, const std::vector<Ref<GFile>> &files)
{
    if (!app || files.empty()) return false;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(parent ? gtk_widget_get_display(parent) : gdk_display_get_default());
    GError *err = nullptr;
    bool ok;
    if (g_app_info_supports_files(app) || !g_app_info_supports_uris(app)) {
        GList *list = nullptr;
        for (auto it = files.rbegin(); it != files.rend(); ++it) list = g_list_prepend(list, it->get());
        ok = g_app_info_launch(app, list, G_APP_LAUNCH_CONTEXT(ctx), &err);
        g_list_free(list);
    } else {
        GList *list = nullptr;
        std::vector<std::string> uris;
        for (auto &f : files) uris.push_back(file_uri(f.get()));
        for (auto it = uris.rbegin(); it != uris.rend(); ++it) list = g_list_prepend(list, (gpointer)it->c_str());
        ok = g_app_info_launch_uris(app, list, G_APP_LAUNCH_CONTEXT(ctx), &err);
        g_list_free(list);
    }
    g_object_unref(ctx);
    if (!ok) {
        show_error(parent, printf_str(_("Could not start %s."), g_app_info_get_display_name(app)), err ? err->message : "");
        g_clear_error(&err);
        return false;
    }
    add_recent(files);
    return true;
}

static void run_executable(GtkWidget *parent, GFile *f)
{
    std::string path = take(g_file_get_path(f));
    Ref<GFile> dir(g_file_get_parent(f));
    std::string wd = dir ? take(g_file_get_path(dir.get())) : home_dir();
    GError *err = nullptr;
    char *argv[] = {(char *)path.c_str(), nullptr};
    if (!g_spawn_async(wd.c_str(), argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &err)) {
        show_error(parent, printf_str(_("Could not run “%s”."), basename_of(path).c_str()), err->message);
        g_error_free(err);
    }
}

static void open_normally(GtkWidget *parent, const std::vector<OpenTarget> &targets)
{
    std::map<std::string, std::pair<Ref<GAppInfo>, std::vector<Ref<GFile>>>> groups;
    for (auto &t : targets) {
        bool local = g_file_is_native(t.file.get());
        GAppInfo *app = g_app_info_get_default_for_type(t.mime.c_str(), !local);
        if (!app) {
            show_open_with_dialog(parent, {t.file}, t.mime);
            continue;
        }
        std::string id = g_app_info_get_id(app) ? g_app_info_get_id(app) : g_app_info_get_executable(app);
        auto &g = groups[id];
        if (!g.first) g.first = Ref<GAppInfo>(app);
        else g_object_unref(app);
        g.second.push_back(t.file);
    }
    for (auto &[id, g] : groups) launch_with(parent, g.first.get(), g.second);
}

void open_files(GtkWidget *parent, const std::vector<OpenTarget> &targets)
{
    std::vector<OpenTarget> normal;
    Ref<GtkWidget> par = Ref<GtkWidget>::share(parent);
    for (auto &t : targets) {
        std::string path = take(g_file_get_path(t.file.get()));
        if (!path.empty() && t.mime == "application/x-desktop") {
            GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(path.c_str());
            if (app) {
                GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(parent));
                GError *err = nullptr;
                if (!g_app_info_launch(G_APP_INFO(app), nullptr, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
                    show_error(parent, _("Could not start the application."), err ? err->message : "");
                    g_clear_error(&err);
                }
                g_object_unref(ctx);
                g_object_unref(app);
                continue;
            }
        }
        if (!path.empty() && t.executable) {
            ExecAction a = settings().executableAction;
            if (a == ExecAction::Run) {
                run_executable(parent, t.file.get());
                continue;
            }
            if (a == ExecAction::Ask) {
                OpenTarget copy = t;
                std::string name = take(g_file_get_basename(t.file.get()));
                ask(parent, printf_str(_("“%s” is an executable file."), name.c_str()),
                    _("Do you want to run it, or open it in an application?"), {_("Cancel"), _("Open"), _("Run")}, 2, 0,
                    [par, copy](int r) {
                        if (r == 2) run_executable(par.get(), copy.file.get());
                        else if (r == 1) open_normally(par.get(), {copy});
                    });
                continue;
            }
        }
        normal.push_back(t);
    }
    if (!normal.empty()) open_normally(parent, normal);
}

// ---- dialog ---------------------------------------------------------------------------------

void show_open_with_dialog(GtkWidget *parent, const std::vector<Ref<GFile>> &files, const std::string &mime)
{
    if (files.empty()) return;
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), _("Open With"));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    if (GtkWindow *pw = window_of(parent)) gtk_window_set_transient_for(GTK_WINDOW(win), pw);
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 540);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_window_set_child(GTK_WINDOW(win), box);
    std::string what = files.size() == 1 ? "“" + take(g_file_get_basename(files[0].get())) + "”" : format_items((int)files.size());
    std::string desc = take(g_content_type_get_description(mime.c_str()));
    GtkWidget *l = gtk_label_new(printf_str(_("Choose the application to open %s (%s):"), what.c_str(), desc.c_str()).c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), l);
    GtkWidget *search = gtk_search_entry_new();
    gtk_box_append(GTK_BOX(box), search);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "navigation-sidebar");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(box), scroll);

    std::vector<Ref<GAppInfo>> recommended = apps_for_type(mime);
    std::vector<Ref<GAppInfo>> others;
    GList *all = g_app_info_get_all();
    for (GList *a = all; a; a = a->next) {
        GAppInfo *app = G_APP_INFO(a->data);
        if (!g_app_info_should_show(app)) continue;
        bool dup = false;
        for (auto &r : recommended)
            if (g_app_info_equal(r.get(), app)) dup = true;
        if (!dup) others.push_back(Ref<GAppInfo>::share(app));
    }
    g_list_free_full(all, g_object_unref);
    std::sort(others.begin(), others.end(), [](const Ref<GAppInfo> &a, const Ref<GAppInfo> &b) {
        return g_utf8_collate(g_app_info_get_display_name(a.get()), g_app_info_get_display_name(b.get())) < 0;
    });
    auto addRows = [&](const std::vector<Ref<GAppInfo>> &apps, const char *section) {
        bool first = true;
        for (auto &app : apps) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *rb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GIcon *icon = g_app_info_get_icon(app.get());
            GtkWidget *img = icon ? gtk_image_new_from_gicon(icon) : gtk_image_new_from_icon_name("application-x-executable");
            gtk_image_set_pixel_size(GTK_IMAGE(img), 24);
            gtk_box_append(GTK_BOX(rb), img);
            GtkWidget *name = gtk_label_new(g_app_info_get_display_name(app.get()));
            gtk_label_set_xalign(GTK_LABEL(name), 0);
            gtk_box_append(GTK_BOX(rb), name);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rb);
            g_object_set_data_full(G_OBJECT(row), "beluga-app", g_object_ref(app.get()), g_object_unref);
            g_object_set_data_full(G_OBJECT(row), "beluga-name", g_strdup(casefold(g_app_info_get_display_name(app.get())).c_str()), g_free);
            if (first) g_object_set_data_full(G_OBJECT(row), "beluga-section", g_strdup(section), g_free);
            first = false;
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
    };
    addRows(recommended, _("Recommended Applications"));
    addRows(others, _("Other Applications"));
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), [](GtkListBoxRow *row, GtkListBoxRow *, gpointer) {
        auto *s = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-section"));
        if (!s) {
            gtk_list_box_row_set_header(row, nullptr);
            return;
        }
        GtkWidget *h = gtk_label_new(s);
        gtk_widget_add_css_class(h, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(h), 0);
        gtk_list_box_row_set_header(row, h);
    }, nullptr, nullptr);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), [](GtkListBoxRow *row, gpointer entry) -> gboolean {
        std::string q = casefold(gtk_editable_get_text(GTK_EDITABLE(entry)));
        auto *n = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-name"));
        return q.empty() || (n && std::string(n).find(q) != std::string::npos);
    }, search, nullptr);
    g_signal_connect_swapped(search, "search-changed", G_CALLBACK(gtk_list_box_invalidate_filter), list);
    if (GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0)) gtk_list_box_select_row(GTK_LIST_BOX(list), first);

    GtkWidget *cmdRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(cmdRow), gtk_label_new(_("Or command:")));
    GtkWidget *cmd = gtk_entry_new();
    gtk_widget_set_hexpand(cmd, TRUE);
    gtk_box_append(GTK_BOX(cmdRow), cmd);
    gtk_box_append(GTK_BOX(box), cmdRow);
    GtkWidget *remember = gtk_check_button_new_with_label(_("Remember application association for this type of file"));
    gtk_box_append(GTK_BOX(box), remember);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    GtkWidget *open = gtk_button_new_with_mnemonic(_("_Open"));
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), open);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_default_widget(GTK_WINDOW(win), open);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);

    struct State {
        GtkWidget *win, *list, *cmd, *remember;
        std::vector<Ref<GFile>> files;
        std::string mime;
        Ref<GtkWidget> parent;
    };
    auto *st = new State{win, list, cmd, remember, files, mime, Ref<GtkWidget>::share(parent)};
    g_object_set_data_full(G_OBJECT(win), "beluga-state", st, [](gpointer p) { delete static_cast<State *>(p); });
    auto go = CB([](GtkWidget *w) {
        auto *s = static_cast<State *>(g_object_get_data(G_OBJECT(gtk_widget_get_root(w)), "beluga-state"));
        if (!s) return;
        Ref<GAppInfo> app;
        std::string command = trim(gtk_editable_get_text(GTK_EDITABLE(s->cmd)));
        if (!command.empty()) {
            app = Ref<GAppInfo>(g_app_info_create_from_commandline(command.c_str(), nullptr, G_APP_INFO_CREATE_SUPPORTS_URIS, nullptr));
        } else if (GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list))) {
            app = Ref<GAppInfo>::share(G_APP_INFO(g_object_get_data(G_OBJECT(row), "beluga-app")));
        }
        if (!app) return;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(s->remember))) g_app_info_set_as_default_for_type(app.get(), s->mime.c_str(), nullptr);
        auto files = s->files;
        Ref<GtkWidget> parent = s->parent;
        gtk_window_destroy(GTK_WINDOW(s->win));
        launch_with(parent.get(), app.get(), files);
    });
    g_signal_connect(open, "clicked", go, nullptr);
    g_signal_connect_swapped(list, "row-activated", go, open);
    g_signal_connect_swapped(cmd, "activate", go, open);
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
    gtk_widget_grab_focus(search);
}

} // namespace beluga
