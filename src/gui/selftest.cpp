// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: --selftest. Runs the file operation engine against a temporary
// folder and, with --screenshots DIR, renders the main window in each view mode to PNG.
// Everything runs in a throwaway profile (XDG dirs point into the temporary folder), so
// the user's settings, places and trash are never touched.
#include "app.hpp"
#include "fileops.hpp"
#include "mainwindow.hpp"
#include "places.hpp"
#include "preferences.hpp"
#include "search.hpp"
#include "settings.hpp"
#include "tabpage.hpp"
#include "trash.hpp"
#include "undo.hpp"

#include <glib/gstdio.h>

#include <cstring>
#include <iostream>

namespace beluga {

namespace {

int g_failures = 0;
std::string g_root;
std::string g_shots;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "PASS " : "FAIL ") << what << std::endl;
    if (!ok) g_failures++;
}

bool exists(const std::string &rel) { return g_file_test((g_root + "/files/" + rel).c_str(), G_FILE_TEST_EXISTS); }

void write(const std::string &rel, const std::string &content)
{
    std::string p = g_root + "/files/" + rel;
    g_mkdir_with_parents(dirname_of(p).c_str(), 0755);
    g_file_set_contents(p.c_str(), content.c_str(), -1, nullptr);
}

Ref<GFile> F(const std::string &rel) { return Ref<GFile>(g_file_new_for_path((g_root + "/files/" + rel).c_str())); }

void run_job(FileJob::Ptr job)
{
    bool done = false;
    auto prev = job->onFinished;
    job->onFinished = [&done, prev](FileJob &j) {
        if (prev) prev(j);
        done = true;
    };
    job->start();
    gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
    while (!done && g_get_monotonic_time() < deadline) g_main_context_iteration(nullptr, TRUE);
}

void wait_ms(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(10000);
    }
}

void core_tests()
{
    write("a.txt", "alpha");
    write("b.txt", "beta needle");
    write("file10.txt", "x");
    write("file2.txt", "x");
    write("Folder 1/c.txt", "gamma");
    write("Folder 2/deep/d.md", "delta");
    write(".hidden", "h");

    run_job(FileJob::copy({F("a.txt")}, F("Folder 1").get()));
    check(exists("Folder 1/a.txt") && exists("a.txt"), "copy a file into a folder");

    run_job(FileJob::copy({F("a.txt")}, F("Folder 1").get()));
    check(exists("Folder 1/a.txt") && !exists("Folder 1/a (1).txt"), "copy conflict without UI skips");

    run_job(FileJob::copy({F("a.txt")}, F("").get()));
    check(exists("a copy.txt"), "copy onto itself makes a copy");

    run_job(FileJob::move({F("b.txt")}, F("Folder 2").get()));
    check(exists("Folder 2/b.txt") && !exists("b.txt"), "move a file");

    UndoManager::instance().undo();
    wait_ms(500);
    check(exists("b.txt") && !exists("Folder 2/b.txt"), "undo move");
    UndoManager::instance().redo();
    wait_ms(500);
    check(exists("Folder 2/b.txt"), "redo move");

    run_job(FileJob::copy({F("Folder 2")}, F("Folder 1").get()));
    check(exists("Folder 1/Folder 2/deep/d.md"), "copy a folder tree");

    auto trashJob = FileJob::trash({F("Folder 1/c.txt")});
    run_job(trashJob);
    bool inTrash = false;
    for (auto &e : trash::list())
        if (ends_with(e.originalPath, "/Folder 1/c.txt")) inTrash = true;
    check(!exists("Folder 1/c.txt") && inTrash, "move to trash");
    check(!trash::is_empty(), "trash is not empty");
    run_job(FileJob::restore(trashJob->trashed()));
    check(exists("Folder 1/c.txt"), "restore from trash");

    run_job(FileJob::trash({F("file2.txt")}));
    UndoManager::instance().undo();
    wait_ms(500);
    check(exists("file2.txt"), "undo trash");

    Ref<GFile> renamed;
    rename_file(F("file10.txt").get(), "renamed.txt", renamed, nullptr);
    UndoCommand c;
    c.type = UndoCommand::Rename;
    c.pairs.emplace_back(file_uri(F("file10.txt").get()), file_uri(renamed.get()));
    UndoManager::instance().push(c);
    check(exists("renamed.txt"), "rename");
    UndoManager::instance().undo();
    check(exists("file10.txt") && !exists("renamed.txt"), "undo rename");

    run_job(FileJob::remove({F("a copy.txt")}));
    check(!exists("a copy.txt"), "delete permanently");

    run_job(FileJob::emptyTrash());
    check(trash::is_empty(), "empty trash");

    // Search: names and content.
    SearchQuery q;
    q.rootUri = file_uri(F("").get());
    q.text = "c.txt";
    int found = 0;
    bool searchDone = false;
    auto job = SearchJob::start(q, [&](std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&r) { found += (int)r.size(); },
                                [&] { searchDone = true; });
    while (!searchDone) g_main_context_iteration(nullptr, TRUE);
    check(found == 1, "search by name");
    q.text = "needle";
    q.content = true;
    found = 0;
    searchDone = false;
    job = SearchJob::start(q, [&](std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&r) { found += (int)r.size(); },
                           [&] { searchDone = true; });
    while (!searchDone) g_main_context_iteration(nullptr, TRUE);
    check(found == 2, printf_str("search by content (%d)", found));
    SearchQuery back;
    check(SearchQuery::fromUri(q.toUri(), back) && back.text == "needle" && back.content, "search URI round trip");

    check(collate_key("file2", 0) < collate_key("file10", 0), "natural sorting");
    check(filter_match("*.txt", "notes.TXT") && filter_match("ote", "Notes") && !filter_match("*.md", "a.txt"), "filter patterns");
    check(unique_name(F("").get(), "a.txt") == "a (1).txt", "unique names");
    check(format_permissions(040755, 0) == "drwxr-xr-x" && format_permissions(0100644, 1) == "0644", "permission formats");

    Places &pl = Places::instance();
    size_t n = pl.list().size();
    pl.add({"Test", file_uri(F("").get()), "folder", false});
    check(pl.list().size() == n + 1 && pl.bestMatch(file_uri(F("Folder 1").get())) == (int)n, "places add and match");
    pl.remove((int)n);
}

} // namespace

// Renders a widget (normally a window) to a PNG file. The paintable is empty until the
// widget has been drawn since its last change; offscreen backends draw rarely, so ask
// for frames until one arrives.
bool save_widget_png(GtkWidget *w, const std::string &path, int retries)
{
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    if (width <= 0 || height <= 0) return false;
    GskRenderNode *node = nullptr;
    for (int tries = 0; tries <= retries && !node; ++tries) {
        GdkPaintable *p = gtk_widget_paintable_new(w);
        GtkSnapshot *s = gtk_snapshot_new();
        gdk_paintable_snapshot(p, s, width, height);
        node = gtk_snapshot_free_to_node(s);
        g_object_unref(p);
        if (!node) {
            gtk_widget_queue_draw(w);
            wait_ms(100);
        }
    }
    if (!node) return false;
    // A private Cairo renderer works on every backend, including offscreen Broadway.
    GskRenderer *r = gsk_cairo_renderer_new();
    gsk_renderer_realize_for_display(r, gtk_widget_get_display(w), nullptr);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
    GdkTexture *t = gsk_renderer_render_texture(r, node, &bounds);
    gsk_renderer_unrealize(r);
    g_object_unref(r);
    gsk_render_node_unref(node);
    bool ok = gdk_texture_save_to_png(t, path.c_str());
    g_object_unref(t);
    return ok;
}

namespace {

bool shoot(GtkWidget *w, const std::string &name)
{
    if (g_shots.empty()) return true;
    std::string path = g_shots + "/" + name + ".png";
    bool ok = save_widget_png(w, path, 40);
    std::cout << (ok ? "SHOT " : "FAIL shot ") << path << std::endl;
    if (!ok) g_failures++;
    return ok;
}

void gui_tests(GtkApplication *app)
{
    App::instance().selftest = true;
    App::instance().init(app);
    // A picture for previews.
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 320, 200);
    gdk_pixbuf_fill(pb, 0x3c8fd0ff);
    gdk_pixbuf_save(pb, (g_root + "/files/picture.png").c_str(), "png", nullptr, nullptr);
    g_object_unref(pb);
    for (int i = 0; i < 30; ++i) write(printf_str("many/item %02d.txt", i), "x");

    MainWindow *w = App::instance().newWindow({file_uri(F("").get())}, false);
    gtk_window_set_default_size(w->window(), 1100, 700);
    wait_ms(1500);
    View *v = w->activeView();
    check(v && v->location() == file_uri(F("").get()), "window shows the folder");
    int files = 0, folders = 0;
    guint64 size = 0;
    if (v) v->countItems(files, folders, size);
    check(folders == 3 && files >= 4, printf_str("view lists items (%d folders, %d files)", folders, files));
    check(v && !v->findItem(file_uri(F(".hidden").get())), "hidden files are hidden");
    v->setHiddenShown(true);
    wait_ms(200);
    check(v->findItem(file_uri(F(".hidden").get())) != nullptr, "show hidden files");
    v->setHiddenShown(false);

    v->setViewMode(ModeIcons);
    wait_ms(1200);
    shoot(w->widget(), "1-icons");
    v->setViewMode(ModeCompact);
    wait_ms(600);
    shoot(w->widget(), "2-compact");
    v->setViewMode(ModeDetails);
    wait_ms(600);
    int before = v->itemView()->allocCount;
    wait_ms(500);
    check(v->itemView()->allocCount - before < 5, printf_str("details view settles (%d allocations in 0.5 s)", v->itemView()->allocCount - before));
    shoot(w->widget(), "3-details");
    v->setGrouped(true);
    v->setSortRole("type");
    wait_ms(600);
    shoot(w->widget(), "4-details-grouped");
    v->setGrouped(false);
    v->setSortRole("text");
    v->setViewMode(ModeIcons);

    w->activeContainer()->navigate(file_uri(F("Folder 2").get()));
    w->activeView()->setViewMode(ModeColumns);
    w->activeContainer()->navigate(file_uri(F("Folder 2/deep").get()));
    wait_ms(1000);
    check(w->activeView()->location() == file_uri(F("Folder 2/deep").get()) && w->activeView()->listMode(), "columns view follows into subfolders");
    shoot(w->widget(), "8-columns");
    w->activeView()->setViewMode(ModeIcons);
    w->activeContainer()->navigate(file_uri(F("").get()));
    wait_ms(600);
    v = w->activeView();

    v->setNameFilter("*.txt");
    wait_ms(200);
    v->countItems(files, folders, size);
    check(folders == 0 && files >= 2, "name filter");
    v->setNameFilter({});

    w->activeContainer()->navigate(file_uri(F("Folder 1").get()));
    wait_ms(600);
    w->activeContainer()->goUp();
    wait_ms(600);
    check(w->activeView()->location() == file_uri(F("").get()), "go up");
    w->activeContainer()->goBack();
    wait_ms(400);
    check(w->activeView()->location() == file_uri(F("Folder 1").get()), "go back");
    w->activeContainer()->goBack();
    wait_ms(400);

    settings().foldersPanel = true;
    settings().infoPanel = true;
    notify_settings_changed();
    w->activeTab()->setSplit(true, file_uri(F("many").get()));
    w->openTab(file_uri(F("Folder 2").get()), {}, false);
    wait_ms(1500);
    shoot(w->widget(), "5-split-panels-tabs");
    check(w->activeTab()->split() && w->tabCount() == 2, "split view and tabs");

    w->activeContainer()->setSearchBarVisible(true);
    SearchQuery q;
    q.rootUri = file_uri(F("").get());
    q.text = "item";
    w->activeContainer()->navigate(q.toUri());
    wait_ms(1000);
    shoot(w->widget(), "6-search");
    int sf = 0, sd = 0;
    w->activeView()->countItems(sf, sd, size);
    check(sf == 30, printf_str("search results in the view (%d)", sf));

    // Run the actions a user reaches from menus and shortcuts on a selected file, so that the
    // dialogs and panels they open are built at least once (catches crashes under ASan).
    w->activeContainer()->navigate(file_uri(F("").get()));
    wait_ms(600);
    w->activeView()->selectUris({file_uri(F("picture.png").get())});
    wait_ms(300);
    {
        GActionGroup *ag = G_ACTION_GROUP(w->window());
        const char *plain[] = {"properties", "folder-properties", "open-with", "view-properties", "selection-mode",
                               "selection-mode", "select-all", "invert-selection", "zoom-in", "zoom-out", "zoom-reset",
                               "copy", "copy-location", "show-menubar", "show-menubar", "focus-other-view",
                               "focus-other-view", "editable-location", "editable-location", "toggle-filter",
                               "toggle-filter", "reload", "duplicate", "split-to-tabs"};
        for (const char *a : plain) {
            g_action_group_activate_action(ag, a, nullptr);
            wait_ms(150);
        }
        for (const char *role : {"size", "modificationtime", "type", "text"}) {
            g_action_group_activate_action(ag, "sort-by", g_variant_new_string(role));
            wait_ms(80);
        }
        w->activeView()->selectUris({file_uri(F("picture.png").get())});
        wait_ms(200);
        w->showContextMenu(w->activeContainer(), 0, 40, 40);
        wait_ms(400);
        w->showContextMenu(w->activeContainer(), -1, 400, 400);
        wait_ms(400);
    }
    int dialogs = 0;
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next) {
        GtkWindow *t = GTK_WINDOW(l->data);
        if (t == w->window() || MainWindow::from(t) || !gtk_widget_get_visible(GTK_WIDGET(t))) continue;
        int minH = 0;
        gtk_widget_measure(GTK_WIDGET(t), GTK_ORIENTATION_VERTICAL, -1, &minH, nullptr, nullptr, nullptr);
        const char *title = gtk_window_get_title(t);
        std::cout << "  dialog \"" << (title ? title : "") << "\" min height " << minH << std::endl;
        check(minH <= 800, printf_str("dialog \"%s\" fits a small screen (%d px)", title ? title : "", minH));
        shoot(GTK_WIDGET(t), printf_str("9-dialog-%d", dialogs));
        dialogs++;
        gtk_window_destroy(t);
    }
    g_list_free(tops);
    wait_ms(300);
    check(dialogs >= 3, printf_str("menu actions and dialogs (%d dialogs)", dialogs));

    w->activeContainer()->navigate("trash:///");
    wait_ms(600);
    check(w->activeView()->isTrash(), "trash location");

    show_preferences(w->widget());
    wait_ms(800);
    GtkWindow *prefs = nullptr;
    for (GList *l = gtk_application_get_windows(app); l; l = l->next)
        if (gtk_widget_has_css_class(GTK_WIDGET(l->data), "beluga-preferences")) prefs = GTK_WINDOW(l->data);
    if (prefs) {
        shoot(GTK_WIDGET(prefs), "7-preferences");
        gtk_window_destroy(prefs);
    }
    check(prefs != nullptr, "preferences window");
}

} // namespace

int run_selftest(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--screenshots") && i + 1 < argc) g_shots = argv[++i];
    char *tmp = g_dir_make_tmp("beluga-selftest-XXXXXX", nullptr);
    if (!tmp) return 2;
    g_root = tmp;
    g_free(tmp);
    for (const char *d : {"config", "data", "state", "cache", "files"}) g_mkdir_with_parents((g_root + "/" + d).c_str(), 0700);
    g_setenv("XDG_CONFIG_HOME", (g_root + "/config").c_str(), TRUE);
    g_setenv("XDG_DATA_HOME", (g_root + "/data").c_str(), TRUE);
    g_setenv("XDG_STATE_HOME", (g_root + "/state").c_str(), TRUE);
    g_setenv("XDG_CACHE_HOME", (g_root + "/cache").c_str(), TRUE);
    if (!g_shots.empty()) g_mkdir_with_parents(g_shots.c_str(), 0755);
    settings().load();
    settings().confirmClosingMultipleTabs = false;
    std::cout << "Self-test folder: " << g_root << std::endl;

    core_tests();

    GtkApplication *app = gtk_application_new(BELUGA_APP_ID ".SelfTest", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *a) {
        g_application_hold(G_APPLICATION(a));
        gui_tests(a);
        App::instance().quit();
        g_application_release(G_APPLICATION(a));
    }), nullptr);
    char *fakeArgv[] = {argv[0], nullptr};
    g_application_run(G_APPLICATION(app), 1, fakeArgv);
    g_object_unref(app);

    GFile *root = g_file_new_for_path(g_root.c_str());
    trash::delete_recursive(root, nullptr, nullptr);
    g_object_unref(root);
    std::cout << (g_failures ? "SELFTEST FAILED: " : "SELFTEST OK") << (g_failures ? std::to_string(g_failures) : "") << std::endl;
    return g_failures ? 1 : 0;
}

} // namespace beluga
