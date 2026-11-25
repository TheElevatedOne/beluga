// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include "fileops.hpp"
#include "gtkutil.hpp"
#include "mainwindow.hpp"
#include "properties.hpp"
#include "settings.hpp"
#include "style.hpp"
#include "tabpage.hpp"
#include "trash.hpp"
#include "undo.hpp"

#include <algorithm>
#include <thread>

namespace beluga {

const std::vector<ShortcutInfo> &shortcut_table()
{
    static const std::vector<ShortcutInfo> t = {
        {"win.new-window", N_("New Window"), {"<Control>n"}},
        {"win.new-tab", N_("New Tab"), {"<Control>t"}},
        {"win.close-tab", N_("Close Tab"), {"<Control>w"}},
        {"win.undo-close-tab", N_("Undo Close Tab"), {"<Control><Shift>t"}},
        {"win.quit", N_("Quit"), {"<Control>q"}},
        {"win.create-folder", N_("Create Folder"), {"F10", "<Control><Shift>n"}},
        {"win.rename", N_("Rename"), {"F2"}},
        {"win.move-to-trash", N_("Move to Trash"), {"Delete"}},
        {"win.delete", N_("Delete"), {"<Shift>Delete"}},
        {"win.duplicate", N_("Duplicate Here"), {"<Control>d"}},
        {"win.properties", N_("Properties"), {"<Alt>Return", "<Alt>KP_Enter"}},
        {"win.undo", N_("Undo"), {"<Control>z"}},
        {"win.redo", N_("Redo"), {"<Control><Shift>z", "<Control>y"}},
        {"win.cut", N_("Cut"), {"<Control>x"}},
        {"win.copy", N_("Copy"), {"<Control>c"}},
        {"win.copy-location", N_("Copy Location"), {"<Control><Alt>c"}},
        {"win.paste", N_("Paste"), {"<Control>v"}},
        {"win.filter", N_("Filter"), {"<Control>i", "slash"}},
        {"win.find", N_("Search"), {"<Control>f"}},
        {"win.selection-mode", N_("Select Files and Folders"), {"space"}},
        {"win.select-all", N_("Select All"), {"<Control>a"}},
        {"win.invert-selection", N_("Invert Selection"), {"<Control><Shift>a"}},
        {"win.copy-to-other-view", N_("Copy to Other View"), {"<Shift>F5"}},
        {"win.move-to-other-view", N_("Move to Other View"), {"<Shift>F6"}},
        {"win.view-mode::icons", N_("Icons View"), {"<Control>1"}},
        {"win.view-mode::compact", N_("Compact View"), {"<Control>2"}},
        {"win.view-mode::details", N_("Details View"), {"<Control>3"}},
        {"win.view-mode::columns", N_("Columns View"), {"<Control>4"}},
        {"win.zoom-in", N_("Zoom In"), {"<Control>plus", "<Control>equal", "<Control>KP_Add"}},
        {"win.zoom-out", N_("Zoom Out"), {"<Control>minus", "<Control>KP_Subtract"}},
        {"win.zoom-reset", N_("Reset Zoom"), {"<Control>0", "<Control>KP_0"}},
        {"win.show-hidden", N_("Show Hidden Files"), {"<Control>h", "<Alt>period"}},
        {"win.split", N_("Split View"), {"F3"}},
        {"win.focus-other-view", N_("Focus Other View"), {"<Control>F3"}},
        {"win.popout-split", N_("Pop out Split View"), {"<Shift>F3"}},
        {"win.split-to-tabs", N_("Split View to Tabs"), {"<Control><Shift>F3"}},
        {"win.reload", N_("Reload"), {"F5", "<Control>r"}},
        {"win.editable-location", N_("Editable Location"), {"F6"}},
        {"win.replace-location", N_("Replace Location"), {"<Control>l", "<Alt>d"}},
        {"win.show-places", N_("Places Panel"), {"F9"}},
        {"win.show-folders", N_("Folders Panel"), {"F7"}},
        {"win.show-info", N_("Information Panel"), {"F11"}},
        {"win.focus-places", N_("Focus Places Panel"), {"<Control>p"}},
        {"win.show-menubar", N_("Show Menubar"), {"<Control>m"}},
        {"win.back", N_("Back"), {"<Alt>Left", "BackSpace", "Back"}},
        {"win.forward", N_("Forward"), {"<Alt>Right", "Forward"}},
        {"win.up", N_("Up"), {"<Alt>Up"}},
        {"win.home", N_("Home"), {"<Alt>Home"}},
        {"win.activate-tab(0)", N_("Go to Tab 1"), {"<Alt>1"}},
        {"win.activate-tab(1)", N_("Go to Tab 2"), {"<Alt>2"}},
        {"win.activate-tab(2)", N_("Go to Tab 3"), {"<Alt>3"}},
        {"win.activate-tab(3)", N_("Go to Tab 4"), {"<Alt>4"}},
        {"win.activate-tab(4)", N_("Go to Tab 5"), {"<Alt>5"}},
        {"win.activate-tab(5)", N_("Go to Tab 6"), {"<Alt>6"}},
        {"win.activate-tab(6)", N_("Go to Tab 7"), {"<Alt>7"}},
        {"win.activate-tab(7)", N_("Go to Tab 8"), {"<Alt>8"}},
        {"win.activate-tab(8)", N_("Go to Tab 9"), {"<Alt>9"}},
        {"win.last-tab", N_("Go to Last Tab"), {"<Alt>0"}},
        {"win.next-tab", N_("Next Tab"), {"<Control>Tab", "<Control>Page_Down"}},
        {"win.prev-tab", N_("Previous Tab"), {"<Control><Shift>ISO_Left_Tab", "<Control><Shift>Tab", "<Control>Page_Up"}},
        {"win.open-terminal", N_("Open Terminal"), {"F4", "<Shift>F4"}},
        {"win.open-terminal-here", N_("Open Terminal Here"), {"<Alt><Shift>F4"}},
        {"win.disk-usage", N_("Disk Usage"), {"<Control><Shift>u"}},
        {"win.preferences", N_("Configure Beluga"), {"<Control><Shift>comma"}},
    };
    return t;
}

App &App::instance()
{
    static App a;
    return a;
}

void App::init(GtkApplication *app)
{
    app_ = app;
    gtk_window_set_default_icon_name(BELUGA_APP_ID);   // dialogs and the properties window too
    set_job_ui(&jobUi_);
    load_style();
    updateAccels();
    UndoManager::instance().onError = [this](const std::string &e) {
        MainWindow *w = activeWindow();
        show_error(w ? w->widget() : nullptr, _("Undo failed."), e);
    };
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    g_signal_connect_swapped(cb, "changed", CB([](App *a) { a->clipboardChanged(); }), this);
    clipboardChanged();
    settingsListener_ = add_settings_listener([this] { applySettings(); });
    JobManager::instance().addListener([this] {
        if (!JobManager::instance().jobs().empty()) return;
        for (MainWindow *w : windows_) w->updateActions();
        if (!settings().trashUseSizeLimit && !settings().trashUseTimeLimit) return;
        std::thread([] {
            std::string warn = trash::apply_limits();
            if (!warn.empty())
                run_on_main([warn] {
                    if (MainWindow *w = App::instance().activeWindow())
                        if (w->activeContainer()) w->activeContainer()->showMessage(warn, false);
                });
        }).detach();
    });
    if (!selftest && (settings().trashUseSizeLimit || settings().trashUseTimeLimit))
        std::thread([] {
            std::string warn = trash::apply_limits();
            if (!warn.empty())
                run_on_main([warn] {
                    if (MainWindow *w = App::instance().activeWindow())
                        if (w->activeContainer()) w->activeContainer()->showMessage(warn, false);
                });
        }).detach();
    if (!selftest) {
        ownFileManagerName();
        // Files opened from archives last session (copies in the cache) are no longer needed.
        std::thread([] {
            Ref<GFile> d(g_file_new_for_path((std::string(g_get_user_cache_dir()) + "/beluga/archives").c_str()));
            trash::delete_recursive(d.get(), nullptr, nullptr);
        }).detach();
    }
}

void App::shutdown()
{
    if (busRegistration_) {
        GDBusConnection *bus = g_application_get_dbus_connection(G_APPLICATION(app_));
        if (bus) g_dbus_connection_unregister_object(bus, busRegistration_);
    }
    if (busOwner_) g_bus_unown_name(busOwner_);
    settings().save();
}

void App::updateAccels()
{
    if (!app_) return;
    for (GtkShortcutTrigger *t : triggers_) g_object_unref(t);
    triggers_.clear();
    for (auto &s : shortcut_table()) {
        std::vector<std::string> accels = s.defaults;
        auto it = settings().shortcuts.find(s.action);
        if (it != settings().shortcuts.end()) accels = it->second;
        std::vector<const char *> p;
        for (auto &a : accels) {
            p.push_back(a.c_str());
            if (GtkShortcutTrigger *t = gtk_shortcut_trigger_parse_string(a.c_str())) triggers_.push_back(t);
        }
        p.push_back(nullptr);
        gtk_application_set_accels_for_action(app_, s.action, p.data());
    }
}

bool App::isShortcut(GdkEvent *event) const
{
    for (GtkShortcutTrigger *t : triggers_)
        if (gtk_shortcut_trigger_trigger(t, event, FALSE) == GDK_KEY_MATCH_EXACT) return true;
    return false;
}

void App::applySettings() { updateAccels(); }

// ---- windows -------------------------------------------------------------------------------

MainWindow *App::newWindow(const std::vector<std::string> &uris, bool split)
{
    auto *w = new MainWindow(app_);
    windows_.push_back(w);
    std::vector<std::string> list = uris;
    if (list.empty()) list.push_back(settings().homeUrl);
    for (size_t i = 0; i < list.size(); ++i) w->openTab(list[i], {}, i == 0);
    w->activateTab(0);
    if ((split || settings().splitView) && w->activeTab()) w->activeTab()->setSplit(true, list.size() > 1 && split ? list[1] : list[0]);
    w->present();
    return w;
}

MainWindow *App::activeWindow() const
{
    if (!app_) return nullptr;
    MainWindow *w = MainWindow::from(gtk_application_get_active_window(app_));
    if (!w && !windows_.empty()) w = windows_.back();
    return w;
}

void App::windowClosed(MainWindow *w)
{
    if (windows_.size() == 1 && windows_[0] == w && settings().rememberOpenedTabs && !selftest) saveSession();
    windows_.erase(std::remove(windows_.begin(), windows_.end(), w), windows_.end());
}

void App::quit()
{
    if (settings().rememberOpenedTabs && !selftest) saveSession();
    auto copy = windows_;
    for (MainWindow *w : copy) gtk_window_close(w->window());
}

void App::openLocations(const std::vector<std::string> &uris, bool select, bool split, bool forceNewWindow)
{
    std::vector<std::string> folders;
    std::vector<std::string> toSelect;
    for (auto &u : uris) {
        Ref<GFile> f(g_file_new_for_uri(u.c_str()));
        bool dir = g_file_query_file_type(f.get(), G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY;
        if (select || (!dir && !is_virtual_uri(u))) {
            Ref<GFile> parent(g_file_get_parent(f.get()));
            std::string pu = parent ? file_uri(parent.get()) : u;
            if (std::find(folders.begin(), folders.end(), pu) == folders.end()) folders.push_back(pu);
            toSelect.push_back(u);
        } else {
            folders.push_back(u);
        }
    }
    MainWindow *w = activeWindow();
    if (w && !forceNewWindow && settings().openExternallyCalledFolderInNewTab) {
        for (size_t i = 0; i < folders.size(); ++i) w->openTab(folders[i], {}, i == 0);
        w->present();
    } else {
        w = newWindow(folders, split);
    }
    if (!toSelect.empty() && w->activeView()) w->activeView()->selectUris(toSelect);
}

// ---- clipboard ---------------------------------------------------------------------------------

void App::setClipboard(const std::vector<Ref<GFile>> &files, bool cut)
{
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    std::vector<GFile *> raw;
    std::string gnome = cut ? "cut" : "copy";
    std::string text;
    for (auto &f : files) {
        raw.push_back(f.get());
        gnome += "\n" + file_uri(f.get());
        if (!text.empty()) text += "\n";
        text += display_location(f.get());
    }
    GdkFileList *list = gdk_file_list_new_from_array(raw.data(), raw.size());
    GBytes *gb = g_bytes_new(gnome.data(), gnome.size());
    GBytes *kde = g_bytes_new(cut ? "1" : "0", 1);
    GBytes *tb = g_bytes_new(text.data(), text.size());
    GdkContentProvider *providers[] = {
        gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list),
        gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", gb),
        gdk_content_provider_new_for_bytes("application/x-kde-cutselection", kde),
        gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", tb),
    };
    GdkContentProvider *u = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
    g_boxed_free(GDK_TYPE_FILE_LIST, list);
    g_bytes_unref(gb);
    g_bytes_unref(kde);
    g_bytes_unref(tb);
    gdk_clipboard_set_content(cb, u);
    g_object_unref(u);
    clipFiles_ = files;
    clipCut_ = cut;
    cutUris_.clear();
    if (cut)
        for (auto &f : files) cutUris_.insert(file_uri(f.get()));
    canPaste_ = true;
    for (MainWindow *w : windows_) {
        w->updateCutState();
        w->updateActions();
    }
}

void App::clipboardChanged()
{
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    GdkContentFormats *f = gdk_clipboard_get_formats(cb);
    canPaste_ = gdk_content_formats_contain_gtype(f, GDK_TYPE_FILE_LIST) || gdk_content_formats_contain_mime_type(f, "text/uri-list")
             || gdk_content_formats_contain_mime_type(f, "x-special/gnome-copied-files");
    if (!gdk_clipboard_is_local(cb)) {
        clipFiles_.clear();
        clipCut_ = false;
        if (!cutUris_.empty()) {
            cutUris_.clear();
            for (MainWindow *w : windows_) w->updateCutState();
        }
    }
    for (MainWindow *w : windows_) w->updateActions();
}

void App::clearClipboardAfterMove()
{
    if (!clipCut_) return;
    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    if (gdk_clipboard_is_local(cb)) gdk_clipboard_set_content(cb, nullptr);
    clipFiles_.clear();
    clipCut_ = false;
    cutUris_.clear();
    canPaste_ = false;
    for (MainWindow *w : windows_) {
        w->updateCutState();
        w->updateActions();
    }
}

namespace {
struct ReadData {
    std::function<void(std::vector<Ref<GFile>>, bool)> cb;
    std::vector<Ref<GFile>> files;
};

// Reads a clipboard stream to the end without blocking the main loop: another program writes it,
// and a slow or hung one would otherwise freeze the window.
void read_stream(GInputStream *in, std::function<void(std::string)> done)
{
    GOutputStream *out = g_memory_output_stream_new_resizable();
    g_output_stream_splice_async(
        out, in, GOutputStreamSpliceFlags(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
        G_PRIORITY_DEFAULT, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer p) {
            std::unique_ptr<std::function<void(std::string)>> fn(static_cast<std::function<void(std::string)> *>(p));
            GOutputStream *o = G_OUTPUT_STREAM(src);
            std::string s;
            if (g_output_stream_splice_finish(o, res, nullptr) >= 0) {
                auto *m = G_MEMORY_OUTPUT_STREAM(o);
                s.assign(static_cast<const char *>(g_memory_output_stream_get_data(m)), g_memory_output_stream_get_data_size(m));
            }
            g_object_unref(o);
            (*fn)(std::move(s));
        },
        new std::function<void(std::string)>(std::move(done)));
}
} // namespace

void App::readClipboard(std::function<void(std::vector<Ref<GFile>>, bool)> cb)
{
    GdkClipboard *clip = gdk_display_get_clipboard(gdk_display_get_default());
    if (gdk_clipboard_is_local(clip) && !clipFiles_.empty()) {
        cb(clipFiles_, clipCut_);
        return;
    }
    GdkContentFormats *f = gdk_clipboard_get_formats(clip);
    auto *data = new ReadData{std::move(cb), {}};
    if (gdk_content_formats_contain_mime_type(f, "x-special/gnome-copied-files")) {
        static const char *mimes[] = {"x-special/gnome-copied-files", nullptr};
        gdk_clipboard_read_async(clip, mimes, G_PRIORITY_DEFAULT, nullptr, [](GObject *src, GAsyncResult *res, gpointer p) {
            std::unique_ptr<ReadData> d(static_cast<ReadData *>(p));
            GInputStream *in = gdk_clipboard_read_finish(GDK_CLIPBOARD(src), res, nullptr, nullptr);
            if (!in) return;
            read_stream(in, [d = std::shared_ptr<ReadData>(d.release())](std::string text) {
                auto lines = split(text, '\n');
                bool cut = !lines.empty() && trim(lines[0]) == "cut";
                for (size_t i = 1; i < lines.size(); ++i) {
                    std::string u = trim(lines[i]);
                    if (!u.empty()) d->files.push_back(Ref<GFile>(g_file_new_for_uri(u.c_str())));
                }
                d->cb(d->files, cut);
            });
            g_object_unref(in);
        }, data);
        return;
    }
    bool kdeCut = gdk_content_formats_contain_mime_type(f, "application/x-kde-cutselection");
    g_object_set_data(G_OBJECT(clip), "beluga-kde-cut", GINT_TO_POINTER(kdeCut));
    gdk_clipboard_read_value_async(clip, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, nullptr, [](GObject *src, GAsyncResult *res, gpointer p) {
        std::unique_ptr<ReadData> d(static_cast<ReadData *>(p));
        const GValue *v = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(src), res, nullptr);
        if (!v) return;
        GSList *l = gdk_file_list_get_files(static_cast<GdkFileList *>(g_value_get_boxed(v)));
        for (GSList *i = l; i; i = i->next) d->files.push_back(Ref<GFile>::share(G_FILE(i->data)));
        g_slist_free(l);
        if (!GPOINTER_TO_INT(g_object_get_data(src, "beluga-kde-cut"))) {
            d->cb(d->files, false);
            return;
        }
        static const char *mimes[] = {"application/x-kde-cutselection", nullptr};
        gdk_clipboard_read_async(GDK_CLIPBOARD(src), mimes, G_PRIORITY_DEFAULT, nullptr, [](GObject *src2, GAsyncResult *res2, gpointer p2) {
            std::unique_ptr<ReadData> d2(static_cast<ReadData *>(p2));
            GInputStream *in = gdk_clipboard_read_finish(GDK_CLIPBOARD(src2), res2, nullptr, nullptr);
            if (!in) {
                d2->cb(d2->files, false);
                return;
            }
            read_stream(in, [d3 = std::shared_ptr<ReadData>(d2.release())](std::string text) {
                d3->cb(d3->files, trim(text) == "1");
            });
            g_object_unref(in);
        }, d.release());
    }, data);
}

// ---- session -------------------------------------------------------------------------------------

void App::saveSession()
{
    GKeyFile *kf = g_key_file_new();
    for (size_t i = 0; i < windows_.size(); ++i) windows_[i]->saveSession(kf, printf_str("Window %zu", i));
    g_key_file_save_to_file(kf, (state_dir() + "/session.conf").c_str(), nullptr);
    g_key_file_free(kf);
}

bool App::restoreSession()
{
    GKeyFile *kf = g_key_file_new();
    bool any = false;
    if (g_key_file_load_from_file(kf, (state_dir() + "/session.conf").c_str(), G_KEY_FILE_NONE, nullptr)) {
        gsize n = 0;
        char **groups = g_key_file_get_groups(kf, &n);
        for (gsize i = 0; i < n; ++i) {
            auto *w = new MainWindow(app_);
            windows_.push_back(w);
            if (w->restoreSession(kf, groups[i])) {
                w->present();
                any = true;
            } else {
                windows_.pop_back();
                gtk_window_destroy(w->window());
                delete w;
            }
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    return any;
}

// ---- org.freedesktop.FileManager1 -------------------------------------------------------------

static const char *kIntrospection =
    "<node><interface name='org.freedesktop.FileManager1'>"
    "<method name='ShowFolders'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItems'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "<method name='ShowItemProperties'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "</interface></node>";

static void fm1_call(GDBusConnection *, const char *, const char *, const char *, const char *method, GVariant *params,
                     GDBusMethodInvocation *inv, gpointer)
{
    GVariantIter *iter = nullptr;
    const char *startup = nullptr;
    g_variant_get(params, "(as&s)", &iter, &startup);
    std::vector<std::string> uris;
    const char *u;
    while (g_variant_iter_next(iter, "&s", &u)) {
        Ref<GFile> f(g_file_new_for_commandline_arg(u));
        uris.push_back(file_uri(f.get()));
    }
    g_variant_iter_free(iter);
    App &app = App::instance();
    std::string m = method;
    if (m == "ShowItemProperties") {
        std::vector<Ref<GFile>> files;
        for (auto &x : uris) files.push_back(Ref<GFile>(g_file_new_for_uri(x.c_str())));
        MainWindow *w = app.activeWindow();
        show_properties_dialog(w ? w->widget() : nullptr, files);
    } else if (!uris.empty()) {
        app.openLocations(uris, m == "ShowItems", false, false);
    }
    g_dbus_method_invocation_return_value(inv, nullptr);
}

void App::ownFileManagerName()
{
    busOwner_ = g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.FileManager1", G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        [](GDBusConnection *conn, const char *, gpointer p) {
            auto *a = static_cast<App *>(p);
            GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
            static const GDBusInterfaceVTable vtable = {fm1_call, nullptr, nullptr, {nullptr}};
            a->busRegistration_ = g_dbus_connection_register_object(conn, "/org/freedesktop/FileManager1", node->interfaces[0], &vtable,
                                                                    nullptr, nullptr, nullptr);
            g_dbus_node_info_unref(node);
        },
        nullptr, nullptr, this, nullptr);
}

} // namespace beluga
